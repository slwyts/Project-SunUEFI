#!/usr/bin/env python3
"""Replace the Frame device/session policy with Piano's KDE and Gamescope session."""
import shutil
import subprocess
import json
import os
import re
import shlex
from datetime import datetime, timezone
from pathlib import Path

from stage_arch_piano import put
from build_nyarch_apps import guest_environment
from prepare_debian_desktop import configure as configure_kde

# These units control Frame hardware or its A/B partition layout. Piano uses
# the shared BSP and SunUEFI disk bootstrap instead.
FRAME_UNITS = (
    'deckard-factory-recovery.service', 'deckard-boot-images.service',
    'deckard-fan-control.service', 'deckard-fpga.service',
    'deckard-led-control.service', 'deckard-typec-logger.service',
    'deckard-charger.service', 'deckard-power-monitor.service',
    'steamos-headset-adb.service', 'steamos-headset-adb.path',
    'steamos-headset-usb-gadget.service', 'steamos-headset-usb-gadget.path',
    'steamos-headset-fpga-config.service', 'steamos-power-monitor.service',
    'dsp_service.service', 'iris-driver-rebind.service',
    'steamvr-program-ble.service', 'steamvr-set-kernel-thread-priorities.service',
    'steamvr-v4l2loopback.service', 'adbd.service', 'adbd-pre.service',
    'adbd-post.service', 'usb-gadget.service',
    'usb-gadget-init.service', 'usb-ncm-gadget@.service',
    'usb-ncm-dnsmasq@.service', 'steamos-boot.service',
    'efi.mount', 'esp.mount', 'systemd-repart.service',
    'rauc.service', 'steamos-update.service',
    'setubootenv.service', 'deckard-fpga-resume.service',
    'deckard-audio-setup.service', 'steamos-offload.target', 'etc.mount',
    'steamos-post-update.service', 'holo-post-update-shutdown.service',
    'gpu-trace.service', 'softapmanager.service', 'steam-video-codec-setup.service',
    'home.mount', 'set-bluetooth-mac-address.service', 'set-wifi-mac-address.service',
    'steamos-devkit-service.service', 'steam-web-debug-portforward.service',
    'steamvr-web-debug-portforward.service',
    'steamos-log-submitter.service',
    # This dumps Frame EEPROM, A/B slots and U-Boot environment failures
    # into /dev/kmsg. Piano uses standard power_supply and a single root.
    'steamos-dump-info.service',
)


def configure_package_updates(root):
    bsp = Path(__file__).resolve().parents[1] / 'linux/bsp/steamos'
    put(root, 'usr/local/libexec/piano-steamos-update',
        (bsp / 'piano-steamos-update').read_text(), 0o755)
    put(root, 'etc/pacman.d/hooks/95-piano-update-adapter.hook',
        (bsp / '95-piano-update-adapter.hook').read_text())
    entry = root / 'usr/bin/steamos-update'
    entry.unlink(missing_ok=True)
    entry.symlink_to('/usr/local/libexec/piano-steamos-update')
    # Device-specific builds are delivered with the BSP. Stock replacements
    # lose the tablet's GPU, sensor and decoder fixes. This same policy also
    # applies to Discover's libalpm backend.
    policy = root / 'etc/pacman.conf'
    text = policy.read_text()
    ignore = ('deckard-mesa-linux-aarch64 gamescope steamos-manager '
              'ffmpeg fastrpc libssc iio-sensor-proxy ibus libibus gtk3')
    block = ('# Piano hardware packages are updated with the BSP image.\n'
             'IgnorePkg = ' + ignore + '\n')
    if block not in text:
        text = re.sub(r'(?m)^# Piano hardware packages are updated with the BSP image\.\n'
                      r'IgnorePkg = [^\n]*\n', '', text)
        text = text.replace('[options]\n', '[options]\n' + block, 1)
    policy.write_text(text)


