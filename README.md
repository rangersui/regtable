# regtable

Expose any Cortex-M MCU's state the way a PLC exposes its registers: one register table, multiple access interfaces.

## What it does

You define a table of named registers (your variables + selected hardware peripherals). The library exposes them through:

- **Serial CLI** `get temp`, `set led true`, `info interval`, `list`
- **Modbus RTU** any SCADA / HMI / PLC master can read/write (planned)
- **MQTT** publish state, subscribe to commands (planned)
- **MCP** AI agents operate your device (planned)
- **Web UI** browser connects via Web Serial / Web Bluetooth (planned)

All interfaces share one typed access path: `reg_set_raw()` / `reg_get_raw()`. Type checking, permission enforcement, range validation, and write callbacks happen once in the core. Protocol adapters only translate wire formats.

## Scope

regtable exposes state. It does not execute logic.

Your control logic lives in your C code. regtable only makes your variables readable and writable from outside. No rule engine, no programming language, no function block library. If your `on_write` hook rejects a value or your `on_change` fires an alarm, that's your C function, not regtable's.

## The idea

MCU development today: you write hundreds of lines of C to configure peripherals, then repeat it for every new project, every new chip. Most of that code is configuration pretending to be programming.

regtable inverts this. You declare what your device exposes:

- name, type, permission, range: and the library handles access.
- Adding a register is one line.
- Switching from UART to BLE is swapping two function pointers.

## Architecture

Two layers on the device, plus host-side projections generated from the same source:

```
                  ┌────────────────────────────┐
   SVD ──────────►│   register table           │
   (silicon)      │   name|type|perm|range     │
                  │                            │
   YAML ─────────►│   typed core API           │
   (product)      │   reg_get_raw / set_raw    │
                  └──────────────┬─────────────┘
                     validation lives here, once
                                 │
               ┌─────────────────┼─────────────────┐
               ▼                 ▼                 ▼
         CLI adapter       Modbus adapter     MQTT adapter      ← on device
         (UART/USB/BLE     (RTU framing,      (broker session,
          byte stream)      3.5-char idle)     pub/sub)
               │                 │                 │
               ▼                 ▼                 ▼
             human           SCADA / PLC         cloud


   host-side projections, generated from the same YAML,
   talk to the device over CLI or Modbus:

         MCP server ──► AI agents
         Web UI     ──► browser (Web Serial / Web Bluetooth)
```

Each adapter owns its transport, framing, and timing; the core table knows nothing about wire formats. One YAML yields four outputs: the C register table, the MCP tool definitions, the Web UI, and the documentation.

The key insight: MCU hardware is already memory-mapped registers. PLC programming has been register-table-driven since 1979 (Modbus). ARM ships machine-readable register descriptions with every chip (CMSIS-SVD). regtable connects these existing pieces.

## Try it on the desktop

No board needed. Your terminal is the UART.

```bash
make run          # Linux, macOS, Git Bash, or Windows cmd with make on PATH
```

```bat
.\build run       # Windows cmd without make (needs gcc or clang on PATH)
```

Then type `help`, `list`, `set led true`, `get voltage`, `info pump`. The example, [example_desktop.c](example_desktop.c), is a tutorial: it walks through STEP 1 to 5 (state, hooks, table, transport, main loop) with `/* your ... here */` markers where you add your own. All three hooks are shown working. Everything you write there moves to the MCU as-is; only the transport functions change.

`make test` (or `.\build test`) runs the regression suite in [regtable_test.c](regtable_test.c): 114 checks covering parsing, ranges, every type, all three hooks, deferred change tracking, and line editing. It's plain C with a capture transport, so it runs anywhere the library compiles.

## Quick start on the MCU

```c
#include "regtable_cli.h"

// 1. Define your state
static float    temp     = 23.4f;
static uint16_t interval = 1000;
static uint8_t  led      = 0;

// 2. Declare the register table
static void led_changed(const RegEntry *e) { HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, led); }

static const RegEntry registry[] = {
    { .name = "temp",     .ptr = &temp,     .type = REG_FLOAT, .perm = REG_RO },
    { .name = "interval", .ptr = &interval, .type = REG_U16,   .perm = REG_RW,
      .min.u = 100, .max.u = 60000 },
    { .name = "led",      .ptr = &led,      .type = REG_BOOL,  .perm = REG_RW,
      .on_change = led_changed },
    { .name = NULL }
};

// 3. Provide transport (platform-specific, ~5 lines)
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

        temp = read_sensor();                       // your business logic
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
temp            FLOAT  RO    23.40
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
modbus: 0x0000
desc:   Sampling interval in ms
```

## What the library handles

| Feature | Status |
| --- | --- |
| Register table with name, type, permission, range | ✅ Done |
| Types: U8/U16/U32, I8/I16/I32, FLOAT, BOOL | ✅ Done |
| Typed core API (`reg_set_raw` / `reg_get_raw`), single validation path for all adapters | ✅ Done |
| Serial CLI (get / set / info / list / help) | ✅ Done |
| Transport abstraction for byte-stream adapters | ✅ Done |
| `on_write` hook (sync guard, can veto) | ✅ Done |
| `on_read` hook (refresh before fetch) | ✅ Done |
| `on_change` hook (deferred, via dirty bitmap + `reg_poll`) | ✅ Done |
| Hardware register access (GPIO ODR, ADC DR, etc.) | ✅ Supported via pointer |
| Modbus RTU slave | 🔲 Planned |
| Python codegen from YAML + SVD | 🔲 Planned |
| MQTT state publish / command subscribe | 🔲 Planned |
| MCP server generation from YAML | 🔲 Planned |
| Web UI via Web Serial / Web Bluetooth | 🔲 Planned |
| JSON output mode (`list --json`) | 🔲 Planned |

