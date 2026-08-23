"""Host-side runtime for generated regtable clients.

`regtable gen` emits one class per YAML description, a typed mirror
of the device's RegEntry[]; this module is what that class inherits.
The mirror is a contract, the device is the truth: connect() fetches
the device's own `list --json` and compares it with the schema baked
into the class. Any difference raises SchemaDriftError before the
first read or write.

The client holds no values. Every attribute read is a wire read;
snapshot() is the one explicit bulk read. A cached value would be
drift by another name.

The wire carries no request ids, so a timeout, or an answer of the
wrong shape, means request and response can no longer be paired. The
client then closes the connection and every further call raises;
reconnecting re-verifies the table. There is no resync: without
correlation, no drain window can prove the line is clean.

Transports are duck-typed: anything with write_line(str),
read_line(timeout) -> str | None, and close(). SerialTransport wraps
pyserial; PipeTransport drives a process over stdin/stdout, which is
how the test suite talks to the desktop CLI binaries without hardware.

Two ways to a client. A generated class (regtable gen, build_client)
is the YAML's contract: typed attributes, verified against the device
at connect. discover() takes the device's own table instead: no YAML,
nothing to drift, access by name (dev["led"]) with the same local
checks; that is what generic hosts use, a panel or a REPL with
nothing but a port.
"""

import json
import math
import os
import queue
import struct
import subprocess
import threading
import time


# -- errors ----------------------------------------------------------- #

class RegtableError(Exception):
    """Base of everything this module raises."""


class TransportError(RegtableError):
    """No usable answer from the device within the timeout."""


class RemoteError(RegtableError):
    """The device refused: its own ERR string, as reported over the wire."""

    def __init__(self, name, message):
        super().__init__(f"{name}: {message}")
        self.name = name
        self.message = message


class SchemaDriftError(RegtableError):
    """The device's table differs from the generated client's schema."""

    def __init__(self, client_name, diff):
        self.diff = list(diff)
        super().__init__(
            f"device table differs from {client_name}:\n  " + "\n  ".join(self.diff))


# -- transports -------------------------------------------------------- #

