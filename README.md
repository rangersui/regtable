# regtable

Expose any Cortex-M MCU's state the way a PLC exposes its registers: one register table, multiple access interfaces.

## What it does

A table of named registers (C variables plus selected hardware peripherals) is exposed through:

- **Serial CLI** `get temp`, `set led true`, `info interval`, `list`; `--json` on every command makes the same CLI the machine interface: discovery, typed calls, structured errors
- **Modbus RTU / TCP** any SCADA / HMI / PLC master can read/write
- **MQTT** publish state, subscribe to commands; the table describes itself on retained metadata topics
- **YAML codegen** one description generates the C table, its documentation, and a typed Python client that verifies itself against the device; silicon registers picked from the vendor's SVD join the table read-only
- **Web panel** browser connects via Web Serial: live table, sliders, toggles, console
- **Python** `pip install regtable`: the generator, a client that takes the device's own table or a typed one from YAML, `regtable connect / watch / serve`; a Stream Deck example makes it a physical panel

All interfaces share one typed access path: `reg_set_raw()` / `reg_get_raw()`. Type checking, permission enforcement, range validation, and write callbacks happen once in the core. Protocol adapters only translate wire formats.

Adding a register is one line in the table. Switching from UART to BLE is swapping two function pointers.

## Scope

regtable exposes state. It does not execute logic.

Control logic lives in the application's C code. regtable makes variables readable and writable from outside. No rule engine, no programming language, no function block library. When an `on_write` hook rejects a value or an `on_change` hook fires an alarm, that is application code, not regtable's.

## State, not commands

A serial assistant is a tool for the wire: bytes in, bytes out, and the meaning stays in the operator's head. regtable is a tool for the state behind the wire, and the wire is one of several ways in.

The difference comes from one decision: the interface is nouns, not verbs. There is no start-pump command anywhere in this library; there is a `pump` register, and writing 1 to it. A command vocabulary grows with every feature, and every protocol reinvents it. A state vocabulary closes: name, type, permission, range. After that, each interface is only a wire format for the same four facts, and validation happens once, below all of them. Modbus made the same choice in 1979; a PLC exposes registers and coils, not verbs.

Two primitives then cover every interface: pull (a `get`, a Modbus read, a poll) and push (a value crossing its last-published shadow, an MQTT publish). A new protocol is an envelope over one of the two.

The project's boundaries fall out of the same decision. History belongs to the host: trends are a poller and a historian, and both ends already exist (a dashboard stack behind the MQTT adapter, a SCADA behind the Modbus adapter). Gateways belong at trust boundaries. The device stays memoryless and answers one question: what is the state, now.

## Architecture

Two layers on the device, plus host-side projections generated from the same source:

```
                  ┌────────────────────────────┐
   YAML ─────────►│   register table           │
   (one source)   │   name|type|perm|range     │
                  │                            │
                  │   typed core API           │
                  │   reg_get_raw / set_raw    │
                  └──────────────┬─────────────┘
                     validation lives here, once
                                 │
               ┌─────────────────┼─────────────────┐
               ▼                 ▼                 ▼
         CLI adapter       Modbus adapter     MQTT adapter      ← on device
         (UART/USB/BLE     (RTU/TCP framing,  (topics/payloads;
          byte stream)      CRC, exceptions)   client owns session)
               │                 │                 │
               ▼                 ▼                 ▼
             human           SCADA / PLC         cloud


   host-side projections discover the table from the device
   itself (list --json) and talk to it over CLI or Modbus:

         scripts and gateways ──► the --json CLI, as-is
         Web panel  ──► browser (Web Serial)
         pip install regtable ──► regtable gen / connect / watch / serve
```

Each adapter owns its transport, framing, and timing; the core table knows nothing about wire formats. One YAML yields the C table, its documentation, and a typed Python client. Host-side tools do not depend on generated code, because the table describes itself over the wire; the generated client uses that self-description to verify itself against the device at connect time.

