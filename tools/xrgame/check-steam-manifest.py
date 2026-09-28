#!/usr/bin/env python3
"""Compare a read-only sha1sum inventory with a cached Steam depot manifest.

This checks whole-file content, independently of the downloader's chunk checks.
It does not authenticate the manifest signature or replace a DepotDownloader copy.
Encrypted filenames are matched by Steam's lower-case Windows-path SHA-1; no depot
key or Steam credentials are read. Reports may contain game paths: keep them private.
"""
import argparse
import hashlib
import json
import pathlib
import struct


def varint(data, pos):
    value = 0
    for shift in range(0, 70, 7):
        if pos >= len(data):
            raise ValueError("truncated protobuf varint")
        byte = data[pos]
        pos += 1
        value |= (byte & 127) << shift
        if byte < 128:
            return value, pos
    raise ValueError("oversized protobuf varint")


def fields(data):
    pos = 0
    while pos < len(data):
        tag, pos = varint(data, pos)
        number, wire = tag >> 3, tag & 7
        if not number:
            raise ValueError("invalid protobuf field")
        if wire == 0:
            value, pos = varint(data, pos)
        elif wire in (1, 2, 5):
            if wire == 2:
                size, pos = varint(data, pos)
            else:
                size = 8 if wire == 1 else 4
            if size > len(data) - pos:
                raise ValueError("truncated protobuf field")
            value, pos = data[pos:pos + size], pos + size
        else:
            raise ValueError("unsupported protobuf wire type")
        yield number, value


def parse_manifest(raw):
    pos, payload, metadata, ended = 0, None, None, False
    while pos + 4 <= len(raw):
        magic = struct.unpack_from('<I', raw, pos)[0]
        pos += 4
        if magic == 0x32C415AB:
            ended = True
            break
        if pos + 4 > len(raw):
            raise ValueError("truncated manifest section size")
        size = struct.unpack_from('<I', raw, pos)[0]
        pos += 4
        if size > len(raw) - pos:
            raise ValueError("truncated manifest section")
        body, pos = raw[pos:pos + size], pos + size
        if magic == 0x71F617D0:
            if payload is not None:
                raise ValueError("duplicate manifest payload")
            payload = [dict(fields(v)) for n, v in fields(body) if n == 1]
        elif magic == 0x1F4812BE:
            if metadata is not None:
                raise ValueError("duplicate manifest metadata")
            metadata = dict(fields(body))
        elif magic != 0x1B81B817:
            raise ValueError("unrecognized manifest section")
    if not ended or pos != len(raw) or payload is None or metadata is None:
        raise ValueError("incomplete manifest or trailing data")
    if not metadata.get(1) or not metadata.get(2):
        raise ValueError("missing depot/manifest identity")
    return metadata, payload


def path_hash(path):
    return hashlib.sha1(path.replace('/', '\\').lower().encode('utf-8')).digest()


def compare(raw, inventory, root):
    metadata, files = parse_manifest(raw)
    prefix = root.rstrip('/') + '/'
    actual = {}
    for line in inventory.splitlines():
        if len(line) < 43 or line[40:42] not in ('  ', ' *'):
            raise ValueError("expected unescaped sha1sum lines")
        digest, path = line[:40], line[42:]
        if len(bytes.fromhex(digest)) != 20 or not path.startswith(prefix):
            raise ValueError("invalid hash or path outside inventory root")
        rel = path[len(prefix):]
        if '..' in rel.split('/') or not rel:
            raise ValueError("invalid relative inventory path")
        key = path_hash(rel)
        if key in actual:
            raise ValueError("case-duplicate inventory paths")
        actual[key] = (rel, digest.lower())
    expected, unsupported, empty_hashes = {}, [], []
    for entry in files:
        if entry.get(3, 0) & 64:  # directory
            continue
        name = entry.get(4, b'')
        content = entry.get(5, b'')
        if entry.get(7):
            unsupported.append(name.hex())
            continue
        if len(name) != 20 or len(content) != 20:
            raise ValueError("missing file name/content SHA-1")
        # Steam may use an all-zero hash for a zero-length file. Check the known
        # SHA-1 of empty content explicitly; never waive a nonempty file's hash.
        if entry.get(2, 0) == 0 and content == bytes(20):
            content = hashlib.sha1(b'').digest()
            empty_hashes.append(name.hex())
        expected[name] = content.hex()  # Steam manifest order: last same path wins
    missing, mismatch, matched = [], [], 0
    for key, digest in expected.items():
        found = actual.get(key)
        if found is None:
            missing.append(key.hex())
        elif found[1] != digest:
            mismatch.append({'path': found[0], 'expected': digest, 'actual': found[1]})
        else:
            matched += 1
    return {'schemaVersion': 1, 'depotId': metadata[1], 'manifestId': str(metadata[2]),
            'manifestSha256': hashlib.sha256(raw).hexdigest(),
            'manifestSignatureVerified': False, 'independentDownloadCompared': False,
            'expectedFiles': len(expected), 'matchedFiles': matched,
            'emptyFilesWithZeroManifestHash': len(empty_hashes),
            'missingPathHashes': missing, 'mismatchedFiles': mismatch,
            'unsupportedSymlinkPathHashes': unsupported,
            'extraFiles': sorted(value[0] for key, value in actual.items() if key not in expected),
            'passed': bool(expected) and not (missing or mismatch or unsupported)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=pathlib.Path, required=True)
    parser.add_argument('--sha1-list', type=pathlib.Path, required=True)
    parser.add_argument('--root', required=True, help='Exact absolute root used by sha1sum, possibly on Android')
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    result = compare(args.manifest.read_bytes(), args.sha1_list.read_text(), args.root)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f"depot={result['depotId']} matched={result['matchedFiles']}/{result['expectedFiles']} passed={result['passed']}")
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
