#!/usr/bin/env python3
"""Prepare the pinned simple-init app for the local Mu/LLVM build, host only."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import urllib.request


def prepare_filesystem(si):
    """Keep disk URLs lossless and resolve the requested protocol, not a parent."""
    from prepare_product_pump import transform
    path = si / 'src/filesystem/volume/uefi.c'
    text = path.read_text()
    text = transform(text, 'if(!(txt=ConvertDevicePathToText(dp,TRUE,TRUE)))DONE(ENOMEM);',
                     'if(!(txt=ConvertDevicePathToText(dp,FALSE,FALSE)))DONE(ENOMEM);', str(path))
    path.write_text(text)

    path = si / 'src/filesystem/layer/uefi.c'
    text = path.read_text()
    text = transform(text, '\tEFI_DEVICE_PATH_PROTOCOL*dp;\n\tEFI_LOADED_IMAGE_PROTOCOL*li=NULL;\n',
                     '\tEFI_DEVICE_PATH_PROTOCOL*dp,*base;\n\tEFI_LOADED_IMAGE_PROTOCOL*li=NULL;\n', str(path))
    old = '''		st=gBS->LocateDevicePath(
			&gEfiDevicePathProtocolGuid,
			&dp,&hand
		);
'''
    new = '''		base=dp;
		st=gBS->LocateDevicePath(
			*type==TYPE_FILE?&gEfiSimpleFileSystemProtocolGuid:&gEfiDiskIoProtocolGuid,
			&dp,&hand
		);
		if(!EFI_ERROR(st)&&!IsDevicePathEnd(dp))st=EFI_NOT_FOUND;
		FreePool(base);
'''
    text = transform(text, old, new, str(path))
    text = transform(text, '\t\tmode|=EFI_FILE_MODE_CREATE;',
                     '\t\tmode|=EFI_FILE_MODE_CREATE|EFI_FILE_MODE_WRITE;', str(path))
    path.write_text(text)

    path = si / 'src/confd/file.c'
    text = path.read_text()
    old = '''		fs_seek(hand->file,0,SEEK_SET);
		r=fs_full_write(hand->file,hand->buff,hand->off);
		if(r!=0)tlog_warn("write config failed: %s",strerror(r));'''
    new = '''		r=fs_seek(hand->file,0,SEEK_SET);
		if(r==0)r=fs_full_write(hand->file,hand->buff,hand->off);
		if(r==0)r=fs_flush(hand->file);
		if(r!=0)tlog_warn("write config failed: %s",strerror(r));'''
    text = transform(text, old, new, str(path))
    text = transform(text, '\tfs_seek(hand->file,0,SEEK_SET);\n\tfs_set_size(hand->file,0);',
                     '\tr=fs_seek(hand->file,0,SEEK_SET);\n\tif(r==0)r=fs_set_size(hand->file,0);\n\tif(r!=0)goto done;', str(path))
    text = transform(text, '\t\tdo_close(hand,true);\n\t\tif(r==0)errno=0;',
                     '\t\tint written=do_close(hand,r==0);\n\t\tif(r==0)r=written;\n\t\terrno=r==0?0:(r>0?r:EIO);', str(path))
    path.write_text(text)


def prepare_defaults(root, si):
    """Load built-in defaults before external static/user configuration."""
    main_c = si / 'src/main/uefimain.c'
    text = main_c.read_text()
    begin = text.index('\tconfd_init();\n')
    end = text.index('\tlogger_init();\n', begin)
    previous = text[begin:end]
    # Migrate the exact earlier bring-up defaults, including the short-lived
    # zero timeout. Unknown startup code must not be silently removed.
    allowed = {
        '\tconfd_init();',
        '\tconfd_set_string("language", "zh_CN.UTF-8");',
        '#if defined(PIANO_PRODUCT_GUI_PUMP) && PIANO_PRODUCT_GUI_PUMP',
        '\tconfd_set_integer("boot.timeout", -1);',
        '\tconfd_set_integer("boot.timeout", 0);',
        '#else',
        '\tconfd_set_integer("boot.timeout", 150);',
        '#endif',
        '\tconfd_set_boolean("gui.driver.pointer.use_first", false);',
    }
    if any(line not in allowed for line in previous.splitlines()):
        raise ValueError('Unexpected SimpleInit startup configuration')
    main_c.write_text(text[:begin] + '\tconfd_init();\n' + text[end:])

    config_c = si / 'src/confd/uefi.c'
    text = config_c.read_text()
    defaults = '''	// The product's real static config is embedded in the application rootfs.
#if defined(PIANO_PRODUCT_GUI_PUMP) && PIANO_PRODUCT_GUI_PUMP
	if(conf_include_file(NULL,"rootfs:///etc/simpleinit.static.uefi.cfg")!=0)
		EDONE(tlog_warn("load built-in product config failed"));
#else
	char *language=strdup("zh_CN.UTF-8");
	if(!language)EDONE(tlog_warn("allocate default language failed"));
	if(conf_set_string_inc("language",language,0,0,true)!=0)free(language);
	conf_set_integer_inc("boot.timeout",150,0,0,true);
	conf_set_boolean_inc("gui.driver.pointer.use_first",false,0,0,true);
#endif
'''
    anchor = '\tdef_fp=NULL,def_path=NULL;\n\tload_default();'
    replacement = '\tdef_fp=NULL,def_path=NULL;\n' + defaults + '\tload_default();'
    if replacement not in text:
        if text.count(anchor) != 1:
            raise ValueError('Unexpected SimpleInit config initialization')
        config_c.write_text(text.replace(anchor, replacement, 1))
    text = config_c.read_text()
    # Product settings belong only to our directory on persist. Do not scan
    # factory partitions or pick an arbitrary writable filesystem for saving.
    begin = text.index('\tload_default();', text.index('int confd_init()'))
    end = text.index('\tif(!def_fp||!def_path)', begin)
    existing = text[begin:end]
    if '#if defined(PIANO_PRODUCT_GUI_PUMP)' not in existing:
        product = '''#if defined(PIANO_PRODUCT_GUI_PUMP) && PIANO_PRODUCT_GUI_PUMP
	fsvol_info *persist=fsvol_lookup_by_part_label("persist");
	if(persist&&fsvol_open_volume(persist,&f)==0){
		fsh *directory=NULL;
		if(fs_open(f,&directory,"sunuefi",FILE_FLAG_FOLDER|FILE_FLAG_CREATE|0700)==0){
			fs_close(&directory);
			conf_load_file(f,"/sunuefi/simpleinit.uefi.cfg");
			set_default(f,"/sunuefi/simpleinit.uefi.cfg");
			f=NULL;
		}
		if(f)fs_close(&f);
	}
#else
'''
        text = text[:begin] + product + existing + '#endif\n' + text[end:]
        config_c.write_text(text)
    text=config_c.read_text()
    text=text.replace('int confd_quit(){\n\treturn -1;\n}',
        'int confd_quit(){\n\tif(def_fp)fs_close(&def_fp);\n\tif(def_path)free(def_path);\n\tdef_path=NULL;\n\treturn 0;\n}')
    config_c.write_text(text)
    source = root / 'config/simpleinit.static.uefi.cfg'
    shutil.copyfile(source, si / 'root/etc/simpleinit.static.uefi.cfg')
    return config_c


def display_default(config, key):
    values = re.findall(r'^' + re.escape(key) + r'\s*=\s*([0-9]+)\s*$', config, re.MULTILINE)
    if len(values) != 1:
        raise ValueError('Expected one integer display default: ' + key)
    value = int(values[0])
    if key == 'gui.dpi' and not 72 <= value <= 1200:
        raise ValueError('SimpleInit default DPI out of range')
    if key == 'gui.rotate' and value not in (0, 90, 180, 270):
        raise ValueError('SimpleInit default rotation out of range')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--product-gui-pump', action='store_true',
                        help='bind the real product client and enable cooperative GUI pump')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    si = root / 'upstream/simple-init'
    build = root / ('build/simpleinit-product-edk2' if args.product_gui_pump else 'build/simpleinit-edk2')
    # Every build reproduces the source hooks. Legacy builds retain the Null
    # library and a disabled GUI wrapper; product changes only DSC binding.
    from prepare_product_pump import prepare
    prepare(root, apply=True)
    ui_hooks=None
    if args.product_gui_pump:
        from prepare_product_ui import prepare as prepare_ui
        ui_hooks=prepare_ui(root,apply=True)
    commit = subprocess.check_output(['git', '-C', str(si), 'rev-parse', 'HEAD'], text=True).strip()
    if commit != '3d66a6e78d519dd050fbebde4db6c5ac933f9aa4':
        raise SystemExit('Unexpected simple-init source commit')
    fontdir = si / 'root/usr/share/fonts'
    fontdir.mkdir(parents=True, exist_ok=True)
    from fontTools.ttLib import TTCollection
    source_font = next((path for path in (
        Path('/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc'),
        Path('/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc'),
    ) if path.is_file()), None)
    target_font = fontdir / 'NotoSansCJKsc-Regular.otf'
    if not target_font.exists():
        if source_font is None:
            raise SystemExit('Noto Sans CJK Regular collection missing; install fonts-noto-cjk or noto-fonts-cjk')
        collection = TTCollection(str(source_font))
        # The installed Noto collection has JP/KR/SC/TC/HK faces in this order.
        face = collection.fonts[2]
        family = face['name'].getDebugName(1)
        if 'SC' not in family:
            raise SystemExit('Expected Simplified Chinese Noto face')
        face.save(str(target_font))
    icons = fontdir / 'fontawesome5.ttf'
    if not icons.exists():
        url = 'https://raw.githubusercontent.com/FortAwesome/Font-Awesome/5.15.4/webfonts/fa-solid-900.ttf'
        icons.write_bytes(urllib.request.urlopen(url, timeout=30).read())
    default_font = si / 'root/etc/default.ttf'
    if default_font.is_symlink():
        default_font.unlink()
    if not default_font.exists():
        default_font.symlink_to('../usr/share/fonts/NotoSansCJKsc-Regular.otf')
    for lang in ('zh_CN',):
        directory = si / f'root/usr/share/locale/{lang}/LC_MESSAGES'
        directory.mkdir(parents=True, exist_ok=True)
        subprocess.run(['msgfmt', '-o', str(directory / 'simple-init.mo'), str(si / f'po/{lang}.po')], check=True)
    config_c = prepare_defaults(root, si)
    prepare_filesystem(si)
    main_c = si / 'src/main/uefimain.c'
    text=main_c.read_text()
    if 'PianoScheduleSnapshot' not in text:
        text=text.replace('int main_retval=0;', 'extern VOID PianoScheduleSnapshot (VOID);\nint main_retval=0;')
        text=text.replace('\tEfiBootManagerConnectAll();', '\tPianoScheduleSnapshot();\n\tEfiBootManagerConnectAll();')
        main_c.write_text(text)
    text = main_c.read_text()
    if 'PianoCancelSnapshot' not in text:
        text = text.replace('extern VOID PianoScheduleSnapshot (VOID);',
                            'extern VOID PianoScheduleSnapshot (VOID);\nextern VOID PianoCancelSnapshot (VOID);')
        text = text.replace('\treturn main_retval;', '\tPianoCancelSnapshot();\n\treturn main_retval;')
        main_c.write_text(text)
    text=main_c.read_text()
    if '\tconfd_quit();\n\treturn main_retval;' not in text:
        text=text.replace('\treturn main_retval;', '\tconfd_quit();\n\treturn main_retval;')
        main_c.write_text(text)
    # Continue Boot calls Boot Services Exit directly, bypassing UefiMain's
    # normal return cleanup. Cancel our callback on that path as well.
    exit_c = si / 'src/boot/exit.c'
    text = exit_c.read_text()
    if 'PianoCancelSnapshot' not in text:
        text = text.replace('int run_boot_exit(boot_config*boot){',
            'extern VOID PianoCancelSnapshot (VOID);\n\nint run_boot_exit(boot_config*boot){')
        anchor = '\tgBS->Exit(gImageHandle,EFI_ABORTED,0,NULL);'
        if text.count(anchor) != 1:
            raise SystemExit('Unexpected simple-init Continue Boot source')
        text = text.replace(anchor,'\tPianoCancelSnapshot();\n'+anchor)
        exit_c.write_text(text)
    text=exit_c.read_text()
    if '#include"confd.h"' not in text:text=text.replace('#include"boot.h"','#include"boot.h"\n#include"confd.h"')
    text=text.replace('\tPianoCancelSnapshot();\n\tgBS->Exit', '\tPianoCancelSnapshot();\n\tconfd_quit();\n\tgBS->Exit')
    exit_c.write_text(text)
    main_inf = si / 'src/main/SimpleInitMain.inf'
    text=main_inf.read_text()
    if 'PianoSnapshot.c' not in text:
        text=text.replace('[Sources]\n','[Sources]\n  PianoSnapshot.c\n')
        text=text.replace('[LibraryClasses]\n','[LibraryClasses]\n  LodePNG\n')
    main_inf.write_text(text)
    shutil.copyfile(root / 'uefi/core/PianoSnapshot.c', si / 'src/main/PianoSnapshot.c')
    # Keep real key-event diagnostics reproducible instead of relying on a
    # one-off edit to the upstream checkout. Merely locating ConSplitter's
    # virtual keyboard does not demonstrate a working physical button.
    keyboard = si / 'src/gui/drivers/uefi_keyboard.c'
    text = keyboard.read_text()
    if 'PIANO_KEY_EVENT' not in text:
        anchor = '\t\tif(EFI_ERROR(kd->kbd->ReadKeyStroke(kd->kbd,&p)))continue;'
        if text.count(anchor) != 1:
            raise SystemExit('Unexpected simple-init keyboard source')
        text = text.replace(anchor, anchor +
            '\n\t\ttlog_notice("PIANO_KEY_EVENT scan=0x%x unicode=0x%x",p.ScanCode,p.UnicodeChar);')
        keyboard.write_text(text)
    menu = si / 'src/gui/interface/core/bootmenu.c'
    text = menu.read_text()
    if 'PIANO_MENU_EXECUTE' not in text:
        text = text.replace('tlog_debug("run config %s",bi->cfg.ident);',
            'tlog_notice("PIANO_MENU_EXECUTE %s",bi->cfg.ident);')
        anchor = '\n\tbi->bm->selected=bi;\n'
        if text.count(anchor) != 1:
            raise SystemExit('Unexpected simple-init selection source')
        text = text.replace(anchor,anchor+'\ttlog_notice("PIANO_MENU_SELECT %s",bi->cfg.ident);\n')
    menu.write_text(text)
    # AbsolutePointer release events may keep the exact same coordinates as
    # the press. The original code skipped those events and never emitted REL.
    touch = si / 'src/gui/drivers/uefi_touch.c'
    text = touch.read_text()
    if 'PianoTouchAxis' not in text:
        text=text.replace('#include"gui/guidrv.h"',
            '#include"gui/guidrv.h"\n#include"PianoTouchInput.h"')
        text=text.replace('static INT64 lx=0,ly=0;',
            'static INT64 lx=0,ly=0;\nstatic bool lp=false;')
        text=text.replace('\tINT64 lx,ly,rx,ry;',
            '\tUINT64 lx,ly,mx,my,rx,ry;\n\tUINT32 buttons;\n\tbool sampled;')
        before='''		if(p.CurrentX==d->lx&&p.CurrentY==d->ly)continue;
		lx=((double)p.CurrentX/(double)d->rx)*gui_w;
		ly=((double)p.CurrentY/(double)d->ry)*gui_h;
		d->lx=p.CurrentX,d->ly=p.CurrentY;
		data->state=LV_INDEV_STATE_PR;'''
        after='''		if(d->sampled&&p.CurrentX==d->lx&&p.CurrentY==d->ly&&p.ActiveButtons==d->buttons)continue;
		int32_t w=gui_w,h=gui_h;
		if(gui_rotate==90||gui_rotate==270){w=gui_h;h=gui_w;}
		lx=PianoTouchAxis(p.CurrentX,d->mx,d->rx,w);
		ly=PianoTouchAxis(p.CurrentY,d->my,d->ry,h);
		lp=PianoTouchPressed(p.ActiveButtons);
		d->lx=p.CurrentX,d->ly=p.CurrentY,d->buttons=p.ActiveButtons,d->sampled=true;'''
        if text.count(before)!=1:
            raise SystemExit('Unexpected simple-init AbsolutePointer source')
        text=text.replace(before,after)
        text=text.replace('\tdata->point.x=lx;','\tdata->state=lp?LV_INDEV_STATE_PR:LV_INDEV_STATE_REL;\n\tdata->point.x=lx;')
        text=text.replace('\tstruct input_data*data=NULL;',
            '\tstruct input_data*data=NULL;\n\tif(!touch||!touch->Mode||touch->Mode->AbsoluteMaxX<=touch->Mode->AbsoluteMinX||touch->Mode->AbsoluteMaxY<=touch->Mode->AbsoluteMinY)return -1;')
        text=text.replace('\tdata->rx=data->touch->Mode->AbsoluteMaxX;',
            '\tdata->mx=data->touch->Mode->AbsoluteMinX;\n\tdata->my=data->touch->Mode->AbsoluteMinY;\n\tdata->rx=data->touch->Mode->AbsoluteMaxX;')
        touch.write_text(text)
    shutil.copyfile(root/'uefi/core/PianoTouchInput.h',touch.parent/'PianoTouchInput.h')
    inc = si / 'SimpleInit.inc'
    text = inc.read_text().replace('EmbeddedPkg/Library/FdtLib/FdtLib.inf',
                                  'MdePkg/Library/BaseFdtLib/BaseFdtLib.inf')
    inc.write_text(text)
    linux_inf = si / 'src/linux-boot/SimpleInitBootLinux.inf'
    text = linux_inf.read_text()
    # The upstream INF lists ArmGicLib but its sources do not call that library.
    linux_inf.write_text(text.replace('  ArmGicLib\n', ''))
    gui_inf = si / 'src/gui/SimpleInitGUI.inf'
    text = gui_inf.read_text().replace('  gEfiUgaDrawProtocolGuid\n', '').replace('  drivers/uefi_uga.c\n', '')
    gui_inf.write_text(text)
    drivers = si / 'src/gui/drivers.c'
    drivers.write_text(drivers.read_text().replace('\t&guidrv_uefiuga,\n', ''))
    compat_inf = si / 'libs/compatible/SimpleInitCompatible.inf'
    text = compat_inf.read_text()
    if 'PianoQuadFloatCompat.c' not in text:
        text = text.replace('[Sources]\n', '[Sources]\n  PianoQuadFloatCompat.c\n')
    compat_inf.write_text(text)
    shutil.copyfile(root / 'uefi/core/QuadFloatCompat.c',
                    si / 'libs/compatible/PianoQuadFloatCompat.c')
    arm = si / 'src/linux-boot/arm.c'
    text = arm.read_text()
    begin = text.index('typedef VOID (*ARM_V7_CACHE_OPERATION)') if 'typedef VOID (*ARM_V7_CACHE_OPERATION)' in text else -1
    if begin >= 0:
        end = text.index('static void exit_boot_services', begin)
        text = text[:begin] + '''static void platform_cleanup(linux_boot*lb){
  WriteBackInvalidateDataCacheRange(lb->kernel.address,lb->kernel.size);
  WriteBackInvalidateDataCacheRange(lb->dtb.address,lb->dtb.size);
  if(lb->initrd.address)WriteBackInvalidateDataCacheRange(lb->initrd.address,lb->initrd.size);
  ArmDisableDataCache();
  ArmDisableMmu();
  ArmInvalidateInstructionCache();
}

''' + text[end:]
        arm.write_text(text)
    dsc = (si / 'SimpleInit.dsc').read_text()
    dsc = dsc.replace('  PLATFORM_VERSION', '  OUTPUT_DIRECTORY               = Build/SimpleInit\n  PLATFORM_VERSION', 1)
    dsc = dsc.replace('ArmPkg/Library/ArmLib/ArmBaseLib.inf', 'MdePkg/Library/ArmLib/ArmBaseLib.inf')
    dsc = dsc.replace('ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf',
                      'MdePkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf')
    dsc = '\n'.join(line for line in dsc.splitlines() if not line.strip().startswith(('ArmGicLib|', 'ArmGicArchLib|'))
                    and 'BaseStackCheckLib/BaseStackCheckLib.inf' not in line)
    dsc = dsc.replace('MdePkg/Library/UefiDebugLibConOut/UefiDebugLibConOut.inf',
                      'MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf')
    dsc = dsc.replace('MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf',
                      'MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf')
    static_config = root / 'config/simpleinit.static.uefi.cfg'
    config_text = static_config.read_text()
    dsc += '''
[LibraryClasses]
  PianoProductPumpLib|MdePkg/Library/PianoProductPumpLibNull/PianoProductPumpLibNull.inf
  SerialPortLib|product-support/Library/RamOnlySerialPortLib/RamOnlySerialPortLib.inf
  MemoryTypeInformationChangeLib|MdeModulePkg/Library/MemoryTypeInformationChangeLibNull/MemoryTypeInformationChangeLibNull.inf
[PcdsFixedAtBuild]
  gSimpleInitTokenSpaceGuid.PcdLoggerdUseConsole|FALSE
  gSimpleInitTokenSpaceGuid.PcdGuiDefaultRotate|{rotate}
  gSimpleInitTokenSpaceGuid.PcdGuiDefaultDPI|{dpi}
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000046
[BuildOptions]
  GCC:*_CLANGDWARF_AARCH64_CC_FLAGS = -Wno-error -Wno-deprecated-non-prototype -Wno-implicit-function-declaration -Wno-int-conversion
'''.format(rotate=display_default(config_text, 'gui.rotate'), dpi=display_default(config_text, 'gui.dpi'))
    if args.product_gui_pump:
        dsc += '''
[LibraryClasses.common.UEFI_APPLICATION, LibraryClasses.common.UEFI_DRIVER, LibraryClasses.common.DXE_DRIVER]
  PianoProductPumpLib|MdePkg/Library/PianoProductPumpLib/PianoProductPumpLib.inf
[BuildOptions]
  GCC:*_CLANGDWARF_AARCH64_CC_FLAGS = -DPIANO_PRODUCT_GUI_PUMP=1
'''
    build.mkdir(parents=True, exist_ok=True)
    (build / 'SunSimpleInit.dsc').write_text(dsc)
    fonts = {'noto': {'source': str(source_font), 'sha256': hashlib.sha256(target_font.read_bytes()).hexdigest()},
             'fontawesome': {'version': '5.15.4', 'sha256': hashlib.sha256(icons.read_bytes()).hexdigest()}}
    owned_paths = (main_c,config_c,static_config,si/'root/etc/simpleinit.static.uefi.cfg',
                    exit_c,main_inf,si/'src/main/PianoSnapshot.c',keyboard,menu,touch,
                    si/'src/filesystem/volume/uefi.c',si/'src/filesystem/layer/uefi.c',si/'src/confd/file.c',
                    touch.parent/'PianoTouchInput.h',si/'src/gui/drivers/uefi_pointer.c',gui_inf,drivers,compat_inf,
                   si/'libs/compatible/PianoQuadFloatCompat.c',arm,linux_inf,inc)
    if args.product_gui_pump:
        from simpleinit_build_identity import PRODUCT_NAVIGATION_SOURCES
        owned_paths += tuple(root/relative for relative in PRODUCT_NAVIGATION_SOURCES)
    (build / 'source-manifest.json').write_text(json.dumps({'commit': commit, 'fonts': fonts,
        'product_gui_pump': args.product_gui_pump,
        'pump_hooks': prepare(root, apply=False),
        'ui_hooks': ui_hooks,
        'owned_sources': {str(path.relative_to(root)):hashlib.sha256(path.read_bytes()).hexdigest()
          for path in owned_paths},
        'dsc_sha256': hashlib.sha256(dsc.encode()).hexdigest()}, indent=2) + '\n')


if __name__ == '__main__':
    main()
