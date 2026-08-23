#!/usr/bin/env python3
"""Regression test for the `regtable gen` validator.

Every case is a YAML document the generator must reject, with a
fragment its error message must contain. Run by `make codegen`.
"""

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
GEN = [sys.executable, str(ROOT / "python" / "regtable"), "gen"]

HEAD = "device: demo\nregisters:\n"
BS = chr(92)


def reg(**kw):
    lines = []
    first = True
    for k, v in kw.items():
        bullet = "  - " if first else "    "
        lines.append(f"{bullet}{k}: {v}")
        first = False
    return "\n".join(lines) + "\n"


GOOD = HEAD + reg(name="temp", type="float", perm="ro", init=23.4)

BAD = [
    ("duplicate yaml key",
     HEAD + "  - name: a\n    type: u8\n    perm: ro\n    perm: rw\n",
     "duplicate key"),
    ("duplicate name",
     HEAD + reg(name="a", type="u8", perm="ro") + reg(name="a", type="u8", perm="ro"),
     "duplicate name"),
    ("uppercase name",
     HEAD + reg(name="Gain", type="u8", perm="ro"), "must match"),
    ("c keyword name",
     HEAD + reg(name="int", type="u8", perm="ro"), "keyword"),
    ("registry symbol collision",
     HEAD + reg(name="demo_registry", type="u8", perm="ro"), "collides"),
    ("min without max",
     HEAD + reg(name="a", type="u16", perm="rw", min=1), "come together"),
    ("range outside domain",
     HEAD + reg(name="a", type="u16", perm="rw", min=0, max=70000), "domain"),
    ("float range inf",
     HEAD + reg(name="a", type="float", perm="rw", min=".inf", max=".inf"),
     "finite"),
    ("float init nan",
     HEAD + reg(name="a", type="float", perm="ro", init=".nan"), "finite"),
    ("float beyond binary32",
     HEAD + reg(name="a", type="float", perm="ro", init="1.0e39"), "binary32"),
    ("non-integer init on u16",
     HEAD + reg(name="a", type="u16", perm="rw", init=1.9), "integer"),
    ("init below range",
     HEAD + reg(name="a", type="u16", perm="rw", min=100, max=200),
     "outside the range"),
    ("init outside domain",
     HEAD + reg(name="a", type="i8", perm="rw", init=200), "domain"),
    ("bool with range",
     HEAD + reg(name="a", type="bool", perm="rw", min=0, max=1), "no range"),
    ("modbus overlap",
     HEAD + reg(name="a", type="u32", perm="rw", modbus=1)
          + reg(name="b", type="u16", perm="rw", modbus=2), "overlap"),
    ("two-word at top of space",
     HEAD + reg(name="a", type="float", perm="ro", modbus=65535), "not fit"),
    ("hook NULL",
     HEAD + reg(name="a", type="u8", perm="rw") .replace("perm: rw",
        "perm: rw\n    hooks: {on_change: 'NULL'}"), "keyword"),
    ("cxx keyword name",
     HEAD + reg(name="class", type="u8", perm="ro"), "keyword"),
    ("hook underscore reserved",
     HEAD + reg(name="a", type="u8", perm="rw").replace("perm: rw",
        "perm: rw\n    hooks: {on_change: _Bool}"), "keyword"),
    ("hook hits generated symbol",
     HEAD + reg(name="a", type="u8", perm="rw").replace("perm: rw",
        "perm: rw\n    hooks: {on_change: demo_registry}"), "table symbol"),
    ("name hits library prefix",
     HEAD + reg(name="reg_table_init", type="u8", perm="ro"),
     "library prefix"),
    ("float subnormal",
     HEAD + reg(name="a", type="float", perm="ro", init="1e-50"), "binary32"),
    ("huge integer on float",
     HEAD + reg(name="a", type="float", perm="ro", init="1" + "0" * 400),
     "binary32"),
    ("huge integer on u32",
     HEAD + reg(name="a", type="u32", perm="rw", init="1" + "0" * 400),
     "domain"),
    ("top-level unknown key",
     "device: demo\nextra: 1\nregisters:\n" + reg(name="a", type="u8", perm="ro"),
     "top-level"),
    ("bool init float",
     HEAD + reg(name="a", type="bool", perm="rw", init="1.0"),
     "bool init"),
    ("unhashable yaml key",
     "device: demo\nregisters:\n  - ? [a, b]\n    : 1\n", "unhashable"),
    ("stdint typedef name",
     HEAD + reg(name="uint8_t", type="u8", perm="ro"), "_t"),
    ("python keyword name",
     HEAD + reg(name="def", type="u8", perm="ro"), "Python keyword"),
    ("python soft keyword name",
     HEAD + reg(name="match", type="u8", perm="ro"), "Python keyword"),
    ("name shadows client method",
     HEAD + reg(name="verify", type="u8", perm="ro"), "generated Python client"),
    ("hook hits stdint macro",
     HEAD + reg(name="a", type="u8", perm="rw").replace("perm: rw",
        "perm: rw\n    hooks: {on_change: UINT8_MAX}"), "uppercase"),
    ("hook hits stdint constant macro",
     HEAD + reg(name="a", type="u8", perm="rw").replace("perm: rw",
        "perm: rw\n    hooks: {on_change: UINT32_C}"), "uppercase"),
    ("hook hits library type",
     HEAD + reg(name="a", type="u8", perm="rw").replace("perm: rw",
        "perm: rw\n    hooks: {on_change: RegEntry}"), "identifier space"),
    ("hook hits library enum",
     HEAD + reg(name="a", type="u8", perm="rw").replace("perm: rw",
        "perm: rw\n    hooks: {on_change: REG_U8}"), "identifier space"),
    ("hook hits library macro",
     HEAD + reg(name="a", type="u8", perm="rw").replace("perm: rw",
        "perm: rw\n    hooks: {on_change: REGTABLE_MAX_ENTRIES}"),
     "identifier space"),
    ("hook hits generated guard",
     HEAD + reg(name="a", type="u8", perm="rw").replace("perm: rw",
        "perm: rw\n    hooks: {on_change: REGTABLE_GEN_DEMO_H}"),
     "identifier space"),
    ("cxx11 keyword name",
     HEAD + reg(name="static_cast", type="u8", perm="ro"), "keyword"),
    ("mixed-type unknown top-level keys",
     "device: demo\nextra: 1\n2: x\nregisters:\n"
     + reg(name="a", type="u8", perm="ro"), "top-level"),
    ("list as type",
     HEAD + reg(name="a", type="[u8]", perm="ro"), "'type' must be"),
    ("hook on_write on ro",
     HEAD + reg(name="a", type="u8", perm="ro").replace("perm: ro",
        "perm: ro\n    hooks: {on_write: f}"), "never runs"),
    ("hook signature conflict",
     HEAD + reg(name="a", type="u8", perm="rw").replace("perm: rw",
        "perm: rw\n    hooks: {on_write: f}")
          + reg(name="b", type="u8", perm="rw").replace("perm: rw",
        "perm: rw\n    hooks: {on_change: f}"), "signatures differ"),
    ("hook collides with register",
     HEAD + reg(name="a", type="u8", perm="rw").replace("perm: rw",
        "perm: rw\n    hooks: {on_change: b}")
          + reg(name="b", type="u8", perm="rw"), "collides"),
    ("65 entries",
     HEAD + "".join(reg(name=f"r{i}", type="u8", perm="ro")
                    for i in range(65)), "REGTABLE_MAX_ENTRIES"),
    ("unknown key",
     HEAD + reg(name="a", type="u8", perm="ro", bogus=1), "unknown keys"),
    ("__dir__ is not a top-level key",
     "device: demo\n__dir__: /nope\nregisters:\n" + reg(name="a", type="u8", perm="ro"),
     "top-level"),
    ("device regtable is reserved",
     "device: regtable\nregisters:\n" + reg(name="a", type="u8", perm="ro"), "reserved"),
    ("name shadows the property decorator",
     HEAD + reg(name="property", type="u8", perm="ro") + reg(name="z", type="u8", perm="ro"),
     "generated Python client uses"),
    ("name shadows a module the class body uses",
     HEAD + reg(name="math", type="u8", perm="ro"), "generated Python client uses"),
    ("name shadows the runtime import",
     HEAD + reg(name="f32", type="u8", perm="ro"), "generated Python client uses"),
]


