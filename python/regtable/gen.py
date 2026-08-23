#!/usr/bin/env python3
"""Generate regtable projections from a YAML register description.

    regtable gen device.yaml -o outdir/

writes registers.c, registers.h, registers.md, and <device>_client.py
(a typed Python client) into outdir/. build_client(path) returns the
client class directly, without writing files.

The YAML is the single source of truth; every constraint the
adapters enforce at init time is checked here at generation time,
so a bad table fails before it compiles:

    device: demo                  # names the generated files' guard
    registers:
      - name: temp                # C identifier, 1..31 chars, lowercase
        type: float               # u8 u16 u32 i8 i16 i32 float bool
        perm: ro                  # ro | rw
        init: 23.4                # optional initial value, default 0
        desc: Water temperature   # optional, ASCII
        min: 0.0                  # optional, min and max together
        max: 100.0
        modbus: 1                 # optional word address, 0 = unmapped
        hooks:                    # optional, C function names;
          on_change: temp_alarm   # bodies live in application code

      - svd: STM32L053.svd        # silicon registers, read-only, from the
        pick:                     # vendor's CMSIS-SVD (path relative to this
          - USART2.ISR            # file; derivedFrom one level): usart2_isr, U32
          - USART2.ISR.TXE        # one field: usart2_isr_txe, BOOL
          - { reg: TIM3.CNT, as: timer_count, desc: Motor timer }
"""

import argparse
import types
import keyword
import math
import re
import struct
import sys
from pathlib import Path

from .client import DOMAIN, FLT_MAX, FLT_MIN, MODBUS_ADDR_MAX
from . import svd as svdmod

try:
    import yaml
except ImportError:
    sys.exit("regtable gen: PyYAML is required (pip install pyyaml)")


class UniqueKeyLoader(yaml.SafeLoader):
    """A duplicate key would silently take the last value; make it loud."""

    def construct_mapping(self, node, deep=False):
        seen = set()
        for key_node, _ in node.value:
            key = self.construct_object(key_node, deep=deep)
            try:
                hash(key)
            except TypeError:
                raise yaml.YAMLError(
                    f"unhashable mapping key at line {key_node.start_mark.line + 1}")
            if key in seen:
                raise yaml.YAMLError(
                    f"duplicate key '{key}' at line {key_node.start_mark.line + 1}")
            seen.add(key)
        return super().construct_mapping(node, deep)

TYPES = {                                 # C type, enum, Modbus words, domain
    "u8":    ("uint8_t",  "REG_U8",    1, DOMAIN["U8"]),
    "u16":   ("uint16_t", "REG_U16",   1, DOMAIN["U16"]),
    "u32":   ("uint32_t", "REG_U32",   2, DOMAIN["U32"]),
    "i8":    ("int8_t",   "REG_I8",    1, DOMAIN["I8"]),
    "i16":   ("int16_t",  "REG_I16",   1, DOMAIN["I16"]),
    "i32":   ("int32_t",  "REG_I32",   2, DOMAIN["I32"]),
    "float": ("float",    "REG_FLOAT", 2, None),
    "bool":  ("uint8_t",  "REG_BOOL",  1, (0, 1)),
}

NAME_RE = re.compile(r"^[a-z][a-z0-9_]{0,30}$")   # C identifier, MQTT level
FUNC_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
HOOKS = ("on_write", "on_read", "on_change")

# names that would parse but not survive as C symbols
C_RESERVED = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "inline", "int", "long", "register", "restrict", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
    "unsigned", "void", "volatile", "while",
    "bool", "true", "false", "NULL",     # stdbool / stddef land
    "_Bool", "_Complex", "_Imaginary", "_Atomic", "_Generic",
    "_Static_assert", "_Alignas", "_Alignof", "_Noreturn", "_Thread_local",
    # the headers are compiled as C++ in Arduino sketches
    "class", "namespace", "template", "typename", "new", "delete", "this",
    "operator", "private", "public", "protected", "virtual", "friend",
    "using", "try", "catch", "throw", "explicit", "mutable", "constexpr",
    "nullptr", "typeid", "wchar_t", "asm", "alignas", "alignof",
    "static_assert", "thread_local", "decltype", "noexcept", "export",
    "and", "or", "not", "xor", "bitand", "bitor", "compl",
    "and_eq", "or_eq", "xor_eq", "not_eq",
    "static_cast", "dynamic_cast", "const_cast", "reinterpret_cast",
    "char8_t", "char16_t", "char32_t", "co_await", "co_return",
    "co_yield", "concept", "requires", "consteval", "constinit",
    "size_t", "ptrdiff_t", "offsetof",
}

LIB_PREFIXES = ("reg_", "regcli_", "regmb_", "regmqtt_")

