#!/usr/bin/env python3
"""End-to-end test for the generated Python client, no hardware.

    client_test.py <cli-binary> <desktop-example-binary> <gen-dir>

Positive path: the client generated from example.yaml talks to a CLI
built over the table generated from the same YAML; the handshake
must pass and reads, writes, and refusals must behave.

Negative path: the same client is pointed at the desktop example,
whose table differs (an extra `counter` register, other Modbus
addresses); the handshake must fail loudly and name the drift.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "python"))

from regtable.client import (  # noqa: E402
    PipeTransport, RemoteError, SchemaDriftError, TransportError)
from regtable import build_client  # noqa: E402

failures = 0
cases = 0


def check(cond, label):
    global failures, cases
    cases += 1
    if not cond:
        failures += 1
        print(f"FAIL: {label}")


def expect_raises(exc, label, fn):
    global failures, cases
    cases += 1
    try:
        fn()
    except exc:
        return
    except Exception as e:  # noqa: BLE001
        failures += 1
        print(f"FAIL: {label}: raised {type(e).__name__} instead of {exc.__name__}: {e}")
        return
    failures += 1
    print(f"FAIL: {label}: nothing raised")


def main():
    cli_bin, example_bin, gen_dir = sys.argv[1:4]
    # absolute, native paths: Windows' CreateProcess does not take a
    # forward-slash relative path as an executable
    cli_bin = str(Path(cli_bin).resolve())
    example_bin = str(Path(example_bin).resolve())
    sys.path.insert(0, str(Path(gen_dir).resolve()))
    import demo_client  # noqa: E402  (generated)
    Dev = demo_client.DemoDevice

    # -- positive: generated client vs generated table -------------- #
    with Dev(PipeTransport([cli_bin])) as dev:
        check(dev.interval == 1000, "initial interval from YAML init")
        check(abs(dev.temp - 23.4) < 1e-5, "initial temp, float coerced")
        check(dev.led is False, "initial led is a bool")
        check(dev.gain == 1.0, "initial gain")

        dev.interval = 500
        check(dev.interval == 500, "set then get interval")

        expect_raises(ValueError, "local range check", lambda: setattr(dev, "interval", 50))
        expect_raises(TypeError, "local type check int", lambda: setattr(dev, "interval", 1.5))
        expect_raises(TypeError, "bool is not an int", lambda: setattr(dev, "interval", True))
        expect_raises(RemoteError, "device range check when local is bypassed",
                      lambda: dev._set("interval", 50))
        expect_raises(RemoteError, "device read-only refusal",
                      lambda: dev._set("temp", 5))
        expect_raises(AttributeError, "RO register has no setter",
                      lambda: setattr(dev, "temp", 1.0))
        expect_raises(AttributeError, "typo is not a new attribute",
                      lambda: setattr(dev, "intreval", 5))

        dev.led = True
        check(dev.led is True, "bool round trip")
        expect_raises(TypeError, "bool register rejects int", lambda: setattr(dev, "led", 1))

        dev.gain = 1.5
        check(dev.gain == 1.5, "float round trip")
        expect_raises(ValueError, "float range", lambda: setattr(dev, "gain", 3.0))
        expect_raises(TypeError, "float type", lambda: setattr(dev, "gain", "x"))

        dev.offset = -10
        check(dev.offset == -10, "signed round trip")

        # no YAML range: the type domain still guards the wire
        dev.small = 255
        check(dev.small == 255, "domain-limited byte, top value")
        expect_raises(ValueError, "u8 domain upper", lambda: setattr(dev, "small", 256))
        expect_raises(ValueError, "u8 domain lower", lambda: setattr(dev, "small", -1))
        expect_raises(ValueError, "float nan refused locally",
                      lambda: setattr(dev, "gain", float("nan")))
        expect_raises(ValueError, "float inf refused locally",
                      lambda: setattr(dev, "gain", float("inf")))
        dev.gain = 2.5000000001          # rounds to binary32 2.5: inside
        check(dev.gain == 2.5, "float bound compared in binary32")

        regs = dev.registers()
        check([r["name"] for r in regs] == list(Dev.__schema__),
              "registers() lists the table in order")
        check(regs == Dev.registers(), "registers() works on the class too")
        check(all(set(r) >= {"name", "type", "perm"} for r in regs),
              "registers() rows carry name/type/perm")
        check(repr(dev) == "<DemoDevice open: " + ", ".join(Dev.__schema__) + ">",
              f"repr names the registers: {dev!r}")

        snap = dev.snapshot()
        check(set(snap) == set(Dev.__schema__), "snapshot covers the schema")
        check(snap["interval"] == 500 and snap["led"] is True, "snapshot values")

        seen = list(dev.watch("interval", "led", every=0, count=2))
        check(("interval", 500) in seen and ("led", True) in seen,
              "watch yields initial values")

        dev.verify()                      # still in sync after traffic
        check(True, "re-verify passes")

    # -- timeout poisoning: no request ids on the wire ---------------- #
    class ScriptedTransport:
        """Answers from a script; a None entry means silence."""
        def __init__(self, script):
            self.script = list(script)
            self.sent = []
        def write_line(self, text):
            self.sent.append(text)
        def read_line(self, timeout):
            if not self.script:
                return None
            item = self.script.pop(0)
            return item
        def close(self):
            pass

    import json as _json
    table_json = _json.dumps([
        {k: v for k, v in {
            "name": n, "type": e["type"], "perm": e["perm"], "value": 0,
            "min": e["min"], "max": e["max"],
            "modbus": e["modbus"] or None, "desc": e.get("desc") or None,
        }.items() if v is not None}
        for n, e in Dev.__schema__.items()], separators=(",", ":"))
    # handshake answers; then a get that never answers; then a STALE
    # answer arriving late, which must never be taken for a reply
    t = ScriptedTransport([table_json, None, '{"value":500}'])
    t.closed = False
    t.close = lambda: setattr(t, "closed", True)
    dev = Dev(t, timeout=0.2)
    expect_raises(TransportError, "first get times out", lambda: dev.interval)
    check(t.closed, "a timeout closes the connection")
    expect_raises(TransportError, "client is closed after a timeout",
                  lambda: setattr(dev, "interval", 600))
    check(t.sent[-1] != "set interval 600 --json",
          "nothing was sent on a closed connection")

    # a normal close() is final too: closed state, zero sends afterwards,
    # and the transport is closed exactly once (a strict one refuses twice)
    t = ScriptedTransport([table_json])
    t.closes = 0
    def strict_close():
        if t.closes:
            raise RuntimeError("closed twice")
        t.closes += 1
    t.close = strict_close
    dev = Dev(t, timeout=0.2)
    dev.close()
    check(dev._dead and "closed" in repr(dev), f"close() marks the client closed: {dev!r}")
    sent_before = len(t.sent)
    expect_raises(TransportError, "read after close() raises", lambda: dev.interval)
    expect_raises(TransportError, "write after close() raises",
                  lambda: setattr(dev, "interval", 600))
    check(len(t.sent) == sent_before, "nothing is sent after close()")
    dev.close()                                   # idempotent
    check(t.closes == 1, "close() twice closes the transport once")
    # and after _fail() the transport is not closed a second time either
    t = ScriptedTransport([table_json, None])
    t.closes = 0
    def strict_close2():
        if t.closes:
            raise RuntimeError("closed twice")
        t.closes += 1
    t.close = strict_close2
    dev = Dev(t, timeout=0.2)
    expect_raises(TransportError, "timeout fails the connection", lambda: dev.interval)
    dev.close()
    check(t.closes == 1, "close() after a failure does not close the transport again")

    # answers of the wrong shape are desync, not data
    t = ScriptedTransport([table_json, '[1,2]'])
    expect_raises(TransportError, "array answer to get is rejected",
                  lambda: Dev(t, timeout=0.2).interval)
    t = ScriptedTransport([table_json, '[]'])
    expect_raises(TransportError, "empty array is not a set success",
                  lambda: setattr(Dev(t, timeout=0.2), "interval", 600))
    t = ScriptedTransport([table_json, '{"value":"x"}'])
    expect_raises(TransportError, "wrong value type is rejected",
                  lambda: Dev(t, timeout=0.2).interval)
    t = ScriptedTransport([table_json, '{"result":"OK"}', '{"value":true}'])
    expect_raises(TransportError, "bool answer to an int register is rejected",
                  lambda: (setattr(Dev(t, timeout=0.2, verify=False), "interval", 600),
                           Dev(t, timeout=0.2, verify=False).interval))

    # a transport that fails mid-exchange is desync too: the write may
    # have gone out in part, so the connection must close
    t = ScriptedTransport([table_json])
    t.closed = False
    t.close = lambda: setattr(t, "closed", True)
    dev = Dev(t, timeout=0.2)
    def boom(_text):
        raise TransportError("wire fell off")
    t.write_line = boom
    expect_raises(TransportError, "transport write error surfaces",
                  lambda: dev.interval)
    check(t.closed and dev._dead, "transport error closes and marks dead")
    t.write_line = lambda text: t.sent.append(text)
    sent_before = len(t.sent)               # the handshake's list is in there
    expect_raises(TransportError, "client stays closed after a transport error",
                  lambda: dev.interval)
    check(len(t.sent) == sent_before, "nothing sent after the transport error")
    t2 = ScriptedTransport([table_json])
    dev = Dev(t2, timeout=0.2)
    def boom_read(_timeout):
        raise OSError("device gone")
    t2.read_line = boom_read
    expect_raises(TransportError, "raw transport exception on read becomes TransportError",
                  lambda: dev.interval)
    check(dev._dead, "read failure marks dead")

    # integer schema bounds compare as integers, exactly
    halfed = table_json.replace('"min":100,"max":60000', '"min":100.5,"max":60000')
    check(halfed != table_json, "fixture has the interval range")
    expect_raises(SchemaDriftError, "fractional device bound on an int register is drift",
                  lambda: Dev(ScriptedTransport([halfed]), timeout=0.2))
    stringy = table_json.replace('"min":100,"max":60000', '"min":"100","max":60000')
    expect_raises(SchemaDriftError, "string device bound is drift, not a crash",
                  lambda: Dev(ScriptedTransport([stringy]), timeout=0.2))
    floaty = table_json.replace('"min":0.5,"max":2.5', '"min":"a","max":2.5')
    if floaty != table_json:
        expect_raises(SchemaDriftError, "non-numeric float bound is drift, not a crash",
                      lambda: Dev(ScriptedTransport([floaty]), timeout=0.2))

    # duplicate names and description drift are drift
    dup = table_json[:-1] + ',' + table_json[1:table_json.index('}') + 1] + ']'
    expect_raises(SchemaDriftError, "duplicate device name is drift",
                  lambda: Dev(ScriptedTransport([dup]), timeout=0.2))
    tweaked = table_json.replace('"desc":"Sampling interval, ms"',
                                 '"desc":"Something else"')
    if tweaked != table_json:
        expect_raises(SchemaDriftError, "description drift is drift",
                      lambda: Dev(ScriptedTransport([tweaked]), timeout=0.2))

    # a failing close() must not hide why the handshake failed
    t = ScriptedTransport(['[{"name":"zzz","type":"U8","perm":"RO"}]'])
    def bad_close():
        raise RuntimeError("close failed")
    t.close = bad_close
    expect_raises(SchemaDriftError, "drift survives a failing close()",
                  lambda: Dev(t, timeout=0.2))

    # nothing too large for binary32 leaks OverflowError out of a float setter
    with Dev(PipeTransport([cli_bin])) as dev2:
        expect_raises(ValueError, "huge int into float is a ValueError",
                      lambda: setattr(dev2, "gain", 10 ** 400))
        expect_raises(ValueError, "finite but beyond binary32 is a ValueError",
                      lambda: setattr(dev2, "gain", 1e100))
        expect_raises(ValueError, "1e39 is a ValueError too",
                      lambda: setattr(dev2, "gain", 1e39))

    # a pipe command that cannot start is a TransportError, not a traceback
    expect_raises(TransportError, "missing pipe command is a TransportError",
                  lambda: PipeTransport(["./no-such-binary-regtable"]))

    # build_client() refuses bad input with an exception the host can catch
    from regtable import GenerationError
    import tempfile
    import os
    fd, bad_yaml = tempfile.mkstemp(suffix=".yaml")
    os.close(fd)
    Path(bad_yaml).write_text(
        "device: demo\nregisters:\n  - name: 9x\n    type: u8\n    perm: ro\n")
    expect_raises(GenerationError, "build_client() raises GenerationError on bad YAML",
                  lambda: build_client(bad_yaml))
    check(issubclass(GenerationError, ValueError), "GenerationError is a ValueError")
    os.unlink(bad_yaml)
    for n in (0, 65536, 70000, True, 2.5):
        expect_raises(GenerationError, f"build_client max_entries={n!r} refused",
                      lambda n=n: build_client(str(ROOT / "tools" / "example.yaml"), n))

    # build_client() gives the same class without writing files
    Mem = build_client(str(ROOT / "tools" / "example.yaml"))
    check(Mem.__name__ == Dev.__name__ and Mem.__schema__ == Dev.__schema__,
          "build_client() matches the generated file")
    check(Mem.registers() == Dev.registers(), "build_client() registers() matches")
    with Mem(PipeTransport([cli_bin])) as dev3:
        check(dev3.interval == 1000, "in-memory client talks to the device")

    # the output directory imports on its own, from the repo root
    import subprocess
    root = Path(gen_dir).resolve().parent
    r = subprocess.run([sys.executable, "-c",
                        f"from {Path(gen_dir).name}.demo_client import DemoDevice; print('ok')"],
                       cwd=root, capture_output=True, text=True)
    check(r.returncode == 0 and "ok" in r.stdout,
          f"from gen.demo_client import works from the repo root: {r.stderr.strip()}")

    # -- negative: same client vs a different device ------------------ #
    try:
        Dev(PipeTransport([example_bin]))
        check(False, "drift against the desktop example must raise")
    except SchemaDriftError as e:
        text = str(e)
        check("counter" in text, f"drift names the extra register: {text!r}")
        check("modbus" in text, f"drift names the modbus mismatch: {text!r}")

    total = cases
    if failures:
        print(f"{failures} of {total} client checks FAILED")
        return 1
    print(f"all {total} client checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