def run(text):
    with tempfile.TemporaryDirectory() as d:
        y = Path(d) / "t.yaml"
        y.write_text(text, encoding="utf-8")
        return subprocess.run(
            [*GEN, str(y), "-o", d],
            capture_output=True, text=True)


def compiles(d):
    """The generated client must be importable Python: compiled, then
    imported for real by an isolated interpreter that sees only the
    output directory (so the class body runs, and the sibling runtime
    copy is the one it finds)."""
    import py_compile
    for f in Path(d).glob("*_client.py"):
        py_compile.compile(str(f), doraise=True)
        want = str((Path(d) / "regtable_client.py").resolve())
        r = subprocess.run([sys.executable, "-I", "-c",
                            f"import sys, pathlib; sys.path.insert(0, {str(d)!r}); import {f.stem} as m; "
                            f"got = str(pathlib.Path(sys.modules[m.RegtableClient.__module__].__file__).resolve()); "
                            f"sys.exit(0 if got == {want!r} else 3)"],
                           capture_output=True, text=True)
        if r.returncode == 3:
            raise RuntimeError(f"{f.name} imported its runtime from somewhere other than the sibling copy")
        if r.returncode != 0:
            raise RuntimeError(f"{f.name}: {r.stderr.strip().splitlines()[-1]}")


def check_escaping():
    """The emitters must keep hostile descriptions harmless: no C
    trigraphs, no broken Markdown table cells, no Python escapes."""
    desc = ("trigraph ???/ pipe | back " + chr(92) + "| "
            + chr(92) + "x41 " + chr(92) + "N{BULLET} " + chr(92) + "u00e9 end")
    text = HEAD + reg(name="a", type="u8", perm="ro") \
        .replace("perm: ro", f"perm: ro\n    desc: '{desc}'")
    with tempfile.TemporaryDirectory() as d:
        y = Path(d) / "t.yaml"
        y.write_text(text, encoding="utf-8")
        r = subprocess.run([*GEN, str(y), "-o", d],
                           capture_output=True, text=True)
        if r.returncode != 0:
            return f"escaping fixture rejected: {r.stderr.strip()}"
        c = (Path(d) / "registers.c").read_text(encoding="ascii")
        md = (Path(d) / "registers.md").read_text(encoding="ascii")
        try:
            compiles(d)
        except Exception as e:   # noqa: BLE001
            return f"generated Python does not compile: {e}"
        if "??" in c:
            return "generated C still contains '??' (trigraph risk)"
        row = [l for l in md.splitlines() if "trigraph" in l][0]
        # escaped pipes must not split the row into extra columns
        cols = re.split(r"(?<!" + re.escape(chr(92)) + r")\|", row)
        if len(cols) != 9:              # empty, 7 fields, empty
            return f"markdown row split into {len(cols) - 2} columns"
    return None


SVD = (ROOT / "python" / "tests" / "demo.svd").as_posix()
SVD_YAML = ROOT / "python" / "tests" / "svd_demo.yaml"


def svd_block(picks, **kw):
    extra = "".join(f"    {k}: {v}\n" for k, v in kw.items())
    return (f"  - svd: {SVD}\n{extra}    pick:\n"
            + "".join(f"      - {p}\n" for p in picks))


