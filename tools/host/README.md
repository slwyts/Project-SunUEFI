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

The initial guest run on2026-10-08 captured a3200×2136 PNG with sRGB caps and a
successful Stop reply. Input commands still require observing their effect;
a completed D-Bus call alone does not establish the resulting UI state.
