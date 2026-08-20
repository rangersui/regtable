#!/usr/bin/env python3
"""Generate regtable projections from a YAML register description.

    python tools/regtable_gen.py device.yaml -o outdir/

writes registers.c, registers.h, and registers.md into outdir/.

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
"""

import argparse
import math
import re
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    sys.exit("regtable_gen: PyYAML is required (pip install pyyaml)")


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

TYPES = {
    "u8":    ("uint8_t",  "REG_U8",    1, (0, 0xFF)),
    "u16":   ("uint16_t", "REG_U16",   1, (0, 0xFFFF)),
    "u32":   ("uint32_t", "REG_U32",   2, (0, 0xFFFFFFFF)),
    "i8":    ("int8_t",   "REG_I8",    1, (-128, 127)),
    "i16":   ("int16_t",  "REG_I16",   1, (-32768, 32767)),
    "i32":   ("int32_t",  "REG_I32",   2, (-2**31, 2**31 - 1)),
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

FLT_MAX = 3.4028234663852886e38          # binary32


FLT_MIN = 1.1754943508222875e-38         # smallest normal binary32


def finite_float(v):
    """A value a float register can hold and a C constant can spell:
    zero, or a normal binary32 magnitude. Integers of any size
    compare without float conversion, so no OverflowError."""
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return False
    if isinstance(v, float) and not math.isfinite(v):
        return False
    return v == 0 or FLT_MIN <= abs(v) <= FLT_MAX


def fail(msg):
    sys.exit(f"regtable_gen: error: {msg}")


def is_ascii(s):
    return all(32 <= ord(c) < 127 for c in s)


def load(path):
    try:
        doc = yaml.load(Path(path).read_text(encoding="utf-8"),
                        Loader=UniqueKeyLoader)
    except yaml.YAMLError as e:
        fail(f"{path}: {e}")
    if not isinstance(doc, dict):
        fail(f"{path}: top level must be a mapping")
    return doc


def validate(doc, max_entries=64):
    device = doc.get("device")
    if not isinstance(device, str) or not NAME_RE.match(device):
        fail("'device' must be a lowercase identifier")
    extra = set(doc) - {"device", "registers"}
    if extra:
        fail(f"unknown top-level keys {sorted(map(repr, extra))}")
    regs = doc.get("registers")
    if not isinstance(regs, list) or not regs:
        fail("'registers' must be a non-empty list")
    if len(regs) > max_entries:
        fail(f"{len(regs)} registers, but the dirty bitmap holds "
             f"{max_entries} (REGTABLE_MAX_ENTRIES); raise it in the "
             f"build and pass --max-entries to match")

    known = {"name", "type", "perm", "init", "desc", "min", "max",
             "modbus", "hooks"}
    seen_names = set()
    spans = []          # (start, end_exclusive, name) for modbus overlap
    hook_kinds = {}     # function name -> set of hook kinds using it

    for i, r in enumerate(regs):
        where = f"registers[{i}]"
        if not isinstance(r, dict):
            fail(f"{where}: must be a mapping")
        extra = set(r) - known
        if extra:
            fail(f"{where}: unknown keys {sorted(map(repr, extra))}")

        name = r.get("name")
        if not isinstance(name, str) or not NAME_RE.match(name):
            fail(f"{where}: 'name' must match {NAME_RE.pattern} "
                 f"(a C identifier that is also one MQTT topic level)")
        prob = symbol_problem(name, device)
        if prob:
            fail(f"{where}: '{name}' {prob}")
        if name in seen_names:
            fail(f"{where}: duplicate name '{name}'")
        seen_names.add(name)
        where = f"register '{name}'"

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
        if not isinstance(mb, int) or isinstance(mb, bool) or mb < 0 or mb > 0xFFFF:
            fail(f"{where}: 'modbus' must be a word address 1..65535 "
                 f"(0 or absent = not mapped)")
        if mb:
            words = TYPES[t][2]
            if mb + words - 1 > 0xFFFF:
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
        f"/* Generated by regtable_gen.py from the '{device}' description.",
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
        "/* the registers, owned here, used by application code */",
    ]
    for r in regs:
        lines.append(f"extern {TYPES[r['type']][0]} {r['name']};")
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
        f"/* Generated by regtable_gen.py from the '{device}' description.",
        " * Edit the YAML, not this file. */",
        "",
        '#include "registers.h"',
        "#include <stddef.h>",
        "",
    ]
    for r in regs:
        t = r["type"]
        init = r.get("init", 0)
        if t == "bool":
            init = 1 if init in (1, True) else 0
        lines.append(f"{TYPES[t][0]} {r['name']} = {c_literal(t, init)};")
    lines += ["", f"const RegEntry {device}_registry[] = {{"]
    for r in regs:
        t = r["type"]
        f = [f'.name = "{r["name"]}"',
             f'.ptr = &{r["name"]}',
             f'.type = {TYPES[t][1]}',
             f'.perm = {"REG_RO" if r["perm"] == "ro" else "REG_RW"}']
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
        "Generated by regtable_gen.py; the YAML description is the source.",
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
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("yaml_file")
    ap.add_argument("-o", "--outdir", default=".",
                    help="directory for registers.c/.h/.md (default: .)")
    ap.add_argument("--max-entries", type=int, default=64,
                    help="REGTABLE_MAX_ENTRIES of the target build (default 64)")
    args = ap.parse_args()

    if not 1 <= args.max_entries <= 0xFFFF:
        fail("--max-entries must be 1..65535 (the table count is uint16)")
    device, regs = validate(load(args.yaml_file), args.max_entries)
    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)
    (out / "registers.h").write_text(emit_h(device, regs), encoding="ascii")
    (out / "registers.c").write_text(emit_c(device, regs), encoding="ascii")
    (out / "registers.md").write_text(emit_md(device, regs), encoding="ascii")
    print(f"regtable_gen: {len(regs)} registers -> "
          f"{out / 'registers.c'}, registers.h, registers.md")


if __name__ == "__main__":
    main()
