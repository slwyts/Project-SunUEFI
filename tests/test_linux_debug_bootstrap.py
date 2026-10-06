from pathlib import Path
import os,subprocess,tempfile,unittest,json,shutil
ROOT=Path(__file__).resolve().parents[1];SCRIPT=ROOT/'bootprofiles/linux-userspace/piano-debug-bootstrap'
class LinuxDebugBootstrapTests(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory(prefix='linux-debug-fixture-');self.base=Path(self.tmp.name);self.fs=self.base/'fs';self.bin=self.base/'bin';self.bin.mkdir();self.trace=self.base/'commands'
  for path in('proc','proc/sys/kernel/random','sys/kernel/config/usb_gadget','sys/class/udc/udc0','sys/class/usb_role/role0','sys/class/net/usb0','sys/devices/platform/soc/usb/dwc3','sys/bus/platform/drivers/dwc3','sys/devices/system/cpu','run','dev'):(self.fs/path).mkdir(parents=True,exist_ok=True)
  (self.fs/'proc/cmdline').write_text('piano.debug_usb=acm-ncm');(self.fs/'proc/mounts').write_text('rootfs / rootfs rw 0 0\nnone /sys/kernel/config configfs rw 0 0\n');(self.fs/'proc/filesystems').write_text('nodev\tconfigfs\n');(self.fs/'proc/sys/kernel/random/boot_id').write_text('fixture-boot\n');(self.fs/'sys/devices/system/cpu/online').write_text('0-7\n')
  u=self.fs/'sys/class/udc/udc0';(u/'state').write_text('not attached\n');(u/'function').write_text('');(u/'current_speed').write_text('UNKNOWN\n');(u/'device').symlink_to(self.fs/'sys/devices/platform/soc/usb/dwc3');(self.fs/'sys/devices/platform/soc/usb/dwc3/driver').symlink_to(self.fs/'sys/bus/platform/drivers/dwc3')
  r=self.fs/'sys/class/usb_role/role0';(r/'role').write_text('device\n');(r/'device').symlink_to(self.fs/'sys/devices/platform/soc/usb');(self.fs/'dev/ttyGS0').symlink_to('/dev/null')
  wrapper='''#!/usr/bin/env python3
import os,sys,pathlib,subprocess,shutil
name=pathlib.Path(sys.argv[0]).name;args=sys.argv[1:]
with open(os.environ['TRACE'],'a')as f:f.write(name+' '+repr(args)+'\\n')
if name=='id':print('0');sys.exit()
if name in('ip','sleep','reboot','agetty'):sys.exit(0)
if name=='mount':sys.exit(0)
if name=='mkdir':
 if os.environ.get('FAIL_FUNCTION')=='1'and any(x.endswith('functions/ncm.usb0')for x in args):sys.exit(1)
 subprocess.run(['/usr/bin/mkdir',*args],check=True)
 for a in args:
  if a.startswith('-'):continue
  p=pathlib.Path(a)
  if p.name=='piano-linux-debug':
   for d in('configs','functions','strings'):(p/d).mkdir(exist_ok=True)
   for f in('idVendor','idProduct','bcdUSB','bcdDevice','bDeviceClass','bDeviceSubClass','bDeviceProtocol','UDC'):(p/f).touch()
  elif p.name=='0x409':
   for f in(('configuration',)if '/configs/'in str(p)else('manufacturer','product','serialnumber')):(p/f).touch()
  elif p.name=='c.1':(p/'strings').mkdir(exist_ok=True);(p/'MaxPower').touch()
  elif p.name=='ncm.usb0':
   for f in('dev_addr','host_addr'):(p/f).touch()
   (p/'ifname').write_text('usb0\\n')
   root=pathlib.Path(os.environ['PIANO_DEBUG_FIXTURE_ROOT'])
   if os.environ.get('CHANGE_DR_MODE'):
    (root/'sys/devices/platform/soc/usb/dwc3/of_node/dr_mode').write_bytes(os.environ['CHANGE_DR_MODE'].encode()+b'\\0')
   if os.environ.get('CHANGE_ROLE'):
    role=root/'sys/class/usb_role/role0';role.mkdir(exist_ok=True)
    (role/'role').write_text(os.environ['CHANGE_ROLE'])
    if not (role/'device').exists():(role/'device').symlink_to(root/'sys/devices/platform/soc/usb')
 sys.exit()
if name=='rmdir':
 for a in args:
  p=pathlib.Path(a)
  for child in list(p.iterdir()):
   if child.is_file()and not child.is_symlink():child.unlink()
   elif child.is_dir()and child.name in('strings','configs','functions')and not any(child.iterdir()):child.rmdir()
  p.rmdir()
 sys.exit()
sys.exit(1)
'''
  for name in('mkdir','rmdir','mount','id','ip','sleep','reboot','agetty'):
   p=self.bin/name;p.write_text(wrapper);p.chmod(0o755)
  self.env={**os.environ,'PATH':str(self.bin)+':'+os.environ['PATH'],'PIANO_DEBUG_FIXTURE_ROOT':str(self.fs),'TRACE':str(self.trace)}
 def tearDown(self):self.tmp.cleanup()
 def run_script(self,action='start',ok=True):
  r=subprocess.run(['sh',str(SCRIPT),action],env=self.env,capture_output=True,text=True);self.assertEqual(r.returncode,0 if ok else 1,r.stdout+r.stderr);self.assertIn('evidence=fixture',r.stdout);return r.stdout
 def commands(self):return self.trace.read_text()if self.trace.exists()else''
 def config(self,text):
  path=self.fs/'etc/piano/linux-debug.conf';path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text)
 def fixed_peripheral(self,driver='dwc3-qcom',mode='peripheral',keep_role=False):
  controller=self.fs/'sys/devices/platform/soc/usb/dwc3'
  link=controller/'driver';link.unlink();target=self.fs/'sys/bus/platform/drivers'/driver;target.mkdir(exist_ok=True);link.symlink_to(target)
  node=self.fs/'sys/firmware/devicetree/base/soc/usb';node.mkdir(parents=True)
  (controller/'of_node').symlink_to(node)
  if mode is not None:(node/'dr_mode').write_bytes(mode.encode()+b'\0')
  if not keep_role:shutil.rmtree(self.fs/'sys/class/usb_role/role0')
 def test_fixed_peripheral_supports_generic_and_flat_qcom_drivers(self):
  self.fixed_peripheral()
  for driver in('dwc3-qcom','dwc3'):
   with self.subTest(driver=driver):
    link=self.fs/'sys/devices/platform/soc/usb/dwc3/driver';link.unlink();link.symlink_to(self.fs/'sys/bus/platform/drivers'/driver)
    self.assertIn('gadget_bound udc=udc0 role=device',self.run_script());self.run_script('stop')
 def test_fixed_peripheral_requires_explicit_dt_mode(self):
  self.fixed_peripheral(mode=None);mode=self.fs/'sys/devices/platform/soc/usb/dwc3/of_node/dr_mode'
  for value in(None,'host','otg',''):
   with self.subTest(mode=value):
    if value is not None:mode.write_bytes(value.encode()+b'\0')
    self.assertIn('device_role_unproven_or_ambiguous',self.run_script(ok=False))
    self.assertFalse((self.fs/'sys/kernel/config/usb_gadget/piano-linux-debug').exists())
 def test_fixed_peripheral_cannot_override_existing_host_role(self):
  self.fixed_peripheral(keep_role=True);role=self.fs/'sys/class/usb_role/role0/role';role.write_text('host\n')
  self.assertIn('role_not_device',self.run_script(ok=False));self.assertEqual(role.read_text(),'host\n')
  (self.fs/'proc/cmdline').write_text('piano.debug_usb=acm-ncm piano.debug_role=missing')
  self.assertIn('device_role_unproven_or_ambiguous',self.run_script(ok=False))
  self.assertFalse((self.fs/'sys/kernel/config/usb_gadget/piano-linux-debug').exists())
 def test_fixed_peripheral_does_not_ignore_explicit_role_selection(self):
  self.fixed_peripheral();(self.fs/'proc/cmdline').write_text('piano.debug_usb=acm-ncm piano.debug_role=missing')
  self.assertIn('device_role_unproven_or_ambiguous',self.run_script(ok=False))
 def test_fixed_peripheral_does_not_override_ambiguous_roles(self):
  self.fixed_peripheral(keep_role=True);role=self.fs/'sys/class/usb_role/role1';role.mkdir();(role/'role').write_text('device');(role/'device').symlink_to(self.fs/'sys/devices/platform/soc/usb')
  self.assertIn('device_role_unproven_or_ambiguous',self.run_script(ok=False))
 def test_fixed_peripheral_mode_is_rechecked_before_bind(self):
  self.fixed_peripheral();self.env['CHANGE_DR_MODE']='host'
  self.assertIn('role_changed_before_bind',self.run_script(ok=False));self.assertFalse((self.fs/'sys/kernel/config/usb_gadget/piano-linux-debug').exists())
 def test_fixed_peripheral_refuses_new_role_provider_before_bind(self):
  self.fixed_peripheral();self.env['CHANGE_ROLE']='host'
  self.assertIn('role_changed_before_bind',self.run_script(ok=False));self.assertFalse((self.fs/'sys/kernel/config/usb_gadget/piano-linux-debug').exists())
 def test_existing_role_is_rechecked_before_bind(self):
  self.env['CHANGE_ROLE']='host'
  self.assertIn('role_changed_before_bind',self.run_script(ok=False));self.assertFalse((self.fs/'sys/kernel/config/usb_gadget/piano-linux-debug').exists())
 def test_real_config_works_with_forced_kernel_cmdline_and_cmdline_overrides(self):
  self.config('usb=acm-ncm\nshell=1\nipv4=192.168.77.1/30\nrecovery_seconds=0\n')
  (self.fs/'proc/cmdline').write_text('piano.root=ram')
  result=self.run_script();self.assertIn('gadget_bound',result);self.assertIn('ip ',self.commands())
  self.run_script('stop')
  (self.fs/'proc/cmdline').write_text('piano.root=ram piano.debug_usb=off')
  result=self.run_script();self.assertIn('usb_disabled',result)
 def test_config_is_data_and_rejects_unknown_or_executable_values(self):
  marker=self.base/'never-execute'
  (self.fs/'proc/cmdline').write_text('piano.root=ram')
  self.config('usb=$(touch '+str(marker)+')\n')
  self.run_script(ok=False);self.assertFalse(marker.exists());self.assertNotIn('mkdir ',self.commands())
  self.config('unknown=value\n');self.run_script(ok=False)
 def test_config_preserves_unique_real_role_requirement(self):
  self.config('usb=acm-ncm\nshell=1\n');(self.fs/'proc/cmdline').write_text('piano.root=ram')
  (self.fs/'sys/class/usb_role/role0/role').write_text('host')
  result=self.run_script(ok=False);self.assertIn('role_not_device',result)
  self.assertFalse((self.fs/'sys/kernel/config/usb_gadget/piano-linux-debug').exists())
 def test_default_diagnostics_no_writes_or_timer(self):
  (self.fs/'proc/cmdline').write_text('');s=self.run_script('diagnose');self.assertIn('diagnostics_ready configured=0',s);self.assertFalse(self.trace.exists());s=self.run_script();self.assertIn('usb_disabled',s);self.assertNotIn('sleep',self.commands());self.assertFalse((self.fs/'sys/kernel/config/usb_gadget/piano-linux-debug').exists())
 def test_explicit_acm_ncm_bind_and_owned_stop(self):
  s=self.run_script();self.assertIn('gadget_bound udc=udc0 role=device ifname=usb0',s);self.assertIn('enumeration_verified=0',s);g=self.fs/'sys/kernel/config/usb_gadget/piano-linux-debug';self.assertEqual((g/'UDC').read_text().strip(),'udc0');self.assertTrue((g/'configs/c.1/acm.usb0').is_symlink());self.assertTrue((g/'configs/c.1/ncm.usb0').is_symlink());self.assertNotIn('mass_storage',str(list(g.rglob('*'))));self.run_script('stop');self.assertFalse(g.exists())
 def test_no_udc_or_ambiguous_refused(self):
  shutil=__import__('shutil');shutil.rmtree(self.fs/'sys/class/udc/udc0');self.assertIn('udc_selection_ambiguous_or_absent',self.run_script(ok=False));self.assertFalse(self.trace.exists()and'usb_gadget'in self.commands())
 def test_host_role_refused_without_role_write(self):
  role=self.fs/'sys/class/usb_role/role0/role';role.write_text('host\n');s=self.run_script(ok=False);self.assertIn('role_not_device',s);self.assertEqual(role.read_text(),'host\n');self.assertFalse((self.fs/'sys/kernel/config/usb_gadget/piano-linux-debug').exists())
 def test_unknown_or_unrelated_role_refused(self):
  (self.fs/'sys/class/usb_role/role0/device').unlink();self.assertIn('device_role_unproven_or_ambiguous',self.run_script(ok=False))
 def test_unknown_provider_and_state_refused(self):
  link=self.fs/'sys/devices/platform/soc/usb/dwc3/driver';link.unlink();link.symlink_to(self.fs/'sys/bus/platform/drivers/unknown');self.assertIn('unsupported_udc_driver',self.run_script(ok=False));link.unlink();link.symlink_to(self.fs/'sys/bus/platform/drivers/dwc3');(self.fs/'sys/class/udc/udc0/state').write_text('unknown-state');self.assertIn('udc_state_unknown',self.run_script(ok=False))
 def test_invalid_controller_path_and_recovery_limit(self):
  (self.fs/'proc/cmdline').write_text('piano.debug_usb=acm-ncm piano.debug_udc=../../anything');self.assertIn('invalid_udc_name',self.run_script(ok=False));(self.fs/'proc/cmdline').write_text('piano.recovery_seconds=999999');self.assertIn('recovery_seconds_too_large',self.run_script('recover',ok=False));self.assertNotIn('reboot',self.commands())
 def test_configfs_missing_and_busy_udc_refused(self):
  (self.fs/'proc/filesystems').write_text('nodev proc\n');self.assertIn('configfs_not_available',self.run_script(ok=False));(self.fs/'proc/filesystems').write_text('nodev configfs\n');(self.fs/'sys/class/udc/udc0/function').write_text('foreign-adb');self.assertIn('udc_already_in_use',self.run_script(ok=False))
 def test_non_ram_root_refused(self):
  (self.fs/'proc/mounts').write_text('/dev/sda1 / ext4 rw 0 0\n');self.assertIn('ram_root_required',self.run_script(ok=False));self.assertFalse(self.trace.exists())
 def test_run_on_persistent_mount_refused(self):
  (self.fs/'proc/mounts').write_text('rootfs / rootfs rw 0 0\n/dev/sda1 /run ext4 rw 0 0\n');self.assertIn('ram_state_directory_required',self.run_script(ok=False));self.assertFalse(self.trace.exists())
 def test_failed_function_rolls_back_own_gadget(self):
  self.env['FAIL_FUNCTION']='1';self.assertIn('setup_failed',self.run_script(ok=False));self.assertFalse((self.fs/'sys/kernel/config/usb_gadget/piano-linux-debug').exists())
 def test_existing_gadget_not_overwritten(self):
  g=self.fs/'sys/kernel/config/usb_gadget/piano-linux-debug';g.mkdir();(g/'sentinel').write_text('preserve');self.assertIn('gadget_directory_already_exists',self.run_script(ok=False));self.assertEqual((g/'sentinel').read_text(),'preserve')
 def test_explicit_recovery_only(self):
  self.assertIn('recovery_disabled',self.run_script('recover'));self.assertNotIn('reboot',self.commands());(self.fs/'proc/cmdline').write_text('piano.recovery_seconds=2');self.assertIn('recovery_armed seconds=2',self.run_script('recover'));self.assertIn('sleep',self.commands());self.assertIn('reboot',self.commands())
 def test_ncm_address_is_explicit_and_no_default_route(self):
  (self.fs/'proc/cmdline').write_text('piano.debug_usb=acm-ncm piano.debug_ipv4=192.168.77.1/30');self.run_script();commands=self.commands();self.assertIn("ip ['link', 'set', 'dev', 'usb0', 'up']",commands);self.assertIn("ip ['address', 'add', '192.168.77.1/30', 'dev', 'usb0']",commands);self.assertNotIn('route',commands)
 def test_shell_is_explicit_and_requires_owned_gadget(self):
  self.run_script();self.assertIn('serial_shell_not_requested',self.run_script('serial',ok=False));(self.fs/'proc/cmdline').write_text('piano.debug_shell=1');self.assertIn('serial_shell_start',self.run_script('serial'));self.assertIn('agetty',self.commands())
 def test_script_scope_has_no_storage_module_or_mmio_actions(self):
  source=SCRIPT.read_text();self.assertNotIn('modprobe ',source);self.assertNotIn('insmod ',source);self.assertNotIn('/dev/mem',source);self.assertNotIn('soft_connect',source.split('# Never request')[0]);subprocess.run(['sh','-n',str(SCRIPT)],check=True)
if __name__=='__main__':unittest.main()
