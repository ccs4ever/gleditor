import json, os, subprocess
from pathlib import Path
repo=Path('/workspace/gleditor'); evidence=repo/('design/reviews/spatial-ui-first-increment/evidence/inspector-'+os.environ.get('SPATIAL_BACKEND','opengl'));evidence.mkdir(exist_ok=True)
text='Harbor left passage.\nMeadow second passage.\nRight one.\nRight two.\nRight three.\n'
lines=text.splitlines(keepends=True); ranges=[];start=0
for line in lines: ranges.append(f'{start},{start+len(line)-1}');start+=len(line)
steps=['--chord','Ctrl+N','--type',text,'--select',ranges[0],'--chord','Ctrl+T','--chord','Ctrl+2']
for r in ranges[:2]: steps+=['--select',r,'--chord','Ctrl+Alt+[']
for r in ranges[2:]: steps+=['--select',r,'--chord','Ctrl+Alt+]']
steps+=['--chord','Ctrl+Alt+L','--select',ranges[0],'--chord','Alt+Shift+N','--dump-a11y','--capture',str(evidence/'quiet.ppm'),'--click-label','Inspect members','--dump-a11y','--capture',str(evidence/'expanded.ppm'),'--chord','Alt+Shift+J','--chord','Alt+Shift+K','--chord','Alt+Shift+X','--chord','Alt+Shift+J','--dump-a11y','--capture',str(evidence/'preview.ppm'),'--chord','Ctrl+Alt+I','--dump-a11y','--capture',str(evidence/'folded.ppm'),'--chord','Alt+Shift+Return','--dump-a11y','--capture',str(evidence/'entered.ppm'),'--chord','Alt+Shift+O','--dump-a11y','--capture',str(evidence/'returned.ppm'),'--chord','Ctrl+Q']
command=[str(repo/'build/xuzz'),'--backend',os.environ.get('SPATIAL_BACKEND','opengl'),'--resize','1440,1000','--profile',*steps]
(evidence/'launch.json').write_text(json.dumps({'command':command,'environment':{k:os.environ[k] for k in ['SDL_VIDEODRIVER','SDL_AUDIODRIVER','LIBGL_ALWAYS_SOFTWARE','XDG_DATA_HOME','XDG_CONFIG_HOME','XDG_CACHE_HOME']}},indent=2))
try:
 with (evidence/'stdout.log').open('w') as out,(evidence/'stderr.log').open('w') as err: r=subprocess.run(command,cwd=repo,stdout=out,stderr=err,timeout=150)
 result={'exit_code':r.returncode,'scope':'Native UI authoring of 2x3 Link with quotation; semantic and independent evidence review pending'}
except subprocess.TimeoutExpired: result={'exit_code':124,'error':'150 second timeout'}
(evidence/'result.json').write_text(json.dumps(result,indent=2)); print(result);raise SystemExit(result['exit_code'])
