import os,json,subprocess,tempfile
from pathlib import Path
repo=Path('/workspace/gleditor');base=repo/'design/reviews/spatial-ui-second-increment/evidence/drawer'
for backend in ['opengl','opengles','vulkan']:
 for width,height in [(640,480),(1440,1000)]:
  dest=base/f'{backend}-{width}';dest.mkdir(parents=True,exist_ok=True)
  profile=Path(tempfile.mkdtemp(prefix='drawer-',dir='/workspace/spatial-build/second'));env=os.environ.copy()
  for var,leaf in [('XDG_DATA_HOME','data'),('XDG_CONFIG_HOME','config'),('XDG_CACHE_HOME','cache')]:
   (profile/leaf).mkdir();env[var]=str(profile/leaf)
  cmd=[str(repo/'build/xuzz'),'--profile','--backend',backend,'--resize',f'{width},{height}','--chord','Ctrl+N','--type','Original passage retained during object creation.','--click-label','Store Object Manager','--dump-a11y','--capture',str(dest/'before.ppm'),'--click-label','New Xanadoc','--dump-a11y','--capture',str(dest/'new-document.ppm'),'--click-label','New Slice','--dump-a11y','--capture',str(dest/'new-slice.ppm'),'--click-label','Close Drawer','--dump-a11y','--capture',str(dest/'closed.ppm'),'--click-label','Store Object Manager','--dump-a11y','--capture',str(dest/'reopened.ppm'),'--chord','Ctrl+Q']
  (dest/'launch.json').write_text(json.dumps({'command':cmd,'environment':{k:env[k] for k in ['SDL_VIDEODRIVER','SDL_AUDIODRIVER','LIBGL_ALWAYS_SOFTWARE','XDG_DATA_HOME','XDG_CONFIG_HOME','XDG_CACHE_HOME']}},indent=2))
  with (dest/'stdout.log').open('w') as out,(dest/'stderr.log').open('w') as err:r=subprocess.run(cmd,cwd=repo,env=env,stdout=out,stderr=err,timeout=120)
  (dest/'result.json').write_text(json.dumps({'exit_code':r.returncode,'scope':'Actual UI creation, named-control activation, close/reopen, narrow/normal backend capture.'},indent=2));print(backend,width,r.returncode,flush=True)
  if r.returncode:raise SystemExit(r.returncode)
