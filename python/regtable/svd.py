"""A narrow CMSIS-SVD reader for `regtable gen`.

An SVD file is the chip vendor's description of the silicon: every
peripheral, its registers, their addresses, sizes, access, and bit
fields. `regtable gen` lets a YAML pick a few of those registers (or
single fields) and exposes them as read-only entries next to the
application's own, so the table shows silicon state over the same
CLI, panel, and MQTT topics with no HAL call and no debugger.

This reader takes what a pick needs and nothing else: peripherals
(with derivedFrom, dim arrays), clusters, registers (address, size,
access, readAction, description, dim arrays), fields (bit position and
width, description, readAction, access). The file is used as written:
the address and meaning of every register are the vendor's, and a
wrong SVD gives a wrong address the same way a wrong `modbus_addr`
gives a wrong map.

It is as strict as the rest of the generator on what it takes. Every
shape it reads is parsed by the CMSIS-SVD 1.3 definition: the child
elements each level may carry (a misspelt or stray element is refused,
it cannot fall back to an inherited value), identifierType for names,
scaledNonNegativeInteger `[+]?(0x|0X|#)?[0-9a-fA-F]+[kmgtKMGT]?` (binary
after #, no don't-care bits: these are addresses, sizes, and counts),
dimIndexType, bitRangeType, the access and readAction enumerations,
derivedFrom at the peripheral level one level deep. Anything repeated,
anything empty, anything undefined for a register that is then picked
(its size or access declared at no level) raises SvdError with the
element's path.

What it declines rather than interprets: a device without
addressUnitBits or with one other than 8, and, at pick time, a
peripheral with a disableCondition (inherited along derivedFrom like
size and access), a write-only field inside the bytes a whole-register
pick reads, and a readAction on any other register at the same address
(an alternate view), each of which `force: true` overrides.

What it accepts and leaves alone, because a read of an address needs
none of it: alternateGroup and the alternate* names themselves,
protection, dataType, resetValue, enumeratedValues, writeConstraint,
modifiedWriteValues, the device width. A pick reads the address the
SVD gives, as an unsigned value of the register's width.

Descriptions are kept as written and rendered to ASCII by a fixed
table when a pick uses them; a character with no ASCII form is refused
by name. Nothing is guessed and nothing is dropped.
"""

import re
import unicodedata
import xml.etree.ElementTree as ET


class SvdError(ValueError):
    """The SVD could not be read, or a pick does not resolve in it."""


ACCESS = {"read-only", "write-only", "read-write", "writeOnce", "read-writeOnce"}
READ_ACTIONS = {"clear", "set", "modify", "modifyExternal"}
ADDR_MAX = 0xFFFFFFFF
DIM_MAX = 65536                 # no array in any device comes near; a guard
CLUSTER_DEPTH_MAX = 16          # against files built to exhaust the reader

# the child elements the CMSIS-SVD 1.3 schema defines at each level; an
# element outside its level's set is refused, so a misspelt <size> or
# <readAction> cannot fall back to an inherited value
_DIM = {"dim", "dimIncrement", "dimIndex", "dimName", "dimArrayIndex"}
_PROPS = {"size", "access", "protection", "resetValue", "resetMask"}
ELEMENTS = {
    "device": {"vendor", "vendorID", "name", "series", "version", "description",
               "licenseText", "cpu", "headerSystemFilename", "headerDefinitionsPrefix",
               "addressUnitBits", "width", "peripherals", "vendorExtensions"} | _PROPS,
    "peripherals": {"peripheral"},
    "peripheral": _DIM | _PROPS | {"name", "version", "description", "alternatePeripheral",
                                   "groupName", "prependToName", "appendToName",
                                   "headerStructName", "disableCondition", "baseAddress",
                                   "addressBlock", "interrupt", "registers"},
    "registers": {"register", "cluster"},
    "cluster": _DIM | _PROPS | {"name", "description", "alternateCluster", "headerStructName",
                                "addressOffset", "register", "cluster"},
    "register": _DIM | _PROPS | {"name", "displayName", "description", "alternateGroup",
                                 "alternateRegister", "addressOffset", "dataType",
                                 "modifiedWriteValues", "writeConstraint", "readAction",
                                 "fields"},
    "cpu": {"name", "revision", "endian", "mpuPresent", "fpuPresent", "fpuDP", "dspPresent",
            "icachePresent", "dcachePresent", "itcmPresent", "dtcmPresent", "vtorPresent",
            "nvicPrioBits", "vendorSystickConfig", "deviceNumInterrupts", "pmuPresent",
            "pmuNumEventCnt", "sauNumRegions", "sauRegionsConfig"},
    "fields": {"field"},
    "field": _DIM | {"name", "description", "bitOffset", "bitWidth", "lsb", "msb", "bitRange",
                     "access", "modifiedWriteValues", "writeConstraint", "readAction",
                     "enumeratedValues"},
}
_ATTRS = {"device": {"schemaVersion"}, "peripheral": {"derivedFrom"}}

