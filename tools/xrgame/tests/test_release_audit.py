import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('release_audit', Path(__file__).parents[1] / 'audit-runtime-release.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class ReleaseAuditTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.packages, self.sources, self.dependencies = [root / name for name in ['packages', 'sources', 'deps']]
        for directory in [self.packages, self.sources, self.dependencies]:
            directory.mkdir()
        self.record = {'releaseReady': True, 'pending': [], 'artifacts': []}
        self.catalog = {'items': {}}
        for category in ['proton', 'fexcore', 'dxvk', 'vkd3d', 'driver', 'imagefs']:
            filename = category + '.wcp'
            path = self.packages / filename
            path.write_bytes(category.encode())
            digest = audit.sha(path)
            self.record['artifacts'].append({'file': filename, 'sha256': digest, 'size': path.stat().st_size})
            self.catalog['items'][category] = [{
                'url': 'https://github.com/tencentmalos/xrgame-native/releases/download/test/' + filename,
                'sha256': digest, 'license': 'MIT',
                'source': 'https://github.com/tencentmalos/xrgame-native/releases/download/test/runtime-sources.tar.xz',
            }]
        for directory, prefix, key in [(self.sources, 'runtime', 'sourceArchive'),
                                       (self.dependencies, 'dependency', 'dependencySources')]:
            archive = directory / (prefix + '-sources.tar.xz')
            archive.write_bytes(b'synthetic-source-fixture')
            index = directory / (prefix + '-source-index.json')
            index.write_text(json.dumps({'archive': archive.name, 'archiveSha256': audit.sha(archive),
                                         'completeCorrespondingSource': True, 'pending': []}))
            self.record[key] = {'sha256': audit.sha(archive), 'indexSha256': audit.sha(index)}
        self.record['sourceIndexSha256'] = self.record['sourceArchive']['indexSha256']
        self.save()

    def save(self):
        catalog = self.packages / 'manifest.draft.json'
        catalog.write_text(json.dumps(self.catalog))
        self.record['catalogSha256'] = audit.sha(catalog)
        (self.packages / 'build-record.json').write_text(json.dumps(self.record))

    def result(self):
        return audit.audit(self.packages, self.sources, self.dependencies)

    def test_consistent_completed_evidence_passes(self):
        self.assertTrue(self.result()['passed'])

    def test_build_success_cannot_override_incomplete_source(self):
        path = self.dependencies / 'dependency-source-index.json'
        index = json.loads(path.read_text())
        index['completeCorrespondingSource'] = False
        path.write_text(json.dumps(index))
        self.record['dependencySources']['indexSha256'] = audit.sha(path)
        self.save()
        self.assertIn('Corresponding source remains incomplete', ' '.join(self.result()['errors']))

    def test_changed_binary_is_rejected(self):
        (self.packages / 'proton.wcp').write_bytes(b'tampered')
        self.assertIn('Artifact hash/size mismatch', ' '.join(self.result()['errors']))

    def test_source_index_cannot_be_replaced_after_finalization(self):
        path = self.sources / 'runtime-source-index.json'
        path.write_text(path.read_text() + '\n')
        self.assertIn('Source index changed', ' '.join(self.result()['errors']))

    def test_upstream_url_is_rejected_even_with_valid_hash(self):
        self.catalog['items']['proton'][0]['url'] = 'https://github.com/GameNative/runtime/releases/download/test/proton.wcp'
        self.save()
        self.assertIn('Unexpected component release URL', self.result()['errors'])

    def test_apk_in_release_directory_is_rejected(self):
        (self.packages / 'internal.apk').write_bytes(b'private')
        self.assertIn('APKs must not be published from the public repository', self.result()['errors'])

    def test_empty_component_category_is_rejected(self):
        self.catalog['items']['proton'] = []
        self.record['artifacts'] = [item for item in self.record['artifacts'] if item['file'] != 'proton.wcp']
        self.save()
        self.assertIn('Each default component category must have exactly one entry', self.result()['errors'])

    def test_pending_build_is_rejected(self):
        self.record['pending'] = ['Clean rebuild not verified']
        self.save()
        self.assertIn('Build record still has release prerequisites', self.result()['errors'])


if __name__ == '__main__':
    unittest.main()