def configure_input_method(root):
    # Qt 6 prefers the compositor protocol; Qt 5 and Xwayland use IBus directly.
    put(root, 'etc/environment.d/60-piano-input-method.conf',
        'QT_IM_MODULES=wayland;ibus\nQT_IM_MODULE=ibus\n'
        'XMODIFIERS=@im=ibus\nSDL_IM_MODULE=ibus\n'
        'LANG=zh_CN.UTF-8\nLANGUAGE=zh_CN:zh\n')
    (root / 'etc/xdg/autostart/piano-ibus.desktop').unlink(missing_ok=True)
    (root / 'etc/xdg/Xwayland-session.d/10-ibus-x11').unlink(missing_ok=True)
    put(root, 'etc/xdg/autostart/ibus.desktop',
        '[Desktop Entry]\nType=Application\nName=IBus\nHidden=true\n')
    profile = root / 'etc/dconf/profile/user'
    text = profile.read_text() if profile.exists() else 'user-db:user\n'
    if 'system-db:local' not in text.splitlines():
        put(root, 'etc/dconf/profile/user', text.rstrip() + '\nsystem-db:local\n')
    defaults = (
        "[general]\npreload-engines=['pinyin', 'xkb:us::eng']\n"
        "engines-order=['pinyin', 'xkb:us::eng']\n"
        "[general/hotkey]\ntriggers=['<Control>space']\n")
    put(root, 'etc/dconf/db/local.d/60-piano-input-method',
        "[desktop/ibus/general]\npreload-engines=['pinyin', 'xkb:us::eng']\n"
        "engines-order=['pinyin', 'xkb:us::eng']\n"
        "[desktop/ibus/general/hotkey]\ntriggers=['<Control>space']\n")
    subprocess.run(['chroot', root, 'dconf', 'update'], check=True)
    # The Frame image has an English-only user value which overrides defaults.
    with guest_environment(root) as guest:
        guest(['runuser', '-u', 'piano', '--', 'dbus-run-session', '--',
               'dconf', 'load', '/desktop/ibus/'], input=defaults, text=True)
    config = root / 'home/piano/.config'
    config.mkdir(parents=True, exist_ok=True)
    kwin = config / 'kwinrc'
    text = kwin.read_text() if kwin.exists() else ''
    text = re.sub(r'(?m)^InputMethod=.*\n?', '', text)
    if '[Wayland]' not in text:
        text = text.rstrip() + '\n\n[Wayland]\n'
    text = text.replace('[Wayland]\n', '[Wayland]\nInputMethod='
                        '/usr/share/applications/org.freedesktop.IBus.Panel.Wayland.Gtk3.desktop\n', 1)
    kwin.write_text(text)
    (config / 'plasma-localerc').write_text(
        '[Formats]\nLANG=zh_CN.UTF-8\n\n'
        '[Translations]\nLANGUAGE=zh_CN:zh\n')
    globals_file = config / 'kdeglobals'
    text = globals_file.read_text() if globals_file.exists() else ''
    if '[Locale]' in text:
        text = re.sub(r'(?ms)^\[Locale\]\n.*?(?=^\[|\Z)',
                      '[Locale]\nLanguage=zh_CN:zh\n', text)
    else:
        text = text.rstrip() + '\n\n[Locale]\nLanguage=zh_CN:zh\n'
    globals_file.write_text(text)
    for path in (config / 'plasma-localerc', globals_file, kwin):
        os.chown(path, 1000, 1000)


def configure_bluetooth_default(root):
    # This is Steam's own preference, not a forced BlueZ power policy.
    # Seed only the public build root, before any account has been used.
    path = root / 'home/piano/.local/share/Steam/config/config.vdf'
    text = path.read_text()
    if re.search(r'"System"\s*\{', text):
        return
    end = text.rfind('}')
    if end < 0:
        raise ValueError('Steam installation configuration has no root object')
    path.write_text(text[:end] +
                    '\t"System"\n\t{\n\t\t"Bluetooth"\n\t\t{\n'
                    '\t\t\t"Enabled" "1"\n\t\t}\n\t}\n' + text[end:])


