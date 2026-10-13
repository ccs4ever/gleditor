#!/usr/bin/env python3
"""Validate all declared concept transitions before rendering; no Xuzz runtime claims."""
import copy
import json
from pathlib import Path
from build import ROOT, patch


def get(value, path):
    for key in path.split('.'):
        value = value.get(key) if isinstance(value, dict) else None
    return value


def validate():
    raw = (ROOT / 'journey-data.js').read_text()
    journeys = json.loads(raw.removeprefix('window.SPATIAL_JOURNEYS = ').strip().removesuffix(';'))['journeys']
    failures, states, actions = [], 0, set()
    def fail(j, route, action, message):
        failures.append({'journey':j, 'route':route, 'action':action, 'message':message})
    for j in journeys:
        routes = [('main',j['initial'],[a for s in j['steps'] for a in s['actions']])]
        routes += [(v['id'],v.get('start',j['initial']),v['actions']) for v in j.get('variants',[])]
        for route, initial, sequence in routes:
            current = copy.deepcopy(initial)
            for a in [None] + sequence:
                aid = a['id'] if a else 'before'
                if a:
                    if aid in actions: fail(j['id'],route,aid,'Duplicate action ID')
                    actions.add(aid)
                    before = copy.deepcopy(current)
                    for key, value in a.get('expect',{}).items():
                        if get(current,key)!=value: fail(j['id'],route,aid,'Prior state mismatch: '+key)
                    patch(current,a['patch'])
                    if before['sourceOps']!=current['sourceOps']: fail(j['id'],route,aid,'Source operation mutation')
                    if a['kind'] in ['inspect','view','save','author'] and before['reading']['visit']!=current['reading']['visit']: fail(j['id'],route,aid,'Unexpected reader visit change')
                    if a['kind']=='restore' and before['readerEffects']['visits']!=current['readerEffects']['visits']: fail(j['id'],route,aid,'Restore added visit')
                    if a['kind']=='enter' and current['readerEffects']['visits']!=before['readerEffects']['visits']+1: fail(j['id'],route,aid,'Enter must add one visit')
                    if not a['patch'].get('panels'): fail(j['id'],route,aid,'Missing result panels')
                    if not a.get('effects'): fail(j['id'],route,aid,'Missing effect record')
                if not current['reading'].get('body'): fail(j['id'],route,aid,'Missing reading body')
                for p in current.get('panels',[]):
                    rows = get(current,p['rowsPath']) if p.get('rowsPath') else p.get('rows',[])
                    if not isinstance(rows,list): fail(j['id'],route,aid,'Non-array rows: '+p['id']);continue
                    count = p.get('count',{})
                    if count.get('visible') is not None and count['visible']!=len(rows): fail(j['id'],route,aid,f'Visible count mismatch in {p["id"]}: {count["visible"]} vs {len(rows)}')
                    if p.get('allRowsPath') and count.get('total') is not None:
                        allrows=get(current,p['allRowsPath'])
                        if not isinstance(allrows,list) or count['total']!=len(allrows): fail(j['id'],route,aid,'Complete inventory mismatch: '+p['id'])
                states+=1
    result={'journeys':len(journeys),'steps':sum(len(j['steps']) for j in journeys),'variants':sum(len(j.get('variants',[])) for j in journeys),'actions':len(actions),'states':states,'failures':failures}
    (ROOT/'evidence'/'validation.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
    return bool(failures)


if __name__=='__main__':
    raise SystemExit(validate())
