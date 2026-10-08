# Local desktop control

Run `piano_desktop_control.py` inside the guest as the active GNOME session user,
with that session's `DBUS_SESSION_BUS_ADDRESS` and `XDG_RUNTIME_DIR`. It is a
one-shot client, not a service to enable. It uses the actual Mutter RD1/SC4 API,
PyGObject Gio/Gst and the installed PipeWire, videoconvert, png and appsink plugins.

```sh
python3 piano_desktop_control.py --screenshot /tmp/piano-desktop.png
python3 piano_desktop_control.py --move 1000 500 --click left --screenshot /tmp/after-click.png
python3 piano_desktop_control.py --keycodes 125 --screenshot /tmp/after-meta.png
```

`--move` uses coordinates from the PNG, not ScreenCast's logical `size` property.
`--keycodes` uses Linux evdev codes (125 is Meta); do not add the X11 offset8.
Each command owns its Gio connection until PNG completion, releases injected
keys/buttons, stops the linked session and closes the connection, including on
error. JSON reports actual PNG dimensions/caps and whether Stop was acknowledged.
No RDP, Shell Eval, shell execution, password change or touch FIFO is involved.

## Focus Pen Pro control observation

Run `piano_pen_control.py` on the tablet with Python GI and BlueZ. Supply the
address of the connected Focus Pen Pro; the tool discovers its actual adapter
and FE11/FE12 characteristics. It observes notifications without pairing or
changing THP scanning by default.

```sh
python3 piano_pen_control.py --address AA:BB:CC:DD:EE:FF --seconds 30
sudo python3 piano_pen_control.py --address AA:BB:CC:DD:EE:FF \
  --screen-on --query-state --forward-stationary --seconds 30
```

The explicit second command sends the OEM screen-on and state-query messages.
Only real `53 01 <state>` or `D2 01 <state>` responses with state 0 or 1 are
forwarded unchanged to `/proc/nvt_thp_pen_stationary`. This interface is provided
by the candidate `0019-nvt-pen-stationary.patch`; it is not yet part of the
default kernel. No state is invented when the pen does not respond. Logs use
`CLOCK_BOOTTIME`, matching THP frame timestamps. The tool releases its BlueZ
notification subscription on exit and does not own the touch FIFO or generate
input events. A successful response confirms control communication, not drawing
support. See [pen protocol](../../docs/devel/piano-pen-protocol.md).

`piano_gamma_probe.py` reads the current Mutter CRTCs and Gamma ramps by default.
Explicit `--probe identity|warm --crtc ACTUAL_ID --backup NEW_FILE` requests a short
standard Gamma change and restores the original ramps in `finally`. A failed
software readback/restore returns status2. `piano_gcv2_report.py` reads previously
captured DRM JSON and kernel text entirely offline. Neither a D-Bus reply nor a
compositor screenshot establishes the panel's hardware color result; use the
[GCv2 validation steps](../../docs/devel/piano-gcv2-validation.md).

The initial guest run on2026-10-08 captured a3200×2136 PNG with sRGB caps and a
successful Stop reply. Input commands still require observing their effect;
a completed D-Bus call alone does not establish the resulting UI state.