def configure_identity(root, base, build_id):
    # /etc/os-release in the Frame image links to this vendor file.
    # Keep Valve's actual source version separately from the derivative.
    path = root / 'usr/lib/os-release'
    fields = {}
    for line in path.read_text().splitlines():
        if not line or line.startswith('#') or '=' not in line:
            continue
        key, value = line.split('=', 1)
        values = shlex.split(value)
        fields[key] = values[0] if values else ''
    fields.update(
        NAME='SteamOS', PRETTY_NAME='SteamOS',
        VERSION_CODENAME='piano', VARIANT='Tablet',
        VARIANT_ID='tablet', VERSION_ID=base['version'] + '.sunuefi',
        BUILD_ID=build_id, SUNUEFI_BASE_VERSION_ID=base['version'],
        SUNUEFI_BASE_BUILD_ID=base['build'], SUNUEFI_BASE_VARIANT_ID='vr',
        SUNUEFI_BASE_CODENAME='holo',
        HOME_URL='https://github.com/slwyts/Project-SunUEFI',
        DOCUMENTATION_URL='https://github.com/slwyts/Project-SunUEFI/tree/main/docs',
        BUG_REPORT_URL='https://github.com/slwyts/Project-SunUEFI/issues')
    path.write_text(''.join(key + '=' + json.dumps(value, ensure_ascii=False) + '\n'
                            for key, value in fields.items()))
    return {key: fields[key] for key in (
        'PRETTY_NAME', 'VERSION_ID', 'VERSION_CODENAME', 'VARIANT_ID', 'BUILD_ID',
        'SUNUEFI_BASE_VERSION_ID', 'SUNUEFI_BASE_BUILD_ID')}


def platform_units(root):
    # Merely masking the offload target is insufficient: RequiresMountsFor
    # can pull individual mounts into login, polkit and temporary-file setup.
    # Piano keeps these directories on its ordinary writable root filesystem.
    offload = tuple(
        path.name for path in sorted((root / 'usr/lib/systemd/system').glob('*.mount'))
        if any(line.startswith('What=/home/.steamos/offload/')
               for line in path.read_text().splitlines()))
    return FRAME_UNITS + offload


def configure_cpu_and_recovery(root):
    # Frame reserves CPUs for its VR pipeline. Let Linux schedule the tablet's
    # services and applications across all CPUs; an empty list resets affinity.
    for manager in ('system', 'user'):
        put(root, f'etc/systemd/{manager}.conf.d/90-piano-cpu.conf',
            '[Manager]\nCPUAffinity=\n')
    # This helper reports Valve's active A/B slot, not the root filesystem
    # partition. Piano has no Valve A/B installation to report here.
    about = root / 'etc/xdg/kcm-about-distrorc'
    if about.is_file():
        about.write_text('\n'.join(
            line for line in about.read_text().splitlines()
            if line.strip() != 'ExtraSoftwareData=/usr/bin/steamos-partition') + '\n')
    # Advertise only the installed platform's capabilities through Manager.
    # Valve's reset assumes its A/B layout; its BIOS/dock/fan tools target Deck.
    path = root / 'usr/share/steamos-manager/platform.toml'
    if path.is_file():
        unsupported = {'factory_reset', 'update_bios', 'update_dock', 'fan_control'}
        lines, omit = [], False
        for line in path.read_text().splitlines():
            stripped = line.strip()
            if stripped.startswith('['):
                section = stripped.strip('[]').split('.', 1)[0]
                omit = section in unsupported
            if not omit:
                lines.append(line)
        path.write_text('\n'.join(lines).strip() + '\n')


