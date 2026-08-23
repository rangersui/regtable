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
    PipeTransport, RemoteError, SchemaDriftError, TransportError, schema_fingerprint)
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
    # the generated client runs on the runtime copy beside it, and its
    # exceptions are that module's; the package's own classes are for
    # the discover() / build_client() paths below
    rt = sys.modules[Dev.__mro__[1].__module__]
    check(Path(rt.__file__).resolve().parent == Path(gen_dir).resolve(),
          f"the generated client runs on the sibling runtime copy: {rt.__file__}")
    RemoteError, SchemaDriftError, TransportError = rt.RemoteError, rt.SchemaDriftError, rt.TransportError  # noqa: N806
    from regtable import client as rc

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
        expect_raises(ValueError, "a nonzero that underflows binary32 to zero is refused",
                      lambda: setattr(dev, "gain", 1e-50))
        rows = dev.record("interval", "led", duration=0, every=0)
        check(len(rows) == 1 and isinstance(rows[0], tuple) and rows[0][0] == 0.0
              and rows[0][1]["interval"] == 500, f"record() rows are (t, values): {rows}")
        rows = dev.record("interval", duration=0.05, every=0.02)   # samples at 0, 0.02, 0.04
        check(len(rows) == 3, f"record() takes one sample per period, {len(rows)} rows for 0.05 s at 0.02")
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

        ident = dev.identity()
        check(ident.get("device") == "demo" and ident.get("fw") == "test" and "hash" not in ident,
              f"identity strings from the firmware: {ident}")
        check(ident.get("regs") == 8 and isinstance(ident.get("regtable"), str)
              and "built" in ident, f"identity facts: {ident}")
        check(ident.get("schema") == f"{schema_fingerprint(Dev.__schema__):08x}",
              f"the device's fingerprint is the one computed from the schema: {ident.get('schema')}")

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
            "name": n, "type": e["type"], "perm": e["perm"],
            "value": {"BOOL": False, "FLOAT": 0.0}.get(e["type"], 0),
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

    # an identity of the wrong shape closes the connection
    fp = f"{schema_fingerprint(Dev.__schema__):08x}"
    t = ScriptedTransport([table_json, '{"regtable":"0.1.0","regs":"8","schema":"' + fp + '"}'])
    expect_raises(TransportError, "identity: regs must be an int", lambda: Dev(t, timeout=0.2).identity())
    t = ScriptedTransport([table_json, '{"regtable":"0.1.0","regs":8,"schema":"xyz"}'])
    expect_raises(TransportError, "identity: schema must be 8 hex digits", lambda: Dev(t, timeout=0.2).identity())
    t = ScriptedTransport([table_json, '{"device":5,"regtable":"0.1.0","regs":8,"schema":"' + fp + '"}'])
    expect_raises(TransportError, "identity: device must be text", lambda: Dev(t, timeout=0.2).identity())
    t = ScriptedTransport([table_json, '{"regtable":"0.1.0","regs":7,"schema":"' + fp + '"}'])
    expect_raises(TransportError, "identity: a count other than the table's is refused",
                  lambda: Dev(t, timeout=0.2).identity())
    t = ScriptedTransport([table_json, '{"regtable":"0.1.0","regs":-1,"schema":"' + fp + '"}'])
    expect_raises(TransportError, "identity: a negative count is refused",
                  lambda: Dev(t, timeout=0.2).identity())
    t = ScriptedTransport([table_json, '{"regtable":"0.1.0","regs":8,"schema":"deadbeef"}'])
    expect_raises(TransportError, "identity: a fingerprint other than the table's is refused",
                  lambda: Dev(t, timeout=0.2).identity())
    t = ScriptedTransport([table_json, '{"built":"x","regtable":"0.1.0","regs":8,"schema":"' + fp + '"}'])
    check(Dev(t, timeout=0.2).identity()["schema"] == fp, "identity: the minimal answer passes")
    # the fingerprint follows the core's definition: every field moves it
    base = {"a": {"type": "U8", "perm": "RO", "min": None, "max": None, "modbus": 0, "desc": ""}}
    def fpo(**kw):
        return schema_fingerprint({"a": {**base["a"], **kw}})
    check(len({fpo(), fpo(perm="RW"), fpo(type="U16"), fpo(min=0, max=5), fpo(modbus=3), fpo(desc="x"),
               schema_fingerprint({"b": base["a"]})}) == 7, "schema_fingerprint: name/type/perm/range/modbus/desc all count")
    check(fpo(type="FLOAT", min=0.5, max=2.5) != fpo(type="FLOAT", min=0.5, max=2.0)
          and fpo(type="I8", min=-5, max=5) != fpo(type="I8", min=-4, max=5)
          and fpo() == fpo(min=0, max=0) and schema_fingerprint({}) == 2166136261,
          "schema_fingerprint: float and signed bounds by their bits, unranged equals 0..0, empty is the FNV offset")

    # a single get is held to the declared domain, like the list is
    t = ScriptedTransport([table_json, '{"value":300}'])
    t.closed = False
    t.close = lambda: setattr(t, "closed", True)
    dev = Dev(t, timeout=0.2)
    expect_raises(TransportError, "get: a U8 value outside its domain closes", lambda: dev.small)
    check(t.closed, "get: domain failure closes the connection")
    t = ScriptedTransport([table_json, '{"value":1}'])
    g = Dev(t, timeout=0.2).gain
    check(g == 1.0 and isinstance(g, float), "get: a float register's 1 arrives as 1.0")
    t = ScriptedTransport([table_json, '{"value":1e-45}'])
    check(Dev(t, timeout=0.2).gain == rt.f32(1e-45), "get: a binary32 subnormal arrives as binary32")
    t = ScriptedTransport([table_json, '{"value":2.5000000001}'])
    check(Dev(t, timeout=0.2).gain == 2.5, "get: a float arrives rounded to binary32")
    near = table_json.replace('"type":"FLOAT","perm":"RO","value":0.0',
                              '"type":"FLOAT","perm":"RO","value":2.5000000001', 1)
    check(Dev(ScriptedTransport([table_json, near]), timeout=0.2).snapshot()["temp"] == 2.5,
          "snapshot: floats are binary32")
    # the table's floats arrive as float, and a subnormal in the table is a value
    floaty1 = table_json.replace('"type":"FLOAT","perm":"RO","value":0.0',
                                 '"type":"FLOAT","perm":"RO","value":1', 1)
    snap = Dev(ScriptedTransport([table_json, floaty1]), timeout=0.2).snapshot()
    check(snap["temp"] == 1.0 and isinstance(snap["temp"], float), "snapshot: FLOAT 1 is 1.0")
    sub = table_json.replace('"type":"FLOAT","perm":"RO","value":0.0',
                             '"type":"FLOAT","perm":"RO","value":1e-45', 1)
    Dev(ScriptedTransport([sub]), timeout=0.2)
    check(True, "list: a binary32 subnormal is accepted")
    # the wire text is the number, not a subclass's __str__
    class Weird(int):
        def __str__(self):
            return "9999"
    t = ScriptedTransport([table_json, '{"result":"OK"}'])
    dev = Dev(t, timeout=0.2)
    dev.interval = Weird(500)
    check(t.sent[-1] == "set interval 500 --json", f"int subclass is sent canonically: {t.sent[-1]}")
    class Sly(int):                     # passes the range as 500, answers 99999 when asked
        def __int__(self):
            return 99999
        def __index__(self):
            return 99999
        def __str__(self):
            return "99999"
    t = ScriptedTransport([table_json, '{"result":"OK"}'])
    dev = Dev(t, timeout=0.2)
    dev.interval = Sly(500)
    check(t.sent[-1] == "set interval 500 --json", f"int subclass overriding __index__: {t.sent[-1]}")
    with rc.RegtableClient.discover(ScriptedTransport([table_json, '{"result":"OK"}']), timeout=0.2) as d2:
        d2._t.sent.clear()
        d2["interval"] = Sly(500)
        check(d2._t.sent[-1] == "set interval 500 --json", f"discover: int subclass canonical: {d2._t.sent[-1]}")
    class Wide(float):
        def __float__(self):
            return 9.0
    t = ScriptedTransport([table_json, '{"result":"OK"}'])
    dev = Dev(t, timeout=0.2)
    dev.gain = Wide(1.5)
    check(t.sent[-1] == "set gain 1.5 --json", f"float subclass overriding __float__: {t.sent[-1]}")

    # the list itself is a protocol boundary: shape, type, perm, value
    from regtable.client import RegtableClient as _RC
    def closes_on(script, label):
        tt = ScriptedTransport(script)
        tt.closed = False
        tt.close = lambda: setattr(tt, "closed", True)
        expect_raises(rc.TransportError, label, lambda: _RC.discover(tt, timeout=0.2))
        check(tt.closed, f"{label}: connection closed")
    closes_on([table_json.replace('"min":100,"max":60000', '"min":"oops","max":60000')],
              "discover: string bound on an int register")
    closes_on([table_json.replace('"min":100,"max":60000', '"min":100')],
              "discover: min without max")
    closes_on([table_json.replace('"min":100,"max":60000', '"min":200,"max":100')],
              "discover: min above max")
    closes_on([table_json.replace('"perm":"RO"', '"perm":"XX"', 1)],
              "discover: unknown perm")
    closes_on([table_json.replace('"type":"U16"', '"type":"U12"', 1)],
              "discover: unknown type")
    closes_on([table_json.replace('"value":0,', '', 1)],
              "discover: missing value")
    closes_on([table_json.replace('"type":"BOOL","perm":"RW","value":false',
                                  '"type":"BOOL","perm":"RW","value":0')],
              "discover: value of the wrong type")
    closes_on([table_json.replace('"desc":"Sampling interval, ms"', '"desc":5')],
              "discover: non-text desc")
    # the domains: what the type can hold, not just the Python shape
    closes_on([table_json.replace('"type":"U8","perm":"RW","value":0',
                                  '"type":"U8","perm":"RW","value":0,"min":-1,"max":300')],
              "discover: U8 bounds outside 0..255")
    closes_on([table_json.replace('"min":0.5,"max":2.5', '"min":1e39,"max":1e40')],
              "discover: FLOAT bounds beyond binary32")
    closes_on([table_json.replace('"min":0.5,"max":2.5', '"min":NaN,"max":2.5')],
              "discover: NaN bound")
    closes_on([table_json.replace('"min":0.5,"max":2.5', '"min":1e-50,"max":2.5')],
              "discover: FLOAT bound that underflows binary32")
    closes_on([table_json.replace('"min":0.5,"max":2.5', '"min":0.5,"max":' + "1" * 401)],
              "discover: 401-digit FLOAT bound")
    closes_on([table_json.replace('"type":"FLOAT","perm":"RO","value":0.0',
                                  '"type":"FLOAT","perm":"RO","value":1e-50', 1)],
              "list: FLOAT value that underflows binary32")
    closes_on([table_json.replace('"type":"FLOAT","perm":"RO","value":0.0',
                                  '"type":"FLOAT","perm":"RO","value":' + "1" * 401, 1)],
              "list: 401-digit FLOAT value")
    closes_on([table_json.replace('"modbus":3', '"modbus":65536')],
              "discover: modbus address beyond 16 bits")
    closes_on([table_json.replace('"modbus":3', '"modbus":-1')],
              "discover: negative modbus address")
    closes_on([table_json.replace('"type":"U8","perm":"RW","value":0',
                                  '"type":"U8","perm":"RW","value":300')],
              "list: U8 value outside its domain")
    closes_on([table_json.replace('"type":"FLOAT","perm":"RO","value":0.0',
                                  '"type":"FLOAT","perm":"RO","value":1e39', 1)],
              "list: FLOAT value beyond binary32")
    closes_on([table_json.replace('"type":"FLOAT","perm":"RO","value":0.0',
                                  '"type":"FLOAT","perm":"RO","value":NaN', 1)],
              "list: NaN value")
    check('"min":0.5,"max":2.5' in table_json and '"modbus":3' in table_json
          and '"type":"U8","perm":"RW","value":0' in table_json
          and '"type":"FLOAT","perm":"RO","value":0.0' in table_json,
          "domain fixtures found their anchors")
    # one domain table for the generator and the runtime
    from regtable import gen as _gen
    from regtable.client import DOMAIN as _DOM
    check(all(_gen.TYPES[k][3] == _DOM[k.upper()] for k in ("u8", "u16", "u32", "i8", "i16", "i32")),
          "generator and runtime share the integer domains")
    # a typed client trips over the same malformed list at the handshake
    tt = ScriptedTransport([table_json.replace('"value":0,', '', 1)])
    expect_raises(TransportError, "typed handshake rejects a list without values",
                  lambda: Dev(tt, timeout=0.2))
    # and a good list still discovers
    good = _RC.discover(ScriptedTransport([table_json]), timeout=0.2)
    check(good.__schema__ == Dev.__schema__, "scripted discover yields the schema")

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
    t = ScriptedTransport(['[{"name":"zzz","type":"U8","perm":"RO","value":0}]'])
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
    expect_raises(rc.TransportError, "missing pipe command is a TransportError",
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

    # discover(): the device's own table, access by name, same checks
    from regtable.client import RegtableClient
    with RegtableClient.discover(PipeTransport([cli_bin])) as disc:
        check(type(disc).__name__ == "DiscoveredDevice", "discovered class name")
        check(disc.__schema__ == Dev.__schema__,
              f"discovered schema equals the generated one: {disc.__schema__}")
        check(disc["interval"] == 1000, "item read on a discovered device")
        disc["interval"] = 700
        check(disc["interval"] == 700, "item write on a discovered device")
        expect_raises(KeyError, "unknown name is a KeyError", lambda: disc["nope"])
        expect_raises(AttributeError, "RO item write refused locally",
                      lambda: disc.__setitem__("temp", 1.0))
        expect_raises(TypeError, "item write type check", lambda: disc.__setitem__("interval", 1.5))
        expect_raises(ValueError, "item write range check", lambda: disc.__setitem__("interval", 50))
        expect_raises(ValueError, "item write domain check (no YAML range)",
                      lambda: disc.__setitem__("small", 256))
        expect_raises(ValueError, "item write float binary32 fit",
                      lambda: disc.__setitem__("gain", 1e39))
        expect_raises(TypeError, "bool item rejects int", lambda: disc.__setitem__("led", 1))
        disc["gain"] = 2.5000000001
        check(disc["gain"] == 2.5, "float item rounds to binary32 before the range check")
        check("led" in disc and "nope" not in disc, "__contains__ on the schema")
        expect_raises(AttributeError, "no typed attributes on a discovered device",
                      lambda: setattr(disc, "interval", 5))
        disc.verify()                     # the device agrees with itself
        check(True, "discovered client verifies against its own device")
        check(disc.registers() == Dev.registers(), "registers() on a discovered device")
    # typed clients take item access too
    with Dev(PipeTransport([cli_bin])) as dev4:
        dev4["interval"] = 650
        check(dev4.interval == 650 and dev4["interval"] == 650, "item access on a typed client")
        expect_raises(ValueError, "item write on a typed client keeps the checks",
                      lambda: dev4.__setitem__("interval", 50))

    # the command line: --pipe passes everything after it through
    from regtable import cli as _cli
    p = _cli.build_parser()
    a = p.parse_args(["watch", "temp", "--count", "1", "--pipe", "x", "-u", "--child-option"])
    check(a.pipe == ["x", "-u", "--child-option"] and a.names == ["temp"] and a.count == 1,
          f"--pipe keeps child options: {a.pipe}")
    a = p.parse_args(["connect", "--pipe"])
    check(a.pipe == [], "empty --pipe parses to an empty command")
    check(_cli.main(["connect", "--pipe"]) == 2, "empty --pipe is refused with exit 2")
    check(_cli.main(["watch", "temp", "--every", "0", "--count", "1",
                     "--pipe", cli_bin, "-u", "--child-option"]) == 0,
          "watch over a pipe with child options runs")
    check(_cli.main(["fetch", "--pipe", cli_bin]) == 0, "fetch over a pipe runs")
    txt = _cli.fetch_text({"device": "demo", "fw": "1.0", "chip": "STM32L053", "regtable": "0.1.0", "regs": 2, "schema": "0a0b0c0d"},
                          {"a": {"perm": "RW"}, "b": {"perm": "RO"}}, ["USART2", "TIM3", "GPIOA"])
    check("regtable" in txt and "USART2 ----|" in txt and "|---- TIM3" in txt and "GPIOA ----|" in txt
          and "demo @ STM32L053" in txt and " fw        1.0" in txt
          and "regs      2 (1 RW, 1 RO)" in txt and all(ord(c) < 128 for c in txt),
          f"fetch_text draws pins and facts in ASCII:\n{txt}")
    # the device's strings are rendered printable: no injected lines, no
    # terminal control sequences, no non-ASCII reaches the terminal
    hostile = {"device": "board\nFAKE", "fw": "1.0\x1b[31mRED", "chip": "\u00b5C", "hash": "a\tb",
               "regtable": "0.1.0", "regs": 0, "schema": "0a0b0c0d"}
    txt = _cli.fetch_text(hostile, {}, [])
    check(all(32 <= ord(c) < 127 or c == "\n" for c in txt) and "board\\x0aFAKE @ \\u00b5C" in txt
          and "1.0\\x1b[31mRED (a\\x09b)" in txt and "\x1b" not in txt and "FAKE" not in txt.split("\n")[0],
          f"fetch_text renders hostile identity strings as printable ASCII:\n{txt}")
    # the pins come from the generated class's record of its SVD picks,
    # never from descriptions; a discovered device has none
    Sil = build_client(ROOT / "python" / "tests" / "svd_demo.yaml")
    check(_cli.silicon_pins(Sil) == ["USART2", "TIM3", "GPIOA", "ADC1"],
          f"silicon_pins: peripherals in table order, each once: {_cli.silicon_pins(Sil)}")
    check(Sil.__silicon__["timer_count"] == "TIM3.CNT" and Sil.__silicon__["adc1_ch_dr"] == "ADC1.CH.DR"
          and "temp" not in Sil.__silicon__ and "led" not in Sil.__silicon__,
          f"__silicon__ maps silicon registers to their SVD path: {Sil.__silicon__}")
    check(_cli.silicon_pins(Dev) == [] and _cli.silicon_pins(_cli.RegtableClient) == [],
          "no SVD picks, no pins")
    check(_cli.main(["watch", "--yaml", str(ROOT / "tools" / "example.yaml"),
                     "--every", "0", "--count", "1", "--pipe", cli_bin]) == 0,
          "watch with --yaml runs")
    bad_yaml = str(ROOT / "tools" / "no-such.yaml")
    check(_cli.main(["watch", "--yaml", bad_yaml, "--pipe", "./no-such-binary"]) == 2,
          "a bad --yaml is refused before the pipe is started")

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
