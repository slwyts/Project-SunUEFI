"""Evidence mapping for retained DT diagnostics, not a hardware readiness gate."""
import hashlib
import re
from collections import Counter, defaultdict


def available(tree, path):
    while True:
        value = tree[path].get('status')
        if value is not None and value.rstrip(b'\0') not in (b'ok', b'okay'):
            return False, path
        if path == '/':
            return True, None
        path = path.rsplit('/', 1)[0] or '/'


def driver_matches(kernel, wanted):
    """Literal OF tables referenced by .of_match_table; helpers are separate."""
    matches, helpers, sources = defaultdict(list), defaultdict(list), {}
    declaration = re.compile(r'\bstruct\s+of_device_id\s+(\w+)\s*\[[^\]]*\]\s*(?:__maybe_unused\s*)?=\s*\{(.*?)\n\};', re.S)
    for directory in ('drivers', 'sound'):
        for path in (kernel / directory).rglob('*.c'):
            text = path.read_text(errors='replace')
            if not any('"' + name + '"' in text for name in wanted):
                continue
            relative = str(path.relative_to(kernel))
            for table in declaration.finditer(text):
                name = table.group(1)
                registered = re.search(r'\.of_match_table\s*=\s*(?:of_match_ptr\s*\(\s*)?' + re.escape(name) + r'\b', text) is not None
                for match in re.finditer(r'\.compatible\s*=\s*"([^"]+)"', table.group(2)):
                    compatible = match.group(1)
                    if compatible not in wanted:
                        continue
                    line = text[:table.start(2) + match.start()].count('\n') + 1
                    row = {'source': relative, 'line': line, 'table': name,
                           'source_sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                    (matches if registered else helpers)[compatible].append(row)
                    sources[relative] = text
    return matches, helpers, sources


def feature(path, compatible):
    text = (path + ' ' + ' '.join(compatible)).lower()
    if 'qtb' in text or 'smmu' in text:
        return 'legacy-DMA-SMMU'
    if any(x in text for x in ('usb', 'hsphy', 'ssphy')):
        return 'USB'
    if any(x in text for x in ('ufs', 'ufshc')):
        return 'UFS'
    if any(x in text for x in ('mdss', 'sde', 'dsi', 'kgsl', 'gpu')):
        return 'display-GPU'
    if any(x in text for x in ('pcie', 'cnss', 'wlan')):
        return 'WLAN-PCIe'
    if any(x in text for x in ('adsp', 'lpass', 'audio')):
        return 'ADSP-audio-power-services'
    if any(x in text for x in ('cvp', 'vidc', 'cam-', 'cam_', 'camera')):
        return 'legacy-camera-video'
    if 'qup_uart' in text:
        return 'serial-debug'
    if any(x in text for x in ('qupv3', 'geni', 'i2c', 'spi')):
        return 'QUP-input-peripheral-residual'
    if 'clock' in text:
        return 'clock-provider'
    return 'other-retained-vendor'


def consumption(source, key, empty):
    """Narrow conclusions for the actually reviewed selected drivers only."""
    if source.endswith('qcom_q6v5_pas.c') and key == 'interconnects':
        return 'matched-driver-unused-property', 'PAS source does not obtain ICC paths; power-domain path is used.', source
    if source.endswith('qcom_geni_serial.c') and key == 'interconnects':
        return 'consumed-probe-blocker', 'geni_serial_resource_init -> geni_icc_get requires valid qup-core/config.', 'drivers/soc/qcom/qcom-geni-se.c'
    if empty and key.endswith('-supply'):
        if source.endswith('phy-qcom-m31-eusb2.c') and key in ('vdd-supply', 'vdda12-supply'):
            return 'consumed-dummy-fallback-dependency', 'M31 bulk normal regulator get consumes this rail; empty phandle leads ENODEV and populated-DT NORMAL_GET dummy fallback. No physical rail control/ownership is proved.', 'drivers/regulator/core.c'
        if source.endswith('ufs-qcom.c') and key == 'vdd-hba-supply':
            return 'consumed-optional-assumed-on', 'ufshcd_populate_vreg tests phandle_exists; empty supply means assume enabled and success, not proven rail ownership.', 'drivers/ufs/host/ufshcd-pltfrm.c'
        unused = {
            'gcc-sm8750.c': {'vdd_cx-supply', 'vdd_mx-supply'},
            'phy-qcom-m31-eusb2.c': {'vdd_refgen-supply', 'dummy-supply'},
            'phy-qcom-qmp-ufs.c': {'vdda-refgen-supply', 'vdda-qref-supply', 'vdd-phy-gdsc-supply'},
            'phy-qcom-qmp-combo.c': {'vdd-supply', 'core-supply', 'usb3_dp_phy_gdsc-supply'},
            'ufs-qcom.c': {'qcom,vccq-parent-supply', 'qcom,vddp-ref-clk-supply', 'qcom,vccq-proxy-vote-supply', 'depends-on-supply'},
            'pcie-qcom.c': {'gdsc-phy-vdd-supply'},
            'dwc3-qcom.c': {'depends-on-supply', 'USB3_GDSC-supply', 'dummy-supply'},
        }
        if key in unused.get(source.rsplit('/', 1)[-1], set()):
            return 'matched-driver-unused-property', 'Published empty legacy property is not requested by this mainline driver; retained raw schema diagnostic.', source
    return 'matched-consumption-review-required', 'Source match alone does not prove property consumption or runtime binding.', source


def map_diagnostics(kernel, parsed, before_errors, after_errors):
    tree = parsed['tree']
    wanted = {c.decode() for row in before_errors for c in tree[row['path']].get('compatible', b'').split(b'\0') if c}
    matches, helpers, _ = driver_matches(kernel, wanted)
    remaining = {(r['path'], r['property'], r['reason']) for r in after_errors}
    rows = []
    for row in before_errors:
        path, key = row['path'], row['property']
        props = tree[path]
        comp = [x.decode(errors='replace') for x in props.get('compatible', b'').split(b'\0') if x]
        enabled, disabled_parent = available(tree, path)
        drivers = [x for c in comp for x in matches.get(c, [])]
        helper_matches = [x for c in comp for x in helpers.get(c, [])]
        if not enabled:
            category, reason, evidence = 'disabled-residual', 'Node or ancestor is not OF-available; no normal probe of this residual.', None
        elif not drivers:
            category, reason, evidence = 'unmatched-vendor-residual', 'No literal OF table registered by a platform driver in selected source matches these compatibles.', None
        else:
            choices = [consumption(x['source'], key, props.get(key) == b'') for x in drivers]
            choices.sort(key=lambda x: x[0] == 'matched-consumption-review-required')
            category, reason, evidence = choices[0]
        still_present = (path, key, row['reason']) in remaining
        if category == 'consumed-probe-blocker' and not still_present:
            category = 'consumed-blocker-corrected'
        rows.append(dict(row, feature=feature(path, comp), compatible=comp,
                         effective_enabled=enabled, disabled_ancestor=disabled_parent,
                         driver_matches=drivers, helper_only_matches=helper_matches,
                         impact=category, rationale=reason, consumption_source=evidence,
                         raw_diagnostic_remaining=still_present, hardware_verified=False))
    return {'source_kernel_only': True, 'hardware_verified': False,
            'diagnostics_before': len(before_errors), 'diagnostics_after': len(after_errors),
            'impact_counts': dict(Counter(x['impact'] for x in rows)),
            'feature_counts': dict(Counter(x['feature'] for x in rows)), 'rows': rows,
            'remaining_consumed_blockers': [x for x in rows if x['impact'] == 'consumed-probe-blocker'],
            'review_required': [x for x in rows if x['impact'] == 'matched-consumption-review-required'],
            'framework_caveat': 'Driver-unused/unmatched does not authorize deletion or prove fw_devlink/sync_state behavior; hardware/provider readiness and published runtime prerequisites remain separate.'}