def configure_platform(root):
    configure_cpu_and_recovery(root)
    link = root / 'etc/systemd/system/graphical.target.wants/power-profiles-daemon.service'
    link.parent.mkdir(parents=True, exist_ok=True)
    link.unlink(missing_ok=True)
    link.symlink_to('/usr/lib/systemd/system/power-profiles-daemon.service')
    units = platform_units(root)
    for unit in units:
        link = root / 'etc/systemd/system' / unit
        link.unlink(missing_ok=True)
        link.symlink_to('/dev/null')
    # Keep systemd's standard UDC target, without Frame's ADB/FunctionFS.
    # Type-C partner attachment starts Piano's gadget after orientation is
    # applied; the UDC target fires before that information is available.
    gadget = root / 'etc/systemd/system/usb-gadget.target'
    if gadget.is_symlink() and str(gadget.readlink()) == '/dev/null':
        gadget.unlink()
    wants = root / 'etc/systemd/system/usb-gadget.target.wants'
    wants.mkdir(parents=True, exist_ok=True)
    for name in ('adbd-pre.service', 'adbd.service', 'adbd-post.service',
                 'dev-usb\\x2dffs-adb.mount', 'usb-gadget.service'):
        (wants / name).unlink(missing_ok=True)
    link = wants / 'piano-usb.service'
    link.unlink(missing_ok=True)
    # The Frame module parameter is absent from mainline ath12k. A same-name
    # local config shadows the headset-only reorder policy across upgrades.
    put(root, 'etc/modprobe.d/ath12k.conf',
        '# Use mainline ath12k firmware defaults; no Frame RX reorder override.\n')
    # The native DRM/KMS path is Wayland. The Frame root also contains Xorg
    # policy for its own displays; use KWin for SDDM as well as the session.
    put(root, 'etc/sddm.conf.d/20-piano-wayland.conf',
        '[General]\nDisplayServer=wayland\n'
        'GreeterEnvironment=QT_WAYLAND_SHELL_INTEGRATION=layer-shell\n'
        '[Wayland]\n'
        'CompositorCommand=kwin_wayland --drm --no-lockscreen --no-global-shortcuts --locale1\n')
    # Use the native DRM session even when the user manager still holds the
    # previous compositor's DISPLAY/WAYLAND_DISPLAY. GLES keeps KWin's Qt
    # Quick windows on the same hardware API as its shared EGL context.
    put(root, 'etc/systemd/user/plasma-kwin_wayland.service.d/60-piano-drm.conf',
        '[Service]\nExecStart=\n'
        'ExecStart=/usr/bin/kwin_wayland_wrapper --drm --xwayland\n'
        'Environment=KWIN_COMPOSE=O2ES\n')
    # Valve's plasma.desktop starts its VR Gamescope target, which then asks
    # SteamVR to present Plasma inside the headset. Select native Plasma via
    # SDDM's standard local-session override instead.
    put(root, 'usr/local/share/wayland-sessions/plasma.desktop',
        '[Desktop Entry]\nName=Plasma (Wayland)\n'
        'Exec=/usr/bin/startplasma-wayland\nTryExec=/usr/bin/startplasma-wayland\n'
        'Type=Application\nDesktopNames=KDE\n')
    for unit in ('steamvr.service', 'steamvr-logs.service', 'steamvr-plasma.service',
                 'steamvr-proxmicmute.service', 'steamvr-v4l2cam.service',
                 'steamvr-nested-desktop.service'):
        link = root / 'etc/systemd/user' / unit
        link.parent.mkdir(parents=True, exist_ok=True)
        link.unlink(missing_ok=True)
        link.symlink_to('/dev/null')
    autostart = root / 'etc/xdg/autostart/steam.desktop'
    if autostart.is_file():
        text = autostart.read_text()
        autostart.write_text('\n'.join(
            'Exec=/usr/bin/steam -silent -language schinese %U' if line.startswith('Exec=') else line
            for line in text.splitlines()) + '\n')
    # The browser is preinstalled. Shadow Frame's Discover installation link,
    # leaving Chromium's actual application entry unchanged across updates.
    put(root, 'usr/local/share/applications/org.chromium.Chromium.desktop',
        '[Desktop Entry]\nType=Application\nName=Chromium\nHidden=true\n')
    # CPU policy is already configured by Piano. Do not load Valve's LAVD
    # sched_ext scheduler over the selected BORE/EEVDF kernel scheduler.
    put(root, 'etc/default/steamos-scheduler', 'GOVERNOR=schedutil\nSCHEDULER=none\n')
    (root / 'etc/NetworkManager/conf.d/90-piano-wifi.conf').unlink(missing_ok=True)
    put(root, 'etc/NetworkManager/conf.d/99-valve-wifi-backend.conf',
        '[device]\nwifi.backend=wpa_supplicant\n')
    # Steam otherwise asks Manager to switch back to iwd at every launch.
    # Use the client's own backend preference, for Gaming Mode and KDE alike.
    put(root, 'etc/environment.d/60-piano-steam-wifi.conf',
        'STEAM_USE_WPASUPPLICANT=1\n')
    bsp = Path(__file__).resolve().parents[1] / 'linux/bsp/steamos'
    put(root, 'usr/share/steamos-manager/devices/xiaomi-piano.toml',
        (bsp / 'xiaomi-piano.toml').read_text())
    return units


