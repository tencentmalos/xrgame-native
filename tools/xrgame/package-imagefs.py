#!/usr/bin/env python3
"""Assemble the Wine dependency closure; fail on missing libraries or package ownership."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tag', default='runtime-20260926-preview1')
args = parser.parse_args()
if not re.fullmatch(r'[A-Za-z0-9._-]+', args.tag) or args.tag in {'.', '..'}:
    parser.error('tag must be a release path segment')
base = Path(os.environ.get('XRGAME_BUILD_ROOT', '/work'))
deps = base / 'termuxfs/aarch64/data/data/com.termux/files/usr'
wine = Path((base / 'output/proton-stage.txt').read_text().strip())
target_prefix = '/data/user/0/com.tencentmalos.xrgamenative/files/imagefs/usr'
snapshot_sha = 'b088df560c9395d7c2cc797d208d8a727d1f06fc70e5ea34eef6118ef64a7e42'
system = {'libc.so', 'libdl.so', 'libm.so', 'liblog.so', 'libandroid.so', 'libaaudio.so',
          'libOpenSLES.so', 'libEGL.so', 'libGLESv1_CM.so', 'libGLESv2.so', 'libGLESv3.so',
          'libjnigraphics.so', 'libnativewindow.so', 'libsync.so', 'libhardware.so', 'libz.so',
          'libmediandk.so'}
# Only Wine's optional dlopen roots may be omitted. A DT_NEEDED edge is mandatory,
# including libgstgl's EGL/GLX dependencies when WINE_GST_NO_GL is set.
optional = {'libEGL.so.1', 'libGL.so.1', 'libGLESv2.so.2', 'libodbc.so'}
media_plugins = (
    'coreelements', 'typefindfunctions', 'playback', 'app', 'isomp4', 'matroska',
    'avi', 'ogg', 'wavparse', 'audioparsers', 'rawparse', 'audioconvert',
    'audioresample', 'videoconvertscale', 'debug', 'deinterlace', 'videofilter',
)


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def elf(path):
    with path.open('rb') as stream:
        return stream.read(4) == b'\x7fELF'


def needed(path):
    text = subprocess.check_output(['readelf', '-d', str(path)], text=True)
    return re.findall(r'\(NEEDED\).*?\[(.*?)\]', text)


owners = {}
status = {}
for paragraph in (deps / 'var/lib/dpkg/status').read_text().split('\n\n'):
    fields = dict(line.split(': ', 1) for line in paragraph.splitlines() if ': ' in line and not line.startswith(' '))
    if fields.get('Status') == 'install ok installed':
        status[fields['Package']] = fields
for listing in (deps / 'var/lib/dpkg/info').glob('*.list'):
    for name in listing.read_text().splitlines():
        prefix = '/data/data/com.termux/files/usr/'
        if name.startswith(prefix):
            owners[name[len(prefix):]] = listing.stem

providers = {}
for path in sorted((deps / 'lib').rglob('*')):
    if path.is_file() and '.so' in path.name:
        # Prefer the top-level SONAME over duplicate plugin copies.
        providers.setdefault(path.name, path)
        if path.parent == deps / 'lib': providers[path.name] = path
for path in (base / 'output/xcb').glob('*.so*'):
    if path.is_file(): providers[path.name] = path
ndk = base / 'toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64'
providers['libc++_shared.so'] = ndk / 'sysroot/usr/lib/aarch64-linux-android/libc++_shared.so'
if not (base / 'output/xcb/libxcb.so').exists():
    raise RuntimeError('Build XRGame libxcb before creating the base image')
own_sysv = deps / 'lib/libandroid-sysvshm.so'  # Built by build-wine.sh.
providers[own_sysv.name] = own_sysv
wine_elf = [p for p in (wine / 'lib/wine/aarch64-unix').iterdir() if p.is_file() and elf(p)]
wine_names = {p.name for p in wine_elf}
queue = set()
for path in wine_elf:
    queue.update(needed(path))
queue.update(set(re.findall(r'^#define SONAME_\w+ "([^"]+)"',
                           (base / 'build/proton-wine/android/include/config.h').read_text(), re.M)) - optional)
queue.update(needed(base / 'output/turnip/libvulkan_freedreno.so'))
queue.add('libandroid-sysvshm.so')
plugins = {f'libgst{name}.so': deps / 'lib/gstreamer-1.0' / f'libgst{name}.so'
           for name in media_plugins}
media_build = base / 'output/gst-libav'
media_record = json.loads((media_build / 'build.json').read_text())
if (media_record['pin'] != '9058212f43074ef7df229e73cea135c4ea96e0d6'
        or sha(media_build / 'libgstlibav.so') != media_record['files']['libgstlibav.so']['sha256']):
    raise RuntimeError('GStreamer libav does not match its pinned build record')
plugins['libgstlibav.so'] = media_build / 'libgstlibav.so'
for name, path in plugins.items():
    if not path.is_file():
        raise RuntimeError(f'Missing media plugin: {path}')
    queue.update(needed(path))
selected = {}
while queue:
    name = queue.pop()
    if name in selected or name in system or name in wine_names:
        continue
    if name not in providers:
        raise RuntimeError(f'Unresolved dependency: {name}')
    path = providers[name]
    selected[name] = path
    queue.update(needed(path))

out = base / 'packages' / args.tag
out.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='imagefs-', dir=base / 'build') as temp:
    tree = Path(temp)
    for name in ['usr/lib', 'usr/lib/gstreamer-1.0', 'usr/bin', 'usr/tmp', 'usr/etc/fonts', 'usr/share/fonts',
                 'usr/share/licenses', 'tmp/.X11-unix', 'tmp/.sysvshm', 'tmp/.sound', 'opt']:
        (tree / name).mkdir(parents=True, exist_ok=True)
    records, packages = {}, set()
    payload = dict(selected)
    payload.update({f'gstreamer-1.0/{name}': path for name, path in plugins.items()})
    for name, path in sorted(payload.items()):
        dest = tree / 'usr/lib' / name
        shutil.copy2(path, dest)  # Flatten SONAME symlinks; no external links in imagefs.
        item = {'sha256': sha(dest), 'needed': needed(dest)}
        if path.parent == base / 'output/xcb':
            item.update(source='libxcb-1.17.0', license='MIT', buildRecipe='tools/xrgame/build-xcb.sh')
        elif path == own_sysv:
            item.update(source='proton-wine/android/android_sysvshm', license='LGPL-2.1-or-later AND BSD-3-Clause',
                        buildRecipe='tools/xrgame/build-wine.sh')
        elif name == 'libc++_shared.so':
            item.update(source='Android NDK r27d', license='Apache-2.0 WITH LLVM-exception')
        elif path.parent == media_build:
            item.update(source=media_record['source'], sourcePin=media_record['pin'],
                        license='LGPL-2.1-or-later', buildRecipe='tools/xrgame/build-gst-libav.sh')
        else:
            owner = owners.get(str(path.relative_to(deps))) or owners.get(str(path.resolve().relative_to(deps)))
            if owner not in status:
                raise RuntimeError(f'Missing package ownership: {path}')
            packages.add(owner)
            item.update(source='Termux snapshot', package=owner, version=status[owner]['Version'])
        records[str(dest.relative_to(tree))] = item
    for path in sorted((deps / 'share/fonts').rglob('*')):
        if path.is_file():
            relative = path.relative_to(deps)
            owner = owners.get(str(relative))
            if owner not in status: raise RuntimeError(f'Font has no package owner: {path}')
            packages.add(owner)
            dest = tree / 'usr' / relative
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, dest)
            records[str(dest.relative_to(tree))] = {'sha256': sha(dest), 'package': owner,
                                                    'version': status[owner]['Version']}
    package_records = {}
    for name in sorted(packages):
        docs = deps / 'share/doc' / name
        if not docs.is_dir() or not any(docs.iterdir()):
            raise RuntimeError(f'Package has no license/documentation directory: {name}')
        shutil.copytree(docs, tree / 'usr/share/licenses' / name, symlinks=False)
        package_records[name] = {'version': status[name]['Version'], 'homepage': status[name].get('Homepage'),
                                 'licenseFiles': [str(p.relative_to(docs)) for p in docs.rglob('*') if p.is_file()]}
    shutil.copy2(base / 'output/xcb/LICENSE', tree / 'usr/share/licenses/libxcb-source-MIT')
    shutil.copy2(media_build / 'LICENSE', tree / 'usr/share/licenses/gst-libav-LGPL-2.1')
    project = Path(os.environ.get('XRGAME_PROJECT_ROOT', base / 'project'))
    shutil.copy2(project / 'tools/xrgame/licenses/android-shmem-BSD-3-Clause.txt',
                 tree / 'usr/share/licenses/android-shmem-BSD-3-Clause.txt')
    shutil.copy2(base / 'src/proton-wine/COPYING.LIB', tree / 'usr/share/licenses/Wine-LGPL-2.1.txt')
    shutil.copy2(base / 'toolchains/android-ndk-r27d/NOTICE.toolchain',
                 tree / 'usr/share/licenses/Android-NDK-toolchain-NOTICE')
    (tree / 'usr/etc/fonts/fonts.conf').write_text(
        '<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "fonts.dtd">\n<fontconfig>\n'
        f'  <dir>{target_prefix}/share/fonts</dir>\n'
        '  <cachedir prefix="xdg">fontconfig</cachedir>\n</fontconfig>\n')
    for path in sorted(tree.rglob('*')):
        if path.is_file():
            records.setdefault(str(path.relative_to(tree)), {'sha256': sha(path)})
    record = {'schema': 1, 'termuxSnapshotSha256': snapshot_sha, 'files': records,
              'packages': package_records, 'systemLibraries': sorted(system),
              'omittedOptionalLibraries': sorted(optional - selected.keys()),
              'mediaPlugins': sorted(plugins), 'gstLibavBuild': media_record, 'releaseReady': False,
              'retiredFiles': {'usr/lib/libmediandk.so': sha(deps / 'lib/libmediandk.so')},
              'pending': ['Corresponding source archives for dependencies', 'Device validation']}
    (tree / 'xrgame-imagefs.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
    with (out / 'imagefs_bionic.txz').open('wb') as output:
        tar = subprocess.Popen(['tar', '--sort=name', '--format=gnu', '--owner=0', '--group=0',
                                '--numeric-owner', '--mtime=@1790380800', '-C', str(tree), '-cf', '-', '.'],
                               stdout=subprocess.PIPE)
        subprocess.run(['xz', '-T4', '-3', '-c'], stdin=tar.stdout, stdout=output, check=True)
        tar.stdout.close()
        if tar.wait(): raise RuntimeError('tar failed')
    (out / 'imagefs-record.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
    print(json.dumps({'libraries': len(selected), 'packages': len(packages),
                      'sha256': sha(out / 'imagefs_bionic.txz'), 'bytes': (out / 'imagefs_bionic.txz').stat().st_size}))
