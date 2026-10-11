#!/usr/bin/env python3
"""Capture native KJV long jumps and verify that navigation does not edit sources."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', choices=('opengl', 'opengles', 'vulkan'),
                        default='opengl')
    parser.add_argument('--root', type=Path,
                        default=Path('build/kjv-link-prototype'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--continuous', action='store_true',
                        help='Also inspect the prepared continuous Psalm member.')
    parser.add_argument('--chrome', action='store_true',
                        help='Also scroll back to read the clipped continuation.')
    args = parser.parse_args()
    if args.chrome and not args.continuous:
        parser.error('--chrome requires --continuous')
    root = args.root.resolve()
    out = args.out.resolve()
    if out.exists():
        raise SystemExit('Choose a fresh --out so evidence is not overwritten.')
    out.mkdir(parents=True)
    for name in ('kjv', 'notes', 'data', 'config', 'cache'):
        if name in ('config', 'cache') and not (root / name).exists():
            (out / name).mkdir()
        else:
            shutil.copytree(root / name, out / name)
    shutil.copy2(root / 'prototype.json', out / 'prototype.json')
    manifest = json.loads((out / 'prototype.json').read_text())
    if args.continuous and not manifest.get('continuous'):
        raise SystemExit('Prepare this root with --continuous before checking it.')
    evidence = out / 'evidence'
    evidence.mkdir()
    sources = [out / name / file for name in ('kjv', 'notes')
               for file in ('ops.nodes', 'store.tables')]
    before = {str(path.relative_to(out)): digest(path) for path in sources}
    env = dict(os.environ, SDL_VIDEODRIVER='offscreen', SDL_AUDIODRIVER='dummy',
               LIBGL_ALWAYS_SOFTWARE='1', SPDLOG_LEVEL='xudu.links=debug',
               KJV_PROTOTYPE_ROOT=str(out))
    command = ['tools/run-kjv-link-prototype.sh', '--backend', args.backend,
               '--resize', '1800,1050', '--chord', 'Alt+Shift+Return',
               '--capture', str(evidence / 'genesis.ppm')]
    for name in ('middle', 'last'):
        command += ['--chord', 'Alt+Shift+J', '--chord', 'Alt+Shift+L',
                    '--chord', 'Alt+Shift+Return', '--capture',
                    str(evidence / (name + '.ppm'))]
    command += ['--chord', 'Alt+Shift+O', '--capture',
                str(evidence / 'origin.ppm')]
    if args.continuous:
        command += ['--chord', 'Alt+Shift+N', '--chord', 'Alt+Shift+J',
                    '--chord', 'Alt+Shift+L', '--chord', 'Alt+Shift+X',
                    '--chord', 'Alt+Shift+L', '--chord', 'Alt+Shift+X',
                    '--capture', str(evidence / 'continuous-preview.ppm'),
                    '--chord', 'Alt+Shift+Return',
                    '--capture', str(evidence / 'continuous-entry.ppm'),
                    '--mouse-move', '700,700', '--wheel', '0,-3',
                    '--capture', str(evidence / 'continuous-scroll.ppm')]
    if args.chrome:
        command += ['--wheel', '0,1', '--capture',
                    str(evidence / 'continuous-readable.ppm')]
    command += ['--dump-a11y', '--profile']
    with (evidence / 'run.log').open('w') as log:
        subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                       check=True)
    transcript = (evidence / 'run.log').read_text()
    for member in manifest['left'] + ([manifest['continuous']] if args.continuous else []):
        expected = f"focus document 0 range [{member['start']},{member['start'] + member['length']})"
        if expected not in transcript:
            raise SystemExit('Exact focus was not observed: ' + expected)
    focus = [(int(start), int(end)) for start, end in
             re.findall(r'focus document 0 range \[(\d+),(\d+)\)', transcript)]
    def bounds(member):
        return member['start'], member['start'] + member['length']
    expected_focus = [bounds(manifest['left'][index]) for index in (0, 0, 1, 2, 0)]
    if args.continuous:
        expected_focus += [bounds(manifest['continuous'])]
        if 'continues across page breaks' not in transcript:
            raise SystemExit('No native multi-page locator in accessibility evidence.')
    if focus != expected_focus:
        raise SystemExit('Preview or scrolling changed focus unexpectedly: ' + repr(focus))
    after = {str(path.relative_to(out)): digest(path) for path in sources}
    if before != after:
        raise SystemExit('Navigation changed a source store; inspect retained evidence.')
    captures = ['genesis', 'middle', 'last', 'origin']
    if args.continuous:
        captures += ['continuous-preview', 'continuous-entry', 'continuous-scroll']
    if args.chrome:
        captures += ['continuous-readable']
    for name in captures:
        path = evidence / (name + '.ppm')
        if not path.is_file() or path.stat().st_size < 1800 * 1050 * 3:
            raise SystemExit('Incomplete native capture: ' + str(path))
    (evidence / 'source-integrity.json').write_text(
        json.dumps({'before': before, 'after': after}, indent=2) + '\n')
    print(f'PASS: exact focus, {len(captures)} native captures, source stores unchanged.')
    print('Inspect images and accessibility evidence before claiming a UX pass: ' + str(evidence))


if __name__ == '__main__':
    main()