# identifierType: a C identifier, with %s (or [%s]) as the index of a dim array
_IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")

# deterministic transliteration of the symbols vendor descriptions use;
# anything outside this table and NFKD is refused by name, never dropped
TRANSLIT = {
    "\u00b5": "u", "\u03bc": "u", "\u00b1": "+/-", "\u00b0": "deg", "\u00b2": "^2",
    "\u00b3": "^3", "\u00d7": "x", "\u00f7": "/", "\u03a9": "ohm", "\u2126": "ohm",
    "\u2013": "-", "\u2014": "-", "\u2212": "-", "\u2018": "'", "\u2019": "'",
    "\u201c": '"', "\u201d": '"', "\u2026": "...", "\u2264": "<=", "\u2265": ">=",
    "\u2260": "!=", "\u2248": "~", "\u2022": "*", "\u2192": "->", "\u2190": "<-",
    "\u00a0": " ", "\u00ae": "(R)", "\u2122": "(TM)", "\u00a9": "(C)", "\u00bd": "1/2",
    "\u00bc": "1/4", "\u00be": "3/4", "\u00ab": "<<", "\u00bb": ">>", "\u2032": "'",
    "\u2033": '"', "\u00b7": ".", "\u2044": "/",
}

_NUMBER = re.compile(r"^\+?(?:0[xX]([0-9a-fA-F]+)|#([01]+)|([0-9]+))([kKmMgGtT])?$")
_SCALE = {"k": 1 << 10, "m": 1 << 20, "g": 1 << 30, "t": 1 << 40}
_DIM_NUM = re.compile(r"^([0-9]+)-([0-9]+)$")
_DIM_ABC = re.compile(r"^([A-Z])-([A-Z])$")
_DIM_LIST = re.compile(r"^[_0-9a-zA-Z]+(?:,\s*[_0-9a-zA-Z]+)+$")
_BITRANGE = re.compile(r"^\[([0-9]{1,2}):([0-9]{1,2})\]$")


class SvdTable(dict):
    """The registers by path, plus what the <device> header says about
    the chip: chip = {"name", "series", "cpu"} (keys present when the
    file gives them). cpu is the schema's own token (cpuNameType:
    CM0PLUS, CM3, other, ...; CM0+ written as CM0PLUS), which is what
    two files describing one chip are compared on; cpu_display() spells
    it out for people."""
    chip = {}


# cpuNameType: the schema's enumeration (CMSIS-SVD 1.3.12), and how
# each core is spelt out; "other" is a stated value (some other
# processor) with no name to show
CPU_NAMES = {
    "CM0": "Cortex-M0", "CM0PLUS": "Cortex-M0+", "CM1": "Cortex-M1",
    "CM3": "Cortex-M3", "CM4": "Cortex-M4", "CM7": "Cortex-M7", "CM23": "Cortex-M23",
    "CM33": "Cortex-M33", "CM35P": "Cortex-M35P", "CM52": "Cortex-M52", "CM55": "Cortex-M55",
    "CM85": "Cortex-M85", "SC000": "SecurCore SC000", "SC300": "SecurCore SC300",
    "ARMV8MML": "Armv8-M Mainline", "ARMV8MBL": "Armv8-M Baseline",
    "ARMV81MML": "Armv8.1-M Mainline", "CA5": "Cortex-A5", "CA7": "Cortex-A7",
    "CA8": "Cortex-A8", "CA9": "Cortex-A9", "CA15": "Cortex-A15", "CA17": "Cortex-A17",
    "CA53": "Cortex-A53", "CA57": "Cortex-A57", "CA72": "Cortex-A72", "SMC1": "SMC1",
    "other": None,
}
_CPU_ALIASES = {"CM0+": "CM0PLUS"}       # two spellings the schema allows for one core