SVD_BAD = [
    ("svd: write-only register", svd_block(["USART1.TDR"]), "write-only"),
    ("svd: read with a side effect", svd_block(["USART1.RDR"]), "side effect"),
    ("svd: side effect through a field", svd_block(["USART1.RDR.RDR"]), "side effect"),
    ("svd: a field's readAction covers the whole-register read",
     svd_block(["SPI1.DR"]), "SPI1.DR.DATA readAction: clear"),
    ("svd: a field's readAction covers a sibling field read",
     svd_block(["SPI1.DR.FLAG"]), "the read covers the whole register"),
    ("svd: sampler symbol reserved for a register name",
     reg(name="usart1_isr_txe_sample", type="u8", perm="ro") + svd_block(["USART1.ISR.TXE"]),
     "collides with the sampler"),
    ("svd: as is not text", svd_block(["{ reg: USART1.ISR, as: {bad: name} }"]), "pick[0]: 'as' must be a name"),
    ("svd: as without a value", svd_block(["{ reg: USART1.ISR, as: }"]), "'as' is written without a value"),
    ("svd: a peripheral with disableCondition is refused without force",
     f"  - svd: {(ROOT / 'python' / 'tests' / 'alt.svd').as_posix()}\n    pick: [DBG.IDCODE]\n", "disableCondition"),
    ("svd: an alternate view's readAction covers the pick",
     f"  - svd: {(ROOT / 'python' / 'tests' / 'alt.svd').as_posix()}\n    pick: [P.SAFE]\n", "P.DESTRUCTIVE (same address) readAction: clear"),
    ("svd: a one-bit alternate at the same address still covers the pick",
     f"  - svd: {(ROOT / 'python' / 'tests' / 'alt.svd').as_posix()}\n    pick: [P.TINYSAFE]\n", "P.TINYBIT (same address) readAction: clear"),
    ("svd: disableCondition is inherited by a derived peripheral",
     f"  - svd: {(ROOT / 'python' / 'tests' / 'alt.svd').as_posix()}\n    pick: [DBG2.IDCODE]\n", "disableCondition"),
    ("svd: a whole-register pick over a write-only field",
     f"  - svd: {(ROOT / 'python' / 'tests' / 'wofield.svd').as_posix()}\n    pick: [P.RW]\n", "write-only field(s) W in the bytes"),
    ("svd: an alternate peripheral's readAction covers the pick",
     f"  - svd: {(ROOT / 'python' / 'tests' / 'alt.svd').as_posix()}\n    pick: [P.PLAIN]\n", "Q.EATS (same address) readAction: modify"),
    ("svd: non-ascii pick", svd_block(["{ reg: \u00dcNIT.R, as: my_reg, desc: plain }"]), "pick[0]: a pick is ASCII text"),
    ("svd: a directory is not an svd file", f"  - svd: {(ROOT / 'python' / 'tests').as_posix()}\n    pick: [USART1.ISR]\n", "is not a file"),
    ("svd: a description with no ASCII form is refused at the pick",
     f"  - svd: {(ROOT / 'python' / 'tests' / 'unidesc.svd').as_posix()}\n    pick: [A.S]\n",
     "registers[0].pick[0]: A.S description has"),
    ("svd: desc error names the pick location", svd_block(["{ reg: USART1.ISR, desc: Temp\u00e9rature }"]),
     "registers[0].pick[0]: 'desc' must be ASCII text"),
    ("svd: desc without a value", svd_block(["{ reg: USART1.ISR, desc: }"]), "'desc' is written without a value"),
    ("svd: readable field of a write-only register",
     f"  - svd: {(ROOT / 'python' / 'tests' / 'wofield.svd').as_posix()}\n    pick: [P.WO.F]\n",
     "P.WO is write-only; nothing to read"),
    ("svd: write-only field of a readable register",
     f"  - svd: {(ROOT / 'python' / 'tests' / 'wofield.svd').as_posix()}\n    pick: [P.RW.W]\n",
     "P.RW.W is write-only"),
    ("svd: access declared nowhere is refused at pick",
     f"  - svd: {(ROOT / 'python' / 'tests' / 'noaccess.svd').as_posix()}\n    pick: [A.R]\n", "access declared at no level"),
    ("svd: sampler symbol reserved for a hook",
     reg(name="a", type="u8", perm="rw").replace("perm: rw", "perm: rw\n    hooks: {on_change: usart1_isr_txe_sample}")
     + svd_block(["USART1.ISR.TXE"]), "collides with the sampler"),
    ("svd: misaligned register", svd_block(["GPIOA.ODD"]), "not aligned"),
    ("svd: unknown register", svd_block(["USART1.NOPE"]), "no such register"),
    ("svd: unknown field", svd_block(["USART1.ISR.NOPE"]), "has no field"),
    ("svd: rw is refused", svd_block(["USART1.ISR"], perm="rw"), "read-only here"),
    ("svd: unknown block key", svd_block(["USART1.ISR"], bogus="1"), "unknown keys"),
    ("svd: unknown pick key", svd_block(["{ reg: USART1.ISR, nope: 1 }"]), "unknown keys"),
    ("svd: missing file", "  - svd: /no/such/file.svd\n    pick: [USART1.ISR]\n", "does not exist or is not a file"),
    ("svd: empty pick", f"  - svd: {SVD}\n    pick: []\n", "non-empty list"),
    ("svd: force must be bool", svd_block(["USART1.ISR"], force="1"), "true or false"),
    ("svd: name collides with an application register",
     reg(name="usart1_isr", type="u8", perm="ro") + svd_block(["USART1.ISR"]), "duplicate name"),
    ("svd: as collides", reg(name="x", type="u8", perm="ro")
     + svd_block(["{ reg: USART1.ISR, as: x }"]), "duplicate name"),
    ("svd: as must be a name", svd_block(["{ reg: USART1.ISR, as: Bad-Name }"]), "pick[0]: the name (pick or as:) must match"),
    ("svd: pick location in name errors",
     reg(name="a", type="u8", perm="ro") + svd_block(["USART1.ISR", "{ reg: USART1.BRR, as: '' }"]),
     "registers[1].pick[1]"),
    ("svd: same register picked twice", svd_block(["USART1.ISR", "USART1.ISR"]), "pick[1]: duplicate name"),
    ("svd: app entry after a block keeps its own location",
     svd_block(["USART1.ISR", "USART1.BRR"]) + reg(name="temp", type="u8", perm="ro")
     + reg(name="temp", type="u8", perm="ro"), "registers[2]: duplicate name"),
    ("_svd injected by hand is an unknown key",
     reg(name="x", type="u32", perm="rw") + "    _svd: {file: f, pick: p, addr: 0, size: 32, lsb: ~, width: ~}\n",
     "unknown keys"),
    ("_svd injected, malformed, still an unknown key",
     reg(name="x", type="u32", perm="rw") + "    _svd: 1\n", "unknown keys"),
]


SVD_HEAD = ('<?xml version="1.0"?><device><name>X</name><addressUnitBits>8</addressUnitBits>'
            '<size>32</size><access>read-write</access><peripherals>')
SVD_TAIL = '</peripherals></device>'


def periph(name, base="0x40000000", regs="", attrs=""):
    inner = f'<registers>{regs}</registers>' if regs else ''     # an empty <registers> is refused
    return (f'<peripheral {attrs}><name>{name}</name><baseAddress>{base}</baseAddress>'
            f'{inner}</peripheral>')


def regx(name, off="0x0", extra=""):
    return f'<register><name>{name}</name><addressOffset>{off}</addressOffset>{extra}</register>'


HDR_HEAD = '<?xml version="1.0"?><device>'
HDR_TAIL = ('<addressUnitBits>8</addressUnitBits><size>32</size><access>read-write</access>'
            '<peripherals>' + periph("A", regs=regx("R")) + '</peripherals></device>')


def header(*parts):
    """A device whose header is the given elements, over one register."""
    return HDR_HEAD + "".join(parts) + HDR_TAIL


