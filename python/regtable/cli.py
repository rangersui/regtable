"""The `regtable` command.

    regtable gen <yaml> [-o DIR] [--max-entries N]
    regtable connect (-p PORT [-b BAUD] | --pipe CMD [ARG ...]) [--yaml FILE]
    regtable watch [NAME ...] (-p PORT | --pipe CMD [ARG ...]) [--yaml FILE] [--every S] [--count N] [--json]
    regtable fetch (-p PORT | --pipe CMD [ARG ...]) [--yaml FILE]
    regtable serve [--port PORT] [--no-browser]

connect and watch take the device as it describes itself (list --json);
with --yaml they build the typed client from the YAML instead and
verify the device against it before anything else: a device whose
table differs ends the command with the SchemaDriftError text and
exit status 2.
"""

import argparse
import code
import json
import sys
import time

from . import __version__
from .client import (PipeTransport, SerialTransport, RegtableClient,
                     RegtableError, TransportError)
from .gen import build_client, GenerationError, main as gen_main


def _transport_args(ap):
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("-p", "--port", help="serial port, e.g. COM6 or /dev/ttyUSB0")
    g.add_argument("--pipe", nargs=argparse.REMAINDER, metavar="CMD...",
                   help="a CLI process over stdin/stdout instead of a port: "
                        "everything after --pipe is the command and its "
                        "arguments, passed through untouched (so place it last)")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--boot-delay", type=float, default=2.0,
                    help="seconds to wait after opening the port (boards that "
                         "reset on open; default 2)")
    ap.add_argument("--timeout", type=float, default=3.0,
                    help="seconds to wait for an answer (default 3)")
    ap.add_argument("--yaml", metavar="FILE",
                    help="build the typed client from this YAML and verify "
                         "the device against it (default: take the device's "
                         "own table)")
    ap.add_argument("--max-entries", type=int, default=64,
                    help="REGTABLE_MAX_ENTRIES of the target build (default 64)")


def _open(args):
    """The YAML first, the hardware second: a bad description never
    opens a port (opening one resets an Arduino)."""
    if args.pipe is not None and not args.pipe:
        raise TransportError("--pipe needs a command after it")
    cls = build_client(args.yaml, args.max_entries) if args.yaml else None
    if args.port:
        t = SerialTransport(args.port, args.baud, boot_delay=args.boot_delay)
    else:
        t = PipeTransport(args.pipe)
    if cls:
        return cls(t, timeout=args.timeout)     # closes t if the handshake fails
    return RegtableClient.discover(t, timeout=args.timeout)


def cmd_gen(args):
    return gen_main([args.yaml_file, "-o", args.outdir,
                     "--max-entries", str(args.max_entries)])


def cmd_connect(args):
    dev = _open(args)
    cls = type(dev)
    access = ("dev.<name> reads, dev.<name> = value writes"
              if args.yaml else "dev['<name>'] reads, dev['<name>'] = value writes")
    banner = f"{dev!r}\n{access}; dev.registers(), dev.snapshot(), dev.watch(...)"
    try:
        code.interact(banner=banner, local={"dev": dev, cls.__name__: cls},
                      exitmsg="")
    finally:
        dev.close()
    return 0


