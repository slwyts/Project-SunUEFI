#!/usr/bin/env python3
"""Seal/verify the real SimpleInit library binding, flags and built application."""
import argparse
import hashlib
import json
from pathlib import Path

from prepare_product_pump import ROOT, prepare


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inspect(build, output, product):
    manifest=json.loads((build/'source-manifest.json').read_text())
    if manifest.get('product_gui_pump') is not product:
        raise ValueError('wrong SimpleInit product/default build mode')
    sources=manifest.get('owned_sources')
    if not isinstance(sources,dict)or not sources:raise ValueError('SimpleInit owned source fingerprint missing')
    for relative,digest in sources.items():
        path=ROOT/relative
        if not path.is_file()or sha(path)!=digest:raise ValueError('SimpleInit actual owned source changed: '+relative)
    if product:
        from prepare_product_ui import prepare as prepare_ui
        if manifest.get('ui_hooks')!=prepare_ui(ROOT,apply=False):raise ValueError('SimpleInit product UI/reboot hooks changed')
    hooks=prepare(ROOT,apply=False)
    if manifest['pump_hooks']!=hooks:
        raise ValueError('built SimpleInit source hook manifest is stale')
    if sha(build/'SunSimpleInit.dsc')!=manifest['dsc_sha256']:
        raise ValueError('SimpleInit DSC differs from its source manifest')
    report=build/'simpleinit-build-report.txt'
    report_text=report.read_text()
    base=build/'Build/SimpleInit/NOOPT_CLANGDWARF/AARCH64'
    gui=base/'src/gui/SimpleInitGUI/GNUmakefile'
    main=base/'src/main/SimpleInitMain/GNUmakefile'
    gui_text=gui.read_text();main_text=main.read_text()
    real='MdePkg/Library/PianoProductPumpLib/PianoProductPumpLib.inf'
    null='MdePkg/Library/PianoProductPumpLibNull/PianoProductPumpLibNull.inf'
    link_real='/MdePkg/Library/PianoProductPumpLib/PianoProductPumpLib/OUTPUT/PianoProductPumpLib.lib'
    link_null='/MdePkg/Library/PianoProductPumpLibNull/PianoProductPumpLibNull/OUTPUT/PianoProductPumpLibNull.lib'
    if product:
        if real not in report_text or null in report_text or link_real not in main_text or link_null in main_text or '-DPIANO_PRODUCT_GUI_PUMP=1' not in gui_text:
            raise ValueError('actual product build did not bind real pump library and GUI macro')
    else:
        if null not in report_text or real in report_text or link_null not in main_text or link_real in main_text or '-DPIANO_PRODUCT_GUI_PUMP=1' in gui_text:
            raise ValueError('actual diagnostic build did not bind Null/disabled GUI pump')
    built=base/'SimpleInitMain.efi';exported=output/'SimpleInit.efi'
    if sha(built)!=sha(exported):raise ValueError('exported application differs from actual build output')
    return {'product_gui_pump':product,'application_bytes':exported.stat().st_size,'application_sha256':sha(exported),
            'source_manifest_sha256':sha(build/'source-manifest.json'),'dsc_sha256':sha(build/'SunSimpleInit.dsc'),
            'pump_hooks':hooks,'report_sha256':sha(report),'gui_makefile_sha256':sha(gui),'main_makefile_sha256':sha(main),
            'physical_validation':False}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation',choices=('seal','verify'))
    parser.add_argument('--build-dir',type=Path,required=True)
    parser.add_argument('--output-dir',type=Path,required=True)
    parser.add_argument('--product-gui-pump',action='store_true')
    args=parser.parse_args();record=inspect(args.build_dir,args.output_dir,args.product_gui_pump)
    marker=args.output_dir/'build-ok.json'
    if args.operation=='seal':marker.write_text(json.dumps(record,indent=2)+'\n')
    elif json.loads(marker.read_text())!=record:raise ValueError('sealed SimpleInit build identity is stale')
    print(json.dumps(record,indent=2))


if __name__=='__main__':main()
