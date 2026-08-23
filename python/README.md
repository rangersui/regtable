# regtable (Python)

Host-side tools for [regtable](https://github.com/rangersui/regtable), the
C99 register-table library for MCUs: one `RegEntry[]` table in the firmware,
reachable over a serial CLI, Modbus RTU/TCP, MQTT, a browser, and Python.

```
pip install regtable
```

## Command line

```
regtable gen device.yaml -o gen            # registers.c/.h/.md + <device>_client.py; `svd:` blocks pick silicon registers
regtable connect -p COM6                   # the board's own table, then a REPL with `dev`
regtable watch a0 led -p COM6              # print changes; --json for scripts
regtable connect -p COM6 --yaml device.yaml  # typed client, verified against the board first
regtable serve                             # the Web Serial panel on localhost
```

Without `--yaml`, `connect` and `watch` take the device as it describes
itself (`list --json`) and reach registers by name: `dev["led"]`,
`dev["pwm9"] = 128`. With `--yaml` they build the typed client from the
YAML in memory and compare the board's table field by field with it; a
board whose table differs ends the command with the differences listed,
before the first read.

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

Without a YAML, the device's own table:

```python
from regtable import RegtableClient, SerialTransport

with RegtableClient.discover(SerialTransport("COM6")) as dev:
    dev.registers()           # whatever the device reports
    dev["led"] = True         # same local checks, by name
    dev["pwm9"] = 300         # ValueError
```

The client keeps no values; every attribute read is a wire read. The wire
carries no request ids, so a timeout or a malformed answer closes the
connection rather than pair a late reply with the wrong command;
reconnecting re-verifies. Transports are duck-typed: `SerialTransport`
(pyserial), `PipeTransport` (a CLI process over stdin/stdout), or anything
with `write_line`, `read_line`, `close`.

The firmware side, the protocol adapters, and the YAML format are documented
in the main repository.