def _cpu_token(raw, what):
    """The cpu name as one of the schema's tokens, aliases folded."""
    token = _CPU_ALIASES.get(raw, raw)
    if token not in CPU_NAMES:
        raise SvdError(f"{what}: cpu name {raw!r} is not one the CMSIS-SVD schema lists "
                       f"({', '.join(list(CPU_NAMES) + list(_CPU_ALIASES))})")
    return token


def cpu_display(token):
    """The core spelt out (CM0PLUS -> Cortex-M0+), None for other."""
    return CPU_NAMES[token]


class Field:
    __slots__ = ("name", "lsb", "width", "desc", "read_action", "access")

    def __init__(self, name, lsb, width, desc, read_action, access):
        self.name, self.lsb, self.width = name, lsb, width
        self.desc, self.read_action, self.access = desc, read_action, access


class Reg:
    __slots__ = ("path", "addr", "size", "access", "read_action", "desc", "fields",
                 "disabled_by")

    def __init__(self, path, addr, size, access, read_action, desc, disabled_by=None):
        self.path, self.addr, self.size = path, addr, size
        self.access, self.read_action, self.desc = access, read_action, desc
        self.fields = {}
        self.disabled_by = disabled_by      # the peripheral's disableCondition, if any


# -- element access: one of each, never empty --------------------------

def _one(el, tag, what):
    """The single child <tag>, or None when absent. Two of them, or an
    empty one, is a malformed file."""
    found = el.findall(tag)
    if not found:
        return None
    if len(found) > 1:
        raise SvdError(f"{what}: <{tag}> appears {len(found)} times")
    text = (found[0].text or "").strip()
    if not text:
        raise SvdError(f"{what}: <{tag}> is empty")
    return text


def _known(el, what):
    """Every child element (and attribute) of el is one the schema
    defines at el's level; a stray or misspelt one is refused by name."""
    allowed = ELEMENTS[el.tag]
    for c in el:
        if c.tag not in allowed:
            raise SvdError(f"{what}: element <{c.tag}> is not defined for <{el.tag}> "
                           f"in CMSIS-SVD (misspelt, or out of place)")
    for a in el.attrib:
        # xsi:schemaLocation and kin on <device> are XML plumbing, not data
        if a.startswith("{http://www.w3.org/2001/XMLSchema-instance}") and el.tag == "device":
            continue
        if a not in _ATTRS.get(el.tag, set()):
            raise SvdError(f"{what}: attribute {a!r} is not defined for <{el.tag}>")


def _ident(name, what):
    """identifierType: a C identifier; %s or [%s] marks a dim index."""
    bare = name.replace("[%s]", "").replace("%s", "")
    if not bare or not _IDENT.match(bare):
        raise SvdError(f"{what} {name!r}: not a CMSIS identifier "
                       f"([A-Za-z_][A-Za-z0-9_]*, %s or [%s] for an array index)")
    return name


def ascii_text(s, what, hint="; give the pick a desc: of its own"):
    """s as printable ASCII on one line: NFKD, then the symbol table;
    a character neither covers is refused by name, never dropped (a
    dropped sign changes what a description means). what names the
    text in the message, hint says what to do about it."""
    out = []
    for ch in " ".join(s.split()):
        if 32 <= ord(ch) < 127:
            out.append(ch)
            continue
        if ch in TRANSLIT:
            out.append(TRANSLIT[ch])
            continue
        base = "".join(c for c in unicodedata.normalize("NFKD", ch)
                       if not unicodedata.combining(c))
        if base and all(32 <= ord(c) < 127 for c in base):
            out.append(base)
            continue
        raise SvdError(f"{what} has {ch!r} (U+{ord(ch):04X}), which has no "
                       f"ASCII form here{hint}")
    return "".join(out)


