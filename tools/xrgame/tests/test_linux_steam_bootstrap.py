import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import json
import zipfile

spec = importlib.util.spec_from_file_location('linux_steam_bootstrap', Path(__file__).parents[1] / 'linux-steam/install-client.py')
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


def manifest(extra='', file='bins.zip', sha='a' * 64):
    return ('"linuxarm64" { "version" "1" "bins_linuxarm64_linuxarm64" '
            '{ "file" "' + file + '" "sha2" "' + sha + '" "size" "123" } ' + extra + ' }')


class BootstrapTests(unittest.TestCase):
    def test_valve_signature_trailers_and_architecture_filter(self):
        source = manifest('"bins_win64" { "file" "windows.zip" }') + ' "kvsign2" { "linuxarm64" "abc" }'
        version, components = bootstrap.parts(source)
        self.assertEqual(version, '1')
        self.assertEqual([x['file'] for x in components], ['bins.zip'])

    def test_rejects_duplicate_keys_and_unknown_trailers(self):
        for source in (manifest('"version" "2"'), manifest() + ' "extra" { }', manifest()[:-1]):
            with self.subTest(source=source), self.assertRaises(ValueError):
                bootstrap.parts(source)

    def test_requires_arm64_and_valid_digest(self):
        for source in (manifest().replace('bins_linuxarm64_linuxarm64', 'bins_win64'), manifest(sha='bad')):
            with self.subTest(source=source), self.assertRaises(ValueError):
                bootstrap.parts(source)

    def test_download_allowlist_applies_to_redirects(self):
        for url in ('http://client-update.steamstatic.com/a', 'https://steamstatic.com.evil.test/a',
                    'https://evilsteamstatic.com/a', 'https://user@steamstatic.com/a',
                    'https://steamstatic.com:444/a', 'https://steamstatic.com/a?token=a'):
            with self.subTest(url=url), self.assertRaises(ValueError):
                bootstrap.valve_url(url)

    def test_archive_paths_symlinks_and_extraction_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'root'
            root.mkdir()
            archive = Path(directory) / 'input.zip'
            for name in ('../escape', '/absolute', 'C:/windows', 'a/../../escape', '..\\escape'):
                with zipfile.ZipFile(archive, 'w') as z:
                    z.writestr(name, b'content')
                with self.subTest(name=name), self.assertRaises(ValueError):
                    bootstrap.extract(archive, root, 100)
            member = zipfile.ZipInfo('link')
            member.create_system = 3
            member.external_attr = 0o120777 << 16
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr(member, '../outside')
            with self.assertRaises(ValueError): bootstrap.extract(archive, root, 100)
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('regular', b'12345')
            with self.assertRaises(ValueError): bootstrap.extract(archive, root, 4)

    def test_preserves_existing_client_without_network(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            marker = root / 'account-data'
            marker.write_text('preserve')
            with self.assertRaises(ValueError): bootstrap.install(root)
            self.assertEqual(marker.read_text(), 'preserve')

    def test_windows_separators_normalized(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'root'
            root.mkdir()
            archive = Path(directory) / 'input.zip'
            with zipfile.ZipFile(archive, 'w') as z: z.writestr('a\\b', b'content')
            self.assertEqual(bootstrap.extract(archive, root, 100), 93)
            self.assertEqual((root / 'a/b').read_bytes(), b'content')

    def test_official_android_library_selected_alongside_linux_client(self):
        android = '"bins_androidarm64_linuxarm64" { "file" "android.zip" "sha2" "' + 'b' * 64 + '" "size" "5" }'
        _, components = bootstrap.parts(manifest(android))
        self.assertEqual({x['file'] for x in components}, {'bins.zip', 'android.zip'})

    def test_windows_selection_contains_only_official_abi_archive(self):
        source = manifest().replace('"linuxarm64"', '"win64"').replace('bins_linuxarm64_linuxarm64', 'bins_win64')
        source = source[:-1] + '"steamwebhelper_win64" { "file" "ignored.zip" } }'
        _, components = bootstrap.parts(source, 'win64')
        self.assertEqual([x['name'] for x in components], ['bins_win64'])
        with self.assertRaises(ValueError): bootstrap.parts(source)

    def test_compat_extract_excludes_windows_desktop_and_preserves_existing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'legacycompat'; root.mkdir()
            archive = Path(directory) / 'input.zip'
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('steamclient64.dll', b'facade')
                z.writestr('steam.exe', b'desktop')
                z.writestr('bin/SteamService.exe', b'service')
            bootstrap.extract(archive, root, 100, bootstrap.COMPAT_FILES)
            self.assertEqual({x.name for x in root.iterdir()}, {'steamclient64.dll'})
            with self.assertRaises(ValueError): bootstrap.install_compat(root, Path(directory) / 'cache')
            self.assertEqual((root / 'steamclient64.dll').read_bytes(), b'facade')

    def test_verified_abi_cache_survives_client_updates_and_repairs_corruption(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'windows-abi'
            def stage(destination, cache):
                destination.mkdir()
                for name in bootstrap.COMPAT_FILES:
                    (destination / name).write_bytes(b'verified facade')
                (destination / 'steampsp-compat.json').write_text(json.dumps({'files': {
                    name: bootstrap.file_sha256(destination / name) for name in bootstrap.COMPAT_FILES}}))
            with patch.object(bootstrap, 'install_compat', side_effect=stage) as install:
                bootstrap.ensure_compat(root, Path(directory) / 'cache')
                client = Path(directory) / 'home/Steam/legacycompat'; client.mkdir(parents=True)
                (client / 'steamclient.dll').write_bytes(b'client update')
                bootstrap.ensure_compat(root, Path(directory) / 'cache')
                self.assertEqual(install.call_count, 1)
                (root / 'steamclient.dll').write_bytes(b'corrupt')
                bootstrap.ensure_compat(root, Path(directory) / 'cache')
                self.assertEqual(install.call_count, 2)
                self.assertTrue(bootstrap.valid_compat(root))
                self.assertEqual((client / 'steamclient.dll').read_bytes(), b'client update')

    def test_unrecognized_abi_directory_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'windows-abi'; root.mkdir()
            (root / 'keep').write_text('existing')
            with self.assertRaises(ValueError): bootstrap.ensure_compat(root, Path(directory) / 'cache')
            self.assertEqual((root / 'keep').read_text(), 'existing')

    def test_internal_symlink_and_later_escape_attempt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'root'; root.mkdir()
            archive = Path(directory) / 'input.zip'
            member = zipfile.ZipInfo('lib/link'); member.create_system = 3; member.external_attr = 0o120777 << 16
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('lib/target', b'valid')
                z.writestr(member, 'target')
            bootstrap.extract(archive, root, 100)
            self.assertEqual((root / 'lib/link').read_bytes(), b'valid')
            with zipfile.ZipFile(archive, 'w') as z: z.writestr(member, '../../outside')
            with self.assertRaises(ValueError): bootstrap.extract(archive, root, 100)
            self.assertEqual((root / 'lib/link').read_bytes(), b'valid')


if __name__ == '__main__': unittest.main()