# malformed or unsupported SVD shapes the reader must refuse, with a
# fragment of the message; the reader is as strict as the validator
SVD_READER_BAD = [
    # the header: what ends up in generated C is checked like the rest
    ("device name not an identifier", header("<name>X-1</name>"), "device name 'X-1': not a CMSIS identifier"),
    ("device name empty", header("<name> </name>"), "device: <name> is empty"),
    ("device name twice", header("<name>X</name><name>Y</name>"), "<name> appears 2 times"),
    ("series with no ASCII form", header("<name>X</name><series>\u4e2d\u6587</series>"), "device series has"),
    ("cpu with an element the schema does not define", header("<name>X</name><cpu><name>CM0</name><bogus>1</bogus></cpu>"),
     "element <bogus> is not defined for <cpu>"),
    ("cpu twice", header("<name>X</name><cpu><name>CM0</name></cpu><cpu><name>CM3</name></cpu>"), "<cpu> appears 2 times"),
    ("cpu without a name", header("<name>X</name><cpu><revision>r0p0</revision></cpu>"), "device cpu: <name> is missing"),
    ("cpu name not in the schema's list", header("<name>X</name><cpu><name>CM99</name></cpu>"), "cpu name 'CM99' is not one"),
    ("cpu name in the wrong case", header("<name>X</name><cpu><name>cm0</name></cpu>"), "cpu name 'cm0' is not one"),
    ("not svd", '<?xml version="1.0"?><html/>', "not a CMSIS-SVD"),
    ("no peripherals", '<?xml version="1.0"?><device><name>X</name><addressUnitBits>8</addressUnitBits></device>', "no <peripherals>"),
    ("bad xml", '<?xml version="1.0"?><device><peripherals>', "no element found"),
    ("register without name", SVD_HEAD + periph("A", regs='<register><addressOffset>0</addressOffset></register>') + SVD_TAIL, "has no name"),
    ("register without offset", SVD_HEAD + periph("A", regs='<register><name>R</name></register>') + SVD_TAIL, "<addressOffset> is missing"),
    ("offset not a number", SVD_HEAD + periph("A", regs=regx("R", "zz")) + SVD_TAIL, "not a CMSIS number"),
    ("negative offset", SVD_HEAD + periph("A", regs=regx("R", "-4")) + SVD_TAIL, "not a CMSIS number"),
    ("address beyond 32 bits", SVD_HEAD + periph("A", base="0xFFFFFFF0", regs=regx("R", "0x10")) + SVD_TAIL, "beyond 32 bits"),
    ("register twice", SVD_HEAD + periph("A", regs=regx("R") + regx("R", "0x4")) + SVD_TAIL, "appears twice"),
    ("peripheral twice", SVD_HEAD + periph("A") + periph("A", "0x40001000") + SVD_TAIL, "appears twice"),
    ("bad access", SVD_HEAD + periph("A", regs=regx("R", extra="<access>rw</access>")) + SVD_TAIL, "access 'rw'"),
    ("bad readAction", SVD_HEAD + periph("A", regs=regx("R", extra="<readAction>eat</readAction>")) + SVD_TAIL, "readAction 'eat'"),
    ("derivedFrom on a register", SVD_HEAD + periph("A", regs=regx("R") + '<register derivedFrom="R"><name>S</name><addressOffset>4</addressOffset></register>') + SVD_TAIL, "not supported"),
    ("derivedFrom unknown peripheral", SVD_HEAD + periph("A") + '<peripheral derivedFrom="Z"><name>B</name><baseAddress>0x1000</baseAddress></peripheral>' + SVD_TAIL, "not a peripheral"),
    ("derivedFrom chain", SVD_HEAD + periph("A") + '<peripheral derivedFrom="A"><name>B</name><baseAddress>0x1000</baseAddress></peripheral>'
     + '<peripheral derivedFrom="B"><name>C</name><baseAddress>0x2000</baseAddress></peripheral>' + SVD_TAIL, "itself derived"),
    ("field beyond register", SVD_HEAD + periph("A", regs=regx("R", extra='<fields><field><name>F</name><bitOffset>30</bitOffset><bitWidth>4</bitWidth></field></fields>')) + SVD_TAIL, "do not fit"),
    ("field twice", SVD_HEAD + periph("A", regs=regx("R", extra='<fields><field><name>F</name><bitOffset>0</bitOffset><bitWidth>1</bitWidth></field>'
     '<field><name>F</name><bitOffset>1</bitOffset><bitWidth>1</bitWidth></field></fields>')) + SVD_TAIL, "appears twice"),
    ("field without bits", SVD_HEAD + periph("A", regs=regx("R", extra='<fields><field><name>F</name></field></fields>')) + SVD_TAIL, "exactly one of"),
    ("dim without %s", SVD_HEAD + periph("A", regs='<register><name>R</name><addressOffset>0</addressOffset><dim>2</dim><dimIncrement>4</dimIncrement></register>') + SVD_TAIL, "needs %s"),
    ("dimIndex count mismatch", SVD_HEAD + periph("A", regs='<register><name>R%s</name><addressOffset>0</addressOffset><dim>2</dim><dimIncrement>4</dimIncrement><dimIndex>A,B,C</dimIndex></register>') + SVD_TAIL, "lists 3 names for dim 2"),
    ("dim zero", SVD_HEAD + periph("A", regs='<register><name>R%s</name><addressOffset>0</addressOffset><dim>0</dim><dimIncrement>4</dimIncrement></register>') + SVD_TAIL, "at least 1"),
    ("size not a register", SVD_HEAD + periph("A", regs=regx("R", extra="<size>128</size>")) + SVD_TAIL, "not a register"),
    ("dimIndex letter range with words", SVD_HEAD + periph("A", regs='<register><name>R%s</name><addressOffset>0</addressOffset><dim>2</dim><dimIncrement>4</dimIncrement><dimIndex>AB-CD</dimIndex></register>') + SVD_TAIL, "not a dimIndexType"),
    ("dimIndex bare dash", SVD_HEAD + periph("A", regs='<register><name>R%s</name><addressOffset>0</addressOffset><dim>2</dim><dimIncrement>4</dimIncrement><dimIndex>-</dimIndex></register>') + SVD_TAIL, "not a dimIndexType"),
    ("empty bitRange", SVD_HEAD + periph("A", regs=regx("R", extra='<fields><field><name>F</name><bitRange/></field></fields>')) + SVD_TAIL, "<bitRange> is empty"),
    ("empty dim", SVD_HEAD + periph("A", regs='<register><name>R%s</name><addressOffset>0</addressOffset><dim></dim><dimIncrement>4</dimIncrement></register>') + SVD_TAIL, "<dim> is empty"),
    ("bad device access", '<?xml version="1.0"?><device><name>X</name><addressUnitBits>8</addressUnitBits><access>rw</access><peripherals>' + periph("A", regs=regx("R")) + SVD_TAIL, "device: access 'rw'"),
    ("size declared nowhere", '<?xml version="1.0"?><device><name>X</name><addressUnitBits>8</addressUnitBits><peripherals>' + periph("A", regs=regx("R")) + SVD_TAIL, "size declared at no level"),
    ("readAction twice, first empty", SVD_HEAD + periph("A", regs=regx("R", extra="<readAction/><readAction>clear</readAction>")) + SVD_TAIL, "<readAction> appears 2 times"),
    ("empty readAction", SVD_HEAD + periph("A", regs=regx("R", extra="<readAction/>")) + SVD_TAIL, "<readAction> is empty"),
    ("access twice", SVD_HEAD + periph("A", regs=regx("R", extra="<access>read-only</access><access>write-only</access>")) + SVD_TAIL, "<access> appears 2 times"),
    ("addressOffset twice", SVD_HEAD + periph("A", regs='<register><name>R</name><addressOffset>0</addressOffset><addressOffset>4</addressOffset></register>') + SVD_TAIL, "<addressOffset> appears 2 times"),
    ("binary with don't-care bits", SVD_HEAD + periph("A", regs=regx("R", "#1x0")) + SVD_TAIL, "not a CMSIS number"),
    ("python octal", SVD_HEAD + periph("A", regs=regx("R", "0o10")) + SVD_TAIL, "not a CMSIS number"),
    ("python underscore", SVD_HEAD + periph("A", regs=regx("R", "1_0")) + SVD_TAIL, "not a CMSIS number"),
    ("bitRange with trailing garbage", SVD_HEAD + periph("A", regs=regx("R", extra='<fields><field><name>F</name><bitRange>[7:0]garbage</bitRange></field></fields>')) + SVD_TAIL, "bitRange must be"),
    ("two bit-position forms", SVD_HEAD + periph("A", regs=regx("R", extra='<fields><field><name>F</name><bitOffset>0</bitOffset><bitWidth>4</bitWidth><bitRange>[7:0]</bitRange></field></fields>')) + SVD_TAIL, "exactly one of"),
    ("name with %s but no dim", SVD_HEAD + periph("A", regs=regx("R%s")) + SVD_TAIL, "no <dim>"),
    ("description twice", SVD_HEAD + periph("A", regs=regx("R", extra="<description>a</description><description>b</description>")) + SVD_TAIL, "<description> appears 2 times"),
    ("dim beyond the guard", SVD_HEAD + periph("A", regs='<register><name>R%s</name><addressOffset>0</addressOffset><dim>4G</dim><dimIncrement>4</dimIncrement></register>') + SVD_TAIL, "beyond 65536"),
    ("misspelt size is refused, not inherited", SVD_HEAD + periph("A", regs=regx("R", extra="<szie>16</szie>")) + SVD_TAIL, "element <szie> is not defined for <register>"),
    ("misspelt access is refused", SVD_HEAD + periph("A", regs=regx("R", extra="<acces>write-only</acces>")) + SVD_TAIL, "<acces> is not defined"),
    ("misspelt readAction is refused", SVD_HEAD + periph("A", regs=regx("R", extra="<readActon>clear</readActon>")) + SVD_TAIL, "<readActon> is not defined"),
    ("stray element in a field", SVD_HEAD + periph("A", regs=regx("R", extra='<fields><field><name>F</name><bitOffset>0</bitOffset><bitWidth>1</bitWidth><acess>read-only</acess></field></fields>')) + SVD_TAIL, "<acess> is not defined for <field>"),
    ("stray element in a peripheral", SVD_HEAD + '<peripheral><name>A</name><baseAddres>0x0</baseAddres><baseAddress>0x0</baseAddress><registers>' + regx("R") + '</registers></peripheral>' + SVD_TAIL, "<baseAddres> is not defined for <peripheral>"),
    ("stray attribute", SVD_HEAD + periph("A", regs='<register derivedfrom="R"><name>S</name><addressOffset>0</addressOffset></register>') + SVD_TAIL, "attribute 'derivedfrom' is not defined"),
    ("name with a dot is not an identifier", SVD_HEAD + periph("A", regs=regx("R.X")) + SVD_TAIL, "not a CMSIS identifier"),
    ("field name with a dot", SVD_HEAD + periph("A", regs=regx("R", extra='<fields><field><name>A.B</name><bitOffset>0</bitOffset><bitWidth>1</bitWidth></field></fields>')) + SVD_TAIL, "not a CMSIS identifier"),
    ("peripheral name with a space", SVD_HEAD + periph("A B") + SVD_TAIL, "not a CMSIS identifier"),
    ("empty registers container", SVD_HEAD + '<peripheral><name>A</name><baseAddress>0x0</baseAddress><registers/></peripheral>' + SVD_TAIL, "<registers> is empty"),
    ("empty fields container", SVD_HEAD + periph("A", regs=regx("R", extra="<fields/>")) + SVD_TAIL, "<fields> is empty"),
    ("empty peripherals", '<?xml version="1.0"?><device><name>X</name><addressUnitBits>8</addressUnitBits><size>32</size><peripherals/></device>', "<peripherals> is empty"),
    ("derived peripheral with empty registers", SVD_HEAD + periph("A", regs=regx("R")) + '<peripheral derivedFrom="A"><name>B</name><baseAddress>0x100</baseAddress><registers/></peripheral>' + SVD_TAIL, "<registers> is empty"),
    ("addressUnitBits missing", '<?xml version="1.0"?><device><name>X</name><size>32</size><peripherals>' + periph("A", regs=regx("R")) + SVD_TAIL, "no <addressUnitBits>"),
    ("addressUnitBits other than 8", '<?xml version="1.0"?><device><name>X</name><size>32</size><addressUnitBits>16</addressUnitBits><peripherals>' + periph("A", regs=regx("R")) + SVD_TAIL, "addressUnitBits 16"),
    ("clusters nested too deep",
     SVD_HEAD + periph("A", regs='<cluster><name>C</name><addressOffset>0</addressOffset>' * 40
                       + regx("R") + '</cluster>' * 40) + SVD_TAIL, "nested deeper than 16"),
]