def _number(text, what):
    """A scaledNonNegativeInteger by the schema's pattern."""
    m = _NUMBER.match(text)
    if not m:
        raise SvdError(f"{what}: {text!r} is not a CMSIS number "
                       f"(decimal, 0x hex, #binary, optional k/m/g/t)")
    hx, bn, dc, scale = m.groups()
    v = int(hx, 16) if hx is not None else int(bn, 2) if bn is not None else int(dc, 10)
    if scale:
        v *= _SCALE[scale.lower()]
    return v


def _int(el, tag, what):
    """Required number child."""
    t = _one(el, tag, what)
    if t is None:
        raise SvdError(f"{what}: <{tag}> is missing")
    return _number(t, f"{what} {tag}")


def _opt_int(el, tag, what):
    t = _one(el, tag, what)
    return None if t is None else _number(t, f"{what} {tag}")


def _name(el, what):
    n = _one(el, "name", what)
    if n is None:
        raise SvdError(f"{what} has no name")
    if el.get("derivedFrom") is not None and el.tag != "peripheral":
        raise SvdError(f"{what} {n}: derivedFrom on a {el.tag} is not supported "
                       f"(peripheral-level derivedFrom is)")
    _known(el, f"{what} {n}")
    return _ident(n, what)


def _access(el, what, inherited):
    a = _one(el, "access", what)
    if a is None:
        return inherited
    if a not in ACCESS:
        raise SvdError(f"{what}: access {a!r} is not one of {sorted(ACCESS)}")
    return a


def _read_action(el, what):
    ra = _one(el, "readAction", what)
    if ra is not None and ra not in READ_ACTIONS:
        raise SvdError(f"{what}: readAction {ra!r} is not one of {sorted(READ_ACTIONS)}")
    return ra


def _desc(el, what):
    """The description as one line of text, kept as the vendor wrote
    it; ascii_text() renders it when a pick uses it. Empty is fine;
    repeated is a malformed file."""
    found = el.findall("description")
    if len(found) > 1:
        raise SvdError(f"{what}: <description> appears {len(found)} times")
    d = found[0] if found else None
    if d is None or not d.text:
        return ""
    return " ".join(d.text.split())


def _size(el, what, inherited):
    s = _opt_int(el, "size", what)
    if s is None:
        return inherited
    if s < 1 or s > 64:
        raise SvdError(f"{what}: size {s} bits is not a register")
    return s


# -- dim arrays ---------------------------------------------------------

def _dim_names(el, name, what):
    """Expand a dim array element into concrete names with their index
    offsets; a plain element yields itself once."""
    dim = _opt_int(el, "dim", what)
    if dim is None:
        if "%s" in name:
            raise SvdError(f"{what}: name has %s but no <dim>")
        return [(name, 0)]
    inc = _int(el, "dimIncrement", what)
    if dim < 1 or inc < 1:
        raise SvdError(f"{what}: dim {dim} / dimIncrement {inc} must be at least 1")
    if dim > DIM_MAX:
        raise SvdError(f"{what}: dim {dim} is beyond {DIM_MAX}")
    if "%s" not in name:
        raise SvdError(f"{what}: a dim array name needs %s for the index")
    index = _one(el, "dimIndex", what)
    if index is None:
        labels = [str(i) for i in range(dim)]
    elif _DIM_NUM.match(index):
        lo, hi = map(int, _DIM_NUM.match(index).groups())
        if hi - lo + 1 != dim:                    # checked before anything is built
            raise SvdError(f"{what}: dimIndex {index!r} spans {max(0, hi - lo + 1)} names for dim {dim}")
        labels = [str(i) for i in range(lo, hi + 1)]
    elif _DIM_ABC.match(index):
        lo, hi = _DIM_ABC.match(index).groups()
        labels = [chr(c) for c in range(ord(lo), ord(hi) + 1)]
    elif _DIM_LIST.match(index):
        labels = [x.strip() for x in index.split(",")]
    else:
        raise SvdError(f"{what}: dimIndex {index!r} is not a dimIndexType "
                       f"(0-3, A-D, or a comma list)")
    if len(labels) != dim:
        raise SvdError(f"{what}: dimIndex lists {len(labels)} names for dim {dim}")
    out = [(name.replace("%s", lab), i * inc) for i, lab in enumerate(labels)]
    if len({c for c, _ in out}) != len(out):
        raise SvdError(f"{what}: dimIndex repeats a name")
    return out


