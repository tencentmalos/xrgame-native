import hashlib
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('steam_check', Path(__file__).resolve().parents[1] / 'check-steam-manifest.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def vi(value):
    out = bytearray()
    while value >= 128:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)


def field(number, value):
    if isinstance(value, int):
        return vi(number << 3) + vi(value)
    return vi(number << 3 | 2) + vi(len(value)) + value


def manifest(link=False, empty=False):
    # Encrypted filename text is intentionally unusable; only its Steam path hash maps it.
    entry = field(1, b'not-a-plaintext-name') + field(2, 0 if empty else 2)
    entry += field(4, checker.path_hash('Data/Example.bin')) + field(5, bytes(20) if empty else hashlib.sha1(b'ok').digest())
    if link:
        entry += field(7, b'target')
    payload = field(1, entry)
    metadata = field(1, 123) + field(2, 456) + field(4, 1)
    return (struct.pack('<II', 0x71F617D0, len(payload)) + payload +
            struct.pack('<II', 0x1F4812BE, len(metadata)) + metadata + struct.pack('<I', 0x32C415AB))


class ManifestTests(unittest.TestCase):
    def test_encrypted_name_matches_case_normalized_path(self):
        line = hashlib.sha1(b'ok').hexdigest() + '  /game/data/EXAMPLE.bin\n'
        result = checker.compare(manifest(), line, '/game')
        self.assertTrue(result['passed'])
        self.assertEqual(result['matchedFiles'], 1)
        self.assertFalse(result['manifestSignatureVerified'])
        self.assertFalse(result['independentDownloadCompared'])

    def test_empty_file_zero_hash_is_checked_not_waived(self):
        empty_line = hashlib.sha1(b'').hexdigest() + '  /game/Data/Example.bin\n'
        result = checker.compare(manifest(empty=True), empty_line, '/game')
        self.assertTrue(result['passed'])
        self.assertEqual(result['emptyFilesWithZeroManifestHash'], 1)
        wrong = hashlib.sha1(b'x').hexdigest() + '  /game/Data/Example.bin\n'
        self.assertFalse(checker.compare(manifest(empty=True), wrong, '/game')['passed'])

    def test_missing_and_corrupt_fail(self):
        self.assertFalse(checker.compare(manifest(), '', '/game')['passed'])
        line = hashlib.sha1(b'wrong').hexdigest() + '  /game/Data/Example.bin\n'
        self.assertEqual(len(checker.compare(manifest(), line, '/game')['mismatchedFiles']), 1)
        self.assertFalse(checker.compare(manifest(True), line, '/game')['passed'])

    def test_incomplete_and_trailing_manifest_rejected(self):
        for raw in (manifest()[:-1], manifest() + b'garbage', b'\xD0\x17\xF6\x71\xff\xff\xff\xff'):
            with self.assertRaises(ValueError):
                checker.parse_manifest(raw)

    def test_outside_paths_and_case_aliases_rejected(self):
        digest = hashlib.sha1(b'ok').hexdigest()
        for inventory in (digest + '  /other/Data/Example.bin', digest + '  /game/../Example.bin',
                          digest + '  /game/Data/Example.bin\n' + digest + '  /game/data/example.bin\n'):
            with self.assertRaises(ValueError):
                checker.compare(manifest(), inventory, '/game')


if __name__ == '__main__':
    unittest.main()