## Design principles

**Each register carries its own rules.** Type, permission, range, and callbacks live in the `RegEntry` struct. Any code that reads or writes a register gets the same checks automatically. You don't write `if ADC ... else if GPIO ...` in every adapter; you call `reg_get_raw()` and the entry handles the rest.

**Define once, access from anywhere.** Without regtable, your CLI code knows how to read the temperature, your Modbus code knows how to read the temperature, your documentation describes how to read the temperature. With regtable, only the register table knows. CLI, Modbus, MQTT, MCP, and docs all read the same table.

**Use what the industry already ships.** ARM chips come with machine-readable register descriptions (CMSIS-SVD). SCADA systems already speak Modbus. regtable plugs into both as they are.

## Atomicity

Each `reg_set_raw` call is atomic at the register level: the value either fully updates or gets rejected, never half-written. But there is no multi-register transaction. If you update three PID parameters with three separate `set` calls, there is a window where some have the new value and others still have the old one. For most slow control loops this doesn't matter. If you need a group of values to take effect together, guard them in your own code (e.g. apply all three in `on_write`, or buffer them and swap in one step).

## Concurrency

regtable takes no locks. If everything (adapters, `reg_poll`, and the code that touches your register variables) runs in one main loop or one RTOS task, you're done; skip this section.

If an ISR or another task also writes a register variable, there is a race. `reg_set_raw` does three steps:

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

regtable leaves this to you because every platform locks differently. The simplest pattern is to keep the ISR out of the table entirely: have it set its own flag or write its own variable, and let the main loop call `reg_mark_dirty` when it picks that up.

## Hooks

Three side-effect points on the access path. The core moves data; hooks are where the outside world gets touched. Each one answers a different question:

| Hook | Question it answers | When | Context |
| --- | --- | --- | --- |
| `on_write` | Is this command allowed right now? | after perm/range check, before store | synchronous |
| `on_read` | Where does this value come from? | before fetch | synchronous |
| `on_change` | Who needs to know it changed? | value changed; runs at next `reg_poll` | deferred |

**`on_write` is a command check.** Someone wrote `pump = 1`; that's a command. The hook decides whether it's allowed right now (interlock open? state machine in the right state?) and returns `false` to refuse. Nothing is stored on refusal and the caller gets `ERR: rejected`.

**`on_read` is a computed value.** From the outside, `voltage` looks like a plain float register. Inside, the hook samples the ADC, converts counts to volts, and writes the result into `*ptr` just before it's read. The caller gets engineering units and never sees the ADC.

**`on_change` is telemetry.** The value changed and something outside needs to hear about it: publish it over MQTT, log it, refresh a display, light an LED that follows a switch. It is deferred: the write only sets a dirty bit, and `reg_poll()` runs the hooks later from the main loop in table order. By the time it runs the value is already stored, so it can report but not refuse. When your own C code changes a variable directly, `reg_mark_dirty()` puts it in the same queue.

## Design decisions (settled)

- **Signed types.** `I8` / `I16` / `I32` are in the enum; raw convention is sign-extension to 32 bits.
- **Range metadata.** `min` / `max` are a `RegLimit` union (`.u` / `.i` / `.f`), read through the member matching the entry's type. Full U32 range and fractional FLOAT ranges are expressible; entry size unchanged.
- **Change notification.** Dirty bitmap in the `RegTable` handle (RAM), entries stay `const` in flash. `REGTABLE_MAX_ENTRIES` (default 64) sizes the bitmap.
- **Modbus float word order.** A device speaks one convention, so ABCD/CDAB is a Modbus adapter parameter, not a per-entry field.

`RegEntry` is considered stable from here; adapters and codegen can build on it.

## Resource budget

- Flash: ~3-5 KB runtime library. Note: FLOAT registers use `%f` formatting, which on newlib-nano requires `-u _printf_float` (adds several KB); a minimal built-in float formatter is a planned alternative.
- Register table: ~44 bytes per entry, in flash (`const`).
- RAM: one `RegTable` handle (~16 bytes at the default 64-entry bitmap) plus one `RegCli` context (~140 bytes with the default 128-byte line buffer).
- Stack: ~200 bytes during command processing.
- Dynamic allocation: zero.
- Dependencies: C99 standard library only.

Runs on Cortex-M0 (64 KB flash / 8 KB RAM) and up.

## Files

```
regtable_core.h     Register entry struct, table handle, type/perm enums, raw convention, core API
regtable_core.c     Lookup, typed raw get/set (validation), dirty tracking + reg_poll, string layer
regtable_cli.h      CLI context struct
regtable_cli.c      Non-blocking byte-fed CLI parser (get/set/info/list/help)
example_desktop.c   Interactive desktop tutorial (STEP 1..5, all hooks demonstrated)
regtable_test.c     Self-checking regression test, plain C
Makefile            make / make run / make test / make clean
build.bat           Same for Windows cmd without make
```

## Chip agnostic

The library is pure C99. Platform-specific code lives entirely in two user-provided function pointers (`read` and `write`). Switching chips means changing those two function bodies, nothing else.

Tested / intended targets:

- STM32 (any, via HAL_UART)
- Arduino (any, via Serial)
- ESP32 (ESP-IDF uart driver)
- Nordic nRF52 (nrfx_uarte)
- Desktop (stdin/stdout, for development)

## License

MIT
