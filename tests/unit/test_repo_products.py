"""Verify the mapping from completed product package sets to native repositories."""
from pathlib import Path
import json
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/repo'))
from products import overlay_matrix, overlay_set, publication_batches, sha
from build.overlay_packages import DEBIAN_GNOME, DEBIAN_SHARED
from desktop import build_libcamera_packages as libcamera
from desktop.build_pipewire_packages import DEBIAN_PACKAGES
from desktop.build_plasma_camera_package import RUNTIME as PLASMA_PACKAGES


class RepoProductsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='sunuefi-package-sets-')
        self.addCleanup(temporary.cleanup)
        self.packages = Path(temporary.name)
        self.camera_deb = self.packages / 'camera/debian/piano-camera_1.0-1_arm64.deb'
        self.camera_pacman = self.packages / 'camera/pacman/piano-camera-1.0-1-aarch64.pkg.tar.zst'
        self.topology_deb = self.packages / 'bsp/audio-topology/debian/piano-audio-topology_1_all.deb'
        self.topology_pacman = self.packages / 'bsp/audio-topology/pacman/piano-audio-topology-1-1-any.pkg.tar.zst'
        self.power_deb = self.packages / 'bsp/power-profiles/debian/piano-power-profiles_1_all.deb'
        self.power_pacman = self.packages / 'bsp/power-profiles/pacman/piano-power-profiles-1-1-any.pkg.tar.zst'
        self.sensor = self.packages / 'bsp/sensors/piano-sensors-5-1-any.pkg.tar.zst'

    def shared_camera(self):
        return patch('products.camera.shared_package', side_effect=(
            (self.camera_deb, {'sha256': 'debian-camera'}),
            (self.camera_pacman, {'sha256': 'pacman-camera'}),
        ))

    def shared_topology(self):
        return patch('products.topology_package', side_effect=(
            (self.topology_deb, {'sha256': 'debian-topology'}),
            (self.topology_pacman, {'sha256': 'pacman-topology'}),
        ))

    def package_set(self, name, files, **identity):
        directory = self.packages / name
        directory.mkdir(parents=True)
        record = {**identity, 'packages': {}}
        if name.startswith('overlays/'):
            record.update(schema_version=1, target=directory.name)
        for filename, content in files.items():
            path = directory / filename
            path.write_bytes(content)
            record['packages'][filename] = {'sha256': sha(path)}
        (directory / 'manifest.json').write_text(json.dumps(record))

    def debian_set(self, target, mesa, *, missing=None):
        directory = self.packages / 'overlays' / target
        directory.mkdir(parents=True)
        record = {'schema_version': 1, 'target': target, 'repo': 'apt',
                  'packages': {}, 'components': {}}
        policy = libcamera.policy()
        abi = libcamera.abi_version(policy)
        for component in (*DEBIAN_SHARED, 'fingerprint',
                          *(DEBIAN_GNOME if target == 'debian-gnome' else ())):
            names = {'libcamera': libcamera.runtime_packages(policy), 'pipewire': DEBIAN_PACKAGES,
                     'plasma_camera': PLASMA_PACKAGES}.get(
                         component, {'test-' + component.replace('_', '-'): 'arm64'})
            version = {'mesa': '27', 'libcamera': libcamera.base_package_version(policy)}.get(component, '1')
            packages = {}
            for name, architecture in names.items():
                if name == missing:
                    continue
                if component == 'mesa':
                    name = 'mesa'
                filename = f'{name}_{version}_{architecture}.deb'
                path = directory / filename
                path.write_bytes(mesa if component == 'mesa' else name.encode())
                metadata = {'version': version, 'architecture': architecture, 'sha256': sha(path)}
                packages[name] = {**metadata, 'file': filename}
                record['packages'][filename] = {**metadata, 'name': name, 'component': component}
            record['components'][component] = {'packages': packages, 'package_version': version}
        dependencies = {name: {key: value for key, value in row.items() if key != 'file'}
                        for name, row in record['components']['libcamera']['packages'].items()
                        if name in (f'libcamera{abi}', 'libcamera-dev', f'libcamera-ipa-{abi}')}
        for component in ('pipewire', 'plasma_camera'):
            record['components'][component]['build_dependencies'] = dependencies
        (directory / 'manifest.json').write_text(json.dumps(record))
        return directory

    def test_maps_kernel_and_arch_packages_to_the_same_index(self):
        kernel = 'linux-piano-1-1-aarch64.pkg.tar.zst'
        graphics = 'mesa-27-1-aarch64.pkg.tar.zst'
        firmware = 'piano-firmware-1-1-any.pkg.tar.zst'
        proxy = 'iio-sensor-proxy-3.9-1-aarch64.pkg.tar.zst'
        deb_kernel = 'linux-piano_1_arm64.deb'
        deb_shared = 'mesa_27_arm64.deb'
        device_deb = self.packages / 'piano-device-config_1_all.deb'
        device_pacman = self.packages / 'piano-device-config-1-1-any.pkg.tar.zst'
        self.package_set('kernel/debian', {deb_kernel: b'kernel'}, format='debian')
        self.package_set('kernel/pacman', {kernel: b'kernel'}, format='pacman')
        # Local display archives in the shared cache are not publication inputs.
        self.package_set('bsp/display-firmware/debian', {
            'piano-firmware-display_0.1.0_all.deb': b'local calibration'})
        self.package_set('bsp/display-firmware/pacman', {
            'piano-firmware-display-0.1.0-1-any.pkg.tar.zst': b'local calibration'})
        debian = self.debian_set('debian-gnome', b'same build')
        self.debian_set('debian-kde', b'same build')
        self.package_set('overlays/nyarch-gnome', {firmware: b'device', graphics: b'arch', proxy: b'arch-proxy'},
                         repo='sunuefi')
        self.package_set('overlays/steamos', {graphics: b'valve', proxy: b'valve-proxy'},
                         repo='sunuefi-steamos')

        targets = ('debian-gnome', 'debian-kde', 'nyarch-gnome', 'steamos')
        with patch('products.bsp_package', side_effect=(
                (device_deb, {'sha256': 'debian'}), (self.power_deb, {'sha256': 'debian-power'}),
                (device_pacman, {'sha256': 'pacman'}), (self.power_pacman, {'sha256': 'pacman-power'}))) as bsp, \
                self.shared_camera() as camera, self.shared_topology() as topology, \
                patch('products.sensors.pacman_package',
                      return_value=(self.sensor, {'sha256': 'sensors'})) as sensors, \
                patch('products.sensors.check_ssc_provider') as provider:
            batches = publication_batches(self.packages, kernel=True, overlays=targets)
        self.assertEqual([call.args for call in bsp.call_args_list],
                         [(self.packages / 'bsp/debian', 'debian'),
                          (self.power_deb.parent, 'debian'),
                          (self.packages / 'bsp/pacman', 'pacman'),
                          (self.power_pacman.parent, 'pacman')])
        self.assertEqual([call.args for call in camera.call_args_list],
                         [('debian', self.camera_deb.parent),
                          ('pacman', self.camera_pacman.parent)])
        self.assertEqual([call.args for call in topology.call_args_list],
                         [(self.packages / 'bsp', 'debian'), (self.packages / 'bsp', 'pacman')])
        sensors.assert_called_once_with(self.sensor.parent)
        self.assertEqual([call.args[0] for call in provider.call_args_list],
                         [self.packages / f'overlays/{target}/{proxy}' for target in targets[2:]])
        self.assertEqual(list(batches), ['apt', 'sunuefi-steamos', 'sunuefi'])
        self.assertEqual({name: [path.name for path in paths] for name, paths in batches.items()},
                         {'apt': sorted({path.name for path in debian.glob('*.deb')} | {
                             deb_kernel, deb_shared, self.topology_deb.name,
                             self.camera_deb.name, device_deb.name, self.power_deb.name}),
                          'sunuefi': [proxy, kernel, graphics, self.topology_pacman.name,
                                      self.camera_pacman.name, device_pacman.name, firmware,
                                      self.power_pacman.name, self.sensor.name],
                          'sunuefi-steamos': [proxy, graphics]})
        arch = self.packages / 'overlays/nyarch-gnome' / graphics
        valve = self.packages / 'overlays/steamos' / graphics
        self.assertEqual(batches['sunuefi'][arch], sha(arch))
        self.assertEqual(batches['sunuefi-steamos'][valve], sha(valve))

    def test_does_not_publish_common_bsp_from_steamos_overlays(self):
        self.package_set('kernel/debian', {'linux.deb': b'kernel'}, format='debian')
        self.package_set('kernel/pacman', {'linux.pkg.tar.zst': b'kernel'}, format='pacman')
        device_deb = self.packages / 'piano-device-config_1_all.deb'
        device_pacman = self.packages / 'piano-device-config-1-1-any.pkg.tar.zst'
        proxy = {'iio-sensor-proxy-3.9-1-aarch64.pkg.tar.zst': b'proxy'}
        self.package_set('overlays/nyarch-gnome', proxy, repo='sunuefi')
        self.package_set('overlays/steamos', {**proxy, device_pacman.name: b'valve'},
                         repo='sunuefi-steamos')
        with patch('products.bsp_package', side_effect=(
                (device_deb, {'sha256': 'debian'}), (self.power_deb, {'sha256': 'debian-power'}),
                (device_pacman, {'sha256': 'pacman'}), (self.power_pacman, {'sha256': 'pacman-power'}))), \
                self.shared_camera(), self.shared_topology(), \
                patch('products.sensors.pacman_package',
                      return_value=(self.sensor, {'sha256': 'sensors'})), \
                patch('products.sensors.check_ssc_provider'):
            with self.assertRaisesRegex(ValueError, 'Common package duplicated'):
                publication_batches(self.packages, kernel=True, overlays=('nyarch-gnome', 'steamos'))

    def test_partial_arch_publication_is_refused_before_any_archive_is_opened(self):
        with self.assertRaisesRegex(ValueError, 'Publish both Arch overlay sets'):
            publication_batches(self.packages, kernel=True, overlays=('nyarch-gnome',))

    def test_duplicate_names_within_one_repo_must_match(self):
        self.debian_set('debian-gnome', b'first')
        self.debian_set('debian-kde', b'different')
        with self.assertRaisesRegex(ValueError, 'share a package filename'):
            publication_batches(self.packages, kernel=False, overlays=('debian-gnome', 'debian-kde'))

    def test_cache_cannot_redirect_a_package_set_to_another_repo(self):
        self.package_set('overlays/steamos', {'mesa.pkg.tar.zst': b'valve'}, repo='sunuefi')
        with self.assertRaisesRegex(ValueError, 'does not match its build target'):
            publication_batches(self.packages, kernel=False, overlays=('steamos',))

    def test_modified_package_is_rejected_before_any_batch_is_returned(self):
        package = 'mesa.pkg.tar.zst'
        self.package_set('overlays/steamos', {package: b'valve'}, repo='sunuefi-steamos')
        (self.packages / 'overlays/steamos' / package).write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'Package manifest differs'):
            publication_batches(self.packages, kernel=False, overlays=('steamos',))

    def test_only_requested_overlay_generations_are_published(self):
        self.debian_set('debian-gnome', b'archive')
        self.package_set('overlays/steamos', {'mesa.pkg.tar.zst': b'valve'}, repo='sunuefi-steamos')
        batches = publication_batches(self.packages, kernel=False,
                                      overlays=overlay_matrix('{"target":["debian-gnome"]}'))
        self.assertEqual(list(batches), ['apt'])
        for invalid in ('{"target":["uefi"]}', '{"target":["steamos","steamos"]}',
                        '{"target":"steamos"}', '["steamos"]'):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                overlay_matrix(invalid)

    def test_incomplete_debian_pipewire_set_cannot_be_published(self):
        directory = self.debian_set('debian-gnome', b'mesa', missing='pipewire-bin')
        with self.assertRaisesRegex(ValueError, 'all native packages at the same version'):
            overlay_set(directory, 'debian-gnome')

    def test_unpaired_camera_packages_cannot_be_published(self):
        directory = self.debian_set('debian-gnome', b'mesa')
        manifest = directory / 'manifest.json'
        record = json.loads(manifest.read_text())
        record['components']['pipewire'].pop('build_dependencies')
        manifest.write_text(json.dumps(record))
        with self.assertRaisesRegex(ValueError, 'pipewire was not built with this libcamera package set'):
            overlay_set(directory, 'debian-gnome')


if __name__ == '__main__':
    unittest.main()