# register names become Python attributes on the generated client:
# keywords would not parse, and these would shadow the base class
PY_CLIENT_API = {
    "serial", "pipe", "verify", "discover", "registers", "snapshot",
    "watch", "record", "close", "__schema__", "__slots__", "__init__", "__setattr__",
    "__enter__", "__exit__",
    # names the generated class body itself uses: a register of that
    # name would shadow them for every register declared after it
    "property", "math", "f32", "bool", "int", "float",
    "RegtableClient", "SerialTransport", "PipeTransport",
}


def py_symbol_problem(ident):
    if keyword.iskeyword(ident) or keyword.issoftkeyword(ident):
        return "is a Python keyword"
    if ident in PY_CLIENT_API:
        return "is a name the generated Python client uses"
    return None


def f32(x):
    """x as binary32: what the C table stores, so the client's local
    checks and schema describe the same boundary as the device."""
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]


def symbol_problem(ident, device):
    """Why ident cannot be a generated C symbol, or None. Closed by
    rule, not by list: whole reserved namespaces are refused."""
    if ident in C_RESERVED:
        return "is a C/C++ keyword"
    if ident.startswith("_"):
        return "starts with '_' (reserved namespace)"
    if ident.endswith("_t"):
        return "ends in '_t' (reserved for type names: uint8_t, size_t, ...)"
    if ident.startswith(("Reg", "REG")):
        return "uses the library's identifier space (Reg* / REG*)"
    if ident == ident.upper():
        return ("is all uppercase (macro territory: UINT8_MAX, FLT_MAX, "
                "EOF, ... expand before the compiler sees the name)")
    if any(ident.startswith(px) for px in LIB_PREFIXES):
        return "uses a library prefix (reg_/regcli_/regmb_/regmqtt_)"
    if ident == f"{device}_registry":
        return "collides with the generated table symbol"
    return None

def finite_float(v):
    """A value a float register can hold and a C constant can spell:
    zero, or a normal binary32 magnitude. Integers of any size
    compare without float conversion, so no OverflowError."""
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return False
    if isinstance(v, float) and not math.isfinite(v):
        return False
    return v == 0 or FLT_MIN <= abs(v) <= FLT_MAX


class GenerationError(ValueError):
    """The YAML description is invalid; the message says where."""


def fail(msg):
    raise GenerationError(msg)


def check_max_entries(n):
    if not isinstance(n, int) or isinstance(n, bool) or not 1 <= n <= 0xFFFF:
        fail("max_entries must be 1..65535 (the table count is uint16)")
    return n


def is_ascii(s):
    return all(32 <= ord(c) < 127 for c in s)


def load(path):
    try:
        doc = yaml.load(Path(path).read_text(encoding="utf-8"),
                        Loader=UniqueKeyLoader)
    except OSError as e:
        fail(f"{path}: {e.strerror or e}")
    except UnicodeDecodeError as e:
        fail(f"{path}: not UTF-8 text ({e.reason} at byte {e.start})")
    except yaml.YAMLError as e:
        fail(f"{path}: {e}")
    if not isinstance(doc, dict):
        fail(f"{path}: top level must be a mapping")
    return doc


SVD_KEYS = {"svd", "pick", "perm", "force"}
SVD_PICK_KEYS = {"reg", "as", "desc"}


def svd_type(size, width=None):
    """The register type for a whole register of `size` bits, or for a
    field of `width` bits: the narrowest that holds it."""
    if width is not None:
        if width == 1:
            return "bool"
        return "u8" if width <= 8 else "u16" if width <= 16 else "u32"
    return {8: "u8", 16: "u16", 32: "u32"}.get(size)


class SvdEntry(dict):
    """An entry the expander made from a pick. Only these may carry
    '_svd'; the key is refused on anything hand-written."""