def check_svd_reader():
    sys.path.insert(0, str(ROOT / "python"))
    from regtable import svd as svdmod
    bad = []
    with tempfile.TemporaryDirectory() as d:
        for label, xml, expect in SVD_READER_BAD:
            f = Path(d) / "x.svd"
            f.write_text(xml, encoding="utf-8")
            try:
                svdmod.load_svd(f)
                bad.append(f"{label}: accepted")
            except svdmod.SvdError as e:
                if expect not in str(e):
                    bad.append(f"{label}: error lacks {expect!r}: {e}")
        # and the fixture resolves the shapes it was written for
        t = svdmod.load_svd(ROOT / "python" / "tests" / "demo.svd")
        want = {"USART2.ISR": 0x4000441C, "USART2.BRR": 0x4000440C, "TIM3.CCR4": 0x40000440,
                "ADC1.CH.DR": 0x40012440, "ADC1.CH.CFG": 0x40012444, "GPIOA.ODD": 0x4001080E}
        for k, a in want.items():
            got = f"{t[k].addr:#x}" if k in t else "missing"
            if k not in t or t[k].addr != a:
                bad.append(f"fixture: {k} -> {got}, want {a:#x}")
        # a path that is a register and a field at once is refused
        amb = (SVD_HEAD + periph("P", regs=regx("C", extra='<fields><field><name>R</name>'
               '<bitOffset>0</bitOffset><bitWidth>1</bitWidth></field></fields>')
               + '<cluster><name>C</name><addressOffset>0x10</addressOffset>'
               + regx("R") + '</cluster>') + SVD_TAIL)
        f = Path(d) / "amb.svd"
        f.write_text(amb, encoding="utf-8")
        ta = svdmod.load_svd(f)
        try:
            svdmod.resolve(ta, "P.C.R")
            bad.append("ambiguous pick P.C.R was resolved")
        except svdmod.SvdError as e:
            if "cannot be resolved" not in str(e):
                bad.append(f"ambiguous pick: unexpected message {e}")
        if "IDR15" not in t["GPIOA.IDR"].fields or t["GPIOA.IDR"].fields["IDR15"].lsb != 15:
            bad.append("fixture: field dim array IDR15 not at bit 15")
        if t["USART2.BRR"].size != 16 or t["ADC1.SR"].size != 8:
            bad.append("fixture: register sizes 16/8 not kept")
        if t["USART1.RDR"].read_action != "modify" or t["USART1.TDR"].access != "write-only":
            bad.append("fixture: readAction / access not kept")
        if t["SPI1.DR"].fields["DATA"].read_action != "clear":
            bad.append("fixture: field-level readAction not kept")
        # the schema's number forms: leading zero, k/m scale, #binary, +
        nums = (SVD_HEAD + periph("A", base="08", regs=regx("R", "+0x10") + regx("S", "4k")
                + regx("T", "#100") + regx("U", "1M")) + SVD_TAIL)
        f = Path(d) / "nums.svd"
        f.write_text(nums, encoding="utf-8")
        tn = svdmod.load_svd(f)
        want_n = {"A.R": 8 + 0x10, "A.S": 8 + 4096, "A.T": 8 + 4, "A.U": 8 + (1 << 20)}
        for k, a in want_n.items():
            if k not in tn or tn[k].addr != a:
                bad.append(f"numbers: {k} -> {tn[k].addr if k in tn else None}, want {a}")
        # access undeclared at every level loads, and is refused at pick time
        noacc = ('<?xml version="1.0"?><device><name>X</name><addressUnitBits>8</addressUnitBits><size>32</size><peripherals>'
                 + periph("A", regs=regx("R") + regx("S", "4", extra="<access>read-only</access>")) + SVD_TAIL)
        f = Path(d) / "noacc.svd"
        f.write_text(noacc, encoding="utf-8")
        tna = svdmod.load_svd(f)
        if tna["A.R"].access is not None or tna["A.S"].access != "read-only":
            bad.append("access: undeclared must stay None, declared must be kept")
        # a derived peripheral with its own registers inherits the base's size
        inh = (SVD_HEAD + '<peripheral><name>A</name><baseAddress>0x0</baseAddress><size>8</size>'
               '<registers>' + regx("R") + '</registers></peripheral>'
               '<peripheral derivedFrom="A"><name>B</name><baseAddress>0x100</baseAddress>'
               '<registers>' + regx("S") + '</registers></peripheral>' + SVD_TAIL)
        f = Path(d) / "inh.svd"
        f.write_text(inh, encoding="utf-8")
        ti = svdmod.load_svd(f)
        if ti["B.S"].size != 8:
            bad.append(f"derived peripheral with own registers: size {ti['B.S'].size}, want 8 from the base")
        # vendor descriptions: symbols transliterated by a fixed table, accents
        # decomposed; a character with no ASCII form is refused by name
        uni = (SVD_HEAD + periph("A", regs=regx("R", extra="<description>10 \u00b5A \u00b11% at 25\u00b0C, r\u00e9sum\u00e9 \u2013 \u2264 3 \u03a9</description>")
               + regx("S", "4", extra="<description>\u4e2d\u6587</description>")) + SVD_TAIL)
        f = Path(d) / "uni.svd"
        f.write_text(uni, encoding="utf-8")
        tu = svdmod.load_svd(f)
        got = svdmod.register_desc(tu["A.R"])
        want_u = "A.R @0x40000000: 10 uA +/-1% at 25degC, resume - <= 3 ohm"
        if got != want_u:
            bad.append(f"transliteration: {got!r} != {want_u!r}")
        try:
            svdmod.register_desc(tu["A.S"])
            bad.append("a description with no ASCII form was accepted")
        except svdmod.SvdError as e:
            if "U+4E2D" not in str(e):
                bad.append(f"no-ASCII refusal lacks the character: {e}")
        # the header: name, series (whitespace folded), cpu spelt out from the
        # schema's list; "other" says nothing; a file without a header gives none
        for parts, want in [
            (("<name>STM32L052</name><series>STM32L0  \n  series</series><cpu><name>CM0+</name>"
              "<revision>r0p0</revision><endian>little</endian><mpuPresent>true</mpuPresent>"
              "<fpuPresent>false</fpuPresent><nvicPrioBits>4</nvicPrioBits>"
              "<vendorSystickConfig>false</vendorSystickConfig></cpu>",),
             {"name": "STM32L052", "series": "STM32L0 series", "cpu": "CM0PLUS"}),
            (("<name>X</name><cpu><name>CM0PLUS</name></cpu>",), {"name": "X", "cpu": "CM0PLUS"}),
            (("<name>X</name><cpu><name>ARMV81MML</name></cpu>",), {"name": "X", "cpu": "ARMV81MML"}),
            (("<name>X</name><cpu><name>CM85</name><pmuPresent>true</pmuPresent>"
              "<pmuNumEventCnt>8</pmuNumEventCnt></cpu>",), {"name": "X", "cpu": "CM85"}),
            (("<name>X</name><cpu><name>other</name></cpu>",), {"name": "X", "cpu": "other"}),
            (("<name>X</name>",), {"name": "X"}),
        ]:
            f = Path(d) / "hdr.svd"
            f.write_text(header(*parts), encoding="utf-8")
            got = svdmod.load_svd(f).chip
            if got != want:
                bad.append(f"header {parts[0][:40]!r}: chip {got} != {want}")
    return "; ".join(bad) if bad else None


