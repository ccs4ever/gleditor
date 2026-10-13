#!/usr/bin/env python3
"""Verify captured route states/effects exactly match current compiled fixtures."""
import copy
import hashlib
import json
from build import ROOT, patch


def verify():
    raw = (ROOT / 'journey-data.js').read_text()
    pack = json.loads(raw.removeprefix('window.SPATIAL_JOURNEYS = ').strip().removesuffix(';'))
    count = 0
    for j in pack['journeys']:
        routes = [('main',j['initial'],[a for s in j['steps'] for a in s['actions']])]
        routes += [(v['id'],v.get('start',j['initial']),v['actions']) for v in j.get('variants',[])]
        for rid, initial, actions in routes:
            current = copy.deepcopy(initial)
            frames = json.loads((ROOT/'evidence'/j['id']/rid/'trace.json').read_text())['frames']
            assert len(frames)==len(actions)+1, (j['id'],rid,'frame count')
            assert frames[0]['state']==current, (j['id'],rid,'initial mismatch')
            for a, frame in zip(actions,frames[1:]):
                patch(current,a['patch'])
                assert frame['state']==current, (j['id'],a['id'],'stale rendered state')
                assert frame['effects']==a['effects'], (j['id'],a['id'],'effect mismatch')
                assert frame['frame']==a['id']
            for frame in frames:
                assert (ROOT/'evidence'/frame['file']).is_file(), frame['file']
                count += 1
    return count


def manifest(count):
    files=[]
    for file in sorted(ROOT.rglob('*')):
        if file.is_file() and '__pycache__' not in file.parts and file.name!='asset-manifest.json':
            files.append({'path':str(file.relative_to(ROOT.parents[2])), 'bytes':file.stat().st_size,
                          'sha256':hashlib.sha256(file.read_bytes()).hexdigest()})
    spec=ROOT.parents[2]/'design/ux_workflow_spatial_concepts.md'
    result={'renderedStateFrames':count,'frozenSpecSHA256':hashlib.sha256(spec.read_bytes()).hexdigest(),'assets':files}
    (ROOT/'evidence/asset-manifest.json').write_text(json.dumps(result,indent=2)+'\n')


if __name__=='__main__':
    count=verify()
    manifest(count)
    print(f'{count} rendered states and effects match the current packs; artifact hashes recorded.')
