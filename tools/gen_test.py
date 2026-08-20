#!/usr/bin/env python3
"""Regression test for regtable_gen.py's validator.

Every case is a YAML document the generator must reject, with a
fragment its error message must contain. Run by `make codegen`.
"""

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).parent
GEN = TOOLS / "regtable_gen.py"

HEAD = "device: demo\nregisters:\n"


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
]


def run(text):
    with tempfile.TemporaryDirectory() as d:
        y = Path(d) / "t.yaml"
        y.write_text(text, encoding="utf-8")
        return subprocess.run(
            [sys.executable, str(GEN), str(y), "-o", d],
            capture_output=True, text=True)


def check_escaping():
    """The emitters must keep hostile descriptions harmless: no C
    trigraphs, no broken Markdown table cells."""
    desc = "trigraph ???/ pipe | back " + chr(92) + "| end"
    text = HEAD + reg(name="a", type="u8", perm="ro") \
        .replace("perm: ro", f"perm: ro\n    desc: '{desc}'")
    with tempfile.TemporaryDirectory() as d:
        y = Path(d) / "t.yaml"
        y.write_text(text, encoding="utf-8")
        r = subprocess.run([sys.executable, str(GEN), str(y), "-o", d],
                           capture_output=True, text=True)
        if r.returncode != 0:
            return f"escaping fixture rejected: {r.stderr.strip()}"
        c = (Path(d) / "registers.c").read_text(encoding="ascii")
        md = (Path(d) / "registers.md").read_text(encoding="ascii")
        if "??" in c:
            return "generated C still contains '??' (trigraph risk)"
        row = [l for l in md.splitlines() if "trigraph" in l][0]
        # escaped pipes must not split the row into extra columns
        cols = re.split(r"(?<!" + re.escape(chr(92)) + r")\|", row)
        if len(cols) != 9:              # empty, 7 fields, empty
            return f"markdown row split into {len(cols) - 2} columns"
    return None


def main():
    failures = 0

    r = run(GOOD)
    if r.returncode != 0:
        print(f"FAIL: the good fixture was rejected: {r.stderr.strip()}")
        failures += 1

    err = check_escaping()
    if err:
        print(f"FAIL: {err}")
        failures += 1

    for label, text, expect in BAD:
        r = run(text)
        if r.returncode == 0:
            print(f"FAIL: accepted: {label}")
            failures += 1
        elif expect not in r.stderr:
            print(f"FAIL: {label}: error lacks '{expect}': {r.stderr.strip()}")
            failures += 1

    if failures:
        print(f"{failures} of {len(BAD) + 2} validator checks FAILED")
        return 1
    print(f"all {len(BAD) + 2} validator checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