def cmd_watch(args):
    dev = _open(args)
    unknown = [n for n in args.names if n not in type(dev).__schema__]
    if unknown:
        dev.close()
        print(f"regtable watch: no such register: {', '.join(unknown)}",
              file=sys.stderr)
        return 2
    try:
        for name, value in dev.watch(*args.names, every=args.every,
                                     count=args.count):
            if args.json:
                print(json.dumps({"t": time.time(), "name": name,
                                  "value": value}), flush=True)
            else:
                stamp = time.strftime("%H:%M:%S")
                shown = f"{value:.9g}" if isinstance(value, float) else value   # as the device prints it
                print(f"{stamp} {name:<16} {shown}", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        dev.close()
    return 0


def printable(s):
    """One line of printable ASCII: any other character is shown as
    its escape (\\x1b, \\u00b5) instead of reaching the terminal. The
    device's strings are its own; what fetch prints stays ASCII."""
    return "".join(c if 32 <= ord(c) < 127 else
                   (f"\\x{ord(c):02x}" if ord(c) < 128 else
                    f"\\u{ord(c):04x}" if ord(c) < 0x10000 else f"\\U{ord(c):08x}")
                   for c in str(s))


def fetch_text(ident, schema, pins):
    """The device drawn as a chip, pure ASCII. The silicon peripherals
    the table exposes sit on the side pins (left, right, alternating,
    in table order); the top and bottom pins are the package. Below,
    what the device said about itself (rendered printable, see
    printable()) and what the table holds."""
    ident = {k: printable(v) if isinstance(v, str) else v for k, v in ident.items()}
    left, right = pins[0::2], pins[1::2]
    rows = max(len(left), len(right), 4)
    inner = 21                                      # the die, between the body walls
    lab = max([len(x) for x in pins] + [6])         # label column width
    top_pins = 9
    pin_row = "-" + "-+" * top_pins + "--"          # as wide as the die: 1 + 18 + 2 = inner
    pin_legs = "   " + ("| " * top_pins).rstrip()   # one leg under each + of the row
    pad = " " * (lab + 5)                           # under the labels and the "----"
    lines = []
    lines.append(f"{pad}{pin_legs}")
    lines.append(f"{pad}+{pin_row}+")
    face = {0: "o", 1: "regtable", 2: f"v{ident.get('regtable', '?')}"}
    for i in range(rows):
        l = left[i] if i < len(left) else ""
        r = right[i] if i < len(right) else ""
        if i == 0:
            mid = " o" + " " * (inner - 2)            # pin 1 mark, top-left of the die
        elif i == 1:
            mid = "regtable".center(inner)
        elif i == 2:
            mid = f"v{ident.get('regtable', '?')}".center(inner)
        else:
            mid = " " * inner
        lines.append(f"{l:>{lab}} ----|{mid}|---- {r}")
    lines.append(f"{pad}+{pin_row}+")
    lines.append(f"{pad}{pin_legs}")
    lines.append("")
    who = ident.get("device") or "device"
    if ident.get("chip"):
        who += f" @ {ident['chip']}"
    lines.append(f"{'':>{lab + 2}}{who}")
    fw = ident.get("fw")
    if fw:
        lines.append(f"{'':>{lab + 2}}{'fw':<9} {fw}" + (f" ({ident['hash']})" if ident.get("hash") else ""))
    for key in ("built", "compiler", "regtable"):
        if key in ident:
            lines.append(f"{'':>{lab + 2}}{key:<9} {ident[key]}")
    rw = sum(1 for e in schema.values() if e["perm"] == "RW")
    lines.append(f"{'':>{lab + 2}}{'regs':<9} {len(schema)} ({rw} RW, {len(schema) - rw} RO)")
    lines.append(f"{'':>{lab + 2}}{'schema':<9} {ident.get('schema', '?')}")
    return "\n".join(l.rstrip() for l in lines)


def silicon_pins(cls):
    """The peripherals a client's silicon registers come from, in
    table order, each once: what the generated class recorded from the
    SVD picks (__silicon__). A discovered device carries no such
    record, so its pins stay bare."""
    pins = []
    for path in cls.__silicon__.values():
        p = path.split(".", 1)[0]
        if p not in pins:
            pins.append(p)
    return pins


def cmd_fetch(args):
    dev = _open(args)
    try:
        ident = dev.identity()
        print(fetch_text(ident, type(dev).__schema__, silicon_pins(type(dev))))
    finally:
        dev.close()
    return 0


def cmd_serve(args):
    from .panel import serve
    return serve(args.port, open_browser=not args.no_browser)


def build_parser():
    ap = argparse.ArgumentParser(prog="regtable",
                                 description=__doc__.splitlines()[0])
    ap.add_argument("--version", action="version",
                    version=f"regtable {__version__}")
    sub = ap.add_subparsers(dest="command", required=True)

    g = sub.add_parser("gen", help="YAML -> registers.c/.h/.md + typed Python client")
    g.add_argument("yaml_file")
    g.add_argument("-o", "--outdir", default=".")
    g.add_argument("--max-entries", type=int, default=64)
    g.set_defaults(fn=cmd_gen)

    c = sub.add_parser("connect", help="open a device, drop into a REPL")
    _transport_args(c)
    c.set_defaults(fn=cmd_connect)

    w = sub.add_parser("watch", help="print register changes as they happen")
    w.add_argument("names", nargs="*", help="registers to watch (default: all)")
    _transport_args(w)
    w.add_argument("--every", type=float, default=1.0, help="poll period, s (default 1)")
    w.add_argument("--count", type=int, default=None, help="stop after N polls")
    w.add_argument("--json", action="store_true", help="one JSON object per line")
    w.set_defaults(fn=cmd_watch)

    f = sub.add_parser("fetch", help="the device's identity and table, drawn as a chip")
    _transport_args(f)
    f.set_defaults(fn=cmd_fetch)

    s = sub.add_parser("serve", help="serve the Web Serial panel on localhost")
    s.add_argument("--port", type=int, default=8321)
    s.add_argument("--no-browser", action="store_true")
    s.set_defaults(fn=cmd_serve)
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        return args.fn(args)
    except (RegtableError, GenerationError) as e:
        print(f"regtable {args.command}: {e}", file=sys.stderr)
        return 2
