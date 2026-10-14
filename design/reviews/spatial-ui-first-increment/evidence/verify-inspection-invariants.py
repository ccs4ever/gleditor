import os,json,hashlib,subprocess,re
from pathlib import Path
repo=Path('/workspace/gleditor'); base=repo/'design/reviews/spatial-ui-first-increment/evidence/inspector-opengl'
meta=json.loads((base/'launch.json').read_text()); env=os.environ.copy(); env.update({k:v for k,v in meta['environment'].items() if k.startswith('XDG_')})
data=Path(env['XDG_DATA_HOME']); stores=data/'xudu/xanadocs'; source=re.search(r'kept untitled xanadoc at (.+)',(base/'stdout.log').read_text()).group(1).strip(); dest=repo/'design/reviews/spatial-ui-first-increment/evidence/inspection-invariants'; dest.mkdir(exist_ok=True)
def hashes(): return {str(p.relative_to(stores)):hashlib.sha256(p.read_bytes()).hexdigest() for p in stores.rglob('ops.nodes')}
def activity(label):
 r=subprocess.run([str(repo/'build/xudu-dump'),'--section=ops','--permascroll='+str(data/'xuzz/permascroll/default'),str(data/'xudu/activity')],cwd=repo,env=env,capture_output=True,text=True)
 (dest/f'activity-{label}.txt').write_text(r.stdout+r.stderr); return r.returncode,r.stdout.count('visit2 ')
before=hashes(); a_before=activity('before'); cmd=[str(repo/'build/xuzz'),'--profile','--backend','opengl','--resize','1440,1000','--select','0,20','--chord','Alt+Shift+N','--dump-a11y','--capture',str(dest/'selected.ppm'),'--click-label','Inspect members','--dump-a11y','--capture',str(dest/'expanded.ppm'),'--chord','Alt+Shift+J','--chord','Alt+Shift+K','--dump-a11y','--capture',str(dest/'preview.ppm'),'--chord','Ctrl+Alt+I','--dump-a11y','--capture',str(dest/'folded.ppm'),'--chord','Ctrl+Q',source]
(dest/'launch.json').write_text(json.dumps({'command':cmd,'environment':{k:env[k] for k in meta['environment']}},indent=2))
with (dest/'stdout.log').open('w') as out,(dest/'stderr.log').open('w') as err: r=subprocess.run(cmd,cwd=repo,env=env,stdout=out,stderr=err,timeout=120)
after=hashes(); a_after=activity('after'); result={'exit_code':r.returncode,'source_ops_sha256_before':before,'source_ops_sha256_after':after,'source_ops_unchanged':before==after,'activity_dump_results_before_after':[a_before,a_after],'scope':'Actual reopened UI select/inspect/member/occurrence/fold; no Enter in this run. Dump visit2 count is a diagnostic record count.'};(dest/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result));raise SystemExit(0 if r.returncode==0 and before==after and a_before==a_after else 1)
