#!/usr/bin/env python3
"""Archive exact runtime sources and recipes; never infer release readiness."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(os.environ.get('XRGAME_BUILD_ROOT', '/work')))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--epoch', type=int, default=1790380800)
    args = parser.parse_args()
    root = args.root.resolve()
    project = Path(os.environ.get('XRGAME_PROJECT_ROOT', root / 'project'))
    spec = importlib.util.spec_from_file_location('components', Path(__file__).with_name('package-components.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    pins = dict(module.PINS, **{'pulseaudio-android': '178f0a379af39ae3da8fefdded8b0b1e5a3fda39',
                                'gstreamer': '9058212f43074ef7df229e73cea135c4ea96e0d6'})
    required = {
        'FEX/External/vixl', 'FEX/External/fmt', 'FEX/External/xxhash', 'FEX/External/range-v3',
        'FEX/External/unordered_dense', 'FEX/External/rpmalloc', 'FEX/Source/Common/cpp-optparse',
        'pulseaudio-android/pulseaudio',
    }
    args.output.mkdir(parents=True, exist_ok=True)
    archive = args.output / 'runtime-sources.tar.xz'
    if archive.exists():
        parser.error(f'Refusing to replace source evidence: {archive}')
    sources, omitted = {}, []
    with tempfile.TemporaryDirectory(prefix='runtime-sources-', dir=root / 'build') as temp:
        tree = Path(temp)

        def export(repo, relative, expected):
            actual = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
            if actual != expected:
                raise RuntimeError(f'{relative}: wrong pin {actual}, expected {expected}')
            run('git', '-C', str(repo), 'diff', '--quiet', 'HEAD', '--ignore-submodules=all')
            dest = tree / 'src' / relative
            dest.mkdir(parents=True, exist_ok=True)
            stream = subprocess.Popen(['git', '-C', str(repo), 'archive', expected], stdout=subprocess.PIPE)
            try:
                with tarfile.open(fileobj=stream.stdout, mode='r|') as tar:
                    tar.extractall(dest, filter='data')
            finally:
                stream.stdout.close()
            if stream.wait():
                raise RuntimeError(f'git archive failed: {relative}')
            url = subprocess.check_output(['git', '-C', str(repo), 'remote', 'get-url', 'origin'], text=True).strip()
            # Keep credentials/local mirrors out of distributable source records.
            if not url.startswith('https://github.com/') or '@' in url:
                raise RuntimeError(f'{relative}: expected public source URL')
            sources[relative] = {'commit': expected, 'url': url}
            entries = subprocess.check_output(['git', '-C', str(repo), 'ls-tree', '-rz', expected])
            for entry in entries.split(b'\0'):
                if not entry:
                    continue
                meta, name = entry.split(b'\t', 1)
                mode, kind, pin = meta.decode().split()
                if mode != '160000':
                    continue
                child = name.decode()
                child_relative = relative + '/' + child
                if (repo / child / '.git').exists():
                    export(repo / child, child_relative, pin)
                else:
                    omitted.append({'path': child_relative, 'commit': pin, 'reason': 'not initialized for this build'})

        for name, pin in pins.items():
            export(root / 'src' / name, name, pin)
        missing = required - sources.keys()
        if missing:
            raise RuntimeError(f'Missing build dependencies: {sorted(missing)}')
        for name in ['tools/xrgame', 'app/src/main/windows/openxr_runtime/builtin']:
            shutil.copytree(project / name, tree / 'project' / name,
                            ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        for name in ['LICENSE', 'THIRD_PARTY_NOTICES']:
            shutil.copy2(project / name, tree / 'project' / name)
        downloaded = {}
        for name, expected in {
            'libxcb-1.17.0.tar.xz': '599ebf9996710fea71622e6e184f3a8ad5b43d0e5fa8c4e407123c88a59a6d55',
            'xcb-proto-1.17.0.tar.xz': '2c1bacd2110f4799f74de6ebb714b94cf6f80fb112316b1219480fd22562148c',
        }.items():
            path = root / 'downloads' / name
            if sha(path) != expected:
                raise RuntimeError(f'Wrong source archive SHA: {name}')
            (tree / 'downloads').mkdir(exist_ok=True)
            shutil.copy2(path, tree / 'downloads' / name)
            downloaded[name] = expected
        env = dict(os.environ, CARGO_HOME=str(root / 'toolchains/cargo'),
                   RUSTUP_HOME=str(root / 'toolchains/rustup'))
        vendor = tree / 'cargo-vendor'
        result = run(str(root / 'toolchains/cargo/bin/cargo'), 'vendor', '--locked', '--offline', str(vendor),
                     cwd=root / 'src/ntsync-android', env=env, capture_output=True, text=True)
        (tree / 'cargo-vendor-config.toml').write_text(result.stdout.replace(str(vendor), 'cargo-vendor'))
        files = {}
        for path in sorted(tree.rglob('*')):
            name = str(path.relative_to(tree))
            if path.is_symlink():
                files[name] = {'symlink': os.readlink(path)}
            elif path.is_file():
                files[name] = {'sha256': sha(path), 'bytes': path.stat().st_size}
        index = {'schema': 1, 'sources': sources, 'omittedSubmodules': omitted,
                 'downloadedSources': downloaded, 'files': files,
                 'scope': 'Runtime source trees, initialized build dependencies, Rust vendor and local recipes',
                 'completeCorrespondingSource': False,
                 'pending': ['Termux base-image dependency sources and exact recipe attribution',
                             'NDK libc++ source/toolchain attribution', 'Rebuild validation from source archive']}
        (tree / 'source-index.json').write_text(json.dumps(index, indent=2, sort_keys=True) + '\n')
        module.archive(tree, archive, args.epoch)
        (args.output / 'runtime-source-index.json').write_text(json.dumps({
            **index, 'archive': archive.name, 'archiveSha256': sha(archive),
        }, indent=2, sort_keys=True) + '\n')
    print(json.dumps({'archive': str(archive), 'sha256': sha(archive), 'sources': len(sources),
                      'files': len(files), 'completeCorrespondingSource': False}))


if __name__ == '__main__':
    main()