def expand_svd(regs, base_dir):
    """Replace every `svd:` block with the read-only entries it picks,
    resolved against the vendor's file. Returns (where, entry) pairs:
    where names the YAML location (registers[i] or registers[i].pick[j])
    so later messages point at the file as written. The entries carry
    the address and bit position under '_svd' for the emitters;
    everything else about them goes through the same checks as a
    hand-written entry."""
    out = []
    cache = {}
    for i, r in enumerate(regs):
        if not (isinstance(r, dict) and "svd" in r):
            out.append((f"registers[{i}]", r))
            continue
        where = f"registers[{i}]"
        extra = set(r) - SVD_KEYS
        if extra:
            fail(f"{where}: unknown keys {sorted(map(repr, extra))} in an svd block "
                 f"(use {', '.join(sorted(SVD_KEYS))})")
        if r.get("perm", "ro") != "ro":
            fail(f"{where}: silicon registers are read-only here; a write to "
                 f"the silicon belongs to the HAL and the application")
        force = r.get("force", False)
        if not isinstance(force, bool):
            fail(f"{where}: 'force' must be true or false")
        path = r["svd"]
        if not isinstance(path, str) or not path:
            fail(f"{where}: 'svd' must be a file path")
        full = Path(base_dir) / path if base_dir else Path(path)
        if not full.is_file():
            fail(f"{where}: svd file {str(full)!r} does not exist or is not a file")
        if full not in cache:
            try:
                cache[full] = svdmod.load_svd(full)
            except svdmod.SvdError as e:
                fail(f"{where}: {e}")
        table = cache[full]
        picks = r.get("pick")
        if not isinstance(picks, list) or not picks:
            fail(f"{where}: 'pick' must be a non-empty list")
        for j, item in enumerate(picks):
            pw = f"{where}.pick[{j}]"
            name_override = desc_override = None
            if isinstance(item, dict):
                extra = set(item) - SVD_PICK_KEYS
                if extra:
                    fail(f"{pw}: unknown keys {sorted(map(repr, extra))} "
                         f"(use {', '.join(sorted(SVD_PICK_KEYS))})")
                for k in ("reg", "as", "desc"):
                    if k in item and item[k] is None:
                        fail(f"{pw}: '{k}' is written without a value")
                pick = item.get("reg")
                name_override = item.get("as")
                desc_override = item.get("desc")
            else:
                pick = item
            if not isinstance(pick, str) or not pick:
                fail(f"{pw}: a pick is 'PERIPH.REG' or 'PERIPH.REG.FIELD'")
            if not is_ascii(pick):
                fail(f"{pw}: a pick is ASCII text (it names a C string and a Markdown cell)")
            if name_override is not None and not isinstance(name_override, str):
                fail(f"{pw}: 'as' must be a name (text)")
            try:
                reg, field = svdmod.resolve(table, pick)
            except svdmod.SvdError as e:
                fail(f"{pw}: {e}")
            # what the silicon lets a read do
            if reg.size not in (8, 16, 32):
                fail(f"{pw}: {reg.path} is {reg.size} bits wide; 8, 16, or 32 fit a register")
            if reg.addr % (reg.size // 8):
                fail(f"{pw}: {reg.path} at 0x{reg.addr:08X} is not aligned to its "
                     f"{reg.size}-bit size (the SVD is the place to fix it)")
            # every emitted read is a whole-register read: the register
            # itself must be readable; a field's own access can only
            # narrow that, never widen it
            if reg.access is None:
                fail(f"{pw}: {reg.path}: access declared at no level (register, "
                     f"cluster, peripheral, device); the schema does not default it")
            if reg.access in ("write-only", "writeOnce"):
                fail(f"{pw}: {reg.path} is {reg.access}; nothing to read "
                     f"(a field of it is read through the whole register)")
            if field and field.access in ("write-only", "writeOnce"):
                fail(f"{pw}: {pick} is {field.access}; nothing to read")
            if not field and not force:
                wo = [f.name for f in reg.fields.values() if f.access in ("write-only", "writeOnce")]
                if wo:
                    fail(f"{pw}: {reg.path} holds write-only field(s) {', '.join(wo)} in the "
                         f"bytes a whole-register read covers; those bits read as undefined. "
                         f"Pick a field, or set force: true on the block")
            # the vendor's guard on the peripheral: a read while the
            # condition holds is the kind of access the SVD warns about,
            # and the generator cannot evaluate the condition
            if reg.disabled_by and not force:
                fail(f"{pw}: {reg.path}: its peripheral carries disableCondition "
                     f"{reg.disabled_by!r}; the generator cannot evaluate it. Set force: "
                     f"true on the block to read regardless")
            # every read the generator emits is a whole-register read of one
            # address: a readAction on the register, on any of its fields, or
            # on any other register the SVD places at that address (an
            # alternate view of the same silicon) applies
            ra = reg.read_action
            ra_where = reg.path
            if not ra:
                for fld in reg.fields.values():
                    if fld.read_action:
                        ra, ra_where = fld.read_action, f"{reg.path}.{fld.name}"
                        break
            if not ra:
                for alias in svdmod.overlapping(table, reg):
                    if alias.read_action:
                        ra, ra_where = alias.read_action, f"{alias.path} (same address)"
                        break
                    for fld in alias.fields.values():
                        if fld.read_action:
                            ra, ra_where = fld.read_action, f"{alias.path}.{fld.name} (same address)"
                            break
                    if ra:
                        break
            if ra and not force:
                fail(f"{pw}: reading {pick} has a side effect on the silicon "
                     f"({ra_where} readAction: {ra}; the read covers the whole "
                     f"register); exposing it changes the state it shows. "
                     f"Set force: true on the block to take that")
            name = name_override if name_override is not None else svdmod.default_name(pick)
            if desc_override is not None:
                if not isinstance(desc_override, str):
                    fail(f"{pw}: 'desc' must be text")
                desc = desc_override
            else:
                try:
                    desc = svdmod.field_desc(reg, field) if field else svdmod.register_desc(reg)
                except svdmod.SvdError as e:
                    fail(f"{pw}: {e}")
            entry = SvdEntry({
                "name": name,
                "type": svd_type(reg.size, field.width if field else None),
                "perm": "ro",
                "desc": desc,
                "_svd": {"file": path, "pick": pick, "addr": reg.addr, "size": reg.size,
                         "lsb": field.lsb if field else None,
                         "width": field.width if field else None},
            })
            out.append((pw, entry))
    return out


def validate(doc, max_entries=64, base_dir=None):
    """Check the description and return (device, entries). base_dir is
    where relative `svd:` paths resolve: the YAML's own directory."""
    device = doc.get("device")
    if not isinstance(device, str) or not NAME_RE.match(device):
        fail("'device' must be a lowercase identifier")
    if device == "regtable":
        fail("'device' regtable is reserved: its client would be named "
             "regtable_client.py, the runtime's own file in the output directory")
    extra = set(doc) - {"device", "registers"}
    if extra:
        fail(f"unknown top-level keys {sorted(map(repr, extra))}")
    regs = doc.get("registers")
    if not isinstance(regs, list) or not regs:
        fail("'registers' must be a non-empty list")
    located = expand_svd(regs, base_dir)
    regs = [r for _, r in located]
    if len(regs) > max_entries:
        fail(f"{len(regs)} registers, but the dirty bitmap holds "
             f"{max_entries} (REGTABLE_MAX_ENTRIES); raise it in the "
             f"build and pass --max-entries to match")

    known = {"name", "type", "perm", "init", "desc", "min", "max",
             "modbus", "hooks"}
    # the generator defines one static <name>_sample per silicon field;
    # nothing else in the table may take those symbols
    samplers = {r["name"] + "_sample" for r in regs
                if isinstance(r, SvdEntry) and r["_svd"]["lsb"] is not None}
    seen_names = set()
    spans = []          # (start, end_exclusive, name) for modbus overlap
    hook_kinds = {}     # function name -> set of hook kinds using it

    for where, r in located:
        if not isinstance(r, dict):
            fail(f"{where}: must be a mapping")
        from_svd = isinstance(r, SvdEntry)
        extra = set(r) - known - ({"_svd"} if from_svd else set())
        if extra:
            fail(f"{where}: unknown keys {sorted(map(repr, extra))}")

        name = r.get("name")
        if not isinstance(name, str) or not NAME_RE.match(name):
            fail(f"{where}: {'the name (pick or as:)' if from_svd else chr(39) + 'name' + chr(39)} "
                 f"must match {NAME_RE.pattern} "
                 f"(a C identifier that is also one MQTT topic level)")
        prob = symbol_problem(name, device) or py_symbol_problem(name)
        if prob:
            fail(f"{where}: '{name}' {prob}")
        if name in samplers:
            fail(f"{where}: '{name}' collides with the sampler the generator "
                 f"defines for silicon field '{name[:-len('_sample')]}'")
        if name in seen_names:
            fail(f"{where}: duplicate name '{name}'")
        seen_names.add(name)
        if not from_svd:
            where = f"register '{name}'"          # a pick keeps its YAML location

        t = r.get("type")
        if not isinstance(t, str) or t not in TYPES:
            fail(f"{where}: 'type' must be one of {' '.join(TYPES)}")
        perm = r.get("perm")
        if perm not in ("ro", "rw"):
            fail(f"{where}: 'perm' must be ro or rw")

        desc = r.get("desc")
        if desc is not None:
            if not isinstance(desc, str) or not is_ascii(desc):
                fail(f"{where}: 'desc' must be ASCII text")

        has_min, has_max = "min" in r, "max" in r
        if has_min != has_max:
            fail(f"{where}: 'min' and 'max' come together")
        if has_min:
            if t == "bool":
                fail(f"{where}: bool takes no range")
            lo, hi = r["min"], r["max"]
            for v in (lo, hi):
                if not isinstance(v, (int, float)) or isinstance(v, bool):
                    fail(f"{where}: range bounds must be numbers")
                if t != "float" and not isinstance(v, int):
                    fail(f"{where}: {t} range bounds must be integers")
                if t == "float" and not finite_float(v):
                    fail(f"{where}: float range bounds must be finite "
                         f"and within binary32")
            if lo > hi:
                fail(f"{where}: min > max")
            dom = TYPES[t][3]
            if dom and not (dom[0] <= lo and hi <= dom[1]):
                fail(f"{where}: range {lo}..{hi} outside the {t} domain "
                     f"{dom[0]}..{dom[1]}")
            if lo == 0 and hi == 0:
                fail(f"{where}: 0..0 means no range check; drop min/max")

        init = r.get("init", 0)
        if t == "bool":
            if isinstance(init, float) or init not in (0, 1, True, False):
                fail(f"{where}: bool init must be 0 or 1")
        elif t == "float":
            if not finite_float(init):
                fail(f"{where}: float init must be finite and within binary32")
        if t not in ("bool", "float"):
            if not isinstance(init, int) or isinstance(init, bool):
                fail(f"{where}: {t} init must be an integer "
                     f"(got {init!r}; nothing is truncated silently)")
            dom = TYPES[t][3]
            if not (dom[0] <= init <= dom[1]):
                fail(f"{where}: init {init} outside the {t} domain")
        if has_min and not isinstance(init, bool):
            if not (r["min"] <= init <= r["max"]):
                fail(f"{where}: init {init} outside the range "
                     f"{r['min']}..{r['max']}; the device would start "
                     f"in a state its own checks refuse "
                     f"(the default init is 0: set one explicitly)")

        mb = r.get("modbus", 0)
        if not isinstance(mb, int) or isinstance(mb, bool) or mb < 0 or mb > MODBUS_ADDR_MAX:
            fail(f"{where}: 'modbus' must be a word address 1..65535 "
                 f"(0 or absent = not mapped)")
        if mb:
            words = TYPES[t][2]
            if mb + words - 1 > MODBUS_ADDR_MAX:
                fail(f"{where}: a {words}-word value at {mb} does not fit "
                     f"the address space")
            for s, e, other in spans:
                if mb < e and s < mb + words:
                    fail(f"{where}: modbus words {mb}..{mb + words - 1} "
                         f"overlap '{other}' at {s}..{e - 1}")
            spans.append((mb, mb + words, name))

        hooks = r.get("hooks", {})
        if not isinstance(hooks, dict):
            fail(f"{where}: 'hooks' must be a mapping")
        for k, fn in hooks.items():
            if k not in HOOKS:
                fail(f"{where}: unknown hook '{k}' (use {', '.join(HOOKS)})")
            if not isinstance(fn, str) or not FUNC_RE.match(fn):
                fail(f"{where}: hook '{k}' must name a C function "
                     f"(got {fn!r})")
            prob = symbol_problem(fn, device)
            if prob:
                fail(f"{where}: hook '{k}' function '{fn}' {prob}")
            if fn in samplers:
                fail(f"{where}: hook '{k}' function '{fn}' collides with the "
                     f"sampler the generator defines for a silicon field")
            hook_kinds.setdefault(fn, set()).add(k)
        if "on_write" in hooks and perm == "ro":
            fail(f"{where}: on_write on a read-only register never runs")

    for fn, kinds in hook_kinds.items():
        if fn in seen_names:
            fail(f"hook '{fn}' collides with the register variable "
                 f"of the same name")
        if "on_write" in kinds and len(kinds) > 1:
            fail(f"hook '{fn}' is used as on_write and as "
                 f"{sorted(kinds - {'on_write'})}: the signatures differ "
                 f"(on_write returns bool and takes the raw value)")

    return device, regs


# -- emitters ------------------------------------------------- #

def c_literal(t, v):
    if t == "float":
        return f"{float(v)}f"
    return str(int(v))


def limit_member(t):
    return {"float": "f"}.get(t, "i" if t.startswith("i") else "u")


def emit_h(device, regs):
    NREGS = len(regs)
    lines = [
        f"/* Generated by regtable gen from the '{device}' description.",
        " * Edit the YAML, not this file. */",
        "",
        f"#ifndef REGTABLE_GEN_{device.upper()}_H",
        f"#define REGTABLE_GEN_{device.upper()}_H",
        "",
        '#include "regtable_core.h"',
        "",
        f"#if REGTABLE_MAX_ENTRIES < {NREGS}",
        f'#error "this table has {NREGS} registers; build with '
        f'-DREGTABLE_MAX_ENTRIES={NREGS} or more"',
        "#endif",
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
    ]
    if any("_svd" not in r for r in regs):
        lines.append("/* the registers, owned here, used by application code */")
    for r in regs:
        if "_svd" in r:
            continue                       # the silicon owns it; no variable
        lines.append(f"extern {TYPES[r['type']][0]} {r['name']};")
    if any("_svd" in r for r in regs):
        lines += ["", "/* silicon registers (from the SVD) are read in place and",
                  " * have no variable here; see registers.c */"]
    hooks = sorted({(k, fn) for r in regs
                    for k, fn in r.get("hooks", {}).items()})
    if hooks:
        lines += ["", "/* hook bodies live in application code */"]
        for k, fn in hooks:
            if k == "on_write":
                lines.append(f"bool {fn}(const RegEntry *entry, uint32_t raw);")
            else:
                lines.append(f"void {fn}(const RegEntry *entry);")
    lines += [
        "",
        f"extern const RegEntry {device}_registry[];",
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
        "",
        f"#endif /* REGTABLE_GEN_{device.upper()}_H */",
        "",
    ]
    return "\n".join(lines)


def emit_c(device, regs):
    lines = [
        f"/* Generated by regtable gen from the '{device}' description.",
        " * Edit the YAML, not this file. */",
        "",
        '#include "registers.h"',
        "#include <stddef.h>",
        "",
    ]
    for r in regs:
        if "_svd" in r:
            continue
        t = r["type"]
        init = r.get("init", 0)
        if t == "bool":
            init = 1 if init in (1, True) else 0
        lines.append(f"{TYPES[t][0]} {r['name']} = {c_literal(t, init)};")
    fields = [r for r in regs if "_svd" in r and r["_svd"]["lsb"] is not None]
    if fields:
        lines += ["",
                  "/* silicon fields: a read samples the register in place and",
                  " * keeps the bits; the entry reads the sample */"]
        for r in fields:
            s = r["_svd"]
            ctype = TYPES[r["type"]][0]
            wtype = {8: "uint8_t", 16: "uint16_t", 32: "uint32_t"}[s["size"]]
            mask = (1 << s["width"]) - 1
            lines += [
                f"static {ctype} {r['name']};",
                f"static void {r['name']}_sample(const RegEntry *entry)",
                "{",
                "    (void)entry;",
                f"    {r['name']} = ({ctype})((*(volatile {wtype} *)0x{s['addr']:08X}U "
                f">> {s['lsb']}) & 0x{mask:X}U);",
                "}",
            ]
    lines += ["", f"const RegEntry {device}_registry[] = {{"]
    for r in regs:
        t = r["type"]
        s = r.get("_svd")
        if s and s["lsb"] is None:
            ptr = f'.ptr = (volatile void *)0x{s["addr"]:08X}U'
        else:
            ptr = f'.ptr = &{r["name"]}'
        f = [f'.name = "{r["name"]}"',
             ptr,
             f'.type = {TYPES[t][1]}',
             f'.perm = {"REG_RO" if r["perm"] == "ro" else "REG_RW"}']
        if s and s["lsb"] is not None:
            f.append(f'.on_read = {r["name"]}_sample')
        if r.get("modbus"):
            f.append(f'.modbus_addr = {r["modbus"]}')
        if "min" in r:
            m = limit_member(t)
            f.append(f'.min.{m} = {c_literal(t, r["min"])}')
            f.append(f'.max.{m} = {c_literal(t, r["max"])}')
        for k in HOOKS:
            if k in r.get("hooks", {}):
                f.append(f'.{k} = {r["hooks"][k]}')
        if r.get("desc"):
            esc = (r["desc"].replace("\\", "\\\\").replace('"', '\\"')
                   .replace("?", "\\?"))   # a ? never meets another: no trigraphs
            f.append(f'.description = "{esc}"')
        body = ",\n      ".join(f)
        lines.append(f"    {{ {body} }},")
    lines += ["    { .name = NULL }", "};", ""]
    return "\n".join(lines)


def emit_md(device, regs):
    def rng(r):
        return f"{r['min']}..{r['max']}" if "min" in r else ""

    def mbcol(r):
        mb = r.get("modbus", 0)
        if not mb:
            return ""
        words = TYPES[r["type"]][2]
        return str(mb) if words == 1 else f"{mb}-{mb + words - 1}"

    def hookcol(r):
        return ", ".join(f"{k}: {fn}" for k, fn in r.get("hooks", {}).items())

    lines = [
        f"# {device} registers",
        "",
        "Generated by regtable gen; the YAML description is the source.",
        "",
        "| Name | Type | Perm | Range | Modbus word | Hooks | Description |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for r in regs:
        desc = (r.get("desc", "").replace(chr(92), chr(92) * 2)
                .replace("|", chr(92) + "|"))   # keep the table a table
        lines.append("| " + " | ".join([
            f"`{r['name']}`", r["type"].upper(), r["perm"].upper(),
            rng(r), mbcol(r), hookcol(r), desc,
        ]) + " |")
    lines.append("")
    silicon = [r for r in regs if "_svd" in r]
    if silicon:
        lines += [
            "## Silicon registers",
            "",
            "Read-only entries picked from the vendor's SVD; the address and meaning are the vendor's.",
            "",
            "| Name | SVD | Register | Address | Bits |",
            "| --- | --- | --- | --- | --- |",
        ]
        for r in silicon:
            s = r["_svd"]
            bits = (str(s["lsb"]) if s["width"] == 1 else f"{s['lsb'] + s['width'] - 1}:{s['lsb']}") \
                if s["lsb"] is not None else f"{s['size'] - 1}:0"
            shown = "".join(c if 32 <= ord(c) < 127 and c != "|" else "?" for c in str(s["file"]))
            lines.append(f"| `{r['name']}` | {shown} | {s['pick']} | 0x{s['addr']:08X} | {bits} |")
        lines.append("")
    return "\n".join(lines)


PY_TYPE = {"u8": "int", "u16": "int", "u32": "int", "i8": "int",
           "i16": "int", "i32": "int", "float": "float", "bool": "bool"}


def class_name(device):
    return "".join(part.capitalize() for part in device.split("_")) + "Device"


def emit_py(device, regs):
    """A typed client: one property per register, RW ones with a
    setter that checks type and range locally before the wire. The
    schema baked into the class is what the runtime compares with the
    device's own list --json at connect time."""
    cls = class_name(device)
    L = []
    L.append(f'"""Generated by regtable gen from the \'{device}\' description.')
    L.append("Edit the YAML, not this file.")
    L.append("")
    L.append("A typed mirror of the device's register table. Connecting")
    L.append("verifies the mirror against the device (SchemaDriftError on any")
    L.append('difference); every attribute read is a wire read."""')
    L.append("")
    L.append("import math")
    L.append("try:")
    L.append("    from .regtable_client import RegtableClient, SerialTransport, PipeTransport, f32")
    L.append("except ImportError:                  # not a package member: the sibling copy this")
    L.append("    try:                             # output was generated with, then the installed runtime")
    L.append("        from regtable_client import RegtableClient, SerialTransport, PipeTransport, f32")
    L.append("    except ImportError:")
    L.append("        from regtable.client import RegtableClient, SerialTransport, PipeTransport, f32")
    L.append("")
    L.append("")
    L.append(f"class {cls}(RegtableClient):")
    L.append(f'    """{device}: {len(regs)} registers."""')
    L.append("")
    L.append("    __slots__ = ()")
    L.append("    __schema__ = {")
    for r in regs:
        t = TYPES[r["type"]][1][4:]      # REG_U16 -> U16
        mn = r.get("min"); mx = r.get("max")
        if r["type"] == "float" and mn is not None:
            mn, mx = f32(mn), f32(mx)   # the device's bounds are binary32
        mb = r.get("modbus", 0)
        L.append(f'        "{r["name"]}": {{"type": "{t}", "perm": "{r["perm"].upper()}", '
                 f'"min": {mn!r}, "max": {mx!r}, "modbus": {mb}, '
                 f'"desc": {(r.get("desc") or "")!r}}},')
    L.append("    }")
    L.append("")
    L.append("    @classmethod")
    L.append("    def serial(cls, port, baud=115200, **kw):")
    L.append('        """Open a serial port (pyserial) and verify the table."""')
    L.append("        return cls(SerialTransport(port, baud), **kw)")
    L.append("")
    L.append("    @classmethod")
    L.append("    def pipe(cls, argv, **kw):")
    L.append('        """Drive a CLI process over stdin/stdout and verify the table."""')
    L.append("        return cls(PipeTransport(argv), **kw)")
    for r in regs:
        n = r["name"]; t = r["type"]; py = PY_TYPE[t]
        rng = f", {r['min']}..{r['max']}" if "min" in r else ""
        doc = repr(f"{r.get('desc') or n} ({r['perm'].upper()}{rng})")   # any text, always a literal
        L.append("")
        L.append(f"    # -- {n} " + "-" * max(2, 44 - len(n)))
        L.append("    @property")
        L.append(f"    def {n}(self) -> {py}:")
        L.append(f"        {doc}")
        L.append(f'        return self._get("{n}", {py})')
        if r["perm"] == "rw":
            L.append("")
            L.append(f"    @{n}.setter")
            L.append(f"    def {n}(self, v: {py}):")
            if t == "bool":
                L.append("        if not isinstance(v, bool):")
                L.append(f'            raise TypeError(f"{n} expects bool, got {{type(v).__name__}}")')
            elif t == "float":
                L.append("        if isinstance(v, bool) or not isinstance(v, (int, float)):")
                L.append(f'            raise TypeError(f"{n} expects float, got {{type(v).__name__}}")')
                L.append("        try:                     # the base type's value, not a subclass's")
                L.append("            v = float(int.__index__(v)) if isinstance(v, int) else float.__float__(v)")
                L.append("        except OverflowError:")
                L.append(f'            raise ValueError(f"{n}: value too large for a float")')
                L.append("        if not math.isfinite(v):")
                L.append(f'            raise ValueError(f"{n}: {{v!r}} is not a finite number")')
                L.append("        try:")
                L.append("            rounded = f32(v)         # what the device will hold")
                L.append("        except ValueError:")
                L.append(f'            raise ValueError(f"{n}: {{v!r}} does not fit a float register")')
                L.append("        if v != 0 and rounded == 0:")
                L.append(f'            raise ValueError(f"{n}: {{v!r}} underflows a float register to zero")')
                L.append("        v = rounded")
                if "min" in r:
                    lo, hi = f32(r["min"]), f32(r["max"])
                    L.append(f"        if not ({lo!r} <= v <= {hi!r}):")
                    L.append(f'            raise ValueError(f"{n}: {{v}} outside {lo!r}..{hi!r}")')
            else:
                dom_lo, dom_hi = TYPES[t][3]
                L.append("        if isinstance(v, bool) or not isinstance(v, int):")
                L.append(f'            raise TypeError(f"{n} expects int, got {{type(v).__name__}}")')
                L.append("        v = int.__index__(v)     # the base type's value, not a subclass's")
                lo = r["min"] if "min" in r else dom_lo
                hi = r["max"] if "min" in r else dom_hi
                what = "" if "min" in r else f" (the {t} domain)"
                L.append(f"        if not ({lo} <= v <= {hi}):")
                L.append(f'            raise ValueError(f"{n}: {{v}} outside {lo}..{hi}{what}")')
            L.append(f'        self._set("{n}", v)')
    L.append("")
    return "\n".join(L)


def build_client(yaml_file, max_entries=64):
    """Validate the YAML and return the typed client class, built in
    memory: the same class `regtable gen` would write as
    <device>_client.py, minus the file. Raises GenerationError on an
    invalid description."""
    device, regs = validate(load(yaml_file), check_max_entries(max_entries),
                            base_dir=Path(yaml_file).resolve().parent)
    name = f"{device}_client"
    mod = types.ModuleType(name)
    mod.__file__ = f"<regtable gen {yaml_file}>"
    exec(compile(emit_py(device, regs), mod.__file__, "exec"), mod.__dict__)
    return getattr(mod, class_name(device))


def main(argv=None):
    ap = argparse.ArgumentParser(prog="regtable gen",
                                 description=__doc__.splitlines()[0])
    ap.add_argument("yaml_file")
    ap.add_argument("-o", "--outdir", default=".",
                    help="directory for registers.c/.h/.md (default: .)")
    ap.add_argument("--max-entries", type=int, default=64,
                    help="REGTABLE_MAX_ENTRIES of the target build (default 64)")
    args = ap.parse_args(argv)
    try:
        device, regs = validate(load(args.yaml_file),
                                check_max_entries(args.max_entries),
                                base_dir=Path(args.yaml_file).resolve().parent)
    except GenerationError as e:
        print(f"regtable gen: error: {e}", file=sys.stderr)
        return 2
    # render and encode everything first: a failure leaves no
    # half-written directory (the outputs are ASCII by construction;
    # this is the check of that construction)
    try:
        files = {
            "registers.h": emit_h(device, regs).encode("ascii"),
            "registers.c": emit_c(device, regs).encode("ascii"),
            "registers.md": emit_md(device, regs).encode("ascii"),
            f"{device}_client.py": emit_py(device, regs).encode("ascii"),
        }
    except UnicodeEncodeError as e:
        print(f"regtable gen: error: generated output is not ASCII ({e})", file=sys.stderr)
        return 2
    runtime = Path(__file__).resolve().parent / "client.py"
    try:
        files["regtable_client.py"] = runtime.read_bytes()
    except OSError as e:
        print(f"regtable gen: error: cannot read the client runtime {runtime}: "
              f"{e.strerror or e}", file=sys.stderr)
        return 2
    # the output directory: created if absent; every target either
    # absent or a plain file, checked before the first write. What can
    # still fail after that is the filesystem itself (space, permission
    # mid-way), reported as an error line like everything else
    out = Path(args.outdir)
    try:
        out.mkdir(parents=True, exist_ok=True)
        for fname in files:
            target = out / fname
            if target.exists() and not target.is_file():
                print(f"regtable gen: error: {target} exists and is not a file",
                      file=sys.stderr)
                return 2
        for fname, data in files.items():
            (out / fname).write_bytes(data)
    except OSError as e:
        print(f"regtable gen: error: cannot write {out}: {e.strerror or e}", file=sys.stderr)
        return 2
    shown = str(out / "registers.c").encode("ascii", "backslashreplace").decode("ascii")
    print(f"regtable gen: {len(regs)} registers -> "
          f"{shown}, registers.h, registers.md, {device}_client.py "
          f"(+ regtable_client.py runtime)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
