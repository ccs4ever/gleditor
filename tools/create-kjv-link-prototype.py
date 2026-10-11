#!/usr/bin/env python3
"""Prepare native stores for the full-text KJV link prototype, without a window."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('build/kjv-link-prototype'))
    parser.add_argument('--source', type=Path, default=Path('tests/samples/kjv.txt'))
    parser.add_argument('--continuous', action='store_true',
                        help='Also prepare a single member crossing the Psalm page break.')
    args = parser.parse_args()
    root = args.root.resolve()
    source = args.source.resolve()
    root.mkdir(parents=True, exist_ok=True)
    manifest = root / 'prototype.json'
    if manifest.exists():
        if args.continuous and not json.loads(manifest.read_text()).get('continuous'):
            raise SystemExit('Existing fixture has no continuous link; choose a fresh --root.')
        print(f'Native KJV prototype already prepared: {root}')
        return
    if (root / 'kjv/ops.nodes').exists() or (root / 'notes/ops.nodes').exists():
        raise SystemExit('Incomplete prototype stores exist; choose a fresh --root.')
    text = source.read_text(encoding='utf-8-sig')
    prepared = root / 'kjv-input.txt'
    prepared.write_text(text, encoding='utf-8')
    locators = []
    encoded = text.encode('utf-8')
    for label in ('Genesis 1:1', 'Psalm 63:5', 'Revelation 22:21'):
        needle = next(line for line in text.splitlines() if line.startswith(label+'\t')).encode('utf-8')
        start = encoded.index(needle)
        locators.append({'label': label, 'start': start, 'length': len(needle)})
    note = ('Creation, satisfaction and blessing\n\n'
            'Compare the three exact passages independently.\n'
            'The sparse comment link has three left members and one right member.\n'
            'The intervening KJV text is not part of that endset.\n\n'
            'Use member controls to choose a passage, then Enter to read it.\n'
            'Cross preserves each side\'s cursor. Origin returns to the starting passage.\n')
    if args.continuous:
        note += ('\nA separate link has one continuous left member:\n'
                 'Psalm 62:12 through Psalm 63:3, including the page break.\n')
    (root / 'notes-input.txt').write_text(note, encoding='utf-8')
    env = dict(os.environ)
    env.update(SDL_VIDEODRIVER='offscreen', SDL_AUDIODRIVER='dummy',
               LIBGL_ALWAYS_SOFTWARE='1', XDG_DATA_HOME=str(root / 'data'),
               XDG_CONFIG_HOME=str(root / 'config'), XDG_CACHE_HOME=str(root / 'cache'))
    binary = str(Path('build/xuzz').resolve())
    def run(*arguments):
        subprocess.run([binary, *map(str, arguments), '--headless'], env=env, check=True)
    run(root / 'kjv', '--import', prepared)
    run(root / 'notes', '--import', root / 'notes-input.txt')
    left = '+'.join(f"0:{v['start']}:{v['length']}" for v in locators)
    run(root / 'kjv', '--open-store', root / 'notes', '--link',
        f'{left},1:0:34:comment:author:Alice', '--export-osmic')
    continuous = None
    if args.continuous:
        def verse(label):
            return next(line for line in text.splitlines()
                        if line.startswith(label+'\t')).encode('utf-8')
        start = encoded.index(verse('Psalm 62:12'))
        last = verse('Psalm 63:3')
        end = encoded.index(last) + len(last)
        continuous = {'start': start, 'length': end-start,
                      'label': 'Psalm 62:12 through Psalm 63:3'}
        run(root / 'kjv', '--open-store', root / 'notes', '--link',
            f'0:{start}:{end-start},1:0:34:comment:author:Alice', '--export-osmic')
    manifest.write_text(json.dumps({'source': str(source),
                                   'sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                                   'textBytes': len(encoded), 'left': locators,
                                   'right': {'start': 0, 'length': 34},
                                   'continuous': continuous,
                                   'middle': 'Near byte midpoint; actual page is reported by native layout.'}, indent=2)+'\n')
    print(f'Prepared full KJV ({len(encoded):,} bytes), exact 3-by-1 native link: {root}')
    print('Launch with tools/run-kjv-link-prototype.sh; use member, place, Cross, Enter and Origin.')


if __name__ == '__main__':
    main()