def configure(root):
    # Frame mounts its own ESP at /efi and links /boot/efi there. SunUEFI's
    # shared fstab mounts SUNUEFI_ESP directly at /boot/efi.
    esp = root / 'boot/efi'
    if esp.is_symlink():
        esp.unlink()
    esp.mkdir(parents=True, exist_ok=True)
    for name in ('steamdeck.conf', 'steamos.conf'):
        (root / 'etc/sddm.conf.d' / name).unlink(missing_ok=True)
    # Frame's microphone processing and fixed two-channel spatial pipeline
    # must not override Piano's standard UCM routes and four-speaker layout.
    for name in ('40-mic-processing.conf', '50-alsa-config.conf',
                 '51-alsa-disable.conf', '60-spatial-audio.conf',
                 '70-spatial-node-config.conf'):
        (root / 'etc/wireplumber/wireplumber.conf.d' / name).unlink(missing_ok=True)
    units = configure_platform(root)
    configure_package_updates(root)
    offload = root / 'etc/tmpfiles.d/steamos-offload.conf'
    offload.parent.mkdir(parents=True, exist_ok=True)
    offload.unlink(missing_ok=True)
    offload.symlink_to('/dev/null')
    # The VR-only payload is not part of the handheld client/runtime. Its
    # PulseAudio link points at a Debian path absent from the Valve Arch image.
    if (root / 'opt/steamvr').exists():
        shutil.rmtree(root / 'opt/steamvr')
    desktop_link = root / 'usr/lib/steam/steam.desktop'
    desktop_link.unlink(missing_ok=True)
    desktop_link.symlink_to('/usr/share/applications/steam.desktop')
    # Keep this link relative to the *nested* x86 root, not the host ARM root.
    keyutils = root / 'usr/share/guestos/fex-mesa/usr/lib32/libkeyutils.so'
    keyutils.unlink()
    keyutils.symlink_to('libkeyutils.so.1')
    # Keep the native client and translation runtime. Do not reuse Frame's
    # VR session, including its low-space game deletion policy.
    bsp = Path(__file__).resolve().parents[1] / 'linux/bsp/steamos'
    for name in ('piano-gamescope-session', 'piano-gamescope-run',
                 'piano-gamescope-steam'):
        put(root, 'usr/local/bin/' + name, (bsp / name).read_text(), 0o755)
    for name in ('gamescope-session.service', 'gamescope-session.target',
                 'gamescope-ibus.service'):
        put(root, 'etc/systemd/user/' + name, (bsp / name).read_text())
    put(root, 'usr/share/wayland-sessions/gamescope-wayland.desktop',
        '[Desktop Entry]\nName=Steam Gaming Mode\n'
        'Exec=/usr/local/bin/piano-gamescope-session\n'
        'Type=Application\nDesktopNames=gamescope\n')
    configure_kde(root, subprocess.run, input_method='ibus')
    configure_input_method(root)
    configure_bluetooth_default(root)
    project = Path(__file__).resolve().parents[1]
    base = json.loads((project / 'config/steamos.json').read_text())['base']
    commit = subprocess.check_output(
        ['git', '-C', project, 'rev-parse', '--short=12', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(
        ['git', '-C', project, 'status', '--porcelain', '--untracked-files=no'], text=True)
    build_id = 'sunuefi-' + datetime.now(timezone.utc).strftime('%Y%m%d') + '-' + commit
    if dirty:
        build_id += '-dirty'
    identity = configure_identity(root, base, build_id)
    # SteamOS Manager writes a later temporary SDDM session override when
    # switching modes. Relogin lets SDDM follow it after the old session exits.
    put(root, 'etc/sddm.conf.d/10-sunuefi.conf',
        '[Autologin]\nUser=piano\nSession=gamescope-wayland.desktop\nRelogin=true\n')
    put(root, 'etc/locale.conf', 'LANG=zh_CN.UTF-8\nLANGUAGE=zh_CN:zh\n')
    # Steam keeps its own language preference, separate from LANG. Seed the
    # ordinary client registry in the fresh image; later user choices persist.
    registry = root / 'home/piano/.steam/registry.vdf'
    registry.parent.mkdir(parents=True, exist_ok=True)
    registry.write_text(
        '"Registry"\n{\n\t"HKCU"\n\t{\n\t\t"Software"\n\t\t{\n'
        '\t\t\t"Valve"\n\t\t\t{\n'
        '\t\t\t\t"Steam"\n\t\t\t\t{\n\t\t\t\t\t"language" "schinese"\n\t\t\t\t}\n'
        '\t\t\t\t"Steamsteamglobal"\n\t\t\t\t{\n\t\t\t\t\t"language" "schinese"\n\t\t\t\t}\n'
        '\t\t\t}\n\t\t}\n\t}\n}\n')
    registry.chmod(0o644)
    os.chown(registry, 1000, 1000)
    os.chown(registry.parent, 1000, 1000)
    localtime = root / 'etc/localtime'
    localtime.unlink(missing_ok=True)
    localtime.symlink_to('/usr/share/zoneinfo/Asia/Shanghai')
    put(root, 'etc/hostname', 'piano\n')
    put(root, 'etc/sudoers.d/20-piano', '%wheel ALL=(ALL:ALL) ALL\n', 0o440)
    resolver = root / 'etc/resolv.conf'
    resolver.unlink(missing_ok=True)
    resolver.symlink_to('/run/systemd/resolve/stub-resolv.conf')
    backlight = root / 'etc/systemd/system/systemd-backlight@.service'
    if backlight.is_symlink() and str(backlight.readlink()) == '/dev/null':
        backlight.unlink()
    # Gaming Mode has no desktop power daemon. Let logind suspend on a short
    # press; Plasma's standard handle-power-key inhibitor takes over in KDE.
    (root / 'etc/systemd/logind.conf.d/10-logind-no-powerbutton.conf').unlink(missing_ok=True)
    put(root, 'etc/systemd/logind.conf.d/20-piano.conf',
        '[Login]\nHandlePowerKey=suspend\nHandleLidSwitch=suspend\n')
    # The standard distribution desktop owns power-button policy.
    for directory in ('etc/environment.d', 'etc/profile.d'):
        for file in (root / directory).glob('*'):
            if file.is_file() and not file.is_symlink():
                text = file.read_text(errors='replace')
                if '/usr/share/deckard/mesavars.sh' in text:
                    file.unlink()
    return {'initial_session': 'gamescope', 'desktop_session': 'plasma',
            'gaming_session': 'gamescope',
            'os_release': identity,
            'frame_units_disabled': list(units),
            'steam_first_launch_requires_network': True,
            'gaming_session_device_verified': False}