# -- fields -------------------------------------------------------------

def _field_bits(f, what):
    """Exactly one of the three bit-position forms."""
    forms = [f.find("bitOffset") is not None or f.find("bitWidth") is not None,
             f.find("bitRange") is not None,
             f.find("lsb") is not None or f.find("msb") is not None]
    if sum(forms) != 1:
        raise SvdError(f"{what}: bit position must be given as exactly one of "
                       f"bitOffset/bitWidth, bitRange, lsb/msb")
    if forms[0]:
        lsb = _int(f, "bitOffset", what)
        width = _int(f, "bitWidth", what)
    elif forms[1]:
        m = _BITRANGE.match(_one(f, "bitRange", what))
        if not m:
            raise SvdError(f"{what}: bitRange must be [msb:lsb]")
        msb, lsb = int(m.group(1)), int(m.group(2))
        width = msb - lsb + 1
    else:
        lsb = _int(f, "lsb", what)
        msb = _int(f, "msb", what)
        width = msb - lsb + 1
    if width < 1:
        raise SvdError(f"{what}: msb below lsb")
    return lsb, width


def _fields(reg_el, reg):
    fs = reg_el.findall("fields")
    if not fs:
        return
    if len(fs) > 1:
        raise SvdError(f"{reg.path}: <fields> appears {len(fs)} times")
    _known(fs[0], f"{reg.path} fields")
    if not fs[0].findall("field"):
        raise SvdError(f"{reg.path}: <fields> is empty")
    for f in fs[0].findall("field"):
        fname = _name(f, f"{reg.path} field")
        what = f"{reg.path}.{fname}"
        lsb, width = _field_bits(f, what)
        facc = _access(f, what, reg.access)
        fra = _read_action(f, what)
        fdesc = _desc(f, what)
        for fn, off in _dim_names(f, fname, what):
            if lsb + off + width > reg.size:
                raise SvdError(f"{reg.path}.{fn}: bits {lsb + off}+{width} do not fit "
                               f"a {reg.size}-bit register")
            if fn in reg.fields:
                raise SvdError(f"{reg.path}: field {fn} appears twice")
            reg.fields[fn] = Field(fn, lsb + off, width, fdesc, fra, facc)


# -- registers and clusters ---------------------------------------------

