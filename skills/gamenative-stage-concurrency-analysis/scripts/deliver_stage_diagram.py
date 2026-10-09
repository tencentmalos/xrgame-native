#!/usr/bin/env python3
"""Deliver a complete GameNative stage view using the installed Archify viewer and palette registry.

The domain analyzer remains authoritative. This is not conversion to the generic timeline schema.
Publish a new immutable output directory only after analysis and viewer generation succeed.
Vendored from Azahar's stage-concurrency skill; see references/provenance.md.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

import analyze_stage_intervals as analyzer


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def find_archify(explicit: Path | None) -> Path:
    # The Windows workstation has no personal Archify skill; the Litep MCP release bundles one.
    litep = sorted((Path.home() / 'PicoMcp/litep/versions').glob('*/skills/archify'), reverse=True)
    candidates = [explicit] if explicit else [
        Path(os.environ['ARCHIFY_ROOT']) if os.environ.get('ARCHIFY_ROOT') else None,
        Path(os.environ.get('CODEX_HOME', str(Path.home() / '.codex'))) / 'skills/archify',
        Path.home() / '.claude/skills/archify',
        *litep,
    ]
    for candidate in candidates:
        if candidate and (candidate / 'renderers/shared/color-palettes.mjs').is_file():
            return candidate.resolve()
    raise ValueError('Archify fork missing: supply --archify or ARCHIFY_ROOT; no auto-install or legacy fallback')


def write_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def deliver(input_path: Path, output: Path, archify: Path, *, palette='studio', theme='dark',
            locale='zh-CN', evidence_kind='measured', frame_count=6, start_frame=None,
            node='node') -> dict:
    input_path, output = input_path.resolve(), output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError('output directory already exists; choose a new run directory (previous delivery retained)')
    if evidence_kind not in ('measured', 'synthetic'):
        raise ValueError('evidence_kind must be measured or synthetic')
    data, input_bytes = analyzer.load_input(input_path)
    data = analyzer.select_frame_window(data, frame_count, start_frame)
    report = analyzer.analyze(data)
    analyzer.require_frame_count(report, frame_count)
    errors = [item for item in report['diagnostics'] if item.get('severity') == 'error']
    if errors:
        raise ValueError('strict stage analysis failed: ' + json.dumps(errors, ensure_ascii=False))
    if evidence_kind == 'measured' and not report.get('identity'):
        raise ValueError('measured delivery needs binary/capture identity; apply the skill evidence gates first')
    report['title'] = ('合成示例 · ' if locale == 'zh-CN' else 'SYNTHETIC · ') + report['title'] if evidence_kind == 'synthetic' else report['title']
    report['evidence_kind'] = evidence_kind
    report['presentation_contract'] = 'gamenative-stage-v2-with-archify-viewer-v1'
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix='.' + output.name + '.', dir=output.parent))
    scripts = Path(__file__).resolve().parent
    try:
        (temporary / 'stage.bundle.json').write_bytes(input_bytes)
        write_json(temporary / 'stage.selected.json', data)
        write_json(temporary / 'stage.report.json', report)
        (temporary / 'stage.raw.svg').write_text(analyzer.render_svg(data, report), encoding='utf-8')
        (temporary / 'stage.review.md').write_text(analyzer.render_review_markdown(
            data, report, Path('stage.bundle.json'), Path('stage.svg'), Path('stage.report.json')), encoding='utf-8')
        subprocess.run([node, str(scripts / 'wrap_archify_stage.mjs'), str(archify), str(temporary),
                        palette, theme, locale], check=True, capture_output=True, text=True)
        (temporary / 'stage.raw.svg').unlink()
        artifacts = {p.name: {'sha256': digest(p), 'bytes': p.stat().st_size}
                     for p in sorted(temporary.iterdir()) if p.is_file()}
        implementation = [archify / 'assets/template.html', archify / 'renderers/shared/utils.mjs',
                          archify / 'renderers/shared/color-palettes.mjs',
                          scripts / 'analyze_stage_intervals.py', scripts / 'wrap_archify_stage.mjs',
                          scripts / 'deliver_stage_diagram.py']
        receipt = {'contract': 'gamenative-stage-archify-delivery-v1', 'evidence_kind': evidence_kind,
                   'input': {'path': str(input_path), 'sha256': hashlib.sha256(input_bytes).hexdigest()},
                   'artifacts': artifacts, 'implementations': {str(p): digest(p) for p in implementation},
                   'palette': palette, 'theme': theme, 'frames': report['frames']['count'],
                   'intervals': report['interval_count'], 'diagnostics': report['diagnostics'],
                   'semantics': 'GameNative active-stage union; not native timeline active-lane concurrency',
                   'validation': {'domain_analysis': 'passed', 'archify_showcase': 'not applicable to domain SVG',
                                  'browser': 'pending', 'perceptual': 'pending',
                                  'capture_identity': 'caller evidence gate; not independently verified by renderer'}}
        write_json(temporary / 'delivery.receipt.json', receipt)
        if output.exists() or output.is_symlink():
            raise ValueError('output appeared during delivery; preserved')
        temporary.rename(output)
        return receipt
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--archify', type=Path)
    parser.add_argument('--palette', default='studio', choices=['studio', 'ocean', 'sunset', 'feishu'])
    parser.add_argument('--theme', default='dark', choices=['light', 'dark'])
    parser.add_argument('--locale', default='zh-CN', choices=['zh-CN', 'en'])
    parser.add_argument('--evidence-kind', required=True, choices=['measured', 'synthetic'])
    parser.add_argument('--select-frame-count', type=int, default=6)
    parser.add_argument('--select-frame-start')
    parser.add_argument('--node', default='node')
    args = parser.parse_args()
    try:
        receipt = deliver(args.input, args.output_dir, find_archify(args.archify), palette=args.palette,
                          theme=args.theme, locale=args.locale, evidence_kind=args.evidence_kind,
                          frame_count=args.select_frame_count, start_frame=args.select_frame_start,
                          node=args.node)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + ('\n' + error.stderr if isinstance(error, subprocess.CalledProcessError) else '') + '\n')
    print(json.dumps({'output': str(args.output_dir), 'frames': receipt['frames'],
                      'intervals': receipt['intervals'], 'browser': 'pending', 'perceptual': 'pending'}))


if __name__ == '__main__':
    main()
