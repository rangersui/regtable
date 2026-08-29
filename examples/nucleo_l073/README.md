# regtable on a NUCLEO-L073RZ

The stock board as a register table, over the ST-Link virtual COM
port: `led`, `button`, `uptime`, and three silicon registers of the
STM32L0x3 (the LED pin and the button pin read back from GPIO, the
die's ID code) picked from the vendor's CMSIS-SVD. One polled UART,
no interrupts: the whole application is [main.c](main.c).

## Build and run

Needs Keil MDK (Arm Compiler 6) with the `Keil::STM32L0xx_DFP` device
pack, and a NUCLEO-L073RZ.

1. Open [MDK-ARM/nucleo_l073.uvprojx](MDK-ARM/nucleo_l073.uvprojx) and build (F7).
2. Flash: copy `MDK-ARM/Out/nucleo_l073.hex` onto the board's
   `NODE_L073RZ` drive (or program from uVision with the on-board
   ST-Link).
3. Open the VCP in a terminal, 115200 8N1, line ending LF:

```
> list
NAME            TYPE   PERM  VALUE
--------------------------------------------
led             BOOL   RW    false
button          BOOL   RO    false
uptime          U32    RO    12345
led_pin         BOOL   RO    false
button_pin      BOOL   RO    true
dbgmcu_idcode   U32    RO    537355335
> set led true
OK
> get led_pin
true
> fetch
```

`set led true` lights LD2; `led_pin` is the same fact read back from
`GPIOA.IDR` bit 5, and `dbgmcu_idcode & 0xFFF` is `0x447`, the die
saying which chip it is. `fetch` draws the identity as a chip.

4. The same port speaks JSON, so the Python side works as-is
   (`pip install regtable`):

```
regtable fetch -p COM7
regtable connect -p COM7
```

or import the typed client generated beside the table:

```python
import sys; sys.path.insert(0, "gen")
from nucleo_client import NucleoDevice

with NucleoDevice.serial("COM7") as dev:
    dev.led = True
    print(dev.led_pin, dev.uptime, hex(dev.dbgmcu_idcode))
```

## What is in here

| Path | What it is |
| --- | --- |
| [regs.yaml](regs.yaml) | the table: three application registers, three SVD picks |
| [main.c](main.c) | the whole application: clock, pins, UART, the CLI loop |
| [gen/](gen/) | `regtable gen regs.yaml` output, checked in: the C table, its documentation, the typed Python client |
| [regtable/](regtable/) | regtable core and CLI, copied from [../../src](../../src) |
| Drivers/ | the ST HAL subset, CMSIS headers, startup and system files for the STM32L073 (BSD-3-Clause, STMicroelectronics; from the STM32CubeL0 package) |
| MDK-ARM/ | the Keil project |

Regenerating `gen/` after editing regs.yaml needs the vendor's SVD
beside it: copy `STM32L0x3.svd` from the installed device pack
(`%LOCALAPPDATA%\Arm\Packs\Keil\STM32L0xx_DFP\<version>\CMSIS\SVD\`)
or from st.com, then `regtable gen regs.yaml -o gen`. Building the
example does not need it: the generated table is checked in.

The board's pins are the stock NUCLEO-64 wiring: LD2 on PA5, B1 on
PC13, the VCP on USART2 (PA2/PA3). Sending a line, letting the answer
finish, then sending the next is the pacing the polled UART expects;
the CLI and the Python client both work that way.