regtable is the upper half of a chain the chip vendors already built the lower half of. A HAL wraps thousands of silicon registers into typed handles for the firmware engineer; regtable wraps application variables into typed entries for the outside world:

```
silicon registers -> HAL handle -> application variables -> RegEntry -> CLI / Modbus / MQTT / panel
```

## Try it on the desktop

No board needed. The terminal is the UART.

```bash
make run          # Linux, macOS, Git Bash, or Windows cmd with make on PATH
```

```bat
.\build run       # Windows cmd without make (needs gcc or clang on PATH)
```

Then type `help`, `list`, `set led true`, `get voltage`, `info pump`. The example, [example_desktop.c](example_desktop.c), is a tutorial: STEP 1 to 5 (state, hooks, table, transport, main loop) with `/* ... here */` markers where application code goes. All three hooks are shown working. Everything in it moves to the MCU as-is; only the transport functions change.

`make test` (or `.\build test`) runs the regression suite in [regtable_test.c](regtable_test.c). It is plain C with a capture transport, so it runs anywhere the library compiles. `make strict` is the same with `-Werror`.

## Try it on an Arduino

The repo is laid out as an Arduino library (`library.properties`, `src/`, `examples/`). Clone or unzip it into `Documents/Arduino/libraries/regtable/`, then File > Examples > regtable > arduino. That sketch, [examples/arduino/arduino.ino](examples/arduino/arduino.ino), exposes the built-in LED, analog pin A0, and `millis()` on an Uno over the USB serial port; open Serial Monitor at 115200 with "Newline" line ending and type `list`.

A second sketch, [examples/arduino_modbus/arduino_modbus.ino](examples/arduino_modbus/arduino_modbus.ino), serves the same registers as a Modbus RTU slave (address 1, 115200) instead of a CLI: point QModMaster or pymodbus at the Uno's COM port and read word 2 for A0, words 3-4 for uptime, write word 1 for the LED. It also shows what t3.5 frame collection looks like on Arduino: a `micros()` gap check.

Opening the COM port resets the Uno (DTR is wired to reset), and the bootloader takes about two seconds before the sketch runs. A request sent in that window gets no answer, so the first poll after connecting times out. Wait two seconds after opening the port, or let the master's automatic retry handle it; this applies to the CLI sketch the same way, where the symptom is just a missed first keystroke.

A third sketch, [examples/arduino_modbus_tcp/arduino_modbus_tcp.ino](examples/arduino_modbus_tcp/arduino_modbus_tcp.ino), is the same slave over Modbus TCP for boards with an Ethernet shield (W5100/W5500): static IP, port 502, MBAP framing instead of t3.5 silence. This one is compile-verified only; the CLI and RTU sketches are verified on hardware.

On AVR (Uno, Nano, Mega) `printf` has no float support unless the sketch is linked with `-lprintf_flt`, so FLOAT registers print as `?` there; integer and BOOL registers work as-is. On 32-bit Arduino boards (SAMD, RP2040, ESP32) FLOAT works. AVR also keeps `const` data in RAM (regtable does not use PROGMEM), so the table costs a few dozen bytes per entry there.

## Web panel

```bash
pip install regtable
regtable serve
```

serves [regtable_panel.html](regtable_panel.html) on localhost and opens it (Web Serial needs a secure context; Chrome or Edge). Connect, pick the COM port, and the panel builds itself from the device's `list --json`: read-only values, click-to-edit fields, sliders for ranged numerics, toggles for booleans, an adjustable poll rate, a raw console, and a reference tab that can copy the live register table as Markdown. The device is the only source of truth; the panel ships no per-device configuration.

## Quick start on the MCU

