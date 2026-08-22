# Stream Deck as a physical panel

[streamdeck_panel.py](streamdeck_panel.py) turns an Elgato Stream Deck into
a control panel for any regtable device, with no per-device configuration:
the script asks the device for its table (`list --json`, the same
self-description the Web panel uses) and lays the registers out over the
keys. Hardware-verified with a Stream Deck + and an Arduino Uno running
[examples/arduino](../arduino/arduino.ino).

```
pip install regtable streamdeck pillow
python examples/streamdeck/streamdeck_panel.py -p COM6
python examples/streamdeck/streamdeck_panel.py -p COM6 --yaml tools/uno.yaml   # typed, verified first
python examples/streamdeck/streamdeck_panel.py --pipe ./example               # no board: the desktop CLI
```

What appears:

- one key per register, name and value, refreshed from the device 4 times
  a second; BOOL RW keys toggle on press and turn green when true
- more registers than keys: pages, with the top-right key as previous and
  the bottom-right key as next
- Stream Deck +: the numeric RW registers of the current page go on the
  dials in page order; turning adds about 1/100 of the range per tick,
  pushing sets zero (or the minimum); the touch strip above each dial
  shows the name, the value, and a bar, and a tap on a segment does what
  pushing its dial does. A FLOAT register without a range gets a key but
  no dial: the panel invents no physical range for a device
- swiping the touch strip left or right turns the page

The deck shows state and the device owns it. A press or a turn writes a
register; what the key shows next is read back from the device, so a
refused write (out of range, read-only) leaves the old value on the key.
Anything else that changes a register, a pot on A0, firmware, another
interface, shows up on the next poll.

## Setup

python-elgato-streamdeck talks to the deck over HID directly:

- close the Elgato Stream Deck application while the script runs; it
  holds the device
- Windows: put `hidapi.dll` (x64, from the `hidapi-win.zip` of the
  [libusb/hidapi releases](https://github.com/libusb/hidapi/releases))
  in a directory on `PATH`
- Linux: `apt install libhidapi-libusb0` and a udev rule for the deck
  (see the library's documentation); macOS: `brew install hidapi`

## Concurrency

The HID callbacks (key, dial) run on the library's reader thread; the
poll loop runs on the main thread. The regtable client is not thread-safe and the serial wire carries no request ids, so every access to the device
goes through one lock. Without it two in-flight commands could pair with
each other's answers, and the client would close the connection on the
first mismatch, by design.