def check_svd():
    """The silicon fixture generates, names as documented, compiles
    where a C compiler is at hand, and force: true takes a side effect
    register on purpose."""
    import shutil
    with tempfile.TemporaryDirectory() as d:
        r = subprocess.run([*GEN, str(SVD_YAML), "-o", d], capture_output=True, text=True)
        if r.returncode != 0:
            return f"svd fixture rejected: {r.stderr.strip()}"
        c = (Path(d) / "registers.c").read_text(encoding="ascii")
        h = (Path(d) / "registers.h").read_text(encoding="ascii")
        md = (Path(d) / "registers.md").read_text(encoding="ascii")
        # every registry entry as a block, keyed by name
        blocks = {}
        for m in re.finditer(r"\{ \.name = \"([a-z0-9_]+)\",(.*?)\}\s*,", c, re.S):
            blocks[m.group(1)] = m.group(2)
        # every generated sampler: name -> (ctype, width type, addr, shift, mask)
        samplers = {}
        for m in re.finditer(
                r"static (\w+) (\w+);\s*static void \2_sample\(const RegEntry \*entry\)\s*\{\s*\(void\)entry;\s*"
                r"\2 = \(\1\)\(\(\*\(volatile (\w+) \*\)0x([0-9A-F]{8})U >> (\d+)\) & 0x([0-9A-F]+)U\);\s*\}", c):
            samplers[m.group(2)] = (m.group(1), m.group(3), int(m.group(4), 16), int(m.group(5)), int(m.group(6), 16))
        expect_inplace = {                       # name: (addr, REG type)
            "usart2_isr": (0x4000441C, "REG_U32"), "timer_count": (0x40000424, "REG_U16"),
            "tim3_ccr3": (0x4000043C, "REG_U32"), "adc1_ch_dr": (0x40012440, "REG_U32"),
        }
        expect_field = {                         # name: (ctype, wtype, addr, shift, mask, REG type)
            "usart2_isr_txe": ("uint8_t", "uint32_t", 0x4000441C, 7, 0x1, "REG_BOOL"),
            "usart2_brr_div_mantissa": ("uint16_t", "uint16_t", 0x4000440C, 4, 0xFFF, "REG_U16"),
            "gpioa_idr_idr5": ("uint8_t", "uint32_t", 0x40010808, 5, 0x1, "REG_BOOL"),
            "adc1_sr_state": ("uint8_t", "uint8_t", 0x40012400, 4, 0x7, "REG_U8"),
        }
        for n, (addr, rtype) in expect_inplace.items():
            b = blocks.get(n)
            if b is None or f".ptr = (volatile void *)0x{addr:08X}U" not in b or f".type = {rtype}" not in b \
                    or ".perm = REG_RO" not in b or ".on_read" in b:
                return f"entry {n}: unexpected block {b!r}"
        for n, (ctype, wtype, addr, shift, mask, rtype) in expect_field.items():
            b = blocks.get(n)
            if b is None or f".ptr = &{n}" not in b or f".on_read = {n}_sample" not in b \
                    or f".type = {rtype}" not in b or ".perm = REG_RO" not in b:
                return f"entry {n}: unexpected block {b!r}"
            if samplers.get(n) != (ctype, wtype, addr, shift, mask):
                return f"sampler {n}: {samplers.get(n)} != {(ctype, wtype, addr, shift, mask)}"
        if len(samplers) != len(expect_field):
            return f"{len(samplers)} samplers generated, {len(expect_field)} expected"
        # the documented description format: path, address, field map / bit position
        expect_desc = {
            "usart2_isr": '"USART2.ISR @0x4000441C: Interrupt & status register [PE[0] RXNE[5] TXE[7]]"',
            "usart2_isr_txe": '"USART2.ISR.TXE (bit 7 @0x4000441C): Transmit data register empty"',
            "usart2_brr_div_mantissa": '"USART2.BRR.DIV_Mantissa (bits 15:4 @0x4000440C)"',
            "timer_count": '"Motor timer, counts up"',        # desc: replaces it
        }
        for n, want_d in expect_desc.items():
            if f".description = {want_d}" not in blocks.get(n, ""):
                return f"entry {n}: description is not {want_d}"
        if not all(n in blocks for n in ("temp", "led")):
            return "application entries missing from the registry"
        if "extern" in h and "usart2_isr;" in h:
            return "registers.h declares a variable for a silicon register"
        if '#define REGTABLE_GEN_SILICON_DEMO_CHIP "DEMOCHIP"' not in h \
                or '#define REGTABLE_GEN_SILICON_DEMO_CPU "Cortex-M0+"' not in h:
            return "registers.h lacks the chip macros from the SVD header"
        # the client records where each silicon register comes from, and only those
        py = (Path(d) / "silicon_demo_client.py").read_text(encoding="ascii")
        sil = py[py.index("    __silicon__ = {"):]
        sil = sil[:sil.index("    }")]
        want_sil = {"usart2_isr": "USART2.ISR", "usart2_isr_txe": "USART2.ISR.TXE",
                    "usart2_brr_div_mantissa": "USART2.BRR.DIV_Mantissa", "timer_count": "TIM3.CNT",
                    "tim3_ccr3": "TIM3.CCR3", "gpioa_idr_idr5": "GPIOA.IDR.IDR5",
                    "adc1_sr_state": "ADC1.SR.STATE", "adc1_ch_dr": "ADC1.CH.DR"}
        got_sil = dict(re.findall(r'"(\w+)": \'([^\']+)\',', sil))
        if got_sil != want_sil:
            return f"__silicon__ is {got_sil}, want {want_sil}"
        if "## Silicon registers" not in md or "| `adc1_ch_dr` | demo.svd | ADC1.CH.DR | 0x40012440 |" not in md:
            return "registers.md lacks the silicon table"
        try:
            compiles(d)
        except Exception as e:   # noqa: BLE001
            return f"generated Python does not compile: {e}"
        cc = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
        if cc:
            r = subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                                "-I", str(ROOT / "src"), "-I", d, "-c",
                                str(Path(d) / "registers.c"), "-o", str(Path(d) / "registers.o")],
                               capture_output=True, text=True)
            if r.returncode != 0:
                return f"generated C does not compile: {r.stderr.strip()}"
        else:
            print("note: no C compiler on PATH; silicon C compile check skipped")
        # a pick whose description transliterates, from a directory with a
        # non-ASCII name (the registers.md cell sanitizes the path)
        udir = Path(d) / "\u00dcbersicht"
        udir.mkdir()
        (udir / "unidesc.svd").write_bytes((ROOT / "python" / "tests" / "unidesc.svd").read_bytes())
        y = Path(d) / "u.yaml"
        y.write_text("device: uni\nregisters:\n  - svd: " + (udir / "unidesc.svd").as_posix()
                     + "\n    pick: [A.R]\n", encoding="utf-8")
        uout = Path(d) / "uout"
        r = subprocess.run([*GEN, str(y), "-o", str(uout)], capture_output=True, text=True)
        if r.returncode != 0:
            return f"unicode path / transliterated desc rejected: {r.stderr.strip()}"
        uc = (uout / "registers.c").read_text(encoding="ascii")
        if '.description = "A.R @0x40000000: 10 uA +/-1%"' not in uc:
            return "transliterated description not in registers.c"
        umd = (uout / "registers.md").read_text(encoding="ascii")
        if "?bersicht" not in umd:
            return "registers.md did not sanitize the non-ASCII svd path"
        # an output target that is a directory is refused before anything is written
        bad_out = Path(d) / "badout"
        bad_out.mkdir()
        (bad_out / "registers.c").mkdir()
        r = subprocess.run([*GEN, str(SVD_YAML), "-o", str(bad_out)], capture_output=True, text=True)
        if r.returncode != 2 or "exists and is not a file" not in r.stderr:
            return f"directory in place of registers.c: rc={r.returncode} {r.stderr.strip()}"
        if (bad_out / "registers.h").exists():
            return "registers.h was written although registers.c could not be"

        # force: true takes disableCondition and alternate-view side effects knowingly
        y = Path(d) / "alt.yaml"
        y.write_text(HEAD + f"  - svd: {(ROOT / 'python' / 'tests' / 'alt.svd').as_posix()}\n"
                     "    force: true\n    pick: [DBG.IDCODE, DBG2.IDCODE, P.SAFE, P.PLAIN, P.TINYSAFE]\n",
                     encoding="utf-8")
        r = subprocess.run([*GEN, str(y), "-o", d], capture_output=True, text=True)
        if r.returncode != 0:
            return f"force: true did not take the alt fixture: {r.stderr.strip()}"
        y.write_text(HEAD + f"  - svd: {(ROOT / 'python' / 'tests' / 'wofield.svd').as_posix()}\n"
                     "    force: true\n    pick: [P.RW]\n", encoding="utf-8")
        r = subprocess.run([*GEN, str(y), "-o", d], capture_output=True, text=True)
        if r.returncode != 0:
            return f"force: true did not take the write-only-field fixture: {r.stderr.strip()}"
        # header strings reach C through the one string-literal escaper:
        # a series with quotes, backslashes and ?? stays a valid literal
        esc = Path(d) / "esc.svd"
        esc.write_text(HDR_HEAD + '<name>ESC</name><series>say "hi" ??/ tail' + BS + '</series>' + HDR_TAIL,
                       encoding="utf-8")
        y = Path(d) / "esc.yaml"
        y.write_text(HEAD + f"  - svd: {esc.as_posix()}\n    pick: [A.R]\n", encoding="utf-8")
        r = subprocess.run([*GEN, str(y), "-o", d], capture_output=True, text=True)
        h2 = (Path(d) / "registers.h").read_text(encoding="ascii") if r.returncode == 0 else ""
        want_line = '#define REGTABLE_GEN_DEMO_SERIES "say ' + BS + '"hi' + BS + '" ' + BS + '?' + BS + '?/ tail' + BS + BS + '"'
        if r.returncode != 0 or want_line not in h2:
            return f"series escaping: rc={r.returncode} {r.stderr.strip()[-200:]} {h2[-300:]!r}"
        if cc:
            r = subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-Wtrigraphs",
                                "-I", str(ROOT / "src"), "-I", d, "-c",
                                str(Path(d) / "registers.c"), "-o", str(Path(d) / "registers.o")],
                               capture_output=True, text=True)
            if r.returncode != 0:
                return f"escaped series does not compile: {r.stderr.strip()}"
        # several SVDs of one chip: header facts merge field by field in
        # either order, and a fact they disagree on is refused
        m1 = Path(d) / "m1.svd"; m2 = Path(d) / "m2.svd"; m3 = Path(d) / "m3.svd"
        m1.write_text(header("<name>MERGE</name><series>S1</series><cpu><name>CM3</name></cpu>"), encoding="utf-8")
        m2.write_text(HDR_HEAD + "<name>MERGE</name><cpu><name>CM3</name></cpu><addressUnitBits>8</addressUnitBits>"
                      "<size>32</size><access>read-write</access><peripherals>"
                      + periph("B", base="0x40001000", regs=regx("R")) + "</peripherals></device>", encoding="utf-8")
        m3.write_text(HDR_HEAD + "<name>MERGE</name><cpu><name>CM4</name></cpu><addressUnitBits>8</addressUnitBits>"
                      "<size>32</size><access>read-write</access><peripherals>"
                      + periph("B", base="0x40001000", regs=regx("R")) + "</peripherals></device>", encoding="utf-8")
        for first, second in ((m1, m2), (m2, m1)):
            y.write_text(HEAD + f"  - svd: {first.as_posix()}\n    pick: [{'A' if first is m1 else 'B'}.R]\n"
                         f"  - svd: {second.as_posix()}\n    pick: [{'A' if second is m1 else 'B'}.R]\n", encoding="utf-8")
            r = subprocess.run([*GEN, str(y), "-o", d], capture_output=True, text=True)
            h2 = (Path(d) / "registers.h").read_text(encoding="ascii") if r.returncode == 0 else ""
            if r.returncode != 0 or '#define REGTABLE_GEN_DEMO_SERIES "S1"' not in h2 \
                    or '#define REGTABLE_GEN_DEMO_CPU "Cortex-M3"' not in h2:
                return f"chip facts merge ({first.name} then {second.name}): rc={r.returncode} {r.stderr.strip()[-200:]}"
        y.write_text(HEAD + f"  - svd: {m1.as_posix()}\n    pick: [A.R]\n"
                     f"  - svd: {m3.as_posix()}\n    pick: [B.R]\n", encoding="utf-8")
        r = subprocess.run([*GEN, str(y), "-o", d], capture_output=True, text=True)
        if r.returncode != 2 or "disagree on the chip cpu: 'CM3' and 'CM4'" not in r.stderr:
            return f"two SVDs disagreeing on the cpu: rc={r.returncode} {r.stderr.strip()[-200:]}"
        # "other" is a stated cpu: it disagrees with CM3, and alone it gives no _CPU macro
        m4 = Path(d) / "m4.svd"
        m4.write_text(HDR_HEAD + "<name>MERGE</name><cpu><name>other</name></cpu><addressUnitBits>8</addressUnitBits>"
                      "<size>32</size><access>read-write</access><peripherals>"
                      + periph("B", base="0x40001000", regs=regx("R")) + "</peripherals></device>", encoding="utf-8")
        for first, second, want_msg in ((m1, m4, "'CM3' and 'other'"), (m4, m1, "'other' and 'CM3'")):
            y.write_text(HEAD + f"  - svd: {first.as_posix()}\n    pick: [{'A' if first is m1 else 'B'}.R]\n"
                         f"  - svd: {second.as_posix()}\n    pick: [{'A' if second is m1 else 'B'}.R]\n", encoding="utf-8")
            r = subprocess.run([*GEN, str(y), "-o", d], capture_output=True, text=True)
            if r.returncode != 2 or f"disagree on the chip cpu: {want_msg}" not in r.stderr:
                return f"cpu other vs CM3 ({first.name} then {second.name}): rc={r.returncode} {r.stderr.strip()[-200:]}"
        y.write_text(HEAD + f"  - svd: {m4.as_posix()}\n    pick: [B.R]\n", encoding="utf-8")
        r = subprocess.run([*GEN, str(y), "-o", d], capture_output=True, text=True)
        h2 = (Path(d) / "registers.h").read_text(encoding="ascii") if r.returncode == 0 else ""
        if r.returncode != 0 or "REGTABLE_GEN_DEMO_CPU" in h2 or '#define REGTABLE_GEN_DEMO_CHIP "MERGE"' not in h2:
            return f"cpu other alone: rc={r.returncode} {r.stderr.strip()[-200:]}"
        # two SVDs naming different chips in one YAML are refused
        y.write_text(HEAD + f"  - svd: {(ROOT / 'python' / 'tests' / 'wofield.svd').as_posix()}\n"
                     "    pick: [P.RW.R]\n"
                     f"  - svd: {(ROOT / 'python' / 'tests' / 'alt.svd').as_posix()}\n"
                     "    pick: [P.PLAIN]\n", encoding="utf-8")
        r = subprocess.run([*GEN, str(y), "-o", d], capture_output=True, text=True)
        if r.returncode != 2 or "describe different chips" not in r.stderr:
            return f"two chips in one YAML: rc={r.returncode} {r.stderr.strip()[-200:]}"
        # the success line is printable on an ASCII-only console, non-ASCII output path included
        env = dict(os.environ, PYTHONIOENCODING="ascii")
        r = subprocess.run([*GEN, str(SVD_YAML), "-o", str(udir / "out\u00e9")],
                           capture_output=True, text=True, env=env)
        if r.returncode != 0 or "registers.c" not in r.stdout:
            return f"success line on an ASCII console: rc={r.returncode} {r.stderr.strip()[-200:]}"

        # force: true takes the side-effect register knowingly
        y = Path(d) / "f.yaml"
        y.write_text(HEAD + svd_block(["USART1.RDR"], force="true"), encoding="utf-8")
        r = subprocess.run([*GEN, str(y), "-o", d], capture_output=True, text=True)
        if r.returncode != 0:
            return f"force: true still refused: {r.stderr.strip()}"
    return None


