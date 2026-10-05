#!/usr/bin/env python3
"""Validate the single product contract. A contract is not a built firmware."""
import argparse
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
FEATURES={"gop","physical_keys","pogo_keyboard_touchpad","touchscreen","dma_smmu",
    "ufs_blockio_read_write","gpt","fat_simplefilesystem","persistent_variables","uefi_shell",
    "setup_hii","simpleinit","usb_device_fastboot","usb_host","debug_logs_screenshot",
    "efi_android_linux_boot","os_exit"}
SURFACES={"boot_selection","SimpleInit","Setup","Shell"}


def validate(data):
    if data.get('schema')!=1 or data.get('artifact')!='PianoUEFI-product.img':raise ValueError('Wrong product identity')
    if data.get('shared_core') is not True or data.get('entry_policy_only') is not True:raise ValueError('Entries must share one core')
    if data.get('entry_points')!=['fastboot_boot','boot','recovery']:raise ValueError('Product entry list changed')
    features=data.get('features',{})
    if set(features)!=FEATURES or any(value is not True for value in features.values()):
        raise ValueError('Product cannot omit or silently disable a required feature')
    boot=data.get('boot_policy',{})
    if boot.get('splash')!='TianoCore' or boot.get('default_application')!='SimpleInit' or boot.get('setup_key')!='F12' or \
       boot.get('setup_application')!='TianoCoreUiApp' or boot.get('diagnostic_reboot_timer') is not False:
        raise ValueError('Product boot policy differs from the requested experience')
    fastboot=data.get('fastboot',{})
    surfaces=fastboot.get('available_in',[])
    if fastboot.get('mode')!='resident_background' or set(surfaces)!=SURFACES or len(surfaces)!=len(SURFACES) or \
       fastboot.get('start_once') is not True or fastboot.get('stop_before_os_handoff') is not True or \
       fastboot.get('download_limit_target_bytes')!=1073741824:
        raise ValueError('Fastboot must remain one resident service through all UEFI applications')
    return data


def validate_build_manifest(contract,manifest):
    validate(contract)
    if manifest.get('target')!='product' or manifest.get('artifact')!=contract['artifact']:
        raise ValueError('A diagnostic image cannot be renamed as the product')
    if manifest.get('features')!=contract['features'] or manifest.get('shared_core') is not True:
        raise ValueError('Built feature set does not match the product')
    if manifest.get('fastboot_mode')!='resident_background' or manifest.get('fastboot_surfaces')!=contract['fastboot']['available_in']:
        raise ValueError('Foreground USB experiment is not product fastboot')
    if manifest.get('default_application')!='SimpleInit' or manifest.get('setup_key')!='F12':
        raise ValueError('Missing product supervisor policy')
    if manifest.get('diagnostic_reboot_timer') is not False:
        raise ValueError('Diagnostic reboot timer must not ship')
    if manifest.get('backend_initialization_required') is not True:
        raise ValueError('Feature declarations alone are not linked runtime initialization')
    return manifest


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--contract',type=Path,default=ROOT/'config/piano-product.json')
    parser.add_argument('--build-manifest',type=Path)
    args=parser.parse_args();contract=validate(json.loads(args.contract.read_text()))
    if args.build_manifest:validate_build_manifest(contract,json.loads(args.build_manifest.read_text()))
    print(json.dumps({'status':'CONTRACT_VALID_NOT_FIRMWARE_VALIDATION','artifact':contract['artifact'],
        'enabled_features':sorted(contract['features']),'fastboot_surfaces':contract['fastboot']['available_in']},indent=2))


if __name__=='__main__':main()
