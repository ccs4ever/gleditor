#!/usr/bin/env python3
"""Compile the independent Luna fixture packs; preserve every alternate branch."""
import copy
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def patch(state, changes):
    for path, value in changes.items():
        keys = path.split('.')
        target = state
        for key in keys[:-1]:
            if not isinstance(target.get(key), dict):
                target[key] = {}
            target = target[key]
        target[keys[-1]] = copy.deepcopy(value)


def build():
    journeys = []
    for file in sorted((ROOT / 'packs').glob('*.json')):
        journeys.extend(json.loads(file.read_text())['journeys'])
    journeys.sort(key=lambda j: int(j['id'][1:]))
    ids = set()
    for journey in journeys:
        assert journey['id'] not in ids
        ids.add(journey['id'])
        current = copy.deepcopy(journey['initial'])
        journey.setdefault('variants', [])
        for step in journey['steps']:
            for action in step['actions']:
                patch(current, action['patch'])
                for branch in action.get('branches', []):
                    branch = copy.deepcopy(branch)
                    if 'panels' in branch:
                        branch['patch']['panels'] = branch.pop('panels')
                    journey['variants'].append({
                        'id': branch['id'] + '-route', 'step': step['step'],
                        'title': branch['label'], 'start': copy.deepcopy(current),
                        'actions': [branch], 'checks': [branch.get('note', branch['label'])],
                        'coverage': ['Explicit alternate action at ' + action['id']],
                    })
    data = {'schema': 1, 'journeys': journeys}
    (ROOT / 'journey-data.js').write_text('window.SPATIAL_JOURNEYS = ' + json.dumps(data, ensure_ascii=False, separators=(',', ':')) + ';\n')
    (ROOT / 'evidence' / 'compiled-manifest.json').write_text(json.dumps({
        'journeys': [{'id':j['id'], 'steps':len(j['steps']), 'actions':sum(len(s['actions']) for s in j['steps']),
                      'variants':len(j['variants']), 'variantActions':sum(len(v['actions']) for v in j['variants'])} for j in journeys]
    }, indent=2) + '\n')
    print(f'Compiled {len(journeys)} journeys; {sum(len(j["steps"]) for j in journeys)} base tasks.')


if __name__ == '__main__':
    build()
