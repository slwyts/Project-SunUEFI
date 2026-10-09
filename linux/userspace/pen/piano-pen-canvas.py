#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Draw using standard Gtk tablet-tool coordinates, pressure and buttons."""
import json
import math

import gi

gi.require_version('Gtk', '4.0')
gi.require_version('Gdk', '4.0')
from gi.repository import Gdk, Gtk


class Canvas(Gtk.Application):
    def __init__(self):
        super().__init__(application_id='org.sunuefi.PenCanvas')
        self.connect('activate', self.activate)
        self.strokes = []
        self.current = None
        self.events = 0
        self.downs = 0

    def activate(self, application):
        window = Gtk.ApplicationWindow(application=application, title='手写笔画板')
        window.set_default_size(1300, 880)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        box.set_margin_top(18)
        box.set_margin_bottom(18)
        box.set_margin_start(22)
        box.set_margin_end(22)
        self.status = Gtk.Label(label='等待手写笔 · 轻划和稍用力划线，观察线宽')
        self.status.add_css_class('status')
        box.append(self.status)
        self.area = Gtk.DrawingArea()
        self.area.set_hexpand(True)
        self.area.set_vexpand(True)
        self.area.set_draw_func(self.draw)
        box.append(self.area)
        pointer = Gtk.EventControllerLegacy()
        pointer.connect('event', self.pointer_event)
        self.area.add_controller(pointer)
        gesture = Gtk.GestureStylus()
        gesture.set_stylus_only(True)
        gesture.set_button(1)
        gesture.connect('down', self.down)
        gesture.connect('motion', self.motion)
        gesture.connect('up', self.up)
        gesture.connect('proximity', self.proximity)
        self.area.add_controller(gesture)
        clear = Gtk.Button(label='清空画板')
        clear.set_size_request(200, 68)
        clear.connect('clicked', self.clear)
        box.append(clear)
        css = Gtk.CssProvider()
        css.load_from_string('.status { font-size:24px; } button { font-size:22px; }')
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        window.set_child(box)
        window.present()

    def pointer_event(self, controller, event):
        if event is None:
            return False
        device = event.get_device()
        if device is None:
            return False
        # Cursor policy belongs to the application, not to the input driver.
        # Restore the mouse cursor when a touchpad/mouse is used on the canvas.
        if event.get_event_type() == Gdk.EventType.PROXIMITY_OUT:
            self.area.set_cursor(None)
        elif device.get_source() == Gdk.InputSource.PEN:
            self.area.set_cursor_from_name('none')
        elif device.get_source() in (Gdk.InputSource.MOUSE, Gdk.InputSource.TOUCHPAD):
            self.area.set_cursor(None)
        return False

    def point(self, gesture, x, y):
        known, pressure = gesture.get_axis(Gdk.AxisUse.PRESSURE)
        if not known or not math.isfinite(pressure):
            self.status.set_label('已检测到笔 · 暂无有效压力')
            return None
        self.events += 1
        self.status.set_label(f'笔尖压力 {pressure * 100:.0f}% · 已画 {self.downs} 笔')
        return x, y, pressure

    def down(self, gesture, x, y):
        point = self.point(gesture, x, y)
        if point is None:
            return
        self.downs += 1
        self.current = [point]
        self.strokes.append(self.current)
        self.area.queue_draw()
        print(json.dumps({'event': 'real_stylus_down', 'x': x, 'y': y,
                          'pressure': point[2]}), flush=True)

    def motion(self, gesture, x, y):
        point = self.point(gesture, x, y)
        if point is not None and self.current is not None:
            self.current.append(point)
            self.area.queue_draw()

    def up(self, gesture, x, y):
        self.current = None
        print(json.dumps({'event': 'real_stylus_up', 'x': x, 'y': y,
                          'events': self.events, 'strokes': self.downs}), flush=True)

    def proximity(self, gesture, x, y):
        if self.current is None:
            self.status.set_label(f'笔在屏幕范围内 · 已画 {self.downs} 笔')

    def clear(self, button):
        self.strokes = []
        self.current = None
        self.area.queue_draw()

    def draw(self, area, context, width, height):
        context.set_source_rgb(1, 1, 1)
        context.paint()
        context.set_source_rgb(.86, .88, .90)
        context.rectangle(.5, .5, width - 1, height - 1)
        context.stroke()
        context.set_source_rgb(.10, .25, .37)
        context.set_line_cap(1)
        for stroke in self.strokes:
            for index, point in enumerate(stroke):
                previous = stroke[max(0, index - 1)]
                context.set_line_width(1.5 + max(0, min(1, point[2])) * 14)
                context.move_to(previous[0], previous[1])
                context.line_to(point[0] + .01, point[1] + .01)
                context.stroke()


if __name__ == '__main__':
    Canvas().run()