```c
#include "regtable_cli.h"

// 1. State
static float    temp     = 23.4f;
static uint16_t interval = 1000;
static uint8_t  led      = 0;

// 2. Register table
static void led_changed(const RegEntry *e) { HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, led); }

static const RegEntry registry[] = {
    { .name = "temp",     .ptr = &temp,     .type = REG_FLOAT, .perm = REG_RO },
    { .name = "interval", .ptr = &interval, .type = REG_U16,   .perm = REG_RW,
      .min.u = 100, .max.u = 60000, .modbus_addr = 1 },
    { .name = "led",      .ptr = &led,      .type = REG_BOOL,  .perm = REG_RW,
      .on_change = led_changed },
    { .name = NULL }
};

// 3. Transport (platform-specific)
static int my_write(const uint8_t *buf, uint16_t len) {
    return HAL_UART_Transmit(&huart2, (uint8_t *)buf, len, 100);
}
static int my_read(uint8_t *buf, uint16_t len, uint32_t timeout) {
    return HAL_UART_Receive(&huart2, buf, len, timeout);
}

// 4. Init and run
int main(void) {
    HAL_Init();
    // ... clock, GPIO, UART init ...

    RegTable table;
    reg_table_init(&table, registry);

    RegTransport tx = { .read = my_read, .write = my_write };
    RegCli cli;
    regcli_init(&cli, &table, tx);

    while (1) {
        uint8_t byte;
        if (HAL_UART_Receive(&huart2, &byte, 1, 10) == HAL_OK) {
            regcli_feed(&cli, byte);
        }

        temp = read_sensor();                       // application code
        reg_mark_dirty(&table, &registry[0]);       // tell the table it moved

        reg_poll(&table);                           // deferred on_change hooks run here
    }
}
```

Connect PuTTY at 115200:

```
> list
NAME            TYPE   PERM  VALUE
--------------------------------------------
temp            FLOAT  RO    23.4
interval        U16    RW    1000
led             BOOL   RW    false

> set led true
OK

> set temp 99
ERR: read-only

> set interval 50
ERR: out of range

> info interval
name:   interval
type:   U16
perm:   RW
value:  1000
range:  100..60000
modbus: 0x0001
desc:   Sampling interval in ms

> info interval --json
{"name":"interval","type":"U16","perm":"RW","value":1000,"min":100,"max":60000,"modbus":1,"desc":"Sampling interval in ms"}

> set interval 500 --json
{"result":"OK"}

> get interval --json
{"value":500}
```

`--json` works on `list`, `get`, `set` and `info`: `list --json` returns the whole table as one array, `get` and `set` answer with one small object, errors come back as `{"error":"..."}`. A host program (a test script, a Web UI, a gateway) can discover every register, its type, its limits, and its description from the device itself, then drive it in the same format. Where a network or trust boundary separates the host from the device, a gateway wraps these same commands and adds the boundary's own concerns (authentication, rate limits, audit).

## Exposing HAL and hardware state

Entries point at any address, so the state worth watching is usually two lines away:

```c
/* what the HAL thinks of its peripheral: plain struct fields */
{ .name = "uart2_err",  .ptr = &huart2.ErrorCode, .type = REG_U32, .perm = REG_RO,
  .description = "UART2 HAL error flags" },

/* the silicon itself: ptr is volatile, CMSIS addresses work directly */
{ .name = "gpioa_idr",  .ptr = &GPIOA->IDR,       .type = REG_U32, .perm = REG_RO,
  .description = "Port A input levels" },
```

Configuration stays with the HAL, which owns the peripheral's state machine; these entries are read-only windows for commissioning and diagnosis. A generated window over many raw peripheral registers (from a CMSIS-SVD description) would sit on this same mechanism.

## Modbus

The Modbus adapter is a slave: one received frame in, one response frame out. RTU and TCP share the same PDU handling and the same register map; only the envelope differs.

```c
#include "regtable_modbus.h"

RegModbus mb;
regmb_init(&mb, &table, 1);              /* slave address 1 */

/* platform code collects bytes until the line is idle for 3.5
 * character times (t3.5, from the serial line spec), then: */
uint16_t n = regmb_process(&mb, frame, frame_len, resp, sizeof(resp));
if (n > 0) uart_send(resp, n);           /* n == 0: stay silent */
```

