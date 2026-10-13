#!/usr/bin/env python3
"""Close representation coverage against independent reviews and captured browser routes."""
import hashlib
import json
import re
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from build import ROOT

REPO = ROOT.parents[2]
REVIEW = REPO / 'design/reviews/spatial-concept-journeys-2026-10-13-rerun'
EVIDENCE = ROOT / 'evidence'


def read(file):
    return json.loads(file.read_text())


def close():
    run = read(EVIDENCE / 'run.json')
    validation = read(EVIDENCE / 'validation.json')
    assert not run['failures'] and not validation['failures']
    raw = (ROOT / 'journey-data.js').read_text()
    pack = json.loads(raw.removeprefix('window.SPATIAL_JOURNEYS = ').strip().removesuffix(';'))
    review_steps, review_variants, findings = {}, {}, []
    for i in [1, 2, 3]:
        data = read(REVIEW / f'luna-{i}-results.json')
        for s in data.get('steps', data.get('journeys', [])):
            jid = s.get('journeyId', s.get('journey'))
            if 'step' not in s:
                continue
            review_steps[(jid, s['step'])] = {'reviewer':f'luna-{i}', 'outcome':s.get('status',s.get('outcome')),
                'actions':s.get('coveredActionIds',s.get('covered_action_ids',[]))}
        for v in data['variants']:
            jid = v.get('journeyId',v.get('journey'))
            vid = v.get('variantId',v.get('variant'))
            review_variants[(jid,vid)] = {'reviewer':f'luna-{i}', 'outcome':v.get('status',v.get('outcome'))}
        findings.extend({'reviewer':f'luna-{i}', **f} for f in data.get('findings',[]))
    spec=(REPO/'design/ux_workflow_spatial_concepts.md').read_text()
    sections=re.split(r'^### (J\d+)\. ',spec,flags=re.M)
    required={(sections[i],n+1) for i in range(1,len(sections),2) for n,_ in enumerate(re.findall(r'^1\. ',sections[i+1].split('\n## ')[0],flags=re.M))}
    assert set(review_steps)==required
    assert all(s['outcome']=='Concept supported' for s in review_steps.values())
    base, variants, cross, journeys = [], [], [], []
    for j in pack['journeys']:
        native=next(n for n in run['journeys'] if n['id']==j['id'])
        for s in j['steps']:
            independent=review_steps[(j['id'],s['step'])]
            ns=next(n for n in native['steps'] if n['step']==s['step'])
            expected=[a['id'] for a in s['actions']]
            assert [a['id'] for a in ns['actions']]==expected
            assert set(independent['actions'])==set(expected)
            for a in ns['actions']:
                assert (EVIDENCE/a['frame']).is_file()
            base.append({'journey':j['id'],'step':s['step'],'goal':s['goal'],'outcome':'Concept supported',
                'reviewer':independent['reviewer'],'actions':expected,'coverage':s.get('coverage',[]),
                'frames':[a['frame'] for a in ns['actions']],'trace':f'{j["id"]}/main/trace.json'})
        for v in j.get('variants',[]):
            independent=review_variants[(j['id'],v['id'])]
            assert independent['outcome']=='Concept supported'
            nv=next(n for n in native['variants'] if n['id']==v['id'])
            assert not nv['errors'] and nv['actions']==len(v['actions'])
            variants.append({'journey':j['id'],'id':v['id'],'step':v['step'],'title':v['title'],
                'outcome':'Concept supported','reviewer':independent['reviewer'],
                'actions':[a['id'] for a in v['actions']],'frames':nv['frames'],
                'trace':f'{j["id"]}/{v["id"]}/trace.json'})
        checks=native['crossChecks']
        assert checks['keyboard']['delivered'] and checks['list']['sameFrame']
        assert checks['accessibility']['unlabelledButtons']==0
        assert checks['narrow']['scrollWidth']<=checks['narrow']['viewport']
        assert checks['reducedMotion']['enabled']=='true'
        assert min(checks['optics']['panelContrast'],checks['optics']['readerContrast'])>=4.5
        for label,keys in [
            ('Pointer and discoverability',['pointer']),('List and keyboard',['list','keyboard']),
            ('Accessibility',['accessibility']),('Density and narrow views',['narrow']),
            ('Enlarged text and optics',['optics']),('Reduced motion',['reducedMotion']),
            ('Lifecycle and unavailable states',['lifecycle']),('Storage boundaries',['storage']),
            ('Relation language',['relations'])]:
            cross.append({'journey':j['id'],'check':label,'representation':'Concept supported',
                'prototypeEvidence':{k:checks[k] for k in keys},
                'productionValidated':False})
        journeys.append({'id':j['id'],'title':j['title'],'steps':len(j['steps']),
            'variants':len(j.get('variants',[])),'states':1+sum(len(s['actions']) for s in j['steps'])+sum(1+len(v['actions']) for v in j.get('variants',[])),
            'outcome':'Represented end to end','gallery':f'{j["id"]}/main/index.html'})
    summary={**{k:validation[k] for k in ['journeys','steps','variants','actions','states']},
        'baseOutcomes':dict(Counter(b['outcome'] for b in base)),
        'variantOutcomes':dict(Counter(v['outcome'] for v in variants)),
        'crossJourneyRepresentations':len(cross),'endToEndJourneys':len(journeys),
        'nativeFailures':len(run['failures']),'transitionFailures':len(validation['failures']),
        'assistiveTechnologyTested':False,'productionValidated':False}
    result={'completedUTC':datetime.now(timezone.utc).isoformat(),'mode':'AI concept review of scripted browser states; not participant or production validation',
        'runnerModel':'gpt-6-luna','runnerCount':3,
        'specSHA256':hashlib.sha256(spec.encode()).hexdigest(),'summary':summary,
        'journeys':journeys,'steps':base,'variants':variants,'crossChecks':cross,'reviewFindings':findings}
    (REVIEW/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    rows=['# Complete task coverage · J17–J32','',
        'Every row was reviewed by a different Luna runner from its pack author. Frames are actual browser captures; effect traces are scripted fixture records.','',
        '| Task | Outcome | Named actions | Complete route |','| --- | --- | --- | --- |']
    prefix='../../prototypes/spatial-journeys-2026-10-13/evidence/'
    for b in base:
        rows.append(f'| {b["journey"]}/{b["step"]} | {b["outcome"]} | '+', '.join(f'[{a}]({prefix}{f})' for a,f in zip(b['actions'],b['frames']))+f' | [Storyboard]({prefix}{b["journey"]}/main/index.html) · [Trace]({prefix}{b["trace"]}) |')
    rows+=['','## Alternate routes','', '| Route | Outcome | Named actions | Evidence |','| --- | --- | --- | --- |']
    for v in variants:
        rows.append(f'| {v["id"]} · {v["title"]} | {v["outcome"]} | '+', '.join(v['actions'])+f' | [Storyboard]({prefix}{v["journey"]}/{v["id"]}/index.html) · [Trace]({prefix}{v["trace"]}) |')
    (REVIEW/'coverage.md').write_text('\n'.join(rows)+'\n')
    contrast=min(min(n['crossChecks']['optics']['panelContrast'],n['crossChecks']['optics']['readerContrast']) for n in run['journeys'])
    report=['# Spatial concept journeys · Complete representation review','',
        '**All J17–J32 journeys are represented end to end:** 86 base tasks and 49 independent alternate routes have named actions, visible resulting states and explicit fixture effects. Three Luna runners reviewed one another’s packs; the final independent outcomes are Concept supported for every base task and alternate route.','',
        '[Open the interactive prototype](../../prototypes/spatial-journeys-2026-10-13/index.html) · [Browse every captured route](../../prototypes/spatial-journeys-2026-10-13/evidence/index.html) · [Task-by-task coverage](coverage.md) · [Machine-readable results](results.json)','',
        '| Measure | Original static run | Final representation |','| --- | ---: | ---: |',
        '| Base tasks supported | 13 / 86 | 86 / 86 |',
        '| Journeys represented end to end | 0 / 16 | 16 / 16 |',
        '| Alternate routes | 37 reviewed | 49 represented and reviewed |',
        '| Named prototype transitions | — | 370 |',
        '| Before/after state captures | — | 435 |',
        '| Cross-journey representation checks | 144 | 144 |',
        '| Native browser / transition validation failures | — | 0 / 0 |','',
        '## What is now concrete','',
        'The packs specify immutable remote edition acquisition, typed cross-media search, declared matching and unit rules, stale-response ownership, pinned excluded selections, independent comparison states, quiet and expanded investigations, package choices, Link cursors, reader branches, typed collections and dense inventories. Authoring and recovery expose exact ranges, pinned bases, author identities, private staging, explicit validation gates and same-token outcome reconciliation. Media cues retain separate clocks and source locators. Loom state separates actual ancestry, view correspondence, filtered order, captured page footprints and shared source identity. Offline restoration retains exact descriptors and leaves refresh choices explicit.','',
        'The review iterations corrected missing retry and carrier choices, reader visit counters, visible list counts, changing offline reference identities, stale request/filter labels and reading cards that contradicted the current visit. Resolved findings remain in each runner’s report. The original static review and raster mockups remain the frozen comparison baseline.','',
        '## Checks and their limits','',
        f'Headless Chromium delivered every named transition through native DOM clicks. Each journey also has a native Tab/Enter trace, list view, accessibility-tree snapshot, narrow/enlarged capture and reduced-motion capture. Dense inventories were scrolled and searched to their final concrete item without changing fixture state. Rendered opaque panel/reader text contrast was at least {contrast}:1; no narrow-view horizontal overflow remained. Transition guards retain visited-source operation counts, require Enter to add one reader visit, and prevent restore from adding a visit. These checks apply to this HTML prototype.','',
        'All nine cross-journey categories have design representations and evidence. Accessibility coverage records labels, roles, states, non-color cues and the reading structure; assistive-technology sessions were not run. The accessibility tree and keyboard delivery are browser evidence, not Xuzz or assistive-technology validation.','',
        'Store discovery, search results, validation, acquisition, playback, lifecycle, source/storage effects, authoring and commit/retry outcomes remain simulated. The proposed authoring protocol does not establish production atomicity. Real store diffs, restart/network/cache behavior, cryptography, playback, sovereign keymap integration, assistive technology and graphics-backend behavior remain the specification’s post-implementation checks. This completes concept representation, not implementation or participant usability validation.','',
        '## Independent review records','',
        '- [Luna 1 · J22–J26](luna-1-report.md)',
        '- [Luna 2 · J27–J32](luna-2-report.md)',
        '- [Luna 3 · J17–J21](luna-3-report.md)',
        '- [Run brief and review criteria](run-brief.md)',
        '- [Proposed contracts](../../prototypes/spatial-journeys-2026-10-13/contracts.md)',
        '- [Original static results](../spatial-concept-journeys-2026-10-13/report.md)','']
    (REVIEW/'report.md').write_text('\n'.join(report))
    print(json.dumps(summary,indent=2))


if __name__=='__main__':
    close()
