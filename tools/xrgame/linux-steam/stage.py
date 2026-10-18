#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage source-built client userspace and adapters; never stage Valve binaries."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--proot', type=Path, required=True)
    p.add_argument('--wine', type=Path, required=True)
    p.add_argument('--rootfs', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--gpu', type=Path, help='output of build-gpu.sh; omit to keep CEF software drawing only')
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    assets = args.output / 'assets/linux-steam'
    native = args.output / 'jniLibs/arm64-v8a'
    assets.mkdir(parents=True, exist_ok=True)
    native.mkdir(parents=True, exist_ok=True)
    proot = json.loads((args.proot / 'proot-build.json').read_text())
    if proot['proot'] != '4dba3afbf3a63af89b4d9c1a59bf2bda10f4d10f':
        raise ValueError('Unexpected PRoot source')
    for name in ('libproot.so', 'libproot-loader.so'):
        if digest(args.proot / name) != proot['files'][name]:
            raise ValueError('Changed PRoot binary: ' + name)
        shutil.copy2(args.proot / name, native / name)
    compiler = Path(os.environ['ANDROID_NDK_HOME']) / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang'
    subprocess.run([str(compiler), '-O2', '-g', '-Wl,--build-id=sha1', str(Path(__file__).with_name('session-probe.c')),
                    '-ldl', '-o', str(native / 'libxrgame_steam_session.so')], check=True)
    repo = Path(__file__).resolve().parents[3]
    droiddeck = repo / 'references/DroidDeck'
    shim_names = ('sysv.c', 'net.c', 'udevmon.c', 'binfmt.c', 'tracer.c', 'lock.c')
    shims = [droiddeck / 'tools/linuxfs/preload' / name for name in shim_names]
    subprocess.run(['aarch64-linux-gnu-gcc', '-O2', '-g', '-shared', '-fPIC', '-pthread', '-Wl,--no-undefined',
                    '-Wl,--build-id=sha1', *map(str, shims), str(Path(__file__).with_name('client-proc.c')),
                    '-ldl', '-o', str(assets / 'libsteampsp-client-compat.so')], check=True)
    (assets / 'client-compat-build.json').write_text(json.dumps({
        'droiddeck': subprocess.check_output(['git', '-C', str(droiddeck), 'rev-parse', 'HEAD'], text=True).strip(),
        'sources': {str(path.relative_to(repo)): digest(path) for path in shims},
        'adapterSha256': digest(Path(__file__).with_name('client-proc.c')),
        'scope': 'glibc official Steam client only; no game or input hooks; web helper GL vendor switch only with XRGAME_WEBHELPER_GL_VENDOR',
    }, indent=2) + '\n')
    # Normalize absolute guest symlinks to relative links before Android extracts them.
    # No devices, credentials, or build-host special files enter this rootfs.
    # aapt2 transparently decompresses .gz assets and drops the suffix; .tgz stays byte-exact.
    old_archive = assets / 'rootfs.tar.gz'
    if old_archive.exists() and not (assets / 'rootfs.tgz').exists():
        old_archive.rename(assets / 'rootfs.tgz')
    rootfs = assets / 'rootfs.tgz'
    if not rootfs.exists() or not (assets / 'rootfs-source.sha256').exists() or (assets / 'rootfs-source.sha256').read_text().strip() != digest(args.rootfs):
        with tarfile.open(args.rootfs) as source, tarfile.open(rootfs, 'w:gz', compresslevel=3) as out:
            for member in source:
                name = PurePosixPath(member.name)
                if name.is_absolute() or '..' in name.parts or not name.parts:
                    raise ValueError('Unsafe userspace archive path')
                if not (member.isfile() or member.isdir() or member.issym() or member.islnk()):
                    raise ValueError('Unsupported userspace entry: ' + member.name)
                if member.issym():
                    if member.linkname.startswith('/'):
                        member.linkname = os.path.relpath(member.linkname.lstrip('/'), str(name.parent))
                    resolved = os.path.normpath(str(name.parent / member.linkname))
                    if resolved == '..' or resolved.startswith('../'):
                        raise ValueError('Userspace symlink escape')
                if member.islnk() and (PurePosixPath(member.linkname).is_absolute() or '..' in PurePosixPath(member.linkname).parts):
                    raise ValueError('Userspace hardlink escape')
                member.uid = member.gid = 0
                member.uname = member.gname = ''
                member.mode &= 0o777
                out.addfile(member, source.extractfile(member) if member.isfile() else None)
        (assets / 'rootfs-source.sha256').write_text(digest(args.rootfs) + '\n')
    wine = json.loads((args.wine / 'wine-client-build.json').read_text())
    if wine['sha256'] != digest(args.wine / 'lsteamclient.so') or wine['wine_pin'] != '5d0d333e7beef02fbb455cc3163b4e8e25615458':
        raise ValueError('Changed Wine adapter')
    for name in ('lsteamclient.so', 'wine-client-build.json'):
        shutil.copy2(args.wine / name, assets / name)
    shutil.copy2(args.proot / 'proot-build.json', assets / 'proot-build.json')
    for name in ('install-client.py', 'launch-client.py', 'ime-text.py', 'game-launch.py'):
        shutil.copy2(Path(__file__).with_name(name), assets / name)
    stage_gpu(args.gpu, assets)
    files = {str(x.relative_to(args.output)): digest(x) for x in sorted(args.output.rglob('*'))
             if x.is_file() and x.name != 'manifest.json'}
    manifest = {'schema': 1, 'purpose': 'internal-client-validation', 'valveBinariesBundled': False, 'files': files}
    (assets / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


def stage_gpu(gpu, assets):
    """Pack the glibc Turnip component byte-exact, or remove a previous one."""
    archive, record_path = assets / 'gpu.tgz', assets / 'gpu-build.json'
    if gpu is None:
        archive.unlink(missing_ok=True)
        record_path.unlink(missing_ok=True)
        return
    record = json.loads((gpu / 'gpu-build.json').read_text())
    if not record['scope'].startswith('official Steam client userspace only'):
        raise ValueError('Unexpected GPU component scope')
    for name, expected in record['files'].items():
        if PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts or digest(gpu / name) != expected:
            raise ValueError('Changed GPU component file: ' + name)
    # Fixed mtimes and owners keep the archive reproducible from the same build.
    with gzip.GzipFile(archive, 'wb', compresslevel=6, mtime=0) as packed, \
            tarfile.open(fileobj=packed, mode='w', format=tarfile.PAX_FORMAT) as out:
        for name in sorted(record['files']):
            info = tarfile.TarInfo(name)
            info.size = (gpu / name).stat().st_size
            info.mode = 0o755 if name.startswith('lib/') else 0o644
            with (gpu / name).open('rb') as stream:
                out.addfile(info, stream)
    shutil.copy2(gpu / 'gpu-build.json', record_path)


if __name__ == '__main__': main()