def _registers(container, base, prefix, size, access, out, depth=0, disable=None):
    """Walk the registers of a peripheral (children of its <registers>)
    or of a cluster (direct children), in document order. size and
    access are the inherited register properties, None when no level
    above declared them."""
    if depth > CLUSTER_DEPTH_MAX:
        raise SvdError(f"{prefix[:-1]}: clusters nested deeper than {CLUSTER_DEPTH_MAX}")
    if container.tag == "cluster":
        children = list(container)
    else:
        regs = container.findall("registers")
        if len(regs) > 1:
            raise SvdError(f"{prefix[:-1]}: <registers> appears {len(regs)} times")
        if regs:
            _known(regs[0], f"{prefix[:-1]} registers")
            if not regs[0].findall("register") and not regs[0].findall("cluster"):
                raise SvdError(f"{prefix[:-1]}: <registers> is empty")
        children = list(regs[0]) if regs else []
    for el in children:
        if el.tag == "cluster":
            cname = _name(el, f"{prefix}cluster")
            what = f"{prefix}{cname}"
            coff = _int(el, "addressOffset", what)
            csize = _size(el, what, size)
            cacc = _access(el, what, access)
            for cn, off in _dim_names(el, cname, what):
                _registers(el, base + coff + off, f"{prefix}{cn}.", csize, cacc, out,
                           depth + 1, disable)
        elif el.tag == "register":
            rname = _name(el, f"{prefix}register")
            what = f"{prefix}{rname}"
            roff = _int(el, "addressOffset", what)
            rsize = _size(el, what, size)
            if rsize is None:
                raise SvdError(f"{what}: size declared at no level (register, cluster, "
                               f"peripheral, device); the schema does not default it")
            racc = _access(el, what, access)
            ra = _read_action(el, what)
            rdesc = _desc(el, what)
            for rn, off in _dim_names(el, rname, what):
                addr = base + roff + off
                if addr + rsize // 8 - 1 > ADDR_MAX:
                    raise SvdError(f"{prefix}{rn}: address 0x{addr:X} is beyond 32 bits")
                r = Reg(f"{prefix}{rn}", addr, rsize, racc, ra, rdesc, disable)
                if r.path in out:
                    raise SvdError(f"{r.path}: appears twice in the SVD")
                _fields(el, r)
                out[r.path] = r


def load_svd(path):
    """All registers of the device, keyed by 'PERIPH.REG' (or
    'PERIPH.CLUSTER.REG'), in document order. Every way the file can be
    wrong raises SvdError. A register's access may be None when no
    level declared it; a pick of such a register is refused by the
    generator."""
    try:
        return _load_svd(path)
    except SvdError:
        raise
    except (ValueError, TypeError, AttributeError, IndexError, KeyError,
            RecursionError, MemoryError) as e:
        raise SvdError(f"{path}: malformed SVD ({type(e).__name__}: {e})")


def _load_svd(path):
    try:
        root = ET.parse(str(path)).getroot()
    except (OSError, ET.ParseError) as e:
        raise SvdError(f"{path}: {e}")
    if root.tag != "device":
        raise SvdError(f"{path}: not a CMSIS-SVD file (root is <{root.tag}>)")
    _known(root, "device")
    # the header is read as strictly as the registers: the name is an
    # identifier (identifierType), the series is text with an ASCII
    # form, <cpu> appears once, holds only what the schema defines
    # there, and names a core the schema lists (kept as the schema's
    # token, "other" included: a stated value, not an absent one). The
    # strings end up in generated C; the reader hands over nothing it
    # did not check.
    chip = {}
    name = _one(root, "name", "device")
    if name:
        chip["name"] = _ident(name, "device name")
    series = _one(root, "series", "device")
    if series:
        chip["series"] = ascii_text(series, "device series", hint="")
    cpus = root.findall("cpu")
    if len(cpus) > 1:
        raise SvdError(f"{path}: <cpu> appears {len(cpus)} times")
    if cpus:
        _known(cpus[0], "device cpu")
        cpu = _one(cpus[0], "name", "device cpu")
        if cpu is None:
            raise SvdError(f"{path}: device cpu: <name> is missing")
        chip["cpu"] = _cpu_token(cpu, "device cpu")
    aub = _opt_int(root, "addressUnitBits", "device")
    if aub is None:
        raise SvdError(f"{path}: device has no <addressUnitBits> (required by the schema; "
                       f"this reader takes 8, byte-addressed)")
    if aub != 8:
        raise SvdError(f"{path}: addressUnitBits {aub}: this reader takes byte-addressed "
                       f"devices (8); addresses and alignment are computed in bytes")
    dev_size = _size(root, "device", None)
    dev_access = _access(root, "device", None)
    periphs = root.findall("peripherals")
    if not periphs:
        raise SvdError(f"{path}: no <peripherals>")
    if len(periphs) > 1:
        raise SvdError(f"{path}: <peripherals> appears {len(periphs)} times")
    _known(periphs[0], "peripherals")
    if not periphs[0].findall("peripheral"):
        raise SvdError(f"{path}: <peripherals> is empty")
    by_name = {}
    for p in periphs[0].findall("peripheral"):
        n = _name(p, "peripheral")
        if n in by_name:
            raise SvdError(f"peripheral {n} appears twice")
        by_name[n] = p

    out = SvdTable()
    out.chip = chip
    for p in periphs[0].findall("peripheral"):
        pname = _one(p, "name", "peripheral")
        src = p
        holders = [p]
        derived = p.get("derivedFrom")
        if derived is not None:
            basep = by_name.get(derived)
            if basep is None:
                raise SvdError(f"{pname}: derivedFrom {derived!r} is not a peripheral")
            if basep.get("derivedFrom") is not None:
                raise SvdError(f"{pname}: derivedFrom {derived!r}, which is itself derived")
            holders = [basep, p]                 # base properties first, own ones win
            if p.find("registers") is None:      # own <registers> replace the base's
                src = basep
        base = _int(p, "baseAddress", pname)
        if base > ADDR_MAX:
            raise SvdError(f"{pname}: baseAddress 0x{base:X} is beyond 32 bits")
        size, access = dev_size, dev_access
        for holder in holders:
            size = _size(holder, pname, size)
            access = _access(holder, pname, access)
        disable = None
        for holder in holders:              # inherited, the peripheral's own wins
            own = _one(holder, "disableCondition", pname)
            if own is not None:
                disable = own
        for pn, off in _dim_names(p, pname, pname):
            _registers(src, base + off, f"{pn}.", size, access, out, 0, disable)
    return out


