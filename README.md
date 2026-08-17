# regtable

Expose any Cortex-M MCU's state the way a PLC exposes its registers: one register table, multiple access interfaces.

## What it does

You define a table of named registers (your variables + selected hardware peripherals). The library exposes them through:

- **Serial CLI** — `get temp`, `set led true`, `info interval`, `list`
- **Modbus RTU** — any SCADA / HMI / PLC master can read/write (planned)
- **MQTT** — publish state, subscribe to commands (planned)
- **MCP** — AI agents operate your device (planned)
- **Web UI** — browser connects via Web Serial / Web Bluetooth (planned)

All interfaces share one typed access path: `reg_set_raw()` / `reg_get_raw()`. Type checking, permission enforcement, range validation, and write callbacks happen once in the core — protocol adapters only translate wire formats.

## Scope

regtable exposes state. It does not execute logic.

Your control logic lives in your C code — regtable only makes
your variables readable and writable from outside. No rule engine,
no programming language, no function block library. If your
`on_write` hook rejects a value or your `on_change` fires an
alarm, that's your C function, not regtable's.

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

## Quick start

```c
#include "regtable_cli.h"

// 1. Define your state
static float    temp     = 23.4f;
static uint16_t interval = 1000;
static uint8_t  led      = 0;

// 2. Declare the register table
static const RegEntry registry[] = {
    { .name = "temp",     .ptr = &temp,     .type = REG_FLOAT, .perm = REG_RO },
    { .name = "interval", .ptr = &interval, .type = REG_U16,   .perm = REG_RW,
      .min = 100, .max = 60000 },
    { .name = "led",      .ptr = &led,      .type = REG_BOOL,  .perm = REG_RW },
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

    RegTransport tx = { .read = my_read, .write = my_write };
    RegCli cli;
    regcli_init(&cli, registry, tx);

    while (1) {
        uint8_t byte;
        if (HAL_UART_Receive(&huart2, &byte, 1, 10) == HAL_OK) {
            regcli_feed(&cli, byte);
        }
        temp = read_sensor();  // your business logic
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

| Feature                                            | Status                   |
| -------------------------------------------------- | ------------------------ |
| Register table with name, type, permission, range  | ✅ Done                  |
| Typed core API (`reg_set_raw` / `reg_get_raw`) — single validation path for all adapters | ✅ Done |
| Serial CLI (get / set / info / list / help)        | ✅ Done                  |
| Transport abstraction for byte-stream adapters     | ✅ Done                  |
| Write callbacks (validation, side effects)         | ✅ Done                  |
| Read callbacks (trigger ADC, refresh cached value) | ✅ Done                  |
| Hardware register access (GPIO ODR, ADC DR, etc.)  | ✅ Supported via pointer |
| Modbus RTU slave                                   | 🔲 Planned               |
| Python codegen from YAML + SVD                     | 🔲 Planned               |
| MQTT state publish / command subscribe             | 🔲 Planned               |
| MCP server generation from YAML                    | 🔲 Planned               |
| Web UI via Web Serial / Web Bluetooth              | 🔲 Planned               |
| JSON output mode (`list --json`)                   | 🔲 Planned               |

## Design principles

**Hardware describes itself, not pretends to be uniform.** HAL hides differences behind a common API; when the differences leak through, you're back to reading datasheets. regtable preserves differences as structured metadata (type, permission, range, side-effect callbacks). Consumers don't need `if ADC ... else if GPIO ...` — they call `reg_get_raw()` and the entry carries its own access contract.

**SVD is silicon truth, YAML is product policy.** The same STM32 chip makes a temperature logger and a motor controller. SVD describes what the chip *has*; YAML describes what your product *exposes*. regtable merges both into one runtime namespace.

**Complexity is described once, projected many times.** Without regtable, CLI code knows hardware details, Modbus code knows hardware details, documentation knows hardware details, test tools know hardware details. With regtable, only `RegEntry` knows. CLI, Modbus, MQTT, MCP, and documentation are projections of the same table.

**Built on what already exists.** regtable reads what ARM already defined (CMSIS-SVD) and what industry already uses (Modbus). Silicon vendors ship SVD today; SCADA masters speak Modbus today. regtable plugs into both as they are.

## Concurrency contract

All protocol adapters and all register variables are accessed from a single execution context — the bare-metal main loop, or one RTOS task. Locking is the caller's job: if an ISR or another task writes a register variable, wrap it in your own critical section. Aligned 32-bit loads and stores are atomic on Cortex-M; read-modify-write sequences and multi-register values (float over Modbus) are not.

## Open design decisions (pre-1.0)

Decisions that change `RegEntry` or the raw convention, so they must land before the struct is frozen:

- **Change notification.** MQTT publish-on-change needs a dirty flag or `on_change` hook; today the table is poll-only.
- **Signed types.** `I8` / `I16` / `I32` are not in the enum yet; adding them extends the raw convention (sign-extension rules).
- **Range metadata.** `min` / `max` are `int32_t` — U32 ranges cap at `INT32_MAX`, and FLOAT ranges are integer-valued. A typed union would fix both at +4 bytes/entry.
- **Modbus float word order.** A float spans two holding registers; the ABCD vs CDAB convention must be declared (per-table or per-entry).

## Resource budget

- Flash: ~3–5 KB runtime library. Note: FLOAT registers use `%f` formatting, which on newlib-nano requires `-u _printf_float` (adds several KB); a minimal built-in float formatter is a planned alternative.
- Register table: ~40 bytes per entry, in flash (`const`).
- RAM: one `RegCli` context (~140 bytes with the default 128-byte line buffer).
- Stack: ~200 bytes during command processing.
- Dynamic allocation: zero.
- Dependencies: C99 standard library only.

Runs on Cortex-M0 (64 KB flash / 8 KB RAM) and up.

## Files

```
regtable_core.h     Register entry struct, type/perm enums, raw convention, core API
regtable_core.c     Lookup, typed raw get/set (validation), string parse layer
regtable_cli.h      CLI context struct
regtable_cli.c      Non-blocking byte-fed CLI parser (get/set/info/list/help)
test_desktop.c      Desktop test harness (gcc)
```

## Chip agnostic

The library is pure C99. Platform-specific code lives entirely in two user-provided function pointers (`read` and `write`). Switching chips means changing those two function bodies — nothing else.

Tested / intended targets:

- STM32 (any, via HAL_UART)
- Arduino (any, via Serial)
- ESP32 (ESP-IDF uart driver)
- Nordic nRF52 (nrfx_uarte)
- Desktop (stdin/stdout, for development)

## License

MIT
