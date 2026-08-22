#!/usr/bin/env python3
"""The generated typed client against a real Uno running
examples/arduino/arduino.ino.

    regtable gen tools/uno.yaml -o gen_uno      # or: python python/regtable gen ...
    python tools/uno_client_demo.py COM6        # or /dev/ttyUSB0

Walks the client through reads, writes, local refusals, the device's
own refusals, and then points the *wrong* client (the demo table from
example.yaml) at the same board to show SchemaDriftError doing its
job on hardware.
"""

import sys
import time
from pathlib import Path

# the Windows console is often gbk; never let an odd byte crash a print
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "gen_uno"))
sys.path.insert(0, str(ROOT / "gen"))

from regtable.client import RemoteError, SchemaDriftError  # noqa: E402

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"

try:
    from uno_client import UnoDevice  # noqa: E402
except ImportError:
    sys.exit("run first: regtable gen tools/uno.yaml -o gen_uno")

print(f"== UnoDevice on {port} (generated from tools/uno.yaml) ==")
with UnoDevice.serial(port) as dev:            # handshake: list --json vs schema
    print("handshake: OK, the board's table matches uno.yaml")
    print("a0      =", dev.a0)
    print("uptime  =", dev.uptime, "ms")
    dev.led = True
    print("led     =", dev.led, "  <- the board's LED is on")
    dev.pwm9 = 128
    print("pwm9    =", dev.pwm9)
    try:
        dev.pwm9 = 300
    except ValueError as e:
        print("pwm9 = 300  ->", type(e).__name__ + ":", e)
    try:
        dev.a0 = 1
    except AttributeError:
        print("a0 = 1      -> AttributeError (read-only: no setter)")
    try:
        dev.lde = True
    except AttributeError as e:
        print("lde = True  ->", type(e).__name__ + ":", e)
    try:
        dev._set("a0", 5)
    except RemoteError as e:
        print("device says ->", e)
    t1 = dev.uptime
    time.sleep(0.5)
    print("uptime advancing:", dev.uptime > t1)
    print("snapshot:", dev.snapshot())
    dev.led = False
    dev.pwm9 = 0

print()
print("== DemoDevice (example.yaml) on the same board: must drift ==")
try:
    from demo_client import DemoDevice  # noqa: E402
except ImportError:
    sys.exit("run first: regtable gen tools/example.yaml -o gen")
try:
    DemoDevice.serial(port)
    print("no error: that would be a bug")
except SchemaDriftError as e:
    print(type(e).__name__ + ":")
    print(e)