An entry joins the Modbus map by setting `modbus_addr`, its word address: one word for U8/I8/U16/I16/BOOL, two words for U32/I32/FLOAT. `modbus_addr` 0 means not mapped, so the map starts at word address 1. Unmapped entries stay reachable through the CLI and are invisible to Modbus.

- Function codes 03/04 (read holding/input, one overlaid map), 06 (write single), 16 (write multiple). Others answer exception 01.
- Writes go through `reg_set_raw`, the same path as every other adapter: type domain, range, `on_write` veto, dirty bit for `on_change`. A refused write answers exception 04.
- A request covering half of a two-word value answers exception 02.
- Two-word values travel high word first; `mb.word_swap = true` switches to low word first for masters that expect CDAB.
- CRC-16 is checked inside `regmb_process`; a bad frame is dropped silently, per the spec. Broadcasts (address 0) apply writes and are never answered.

Frame boundary detection (the t3.5 silence) belongs to platform code: a UART idle interrupt on STM32, a `micros()` gap check on Arduino, an inter-byte timeout on a desktop serial port. The adapter itself runs no timers and does no I/O.

For Modbus TCP, `regmb_process_tcp` takes one ADU (the 7-byte MBAP header plus the PDU, read off the stream using the header's length field), echoes the transaction and unit identifiers, and answers on the same map. There is no CRC and no broadcast on TCP; an ADU the messaging guide says to discard (wrong protocol id, disagreeing length) returns 0. [example_modbus_tcp.c](example_modbus_tcp.c) is a desktop slave on 127.0.0.1:1502: `make run-tcp` (or `.\build tcpslave`, then `.\tcpslave`), then point QModMaster (TCP mode) or pymodbus at it, no board or serial port involved.

`list --json` on the CLI shows each entry's `modbus` address, so a host can discover the register map from the device itself.

## MQTT

The MQTT adapter is a projection: topics and payloads in, topics and payloads out. The MQTT protocol itself (connect, keepalive, QoS, reconnect) belongs to an MQTT client library in platform code, the way the UART belongs to the HAL.

```c
#include "regtable_mqtt.h"

static int my_publish(const char *topic, const char *payload,
                      bool retain, void *user) {
    return mqtt_client_publish(topic, payload, retain) ? 0 : -1;
}

RegMqtt mq;
regmqtt_init(&mq, &table, "boiler", my_publish, NULL);
regmqtt_publish_all(&mq);            /* after (re)connect: full state */

/* main loop, at telemetry cadence: */
regmqtt_poll(&mq);                   /* publishes whatever changed */

/* from the client's message callback (subscribed to boiler/+/set): */
regmqtt_handle(&mq, topic, payload); /* returns the RegResult */
```

Topics are `boiler/temp` for state (retained, payload is the register's text form) and `boiler/temp/set` for commands. A set travels the same `reg_set_raw` path as every adapter; a refusal answers by silence on the state topic, so subscribers see the old value still standing.

A `/set` is a one-shot command and is published with the retain flag clear: a retained `/set` would be replayed by the broker on every new subscription, re-executing the command after each reconnect. Persisting a desired value for an offline device is the broker or cloud layer's job (a device shadow service). Register names become topic levels, so `regmqtt_init` rejects names with `/`, `+`, or `#` and names longer than 31 characters; the prefix may span multiple levels (`plant/boiler`) but carries no wildcards.

The table describes itself over MQTT the way it does over the CLI: `regmqtt_announce(&mq)` publishes, retained, a descriptor `boiler/$meta/$table` (`{"count":4,"schema":"1a2b3c4d"}`) and one `boiler/$meta/<name>` per register, a JSON object with the fields of a `list --json` entry minus `value`, plus the entry's `index` in the table and the same `schema`. The schema is the table's fingerprint (FNV-1a over every meta, in table order): the same registers in the same shape give the same value, a renamed register or a changed range gives another. Retained messages outlive the firmware that published them, and the fingerprint is what tells a current meta from a leftover: a host takes `$table`, keeps the metas whose schema matches, and has the whole table once those cover indices `0..count-1`; a partial publish leaves it incomplete, never mixed with an older table. Call it after each connect, before `regmqtt_publish_all`: shape first, then state. `<prefix>/+` carries state only, the metadata sits two levels down, and names starting with `$` are refused by `regmqtt_init` so nothing collides.

Change detection is a shadow array: the raw value last accepted by `publish()`, one per entry. The pair works like a transactional outbox with the difference as the pending entry: `publish()` returning non-zero leaves the shadow alone and the next poll retries, payloads are absolute values so repeats are harmless, and `regmqtt_publish_all` after a reconnect resends everything. The dirty bitmap and `on_change` stay the application's; the adapter compares values instead, so both consumers see every write without stealing from each other. A change that reverts between two polls publishes nothing: `on_change` answers "did something happen", the shadow answers "is the net value out of sync".

[example_mqtt_desktop.c](example_mqtt_desktop.c) shows the pipeline with stdout standing in for the broker (`make run-mqtt`); [examples/arduino_mqtt/arduino_mqtt.ino](examples/arduino_mqtt/arduino_mqtt.ino) is the real-client integration with PubSubClient, an Ethernet shield, and a last-will `status` topic (compile-verified, like the Arduino TCP sketch).

## YAML codegen

```bash
pip install regtable                       # or, from a checkout: python python/regtable ...
regtable gen device.yaml -o gen            # registers.c/.h/.md + <device>_client.py
regtable connect -p COM6                   # the board's own table, a Python REPL with `dev`
regtable watch a0 led -p COM6              # print changes; --json for scripts
regtable connect -p COM6 --yaml device.yaml  # the typed client, verified against the board first
```

`regtable gen` ([python/regtable/gen.py](python/regtable/gen.py)) turns one YAML description into four projections of the same table: `registers.c` (storage and the `RegEntry[]`), `registers.h` (externs and hook prototypes; hook bodies stay in application code), `registers.md` (the documentation table), and `<device>_client.py`, a typed Python client, with its runtime copied alongside so the output directory imports on its own.

The client is a host-side mirror of the device's `RegEntry[]`: one property per register with the type in its signature, range and type checked locally before the wire, read-only registers with no setter at all, so an IDE completes `dev.interval = 500` and rejects `dev.temp = 99` before anything runs. The mirror is a contract and the device is the truth: connecting fetches the device's own `list --json` and compares it field by field with the schema baked into the class; any difference raises `SchemaDriftError` naming the register and the field, before the first read. `registers()` lists the table the client was built from, without touching the wire. The client keeps no values, so every attribute read is a wire read; `snapshot()` is the one bulk read, `watch()` polls for changes, `record()` samples over time. The wire carries no request ids, so a timeout or a malformed answer closes the connection rather than risk pairing a late reply with the wrong command; reconnecting re-verifies. Transports are duck-typed: a serial port (pyserial) or a CLI process over a pipe (`--pipe` on the command line), which is how the test suite drives the desktop binaries without hardware. `regtable.build_client("device.yaml")` returns the same class without writing files. `RegtableClient.discover(transport)` needs no YAML at all: the schema is the device's own `list --json`, registers are reached by name (`dev["led"]`, `dev["pwm9"] = 128`) with the same local checks, and nothing can drift because nothing was assumed; `regtable connect` and `regtable watch` use it unless `--yaml` names a contract. [examples/streamdeck/streamdeck_panel.py](examples/streamdeck/streamdeck_panel.py) builds on discover(): a Stream Deck becomes a physical panel for any regtable device, registers laid out over keys and pages, numeric registers on the dials, values refreshed from the device.

```yaml
device: demo
registers:
  - name: interval
    type: u16
    perm: rw
    init: 1000
    min: 100
    max: 60000
    modbus: 3
    desc: Sampling interval, ms
```

Every constraint the adapters enforce at init time is checked at generation time instead: duplicate names, Modbus word overlaps and widths, MQTT topic rules on names, ranges against the type's domain, hooks on registers that could never run them. A bad table fails before it compiles. `make codegen` generates from [tools/example.yaml](tools/example.yaml), compiles the output with `-Werror`, runs a smoke test against it, and drives the Python client and command against a CLI built over the generated table. The package lives in [python/](python/) with its own [README](python/README.md); `pip install .` from the checkout installs the same thing PyPI ships.

### Silicon registers from the SVD

The chip vendor's CMSIS-SVD file describes the silicon the way the YAML describes the application: every peripheral register, its address, size, access, and bit fields. A YAML can pick a few of those and the generator exposes them as read-only entries next to the application's own, so a serial `list` shows silicon state with no debugger attached and no HAL call written:

```yaml
registers:
  - name: temp
    type: float
    perm: ro

  - svd: STM32L053.svd                 # the vendor's file, relative to this YAML
    pick:
      - USART2.ISR                     # usart2_isr, U32: the whole register
      - USART2.ISR.TXE                 # usart2_isr_txe, BOOL: one field
      - { reg: TIM2.CNT, as: motor_count, desc: Motor timer }
```

A whole register becomes an entry whose `ptr` is the register's address, read in place; a field becomes an entry with a generated `on_read` that samples the register and keeps the bits (BOOL for one bit, the narrowest integer type otherwise). Names follow the pick (`USART2.ISR` becomes `usart2_isr`), go through every rule a hand-written name does, and `as:` renames. The description carries the register path, the address, and the field map with bit positions, so `info` shows where a value comes from and what its bits mean (a `desc:` on the pick replaces it); `registers.md` adds a table of the picks with their addresses, whatever the description says. A whole register with many fields gets a long description: the CLI streams it whole, the MQTT `$meta` leaves it out when it does not fit `REGTABLE_MQTT_META_SIZE` (the rest of the meta still goes).

The SVD is used as written: the address of every picked register is the vendor's, and a wrong SVD gives a wrong address the same way a wrong `modbus_addr` gives a wrong map; the vendor's file is the place to fix it. A pick reads one address, as an unsigned value of the register's width; what the generator checks is what the SVD says about reading that address: the pick names one register or field, 8, 16, or 32 bits wide and aligned to its size; the register is readable (its access declared at some level and not write-only, with no write-only field in the bytes read); no `readAction` on the register, on any of its fields, or on any other register the SVD places at the same address (an alternate view), since every emitted read is a whole-register read and a field that clears itself when read, a data register's payload, would be consumed by showing it; no `disableCondition` on the peripheral, whose expression the generator cannot evaluate. `force: true` on the block takes the last two knowingly. The check sees only what the vendor marked: the STM32L0 files, for one, mark no `readAction` at all, so their receive data registers (`USART2.RDR`, `SPI1.DR`) pass the check and would be drained by every poll; picking a data register is a decision, not an accident. Silicon entries are read-only: a write to the silicon belongs to the HAL and the application. The peripheral's clock is the application's: reading an unclocked peripheral faults on many parts, so the picks are registers of peripherals the application runs. The generated table compiles with the host compilers in CI (GCC and Clang on Linux, macOS, Windows) and with `arm-none-eabi-gcc` for Cortex-M0+; what it reads is verified by running it on the chip the SVD describes. The reader is the generator's own (`python/regtable/svd.py`), and it stops at what a read needs: peripherals with `derivedFrom` (one level) and `dim` arrays, clusters, registers and fields with `dim`, `bitOffset`/`bitWidth`, `bitRange`, `lsb`/`msb`, access, `readAction`, `disableCondition`; elements are checked against the schema's list for each level, so a misspelt `<size>` is refused instead of inherited; a device whose `addressUnitBits` is not 8 is refused; `alternateGroup`, `alternateCluster`, `protection`, `dataType`, `resetValue`, `enumeratedValues`, `writeConstraint`, `modifiedWriteValues`, and the device `width` are accepted as schema elements and left alone (a pick reads an unsigned value, whatever `dataType` says); vendor descriptions are rendered to ASCII by a fixed table (`\u00b5A` to `uA`, `\u00b1` to `+/-`, accents decomposed) and a character with no ASCII form refuses the pick by name, for a `desc:` of its own to replace it; every malformed or unsupported shape is refused with its location; no dependency beyond the standard library.

## Atomicity

Each `reg_set_raw` call is atomic at the register level: the value either fully updates or gets rejected, never half-written. There is no multi-register transaction. Updating three PID parameters with three separate `set` calls leaves a window where some have the new value and others still have the old one. For most slow control loops this doesn't matter. A group of values that must take effect together is guarded in application code (apply all three in `on_write`, or buffer them and swap in one step).

## Concurrency

regtable takes no locks. When everything (adapters, `reg_poll`, and the code that touches the register variables) runs in one main loop or one RTOS task, nothing more is needed.

When an ISR or another task also writes a register variable, there is a race. `reg_set_raw` does three steps:

```
1. old = *ptr          read the current value
2. *ptr = raw          store the new one
3. mark dirty          set the bit in the bitmap
```

If the main loop is between step 1 and step 2 when an interrupt writes the same variable, step 2 overwrites the interrupt's value, it's lost. The dirty bitmap has the same problem on its own: `reg_mark_dirty` does `bits |= x` and `reg_poll` does `bits &= ~x`, both read-modify-write, so an ISR calling `reg_mark_dirty` while the main loop is inside `reg_poll` can lose a bit.

The fix is a critical section around the access:

```c
disable_irq();                      // Cortex-M: __disable_irq()
reg_set_raw(&table, entry, raw);    // FreeRTOS: taskENTER_CRITICAL()
enable_irq();                       // Arduino:  noInterrupts()
```

regtable leaves this to the application because every platform locks differently. The simplest pattern keeps the ISR out of the table entirely: the ISR sets its own flag or writes its own variable, and the main loop calls `reg_mark_dirty` when it picks that up.

## Hooks

Three side-effect points on the access path. The core moves data; hooks are where the outside world gets touched. Each one answers a different question:

| Hook | Question it answers | When | Context |
| --- | --- | --- | --- |
| `on_write` | Is this command allowed right now? | after perm/range check, before store | synchronous |
| `on_read` | Where does this value come from? | before fetch | synchronous |
| `on_change` | Who needs to know it changed? | value changed; runs at next `reg_poll` | deferred |

**`on_write` is a command check.** Someone wrote `pump = 1`; that's a command. The hook decides whether it's allowed right now (interlock open? state machine in the right state?) and returns `false` to refuse. Nothing is stored on refusal and the caller gets `ERR: rejected`.

**`on_read` is a computed value.** From the outside, `voltage` looks like a plain float register. Inside, the hook samples the ADC, converts counts to volts, and writes the result into `*ptr` just before it's read. The caller gets engineering units and never sees the ADC.

**`on_change` is telemetry.** The value changed and something outside needs to hear about it: publish it over MQTT, log it, refresh a display, light an LED that follows a switch. It is deferred: the write only sets a dirty bit, and `reg_poll()` runs the hooks later from the main loop in table order. By the time it runs the value is already stored, so it can report but not refuse. When application code changes a variable directly, `reg_mark_dirty()` puts it in the same queue.

## Resource use

- No dynamic allocation. Dependencies: C99 standard library only.
- The register table is `const` and lives in flash. RAM is one `RegTable` handle (dirty bitmap) and one `RegCli` context (line buffer, `REGTABLE_CLI_BUF_SIZE`, default 128).
- FLOAT registers use printf floating-point formatting, which on newlib-nano requires `-u _printf_float` (adds several KB); a minimal built-in float formatter is a planned alternative.
- Platform-specific code is the two transport functions (`read`, `write`). Verified on the desktop; the STM32 HAL calls above show the shape on an MCU.

## License

MIT
