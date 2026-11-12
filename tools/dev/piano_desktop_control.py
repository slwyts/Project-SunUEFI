#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""One-shot local Mutter desktop capture/input client, run as the guest session user.

Uses RemoteDesktop Version1 + ScreenCast Version4. No persistent daemon, RDP,
Shell Eval, shell command execution, password changes or touch FIFO access.
Official ABI: GNOME/mutter48.4 data/dbus-interfaces/{RemoteDesktop,ScreenCast}.xml.
"""
import argparse
import json
import math
import os
from pathlib import Path
import signal
import struct
import sys
import time

RD='org.gnome.Mutter.RemoteDesktop'
SC='org.gnome.Mutter.ScreenCast'
RD_PATH='/org/gnome/Mutter/RemoteDesktop'
SC_PATH='/org/gnome/Mutter/ScreenCast'


def arguments():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--monitor',default='',help='connector name; empty selects the default active monitor')
    p.add_argument('--screenshot',type=Path,help='save one PNG after the specified input')
    p.add_argument('--move',nargs=2,type=float,metavar=('X','Y'),help='absolute pointer in captured-stream pixels')
    p.add_argument('--click',choices=('left','middle','right'),help='one press/release at the pointer position')
    p.add_argument('--keycodes',nargs='+',type=int,metavar='EVDEV',help='one evdev chord, released in reverse order')
    p.add_argument('--settle-ms',type=int,default=250,help='allow this many milliseconds for UI rendering after input')
    p.add_argument('--timeout',type=float,default=12,help='bounded stream/frame timeout in seconds')
    a=p.parse_args()
    if not any((a.screenshot,a.move,a.click,a.keycodes)):
        p.error('choose --screenshot and/or one pointer/keyboard operation')
    if a.click and a.move is None:
        p.error('--click requires --move so the target is explicit')
    if a.keycodes and (len(a.keycodes)>10 or any(k<1 or k>0x2ff for k in a.keycodes)):
        p.error('use at most10 valid evdev keycodes, not X11 keycodes')
    if not 0<=a.settle_ms<=3000 or not 1<=a.timeout<=30:
        p.error('settle must be0..3000ms and timeout1..30s')
    if a.move and any(not math.isfinite(v) or v<0 for v in a.move):
        p.error('pointer coordinates must be finite and nonnegative')
    return a


def gi_import():
    import gi
    gi.require_version('Gio','2.0')
    gi.require_version('Gst','1.0')
    from gi.repository import Gio,GLib,Gst
    Gst.init(None)
    return Gio,GLib,Gst


class Desktop:
    def __init__(self,Gio,GLib,Gst,args):
        self.Gio,self.GLib,self.Gst,self.args=Gio,GLib,Gst,args
        self.connection=None;self.remote=None;self.cast=None;self.stream=None
        self.pipeline=None;self.signal_id=None;self.node_id=None;self.closed=False
        self.pressed_keys=[];self.pressed_buttons=[];self.parameters={}
        self.cleanup_errors=[];self.remote_stop_ack=False

    def call(self,destination,path,interface,method,signature=None,values=()):
        parameters=self.GLib.Variant(signature,values) if signature else None
        return self.connection.call_sync(destination,path,interface,method,parameters,
                                         None,self.Gio.DBusCallFlags.NONE,5000,None)

    def property(self,destination,path,interface,name):
        result=self.call(destination,path,'org.freedesktop.DBus.Properties','Get',
                         '(ss)',(interface,name))
        return result.get_child_value(0).get_variant().unpack()

    def pump(self):
        context=self.GLib.MainContext.default()
        while context.pending():context.iteration(False)

    def wait(self,predicate,deadline,description):
        while not predicate():
            self.pump()
            if self.closed:raise RuntimeError('Mutter closed the session')
            if time.monotonic()>=deadline:raise TimeoutError(description)
            time.sleep(0.01)

    def open(self):
        address=os.environ.get('DBUS_SESSION_BUS_ADDRESS')
        if not address:
            address=self.Gio.dbus_address_get_for_bus_sync(self.Gio.BusType.SESSION,None)
        flags=(self.Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT|
               self.Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION)
        # A private connection owns BOTH sessions throughout the whole command.
        self.connection=self.Gio.DBusConnection.new_for_address_sync(address,flags,None,None)
        self.connection.set_exit_on_close(False)
        versions={'remote_desktop':self.property(RD,RD_PATH,RD,'Version'),
                  'screen_cast':self.property(SC,SC_PATH,SC,'Version')}
        if versions['remote_desktop']!=1 or versions['screen_cast']<4:
            raise RuntimeError('This client expects actual Mutter RD1/SC4 ABI: '+str(versions))
        self.remote=self.call(RD,RD_PATH,RD,'CreateSession').unpack()[0]
        session_id=self.property(RD,self.remote,RD+'.Session','SessionId')
        self.cast=self.call(SC,SC_PATH,SC,'CreateSession','(a{sv})',
                            ({'remote-desktop-session-id':self.GLib.Variant('s',session_id)},)).unpack()[0]
        self.stream=self.call(SC,self.cast,SC+'.Session','RecordMonitor','(sa{sv})',
                              (self.args.monitor,{'cursor-mode':self.GLib.Variant('u',1),
                                                 'is-recording':self.GLib.Variant('b',False)})).unpack()[0]
        self.parameters=self.property(SC,self.stream,SC+'.Stream','Parameters')
        def added(connection,sender,path,interface,member,parameters,user_data):
            self.node_id=parameters.unpack()[0]
        self.signal_id=self.connection.signal_subscribe(SC,SC+'.Stream','PipeWireStreamAdded',
                                    self.stream,None,self.Gio.DBusSignalFlags.NONE,added,None)
        self.closed_signal=self.connection.signal_subscribe(RD,RD+'.Session','Closed',
                                    self.remote,None,self.Gio.DBusSignalFlags.NONE,
                                    lambda *unused:setattr(self,'closed',True),None)
        # The linked ScreenCast session starts through RemoteDesktop.Start.
        self.call(RD,self.remote,RD+'.Session','Start')
        self.wait(lambda:self.node_id is not None,time.monotonic()+self.args.timeout,
                  'PipeWireStreamAdded not received')
        return versions

    def input(self):
        if self.args.move:
            self.call(RD,self.remote,RD+'.Session','NotifyPointerMotionAbsolute',
                      '(sdd)',(self.stream,*self.args.move))
            self.input_delay(0.05)
        if self.args.click:
            button={'left':0x110,'right':0x111,'middle':0x112}[self.args.click]
            self.call(RD,self.remote,RD+'.Session','NotifyPointerButton','(ib)',(button,True))
            self.pressed_buttons.append(button)
            self.input_delay(0.08)
            self.call(RD,self.remote,RD+'.Session','NotifyPointerButton','(ib)',(button,False))
            self.pressed_buttons.remove(button)
        if self.args.keycodes:
            for key in self.args.keycodes:
                self.call(RD,self.remote,RD+'.Session','NotifyKeyboardKeycode','(ub)',(key,True))
                self.pressed_keys.append(key)
            self.input_delay(0.08)
            for key in reversed(self.args.keycodes):
                self.call(RD,self.remote,RD+'.Session','NotifyKeyboardKeycode','(ub)',(key,False))
                self.pressed_keys.remove(key)
        deadline=time.monotonic()+self.args.settle_ms/1000
        while time.monotonic()<deadline:
            self.pump();time.sleep(0.01)

    def input_delay(self,seconds):
        # Let the compositor deliver focus/press before sending release.
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            self.pump();time.sleep(0.005)

    def screenshot(self,path):
        Gst=self.Gst
        pipeline=Gst.Pipeline.new('piano-one-frame')
        self.pipeline=pipeline
        elements=[Gst.ElementFactory.make(name,None) for name in
                  ('pipewiresrc','queue','videoconvert','pngenc','appsink')]
        if any(element is None for element in elements):
            raise RuntimeError('Missing actual GStreamer pipewire/queue/videoconvert/png/appsink plugin')
        source,queue,convert,encoder,sink=elements
        source.set_property('path',str(self.node_id));source.set_property('do-timestamp',True)
        queue.set_property('max-size-buffers',2)
        queue.set_property('max-size-bytes',0);queue.set_property('max-size-time',0)
        encoder.set_property('snapshot',True)
        sink.set_property('sync',False);sink.set_property('max-buffers',1)
        for element in elements:pipeline.add(element)
        for left,right in zip(elements,elements[1:]):
            if not left.link(right):raise RuntimeError('Unable to link GStreamer PNG capture')
        # Start the capture AFTER input and settle, so the first frame is current.
        if pipeline.set_state(Gst.State.PLAYING)==Gst.StateChangeReturn.FAILURE:
            raise RuntimeError('PipeWire PNG pipeline failed to start')
        bus=pipeline.get_bus();deadline=time.monotonic()+self.args.timeout
        while time.monotonic()<deadline:
            self.pump()
            if self.closed:raise RuntimeError('Mutter session closed during capture')
            sample=sink.emit('try-pull-sample',100*Gst.MSECOND)
            if sample:
                buffer=sample.get_buffer();data=bytes(buffer.extract_dup(0,buffer.get_size()))
                if len(data)<33 or data[:8]!=b'\x89PNG\r\n\x1a\n' or data[12:16]!=b'IHDR':
                    raise RuntimeError('Actual pipeline did not return a valid PNG header')
                width,height=struct.unpack('>II',data[16:24])
                if not width or not height:raise RuntimeError('Captured PNG has zero dimensions')
                path=path.resolve();path.parent.mkdir(parents=True,exist_ok=True)
                temporary=path.with_name(path.name+'.part-'+str(os.getpid()))
                try:
                    with temporary.open('xb') as file:file.write(data)
                    os.replace(temporary,path)
                finally:
                    if temporary.exists():temporary.unlink()
                return {'path':str(path),'bytes':len(data),'width':width,'height':height,
                        'caps':sample.get_caps().to_string()}
            message=bus.pop_filtered(Gst.MessageType.ERROR|Gst.MessageType.EOS)
            if message:
                if message.type==Gst.MessageType.ERROR:
                    error,debug=message.parse_error()
                    raise RuntimeError('GStreamer: '+str(error)+'; '+str(debug))
                raise RuntimeError('Capture ended before a PNG sample')
        raise TimeoutError('Timed out waiting for actual PNG frame')

    def close(self):
        if self.pipeline:
            try:self.pipeline.set_state(self.Gst.State.NULL)
            except Exception as error:self.cleanup_errors.append('pipeline close: '+str(error))
            self.pipeline=None
        if self.connection and self.remote:
            for key in reversed(self.pressed_keys):
                try:self.call(RD,self.remote,RD+'.Session','NotifyKeyboardKeycode','(ub)',(key,False))
                except Exception as error:self.cleanup_errors.append('key release: '+str(error))
            for button in self.pressed_buttons:
                try:self.call(RD,self.remote,RD+'.Session','NotifyPointerButton','(ib)',(button,False))
                except Exception as error:self.cleanup_errors.append('button release: '+str(error))
            try:
                self.call(RD,self.remote,RD+'.Session','Stop')
                self.remote_stop_ack=True
            except Exception as error:self.cleanup_errors.append('session Stop: '+str(error))
        if self.connection:
            try:
                if self.signal_id:self.connection.signal_unsubscribe(self.signal_id)
                if getattr(self,'closed_signal',None):self.connection.signal_unsubscribe(self.closed_signal)
            except Exception as error:self.cleanup_errors.append('signal cleanup: '+str(error))
            try:self.connection.close_sync(None)
            except Exception as error:self.cleanup_errors.append('owner connection close: '+str(error))
            self.connection=None


def main():
    args=arguments();Gio,GLib,Gst=gi_import();desktop=Desktop(Gio,GLib,Gst,args)
    result={'software_desktop_only':True}
    def interrupt(signum,frame):raise KeyboardInterrupt('signal '+str(signum))
    signal.signal(signal.SIGTERM,interrupt)
    try:
        result['versions']=desktop.open()
        result['stream_parameters']=desktop.parameters
        result['pipewire_node']=desktop.node_id
        desktop.input()
        if args.screenshot:result['screenshot']=desktop.screenshot(args.screenshot)
        result['status']='completed'
    except (Exception,KeyboardInterrupt) as error:
        result.update(status='failed',error=str(error))
    finally:
        desktop.close()
        result['session_stop_acknowledged']=desktop.remote_stop_ack
        if desktop.cleanup_errors:result['cleanup_errors']=desktop.cleanup_errors
    print(json.dumps(result,ensure_ascii=False,indent=2))
    return 0 if result['status']=='completed' and not desktop.cleanup_errors else 1

if __name__=='__main__':raise SystemExit(main())
