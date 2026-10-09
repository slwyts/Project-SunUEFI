#!/usr/bin/env python3
"""Build the pinned RAM hardware adapters; optionally stage an explicit derived tree.

No service execution, hardware access, downloads or changes to public sources.
The default only creates a new workspace build directory for review.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = 'a75f8c5d5fa099d65c171ac839c2e3bb6c63ec45'
CHECKER_SHA = '1f2c26329c00b5b791a101d409032cd3d8b1962f83018937807c5cbbb7f5a2e1'
CONTEXT_CHECKER_SHA = '9731f4027c0656d50a930ed2fb087d0ccfb3a99bb4373cfb65e9e0a0865ac63d'
PINS = {
    'display-start': 'a371d7cfa526b575f7393eeed60fcc7a6dba85b6971ebd0c977ae880ab60a8ba',
    'touch-start': '77aacb8b37d2513b4a919d568d9b57048ef09ea32df20758babb8c8c1d83acdd',
    'radio-start': 'eb638760bf6a9e0bbfc8bc41b67e2bb00ec9204deae975547fa5b13f1c02e66d',
    'adsp-start': 'c46528e2be4fae974f22b5ea9660f97efc511f8c9a6d2349e99882cd9c907066',
    'audio-start': 'a19b542542eeef5539728b19f52d92c6fbfb7f6deb0e15668609666e76522f2c',
    'keyboard-start': 'e09115a093c9cdfc239706b280baeda09ab038cf108c1360f902f1b5558609ac',
    'video-start': 'b6c0e2a77d58981592fe779405d5d52be5faff4fbc11837b7c3403497d86aa62',
    'camera-start': '943679e42c36ae95325566136ecbe599ce7a6a4070a16d11a25079ff0a12d0e3',
}
GUARD = '/usr/lib/piano/piano-ram-hardware-prepare --require-scope '
LEGACY_GATE = "[ -f /run/piano-smmu-ready ] || { echo 'SMMU initialization missing'; exit 1; }"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise ValueError('pinned service patch anchor count changed: ' + before)
    return text.replace(before, after)


def adapt(name, data):
    if digest(data) != PINS[name]:
        raise ValueError('pinned public script drift: ' + name)
    text = data.decode()
    if name in ('touch-start', 'radio-start'):
        text = replace_once(text, LEGACY_GATE, GUARD + ('touch' if name == 'touch-start' else 'qup --wait-seconds 20'))
    if name == 'radio-start':
        text = replace_once(text,
            '# Only this verified register is safe; do not probe adjacent TLMM windows.\n'
            'v=$(/usr/lib/piano/busybox devmem 0x0f204008 32)\n'
            '/usr/lib/piano/busybox devmem 0x0f204008 32 "$((v | 1))"\n',
            '# Reference clocks are enabled by the bound normal PHY clock consumer.\n'
            + GUARD + 'clock\n')
        text = replace_once(text, 'modprobe --ignore-install phy_qcom_qmp_pcie',
                            'modprobe --ignore-install phy_qcom_qmp_pcie\n'
                            '# Normal PHY/host probing enumerates the real PCI endpoint first.\n'
                            + GUARD + 'radio --wait-seconds 30')
    elif name == 'display-start':
        # Register snapshots do not prepare a domain or authorize its owner.
        # Keep them in the standalone diagnostic CLI, off the boot path.
        text = replace_once(text,
            '    # No stream match may be added behind the SMMU driver\'s back once it\n'
            '    # is bound, and it must adopt the /pianoinit ones.\n'
            "    [ -f /run/piano-smmu-ready ] || die 'apps SMMU stream matches not installed'",
            '    # The normal kernel owns consumer routes; require its actual binding.')
        text = text.replace('                (UFS, USB, display, and the ones /pianoinit added) as bypass',
                            '                through normal kernel consumer attachment')
    elif name in ('video-start', 'camera-start'):
        scope = name.removesuffix('-start')
        observation = ('# The normal driver owns its DMA domain; readback adds diagnostics only.\n'
                       + '/usr/lib/piano/piano-ram-hardware-prepare --observe-scope ' + scope + ' ||\n'
                       + '    say "WARN ' + scope + ' context observation unavailable; driver remains bound"\n')
        if name == 'video-start':
            text = replace_once(text, 'done\nsay "DONE ', 'done\n' + observation + 'say "DONE ')
        else:
            text = replace_once(text, 'wait_bound "$CAMSS" \'CAMSS\'\n',
                                'wait_bound "$CAMSS" \'CAMSS\'\n' + observation)
    else:
        scope = name.removesuffix('-start')
        # Before the first hardware operation, after the published disable option.
        anchors = {'keyboard-start': 'n=0\nuntil [ -e "$BUS/driver" ]; do',
                   'adsp-start': 'say "BEGIN power"', 'audio-start': 'say "BEGIN card"'}
        if name in anchors:
            before = anchors[name]
            preparation = 'modprobe fastrpc || { say "FAIL fastrpc"; exit 1; }\n' if name == 'adsp-start' else ''
            wait = ' --wait-seconds 20' if name in ('keyboard-start', 'adsp-start', 'audio-start') else ''
            if name == 'adsp-start':
                # FastRPC has already bound through the native driver here.
                # Its context readback is diagnostic, as for CAMSS and Iris.
                probe = '/usr/lib/piano/piano-ram-hardware-prepare --observe-scope adsp'
            else:
                probe = GUARD + scope + wait
            text = replace_once(text, before, preparation + probe + '\n\n' + before)
    if name in ('adsp-start', 'audio-start'):
        # A failed oneshot must not be reported successful by systemd.
        text = text.replace('; exit 0; }', '; exit 1; }')
        text = text.replace('    exit 0\nfi', '    exit 1\nfi')
        # Keep explicitly configured disable=0 as the public user's choice.
        text = text.replace("say \"disabled by /etc/piano/" + name.split('-')[0] + ".conf\"; exit 1; }",
                            "say \"disabled by /etc/piano/" + name.split('-')[0] + ".conf\"; exit 0; }")
    if 'devmem' in text or 'piano-smmu-ready' in text:
        raise ValueError('legacy register access/global marker survived: ' + name)
    return text.encode()


def safe_destination(rootfs, relative):
    dest = rootfs / relative
    for parent in (dest, *dest.parents):
        if parent == rootfs:
            break
        if parent.is_symlink():
            raise ValueError('guest symlink in hardware adapter destination: ' + str(parent))
    return dest


def build(output, source, rootfs=None):
    output, source = Path(output).resolve(), Path(source).resolve()
    if not output.is_relative_to(ROOT / 'build') or output.exists():
        raise ValueError('output must be a new workspace build directory')
    commit = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if commit != PUBLIC or subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True):
        raise ValueError('public source must have the pinned clean commit')
    check = (ROOT / 'tools/check_piano_kernel_dma_routes.py').read_bytes()
    if digest(check) != CHECKER_SHA:
        raise ValueError('reviewed six-master DMA verifier drift')
    contexts = (ROOT / 'tools/check_piano_kernel_contexts.py').read_bytes()
    if digest(contexts) != CONTEXT_CHECKER_SHA:
        raise ValueError('reviewed translated/masked DMA context verifier drift')
    files = {'usr/lib/piano/piano_dma_routes.py': check,
             'usr/lib/piano/piano_dma_contexts.py': contexts,
             'usr/lib/piano/piano-ram-hardware-prepare':
             (ROOT / 'linux/userspace/piano-ram-hardware-prepare').read_bytes()}
    # Install these local additions from the same files as the generic BSP.
    for relative in ('etc/modprobe.d/piano.conf',
                     'etc/modules-load.d/piano-rgb.conf',
                     'etc/modules-load.d/piano-bluetooth.conf',
                     'usr/share/piano/pen/p81c-radio.json',
                     'usr/share/piano/pen/calibration/piano-boe.ini',
                     'usr/share/piano/pen/calibration/piano-csot.ini',
                     'usr/lib/systemd/system/piano-touch.service',
                     'etc/pipewire/client.conf.d/60-piano-audio.conf',
                     'etc/pipewire/pipewire-pulse.conf.d/60-piano-audio.conf',
                     'etc/wireplumber/wireplumber.conf.d/50-piano-audio.conf',
                     'usr/lib/systemd/system/upower.service.d/20-piano-keyboard.conf'):
        files[relative] = (ROOT / 'linux/bsp/common' / relative).read_bytes()
    audio_path = 'usr/share/alsa/ucm2/Qualcomm/sm8750/Xiaomi-Pad-8-Pro/HiFi.conf'
    original_audio = (source / 'rootfs/overlay' / audio_path).read_bytes()
    if digest(original_audio) != 'ed1b91e6f7e4cef76f1f60cfe3dc01d249cfb0267bb983bfc362df8f4f97609c':
        raise ValueError('Reviewed public audio UCM changed')
    # One tracked UCM source for complete releases and distro BSP packages.
    files[audio_path] = (ROOT / 'linux/bsp/common' / audio_path).read_bytes()
    # Keep the pinned service intact; order the real Debian display manager
    # behind completed native backlight/DPU/DSI readiness, including cold boots.
    files['etc/systemd/system/piano-display.service.d/20-native-kms-order.conf'] = (
        '[Unit]\nBefore=display-manager.service gdm3.service gdm.service\n').encode()
    files['etc/systemd/system/gdm3.service.d/20-piano-display.conf'] = (
        '[Unit]\nRequires=piano-display.service\nAfter=piano-display.service\n').encode()
    inputs = {}
    for name in PINS:
        original = (source / 'rootfs/overlay/usr/lib/piano' / name).read_bytes()
        files['usr/lib/piano/' + name] = adapt(name, original)
        inputs[name] = digest(original)
    # Preflight every staged path before any mutation; do not follow absolute
    # guest /lib or /bin links on the host. Canonical destinations use /usr.
    if rootfs:
        rootfs = Path(rootfs).resolve()
        if not rootfs.is_relative_to(ROOT / 'build/distros') or not rootfs.is_dir():
            raise ValueError('stage target must be an existing derived build/distros tree')
        for name in files:
            safe_destination(rootfs, Path(name))
        safe_destination(rootfs, Path('usr/share/piano-provenance/ram-hardware.json'))
        for name in PINS:
            unit = Path('usr/lib/systemd/system') / ('piano-' + name.removesuffix('-start') + '.service')
            installed = safe_destination(rootfs, unit)
            original = source / 'rootfs/overlay' / unit
            if not installed.is_file() or installed.read_bytes() != original.read_bytes():
                raise ValueError('complete pinned public service unit must already be staged: ' + str(unit))
    output.mkdir(parents=True)
    for relative, data in files.items():
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(0o755 if relative.startswith('usr/lib/piano/') and not relative.endswith('.py') else 0o644)
        if relative.endswith('-start'):
            subprocess.run(['/bin/sh', '-n', str(target)], check=True)
    result = {'status': 'SCOPED_RAM_HARDWARE_ADAPTERS_PREPARED_NOT_DEVICE_VERIFIED',
              'source_commit': commit, 'source_scripts': inputs,
              'checker_sha256': CHECKER_SHA, 'context_checker_sha256': CONTEXT_CHECKER_SHA,
              'required_kernel_context_abi': 'piano-dma-context-v1',
              'files': {name: digest(data) for name, data in files.items()},
              'all_public_services_preserved': True, 'ufs_loaded_by_bootstrap': False,
              'global_ready_marker': False, 'register_programming': False,
              'device_tested': False, 'full_hardware_ready': False,
              'supported_readback_scopes': ['usb', 'qup', 'touch', 'keyboard', 'storage',
                                            'gpu', 'gmu', 'mdss', 'display', 'display-active', 'video', 'camera', 'adsp', 'audio', 'radio'],
              'clock_scope': 'ACTUAL_DT_CONSUMERS_AND_BOUND_PROVIDER_ONLY_NO_RATE_ENABLE_READBACK',
              'context_scope': 'KERNEL_PRIVATE_AND_HARDWARE_CONFIGURATION_ONLY_NO_DMA_TRANSFER',
              'audio_profile': {'source_sha256': digest(original_audio), 'microphone': 'DMIC2', 'capture_channels': 1, 'capture_positions': ['MONO'], 'dec0_gain_db': 14, 'playback_channels': 4, 'speaker_mix': 'simple left/right duplication for stereo streams', 'required_desktop_packages': ['libcanberra-pulse', 'rtkit'], 'noise_quality_verified': False},
              'device_transfer_validation_pending': True,
              'pci_parf_hardware_table_verified': False,
              'domain_forced': False}
    (output / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    if rootfs:
        for name in files:
            dest = safe_destination(rootfs, Path(name))
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(output / name, dest)
        manifest = rootfs / 'usr/share/piano-provenance/ram-hardware.json'
        manifest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(output / 'manifest.json', manifest)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, default=ROOT / 'upstream/debian-piano-current')
    ap.add_argument('--output-dir', type=Path, required=True)
    ap.add_argument('--rootfs', type=Path)
    args = ap.parse_args()
    try:
        print(json.dumps(build(args.output_dir, args.source, args.rootfs), indent=2))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        ap.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