def overlapping(table, reg):
    """Every other register whose bytes share an address with reg: the
    alternate views (alternateRegister, alternateGroup, an alternate
    peripheral at the same base) and any plain overlap. What the SVD
    says about those applies to a read of reg."""
    def span(r):                        # bytes touched, a 1-bit register touches one
        return r.addr, r.addr + (r.size + 7) // 8
    lo, hi = span(reg)
    return [o for o in table.values()
            if o is not reg and o.addr < hi and lo < span(o)[1]]


# -- picks --------------------------------------------------------------

def resolve(table, pick):
    """'PERIPH.REG' -> (Reg, None); 'PERIPH.REG.FIELD' -> (Reg, Field).
    A path that is both a register (inside a cluster) and a field of
    a register is refused as ambiguous rather than guessed."""
    head, _, fname = pick.rpartition(".")
    as_field = head in table and fname in table[head].fields
    if pick in table:
        if as_field:
            raise SvdError(f"{pick}: is both register {pick} and field {fname} of "
                           f"{head}; the SVD names collide, the pick cannot be resolved")
        return table[pick], None
    if as_field:
        return table[head], table[head].fields[fname]
    if head in table:
        raise SvdError(f"{pick}: register {head} has no field {fname!r} "
                       f"(fields: {', '.join(table[head].fields) or 'none'})")
    raise SvdError(f"{pick}: no such register in the SVD")


def default_name(pick):
    """USART2.ISR -> usart2_isr; USART2.ISR.TXE -> usart2_isr_txe."""
    return re.sub(r"[^a-z0-9_]", "_", pick.lower())


def register_desc(reg):
    """The register's one-line description with its fields: the
    operator reads the bit meaning off the CLI. Raises SvdError when
    the vendor's text has no ASCII form."""
    s = f"{reg.path} @0x{reg.addr:08X}"
    if reg.desc:
        s += f": {ascii_text(reg.desc, f'{reg.path} description')}"
    if reg.fields:
        bits = []
        for f in reg.fields.values():
            span = f"{f.lsb}" if f.width == 1 else f"{f.lsb + f.width - 1}:{f.lsb}"
            bits.append(f"{f.name}[{span}]")
        s += " [" + " ".join(bits) + "]"
    return s


def field_desc(reg, field):
    span = f"bit {field.lsb}" if field.width == 1 else f"bits {field.lsb + field.width - 1}:{field.lsb}"
    s = f"{reg.path}.{field.name} ({span} @0x{reg.addr:08X})"
    if field.desc:
        s += f": {ascii_text(field.desc, f'{reg.path}.{field.name} description')}"
    return s
