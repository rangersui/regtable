# regtable (Python)

Host-side tools for [regtable](https://github.com/rangersui/regtable), the
C99 register-table library for MCUs: one `RegEntry[]` table in the firmware,
reachable over a serial CLI, Modbus RTU/TCP, MQTT, a browser, and Python.

```
pip install regtable
```

## Command line

```
regtable gen device.yaml -o gen            # registers.c/.h/.md + <device>_client.py
regtable connect device.yaml -p COM6       # verify the board, then a REPL with `dev`
regtable watch device.yaml -p COM6 a0 led  # print changes; --json for scripts
regtable serve                             # the Web Serial panel on localhost
```

`connect` and `watch` build the typed client from the YAML in memory, open
the port, fetch the board's own `list --json`, and compare it field by field
with the YAML. A board whose table differs ends the command with the
differences listed, before the first read.

## Typed client

```python
from regtable import build_client

UnoDevice = build_client("uno.yaml")       # or import the generated <device>_client.py
with UnoDevice.serial("COM6") as dev:
    dev.led = True
    dev.pwm9 = 300            # ValueError: 0..255, refused before the wire
    dev.a0 = 1                # AttributeError: read-only, there is no setter
    dev.registers()           # the table the client was generated from
    dev.snapshot()            # every value, one round trip
```

The client keeps no values; every attribute read is a wire read. The wire
carries no request ids, so a timeout or a malformed answer closes the
connection rather than pair a late reply with the wrong command;
reconnecting re-verifies. Transports are duck-typed: `SerialTransport`
(pyserial), `PipeTransport` (a CLI process over stdin/stdout), or anything
with `write_line`, `read_line`, `close`.

The firmware side, the protocol adapters, and the YAML format are documented
in the main repository.