class SerialTransport:
    """A serial port speaking the regtable CLI. Opening the port resets
    an Arduino (DTR); boot_delay covers the bootloader before the first
    command is sent."""

    def __init__(self, port, baud=115200, boot_delay=2.0):
        import serial                      # pyserial, only when used
        # a small fixed read timeout, set once: reassigning the
        # timeout property on an open port reconfigures it, which can
        # cut a line mid-byte and pulse the reset on an Arduino
        try:
            self._ser = serial.Serial(port, baud, timeout=0.05)
        except Exception as e:
            raise TransportError(f"cannot open {port}: {e}") from e
        self._buf = b""
        try:
            time.sleep(boot_delay)
            self._ser.reset_input_buffer()   # nothing is in flight yet
        except Exception as e:
            self.close()
            raise TransportError(f"{port} failed after open: {e}") from e

    def write_line(self, text):
        try:
            self._ser.write((text + "\n").encode("ascii"))
        except Exception as e:
            raise TransportError(f"write failed: {e}") from e

    def read_line(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            nl = self._buf.find(b"\n")
            if nl >= 0:
                line = self._buf[:nl]
                self._buf = self._buf[nl + 1:]
                return line.decode("ascii", "replace").rstrip("\r")
            if time.monotonic() >= deadline:
                return None
            try:
                self._buf += self._ser.read(256)
            except Exception as e:
                raise TransportError(f"read failed: {e}") from e

    def close(self):
        try:
            self._ser.close()
        except Exception:
            pass


_EOF = object()


class PipeTransport:
    """A child process speaking the regtable CLI on stdin/stdout: the
    desktop example, or any binary built over a generated table."""

    def __init__(self, argv):
        argv = list(argv)
        if "/" in argv[0] or os.sep in argv[0]:
            # a relative path with forward slashes does not start on
            # Windows (CreateProcess); an absolute native one does everywhere
            argv[0] = os.path.abspath(argv[0])
        try:
            self._proc = subprocess.Popen(
                argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, text=True, bufsize=1)
        except OSError as e:
            raise TransportError(f"cannot start {argv[0]!r}: {e}") from e
        self._lines = queue.Queue()
        self._eof = False
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self._proc.stdout:
            self._lines.put(line.rstrip("\r\n"))
        self._lines.put(_EOF)          # wake a waiting reader at once

    def write_line(self, text):
        if self._eof:
            raise TransportError("process ended")
        try:
            self._proc.stdin.write(text + "\n")
            self._proc.stdin.flush()
        except (BrokenPipeError, OSError, ValueError) as e:
            raise TransportError(f"process ended: {e}")

    def read_line(self, timeout):
        if self._eof:
            return None
        try:
            item = self._lines.get(timeout=timeout)
        except queue.Empty:
            return None
        if item is _EOF:
            self._eof = True
            return None
        return item

    def close(self):
        p = self._proc
        for f in (p.stdin,):
            try: f.close()
            except Exception: pass
        try:
            p.wait(timeout=2)
        except Exception:
            p.kill()
            try: p.wait(timeout=2)
            except Exception: pass
        try: p.stdout.close()
        except Exception: pass


# -- client ------------------------------------------------------------ #

PY_TYPES = {
    "U8": int, "U16": int, "U32": int, "I8": int, "I16": int, "I32": int,
    "FLOAT": float, "BOOL": bool,
}


def f32(x):
    """x rounded to binary32, the only float a register can hold. The
    generated C table, the client's local range check, and the schema
    comparison all go through this, so they describe one boundary.
    Raises ValueError for anything binary32 cannot hold."""
    try:
        return struct.unpack("<f", struct.pack("<f", float(x)))[0]
    except (OverflowError, TypeError, ValueError) as e:
        raise ValueError(f"{x!r} does not fit a float register") from e


def _is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def _is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


# the register domains: one table, used by the generator (YAML checks),
# the local write checks, and the checks on what a device describes
DOMAIN = {
    "U8": (0, 255), "U16": (0, 65535), "U32": (0, 4294967295),
    "I8": (-128, 127), "I16": (-32768, 32767), "I32": (-2147483648, 2147483647),
}
FLT_MAX = 3.4028234663852886e38          # largest binary32
FLT_MIN = 1.1754943508222875e-38         # smallest normal binary32
MODBUS_ADDR_MAX = 0xFFFF                 # word addresses are 16-bit; 0 = unmapped


def in_domain(value, t):
    """Is value a Python value of register type t that the register can
    hold: a bool; a non-bool int inside the type's range; for FLOAT,
    a finite number a binary32 can hold, subnormals included (a device
    computes them), and nothing a binary32 cannot: beyond FLT_MAX, or
    nonzero yet rounding to zero. Integers of any size compare without
    float conversion, so a 400-digit int is refused, not raised on.
    (YAML constants have the stricter rule of the generator: a C
    literal must spell them.)"""
    if t == "BOOL":
        return isinstance(value, bool)
    if t == "FLOAT":
        if not _is_num(value):
            return False
        if isinstance(value, float) and not math.isfinite(value):
            return False
        if abs(value) > FLT_MAX:
            return False
        return value == 0 or f32(value) != 0
    return _is_int(value) and DOMAIN[t][0] <= value <= DOMAIN[t][1]


def check_value(name, entry, value):
    """The local write check for one schema entry: type, finiteness,
    binary32 fit, range (the YAML's, else the type's domain). Returns
    the value as it will go on the wire. Generated setters carry the
    same rules inline; this is the one place for clients whose schema
    arrived at run time."""
    t = entry["type"]
    lo, hi = entry.get("min"), entry.get("max")
    if t == "BOOL":
        if not isinstance(value, bool):
            raise TypeError(f"{name} expects bool, got {type(value).__name__}")
        return value
    if t == "FLOAT":
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise TypeError(f"{name} expects float, got {type(value).__name__}")
        try:                               # the base type's own value, not a subclass's story
            value = float(int.__index__(value)) if isinstance(value, int) else float.__float__(value)
        except OverflowError:
            raise ValueError(f"{name}: value too large for a float")
        if not math.isfinite(value):
            raise ValueError(f"{name}: {value!r} is not a finite number")
        try:
            rounded = f32(value)
        except ValueError:
            raise ValueError(f"{name}: {value!r} does not fit a float register")
        if value != 0 and rounded == 0:
            raise ValueError(f"{name}: {value!r} underflows a float register to zero")
        value = rounded
        if lo is not None and not (f32(lo) <= value <= f32(hi)):
            raise ValueError(f"{name}: {value} outside {f32(lo)!r}..{f32(hi)!r}")
        return value
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{name} expects int, got {type(value).__name__}")
    value = int.__index__(value)           # the base type's own value, not a subclass's story
    what = ""
    if lo is None:
        lo, hi = DOMAIN[t]
        what = f" (the {t} domain)"
    if not (lo <= value <= hi):
        raise ValueError(f"{name}: {value} outside {lo}..{hi}{what}")
    return value


def _find_json(line):
    """The JSON object or array on a line, tolerating a prompt or noise
    before it; None when the line carries none (an echo, a banner)."""
    starts = [i for i in (line.find("{"), line.find("[")) if i >= 0]
    if not starts:
        return None
    try:
        return json.loads(line[min(starts):])
    except json.JSONDecodeError:
        return None


class RegtableClient:
    """Base class for device clients. Generated subclasses set
    __schema__ and define one typed property per register;
    discover() builds a subclass from the device's own table, with
    access by name only. dev[name] and dev[name] = value work on
    both, with the same local checks as the typed setters."""

    __slots__ = ("_t", "_timeout", "_dead")
    __schema__ = {}           # name -> {"type", "perm", "min", "max", "modbus"}

    def __init__(self, transport, *, timeout=3.0, verify=True):
        object.__setattr__(self, "_t", transport)
        object.__setattr__(self, "_timeout", timeout)
        object.__setattr__(self, "_dead", False)
        if verify:
            try:
                self.verify()
            except Exception:
                try:                   # a refused handshake leaves no open port behind
                    self.close()
                except Exception:
                    pass               # and close() cannot mask why it was refused
                raise

    # a typo on the left of '=' is an error, never a new attribute
    def __setattr__(self, name, value):
        if name not in self.__schema__ and name not in type(self).__slots__:
            raise AttributeError(
                f"{type(self).__name__} has no register '{name}'")
        object.__setattr__(self, name, value)

    # -- wire -------------------------------------------------------- #

    def _fail(self, why, cause=None):
        """A timeout, an answer of the wrong shape, or a transport that
        failed mid-exchange (a write may have gone out in part) all
        mean the request and response streams can no longer be paired:
        there are no request ids on this wire. The only safe move is
        to close the connection; the caller reconnects (which
        re-verifies)."""
        object.__setattr__(self, "_dead", True)
        try:
            self._t.close()
        except Exception:
            pass
        raise TransportError(why + "; connection closed, reconnect") from cause

    def _cmd_json(self, cmd, max_lines=8):
        """Send one --json command, return its parsed JSON answer."""
        if self._dead:
            raise TransportError("connection closed; reconnect")
        try:
            self._t.write_line(cmd)
        except Exception as e:         # transports are duck-typed: any failure counts
            self._fail(f"transport failed sending {cmd!r}: {e}", e)
        deadline = time.monotonic() + self._timeout
        for _ in range(max_lines):
            left = deadline - time.monotonic()
            if left <= 0:
                break
            try:
                line = self._t.read_line(left)
            except Exception as e:
                self._fail(f"transport failed reading the answer to {cmd!r}: {e}", e)
            if line is None:
                break
            obj = _find_json(line)
            if obj is not None:
                return obj
        self._fail(f"no JSON answer to {cmd!r} within {self._timeout}s")

    def _expect_dict(self, obj, cmd):
        if not isinstance(obj, dict):
            self._fail(f"answer to {cmd!r} is not an object: {obj!r}")
        if "error" in obj:
            if not isinstance(obj["error"], str):
                self._fail(f"malformed error answer to {cmd!r}: {obj!r}")
            return obj
        return obj

    def _get(self, name, pytype):
        cmd = f"get {name} --json"
        obj = self._expect_dict(self._cmd_json(cmd), cmd)
        if "error" in obj:
            raise RemoteError(name, obj["error"])
        if "value" not in obj:
            self._fail(f"answer to {cmd!r} has no value: {obj!r}")
        v = obj["value"]
        t = self.__schema__[name]["type"]
        if not in_domain(v, t):
            self._fail(f"{name}: value {v!r} does not fit a {t}")
        return f32(v) if t == "FLOAT" else v     # the binary32 the device holds

    def _set(self, name, value):
        if isinstance(value, bool):
            text = "true" if value else "false"
        elif isinstance(value, float):
            text = repr(float.__float__(value))   # shortest round-trip; the device re-rounds to binary32
        else:
            text = str(int.__index__(value))      # the number itself; no subclass method is consulted
        cmd = f"set {name} {text} --json"
        obj = self._expect_dict(self._cmd_json(cmd), cmd)
        if "error" in obj:
            raise RemoteError(name, obj["error"])
        if obj.get("result") != "OK":
            self._fail(f"answer to {cmd!r} is not a set result: {obj!r}")

    # -- contract ---------------------------------------------------- #

    def _list(self):
        """list --json, checked for shape: an array of objects with a
        string name, a known type, perm RO or RW, and a value of that
        type. Anything else is a desynchronized or foreign wire, and
        closes the connection. Bounds and the rest are the contract's
        business: verify() for a generated client, discover() for one
        built from this very answer."""
        table = self._cmd_json("list --json")
        if not isinstance(table, list):
            self._fail("list --json did not return an array")
        for e in table:
            if not (isinstance(e, dict) and isinstance(e.get("name"), str)
                    and isinstance(e.get("type"), str)
                    and isinstance(e.get("perm"), str)):
                self._fail(f"list --json entry is malformed: {e!r}")
            if e["type"] not in PY_TYPES:
                self._fail(f"{e['name']}: unknown type {e['type']!r}")
            if e["perm"] not in ("RO", "RW"):
                self._fail(f"{e['name']}: unknown perm {e['perm']!r}")
            if "value" not in e or not in_domain(e["value"], e["type"]):
                self._fail(f"{e['name']}: value {e.get('value')!r} does not fit a {e['type']}")
            if e["type"] == "FLOAT":
                e["value"] = f32(e["value"])     # the binary32 the device holds
        return table

    def verify(self):
        """Compare the device's self-description with the generated
        schema; raise SchemaDriftError with every difference."""
        table = self._list()
        device = {}
        diff = []
        for e in table:
            if e["name"] in device:
                diff.append(f"duplicate name on device: {e['name']}")
            device[e["name"]] = e
        for n in sorted(set(self.__schema__) - set(device)):
            diff.append(f"missing on device: {n}")
        for n in sorted(set(device) - set(self.__schema__)):
            e = device[n]
            diff.append(f"extra on device: {n} ({e.get('type')} {e.get('perm')})")
        for n in sorted(set(self.__schema__) & set(device)):
            want, got = self.__schema__[n], device[n]
            for key in ("type", "perm"):
                if want[key] != got.get(key):
                    diff.append(f"{n}: {key} {want[key]} (client) vs {got.get(key)} (device)")
            for key in ("min", "max"):
                a, b = want.get(key), got.get(key)
                if a is None or b is None:
                    same = a is None and b is None
                elif want["type"] == "FLOAT":
                    # both are binary32 on the device; anything that is
                    # not a number, or does not fit, is a difference
                    try:
                        same = _is_num(b) and f32(a) == f32(b)
                    except ValueError:
                        same = False
                else:
                    # integers compare exactly, and only as integers:
                    # a device bound of 1.5 is not "1"
                    same = _is_int(a) and _is_int(b) and a == b
                if not same:
                    diff.append(f"{n}: {key} {a} (client) vs {b} (device)")
            a, b = want.get("modbus", 0) or 0, got.get("modbus", 0) or 0
            if a != b:
                diff.append(f"{n}: modbus {a} (client) vs {b} (device)")
            a, b = want.get("desc") or "", got.get("desc") or ""
            if a != b:
                diff.append(f"{n}: desc {a!r} (client) vs {b!r} (device)")
        if diff:
            raise SchemaDriftError(type(self).__name__, diff)

    # -- access by name ------------------------------------------------ #

    def __getitem__(self, name):
        e = self.__schema__.get(name)
        if e is None:
            raise KeyError(f"{type(self).__name__} has no register {name!r}")
        return self._get(name, PY_TYPES[e["type"]])

    def __setitem__(self, name, value):
        e = self.__schema__.get(name)
        if e is None:
            raise KeyError(f"{type(self).__name__} has no register {name!r}")
        if e["perm"] != "RW":
            raise AttributeError(f"{name} is read-only")
        self._set(name, check_value(name, e, value))

    def __contains__(self, name):
        return name in self.__schema__

    @classmethod
    def discover(cls, transport, *, timeout=3.0):
        """A client built from the device's own `list --json`: the
        schema is whatever the device reports, so there is no YAML and
        nothing to verify against; registers are reached by name
        (dev["led"]), with the same local checks as generated setters.
        The class is named DiscoveredDevice."""
        probe = RegtableClient(transport, timeout=timeout, verify=False)
        schema = {}
        for e in probe._list():                 # shape, type, perm, value checked there
            n, t = e["name"], e["type"]
            if n in schema:
                probe._fail(f"device lists {n!r} twice")
            mn, mx = e.get("min"), e.get("max")
            if (mn is None) != (mx is None):
                probe._fail(f"{n}: min and max come together, got {e!r}")
            if mn is not None:
                if t == "BOOL" or not (in_domain(mn, t) and in_domain(mx, t)) or mn > mx:
                    probe._fail(f"{n}: bounds {mn!r}..{mx!r} do not fit a {t}")
            mb = e.get("modbus", 0)
            if not _is_int(mb) or not 0 <= mb <= MODBUS_ADDR_MAX:
                probe._fail(f"{n}: modbus {mb!r} is not a word address (0..{MODBUS_ADDR_MAX})")
            desc = e.get("desc", "")
            if not isinstance(desc, str):
                probe._fail(f"{n}: desc {desc!r} is not text")
            if t == "FLOAT" and mn is not None:
                mn, mx = f32(mn), f32(mx)        # bounds are binary32 on the device too
            schema[n] = {"type": t, "perm": e["perm"], "min": mn, "max": mx,
                         "modbus": mb, "desc": desc}
        sub = type("DiscoveredDevice", (RegtableClient,),
                   {"__slots__": (), "__schema__": schema,
                    "__doc__": f"discovered: {len(schema)} registers."})
        return sub(transport, timeout=timeout, verify=False)

    # -- what is there ------------------------------------------------ #

    @classmethod
    def registers(cls):
        """The table this client was generated from: one dict per
        register (name, type, perm, min, max, modbus, desc), in table
        order. No wire traffic; usable on the class before connecting."""
        return [{"name": n, **e} for n, e in cls.__schema__.items()]

    def __repr__(self):
        state = "closed" if self._dead else "open"
        return (f"<{type(self).__name__} {state}: "
                + ", ".join(self.__schema__) + ">")

    # -- bulk and time ------------------------------------------------ #

    def snapshot(self):
        """One list --json: every register's current value, by name."""
        return {e["name"]: e["value"] for e in self._list()}

    def watch(self, *names, every=1.0, count=None):
        """Poll; yield (name, value) for each change (and once at start)."""
        names = names or tuple(self.__schema__)
        last = {}
        rounds = 0
        while count is None or rounds < count:
            snap = self.snapshot()
            for n in names:
                v = snap.get(n)
                if n not in last or last[n] != v:
                    last[n] = v
                    yield n, v
            rounds += 1
            if count is None or rounds < count:
                time.sleep(every)

    def record(self, *names, duration, every=1.0):
        """Sample names every `every` seconds for `duration` seconds;
        returns rows of (seconds, {name: value}), the time kept apart
        from the values so no register name can stand in its place."""
        names = names or tuple(self.__schema__)
        rows, t0 = [], time.monotonic()
        while True:
            t = time.monotonic() - t0
            snap = self.snapshot()
            rows.append((round(t, 3), {n: snap.get(n) for n in names}))
            if t + every > duration:
                return rows
            time.sleep(every)

    def close(self):
        """Close for good: the transport is released and every later
        call raises TransportError without sending anything. The
        transport is closed once: after _fail(), or a first close(),
        this is a no-op."""
        if self._dead:
            return
        object.__setattr__(self, "_dead", True)
        self._t.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
