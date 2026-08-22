#!/usr/bin/env python3
"""A Stream Deck as a physical panel for any regtable device.

    pip install regtable streamdeck pillow
    python examples/streamdeck/streamdeck_panel.py -p COM6
    python examples/streamdeck/streamdeck_panel.py -p COM6 --yaml tools/uno.yaml
    python examples/streamdeck/streamdeck_panel.py --pipe ./example   # no hardware: the desktop CLI

No per-device configuration: the panel asks the device for its table
(list --json, the same self-description the Web panel uses) and lays
the registers out in table order, one per key. With --yaml the typed
client is built from the YAML and verified against the device first.

Layout rules:
    every register        one key: name and value, refreshed from the
                          device every POLL seconds; BOOL RW keys
                          toggle on press and show green when true
    more than fit         pages; top-right key = previous page,
                          bottom-right key = next page
    numeric RW on a page  the dials, in page order (Stream Deck +):
                          turn adds ticks * step, push sets zero (or
                          the minimum); the touch strip above each
                          dial shows name, value, and a bar; a tap on
                          a segment does what pushing its dial does.
                          A FLOAT without a range gets no dial: the
                          panel invents no physical range for a device
    touch strip swipe     left = next page, right = previous page

The Stream Deck shows state, the device owns it: a press or a turn
writes a register, the display then shows what the device read back,
so a refused write shows the old value, not the attempted one.

Needs python-elgato-streamdeck, which talks to the deck over HID
directly: close the Elgato Stream Deck application first (it holds
the device); on Windows put hidapi.dll (x64, from libusb/hidapi
releases, hidapi-win.zip) in a directory on PATH; on Linux install
libhidapi-libusb0 and a udev rule for the deck. The client is not
thread-safe and the wire has no request ids, so every access to `dev`
goes through one lock: the HID callbacks and the poll loop never
interleave on the port.
"""

import argparse
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "python"))               # checkout without pip install

from regtable import (RegtableClient, SerialTransport, PipeTransport,  # noqa: E402
                      build_client, GenerationError, TransportError, SchemaDriftError)
from regtable.client import DOMAIN  # noqa: E402

try:
    from PIL import ImageDraw, ImageFont
    from StreamDeck.DeviceManager import DeviceManager
    from StreamDeck.Devices.StreamDeck import DialEventType, TouchscreenEventType
    from StreamDeck.ImageHelpers import PILHelper
except ImportError as e:
    sys.exit(f"{e}\npip install streamdeck pillow")

POLL = 0.25                                             # seconds between device snapshots
NUMERIC = set(DOMAIN) | {"FLOAT"}


# -- layout: registers -> keys and dials ---------------------------------- #

def bounds(entry):
    """The range a dial sweeps: the register's own if it has one, else
    the integer type's domain. None for a FLOAT without a range: the
    device said nothing about it and the panel invents nothing."""
    if entry["min"] is not None:
        return entry["min"], entry["max"]
    if entry["type"] == "FLOAT":
        return None
    return DOMAIN[entry["type"]]


def dialable(entry):
    return entry["perm"] == "RW" and entry["type"] in NUMERIC and bounds(entry) is not None