def main():
    failures = 0

    r = run(GOOD)
    if r.returncode != 0:
        print(f"FAIL: the good fixture was rejected: {r.stderr.strip()}")
        failures += 1
    with tempfile.TemporaryDirectory() as d:
        y = Path(d) / "t.yaml"
        y.write_text(GOOD, encoding="utf-8")
        subprocess.run([*GEN, str(y), "-o", d],
                       capture_output=True, text=True)
        try:
            compiles(d)
        except Exception as e:   # noqa: BLE001
            print(f"FAIL: good fixture's Python client does not compile: {e}")
            failures += 1

    err = check_escaping()
    if err:
        print(f"FAIL: {err}")
        failures += 1

    err = check_svd_reader()
    if err:
        print(f"FAIL: svd reader: {err}")
        failures += 1

    err = check_svd()
    if err:
        print(f"FAIL: {err}")
        failures += 1

    for label, text, expect in BAD + [(l, HEAD + t, e) for l, t, e in SVD_BAD]:
        r = run(text)
        if r.returncode == 0:
            print(f"FAIL: accepted: {label}")
            failures += 1
        elif r.returncode != 2 or "Traceback" in r.stderr or "regtable gen: error:" not in r.stderr:
            print(f"FAIL: {label}: not a clean refusal (rc={r.returncode}): {r.stderr.strip()[-200:]}")
            failures += 1
        elif expect not in r.stderr:
            print(f"FAIL: {label}: error lacks '{expect}': {r.stderr.strip()}")
            failures += 1

    # a YAML that is not UTF-8 text is refused the same way
    with tempfile.TemporaryDirectory() as d:
        y = Path(d) / "bad.yaml"
        y.write_bytes(b"\xff\xfedevice: demo\n")
        r = subprocess.run([*GEN, str(y), "-o", d], capture_output=True, text=True)
        if r.returncode != 2 or "not UTF-8" not in r.stderr or "Traceback" in r.stderr:
            print(f"FAIL: non-UTF-8 YAML: rc={r.returncode} {r.stderr.strip()[-200:]}")
            failures += 1

    total = len(BAD) + len(SVD_BAD) + 5
    if failures:
        print(f"{failures} of {total} validator checks FAILED")
        return 1
    print(f"all {total} validator checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
