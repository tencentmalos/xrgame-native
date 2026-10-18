import hashlib
import importlib.util
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

path = Path(__file__).parents[1] / 'linux-steam/stage.py'
spec = importlib.util.spec_from_file_location('linux_steam_stage', path)
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)


class GpuComponentStageTest(unittest.TestCase):
    def component(self, root):
        files = {'lib/libvulkan_freedreno.so': b'\x7fELF driver', 'share/doc/mesa/license.rst': b'MIT'}
        for name, data in files.items():
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_bytes(data)
        record = {'scope': 'official Steam client userspace only; games use the Android Turnip',
                  'files': {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}}
        (root / 'gpu-build.json').write_text(json.dumps(record))

    def test_archive_is_reproducible_and_verified(self):
        with tempfile.TemporaryDirectory() as temp:
            gpu, assets = Path(temp, 'gpu'), Path(temp, 'assets')
            assets.mkdir()
            self.component(gpu)
            stage.stage_gpu(gpu, assets)
            first = (assets / 'gpu.tgz').read_bytes()
            stage.stage_gpu(gpu, assets)
            self.assertEqual(first, (assets / 'gpu.tgz').read_bytes())
            with tarfile.open(assets / 'gpu.tgz') as archive:
                members = {m.name: m for m in archive.getmembers()}
            self.assertEqual({'lib/libvulkan_freedreno.so', 'share/doc/mesa/license.rst'}, set(members))
            self.assertEqual(0o755, members['lib/libvulkan_freedreno.so'].mode)
            self.assertEqual(0, members['share/doc/mesa/license.rst'].mtime)
            self.assertTrue((assets / 'gpu-build.json').is_file())

    def test_changed_file_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            gpu, assets = Path(temp, 'gpu'), Path(temp, 'assets')
            assets.mkdir()
            self.component(gpu)
            (gpu / 'lib/libvulkan_freedreno.so').write_bytes(b'changed')
            with self.assertRaises(ValueError):
                stage.stage_gpu(gpu, assets)

    def test_omitting_the_component_removes_a_previous_one(self):
        with tempfile.TemporaryDirectory() as temp:
            gpu, assets = Path(temp, 'gpu'), Path(temp, 'assets')
            assets.mkdir()
            self.component(gpu)
            stage.stage_gpu(gpu, assets)
            stage.stage_gpu(None, assets)
            self.assertFalse((assets / 'gpu.tgz').exists())
            self.assertFalse((assets / 'gpu-build.json').exists())


if __name__ == '__main__':
    unittest.main()