def dial_step(entry):
    """About 100 ticks from one end of the range to the other, at least 1
    for integers."""
    lo, hi = bounds(entry)
    if entry["type"] == "FLOAT":
        return (hi - lo) / 100
    return max(1, (hi - lo) // 100)


class Layout:
    """Pages of registers over the deck's keys; the numeric RW ones
    on each page get the dials."""

    def __init__(self, schema, key_count, key_cols, dial_count):
        if key_count < 1:
            raise ValueError("at least one key")
        self.names = list(schema)
        self.schema = schema
        self.dial_count = dial_count
        if len(self.names) <= key_count:
            self.per_page = key_count
            self.prev_key = self.next_key = None
        else:
            if key_count < 3:
                raise ValueError(f"{len(self.names)} registers do not fit {key_count} keys, "
                                 "and paging needs 2 navigation keys plus one")
            self.per_page = key_count - 2
            self.next_key = key_count - 1                 # the last active key
            # the top-right key when the active keys span a full row, else the one before last
            self.prev_key = key_cols - 1 if key_cols - 1 < self.next_key else self.next_key - 1
        self.pages = max(1, -(-len(self.names) // self.per_page))
        self.page = 0

    def page_names(self):
        start = self.page * self.per_page
        return self.names[start:start + self.per_page]

    def keys(self):
        """key index -> register name on the current page."""
        out = {}
        slot = 0
        for n in self.page_names():
            while slot in (self.prev_key, self.next_key):
                slot += 1
            out[slot] = n
            slot += 1
        return out

    def dials(self):
        """dial index -> register name: numeric RW registers of the page."""
        numeric = [n for n in self.page_names() if dialable(self.schema[n])]
        return dict(enumerate(numeric[:self.dial_count]))

    def turn(self, delta):
        self.page = (self.page + delta) % self.pages


# -- drawing ---------------------------------------------------------------- #

def fmt(value):
    if value is None:
        return "-"
    if isinstance(value, bool):
        return "ON" if value else "OFF"
    if isinstance(value, float):
        return f"{value:.4g}"
    return str(value)


def font(size):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:                                   # Pillow < 10.1
        return ImageFont.load_default()


def key_image(deck, name, entry, value):
    img = PILHelper.create_key_image(deck)
    d = ImageDraw.Draw(img)
    w, h = img.size
    toggle = entry["type"] == "BOOL" and entry["perm"] == "RW"
    bg = (30, 120, 60) if toggle and value is True else (28, 28, 32)
    d.rectangle((0, 0, w, h), fill=bg)
    name_color = (150, 170, 200) if entry["perm"] == "RO" else (230, 230, 230)
    d.text((w / 2, h * 0.16), name, font=font(max(12, h // 7)), fill=name_color, anchor="mm")
    d.text((w / 2, h * 0.55), fmt(value), font=font(max(16, h // 4)), fill=(255, 255, 255), anchor="mm")
    if toggle:
        d.text((w / 2, h * 0.86), "press: toggle", font=font(max(10, h // 10)),
               fill=(180, 180, 180), anchor="mm")
    return PILHelper.to_native_key_format(deck, img)


def nav_image(deck, text, page, pages):
    img = PILHelper.create_key_image(deck)
    d = ImageDraw.Draw(img)
    w, h = img.size
    d.rectangle((0, 0, w, h), fill=(40, 40, 60))
    d.text((w / 2, h * 0.4), text, font=font(max(16, h // 4)), fill=(255, 200, 80), anchor="mm")
    d.text((w / 2, h * 0.78), f"{page + 1}/{pages}", font=font(max(12, h // 7)),
           fill=(200, 200, 200), anchor="mm")
    return PILHelper.to_native_key_format(deck, img)


def strip_image(deck, dials, schema, values):
    """The touch strip: one segment above each dial."""
    img = PILHelper.create_touchscreen_image(deck)
    d = ImageDraw.Draw(img)
    w, h = img.size
    seg = w // max(1, deck.dial_count())
    for i in range(deck.dial_count()):
        x0 = i * seg
        n = dials.get(i)
        if n is None:
            continue
        lo, hi = bounds(schema[n])
        v = values.get(n)
        d.text((x0 + seg / 2, h * 0.22), n, font=font(20), fill=(230, 230, 230), anchor="mm")
        d.text((x0 + seg / 2, h * 0.56), fmt(v), font=font(26), fill=(255, 255, 255), anchor="mm")
        d.rectangle((x0 + 16, h - 18, x0 + seg - 16, h - 8), outline=(120, 120, 130))
        if v is not None and hi > lo:
            frac = min(1.0, max(0.0, (v - lo) / (hi - lo)))
            d.rectangle((x0 + 16, h - 18, x0 + 16 + (seg - 32) * frac, h - 8), fill=(80, 180, 255))
    return PILHelper.to_native_touchscreen_format(deck, img)


# -- main ------------------------------------------------------------------- #

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("-p", "--port", help="serial port, e.g. COM6 or /dev/ttyUSB0")
    g.add_argument("--pipe", nargs=argparse.REMAINDER, metavar="CMD...",
                   help="a regtable CLI process over stdin/stdout; place it last")
    ap.add_argument("--yaml", help="typed client from this YAML, verified against the device")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--keys", type=int, metavar="N",
                    help="use only the first N keys (to try the paging with a small table)")
    args = ap.parse_args()
    if args.pipe is not None and not args.pipe:
        ap.error("--pipe needs a command after it")
    if args.keys is not None and args.keys < 1:
        ap.error("--keys must be a positive number")
    where = args.port or " ".join(args.pipe)

    # the YAML first: a bad description touches neither the deck nor the port
    try:
        cls = build_client(args.yaml) if args.yaml else None
    except GenerationError as e:
        sys.exit(f"{args.yaml}: {e}")

    decks = DeviceManager().enumerate()
    if not decks:
        sys.exit("no Stream Deck found (is the Elgato application still running?)")
    deck = decks[0]
    deck.open()
    deck.reset()
    deck.set_brightness(60)
    rows, cols = deck.key_layout()
    print(f"{deck.deck_type()}: {deck.key_count()} keys ({rows}x{cols}), {deck.dial_count()} dials")

    try:
        t = SerialTransport(args.port, args.baud) if args.port else PipeTransport(args.pipe)
        dev = cls(t) if cls else RegtableClient.discover(t)
    except (TransportError, SchemaDriftError) as e:
        deck.reset(); deck.close()
        sys.exit(str(e))
    schema = type(dev).__schema__
    print(f"{dev!r} on {where}")
    key_count = min(deck.key_count(), args.keys) if args.keys else deck.key_count()
    try:
        layout = Layout(schema, key_count, cols, deck.dial_count())
    except ValueError as e:
        dev.close(); deck.reset(); deck.close()
        sys.exit(str(e))
    print(f"{len(schema)} registers on {layout.pages} page(s)")

    lock = threading.Lock()                              # one owner of the wire at a time
    stop = threading.Event()
    current = {}                                         # latest value seen per register
    shown = {}                                           # what each key / the strip displays

    def paint(values=None, force=False):
        """Merge fresh values in, repaint what changed (everything on a
        page turn). The strip is always drawn from the full set, so a
        partial refresh never blanks a neighbour."""
        if values:
            current.update(values)
        keys, dials = layout.keys(), layout.dials()
        for k in range(deck.key_count()):
            n = keys.get(k)
            if n is not None:
                want = ("reg", n, current.get(n))
            elif k == layout.prev_key:
                want = ("nav", "< prev", layout.page)
            elif k == layout.next_key:
                want = ("nav", "next >", layout.page)
            else:
                want = ("blank",)
            if force or shown.get(k) != want:
                if want[0] == "reg":
                    deck.set_key_image(k, key_image(deck, n, schema[n], current.get(n)))
                elif want[0] == "nav":
                    deck.set_key_image(k, nav_image(deck, want[1], layout.page, layout.pages))
                else:
                    deck.set_key_image(k, None)
                shown[k] = want
        if deck.dial_count():
            want = ("strip", tuple((i, n, current.get(n)) for i, n in dials.items()))
            if force or shown.get("strip") != want:
                fmt_ = deck.touchscreen_image_format()["size"]
                deck.set_touchscreen_image(strip_image(deck, dials, schema, current), 0, 0, *fmt_)
                shown["strip"] = want

    def readback(names):
        """Read the named registers and paint them: after a write the
        display shows what the device holds, not what was sent."""
        paint({n: dev[n] for n in names})

    def write(name, value):
        try:
            dev[name] = value
        except TransportError as e:
            print(e); stop.set(); return
        except Exception as e:                           # refused: RemoteError, ValueError
            print(f"{name}: {e}")
        readback([name])

    def on_key(_deck, k, pressed):
        if not pressed:
            return
        with lock:
            if k == layout.prev_key:
                layout.turn(-1); paint(force=True); return
            if k == layout.next_key:
                layout.turn(+1); paint(force=True); return
            n = layout.keys().get(k)
            if n is None:
                return
            e = schema[n]
            if e["type"] == "BOOL" and e["perm"] == "RW":
                try:
                    write(n, not dev[n])
                except TransportError as err:
                    print(err); stop.set()

    def zero(n):
        """Dial push and strip tap: zero if the range allows, else the minimum."""
        e = schema[n]
        lo, hi = bounds(e)
        z = 0 if lo <= 0 <= hi else lo
        write(n, float(z) if e["type"] == "FLOAT" else int(z))

    def on_dial(_deck, i, ev, value):
        with lock:
            n = layout.dials().get(i)
            if n is None:
                return
            e = schema[n]
            lo, hi = bounds(e)
            try:
                if ev == DialEventType.TURN:
                    cur = dev[n]
                    new = min(hi, max(lo, cur + value * dial_step(e)))
                    if e["type"] != "FLOAT":
                        new = int(new)
                    if new != cur:
                        write(n, new)
                elif ev == DialEventType.PUSH and value:
                    zero(n)
            except TransportError as err:
                print(err); stop.set()

    SWIPE = 60                                           # px of horizontal travel that counts

    def on_touch(_deck, ev, value):
        with lock:
            if ev == TouchscreenEventType.DRAG:
                dx = value["x_out"] - value["x"]
                if dx <= -SWIPE:
                    layout.turn(+1); paint(force=True)   # swipe left: next page
                elif dx >= SWIPE:
                    layout.turn(-1); paint(force=True)   # swipe right: previous page
            elif ev == TouchscreenEventType.SHORT:
                seg = deck.touchscreen_image_format()["size"][0] // max(1, deck.dial_count())
                n = layout.dials().get(value["x"] // seg)
                if n is not None:
                    try:
                        zero(n)
                    except TransportError as err:
                        print(err); stop.set()

    deck.set_key_callback(on_key)
    deck.set_dial_callback(on_dial)
    deck.set_touchscreen_callback(on_touch)
    try:
        while not stop.is_set():
            with lock:
                snap = dev.snapshot()
                paint(snap)
            time.sleep(POLL)
    except TransportError as e:
        print(e)
    except KeyboardInterrupt:
        pass
    finally:
        with lock:
            dev.close()
        deck.reset()
        deck.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
