import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('packages', Path(__file__).parents[1] / 'verify-runtime-packages.py')
packages = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packages)


class PackageValidationTest(unittest.TestCase):
    def archive(self, *, corrupt=False, extra=None, links=None, omit_record=False):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        path = Path(temp.name) / 'driver.txz'
        library = b'synthetic-library-fixture'
        files = {'libvulkan_freedreno.so': library,
                 'meta.json': json.dumps({'libraryName': 'libvulkan_freedreno.so'}).encode()}
        record = {'files': {name: {'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data)}
                            for name, data in files.items()}}
        for name, target in (links or {}).items():
            record['files'][name] = {'symlink': target}
        if corrupt:
            files['libvulkan_freedreno.so'] = b'changed'
        if extra:
            files[extra] = b'escape'
        if not omit_record:
            files['xrgame-build.json'] = json.dumps(record).encode()
        with tarfile.open(path, 'w:xz') as archive:
            for name, data in files.items():
                member = tarfile.TarInfo(name)
                member.size = len(data)
                archive.addfile(member, io.BytesIO(data))
            for name, target in (links or {}).items():
                member = tarfile.TarInfo(name)
                member.type, member.linkname = tarfile.SYMTYPE, target
                archive.addfile(member)
        return path

    def test_valid_archive(self):
        self.assertEqual(packages.inspect(self.archive(), 'driver')['recordedFiles'], 2)

    def test_payload_differs_from_internal_record(self):
        with self.assertRaisesRegex(ValueError, 'File record mismatch'):
            packages.inspect(self.archive(corrupt=True), 'driver')

    def test_missing_file_inventory(self):
        with self.assertRaisesRegex(ValueError, 'Missing runtime file inventory'):
            packages.inspect(self.archive(omit_record=True), 'driver')

    def test_parent_path_escape(self):
        with self.assertRaisesRegex(ValueError, 'Escaping archive path'):
            packages.inspect(self.archive(extra='../outside'), 'driver')

    def test_absolute_link(self):
        with self.assertRaisesRegex(ValueError, 'Absolute runtime link'):
            packages.inspect(self.archive(links={'alias': '/outside'}), 'driver')

    def test_link_cycle(self):
        with self.assertRaisesRegex(ValueError, 'Link cycle'):
            packages.inspect(self.archive(links={'one': 'two', 'two': 'one'}), 'driver')

    def test_steam_client_requires_all_binaries_and_matching_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'steamclient.zip'
            binaries = {'steamclient_loader_x64.exe': b'loader', 'steamclient64.dll': b'client',
                        'extra/steamclient_extra_x64.dll': b'extra'}
            provenance = json.dumps({'sha256': {name: hashlib.sha256(data).hexdigest()
                                               for name, data in binaries.items()}}).encode()
            for missing, corrupt in [(False, False), (True, False), (False, True)]:
                with zipfile.ZipFile(path, 'w') as archive:
                    for name, data in binaries.items():
                        if missing and name.endswith('.exe'):
                            continue
                        archive.writestr(name, b'changed' if corrupt else data)
                    archive.writestr('LICENSE', b'license')
                    archive.writestr('provenance.json', provenance)
                if missing or corrupt:
                    with self.assertRaises(ValueError):
                        packages.inspect(path, 'steamclient')
                else:
                    self.assertEqual(packages.inspect(path, 'steamclient')['recordedFiles'], 3)


if __name__ == '__main__':
    unittest.main()
