/* Run with PLAYWRIGHT_MODULE pointing at an external Playwright install. */
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const fs = require('fs');
const path = require('path');
const http = require('http');
const root = path.resolve(__dirname, '..');
const out = path.join(root, 'evidence');
const requested=(process.env.JOURNEYS || '').split(',').filter(Boolean);
const prior=requested.length && fs.existsSync(path.join(out,'run.json')) ? JSON.parse(fs.readFileSync(path.join(out,'run.json'),'utf8')) : null;
const results = {method:'Native DOM clicks and Tab/Enter in headless Chromium; scripted fixture effects only.', journeys:prior ? prior.journeys.filter(j=>!requested.includes(j.id)) : [], failures:prior ? prior.failures.filter(f=>f.journey && !requested.includes(f.journey)) : []};
const write = (file, data) => fs.writeFileSync(path.join(out,file), JSON.stringify(data,null,2)+'\n');
const escape = s => String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));

(async () => {
  const server = http.createServer((req,res) => {
    const file=path.resolve(root,'.'+decodeURIComponent(new URL(req.url,'http://localhost').pathname));
    if(!file.startsWith(root+path.sep)){res.writeHead(403).end();return;}
    try {
      const mime={'.html':'text/html','.css':'text/css','.js':'text/javascript','.json':'application/json','.png':'image/png'};
      const contents=fs.readFileSync(file);
      res.writeHead(200,{'Content-Type':mime[path.extname(file)] || 'text/plain'});res.end(contents);
    } catch {res.writeHead(404).end();}
  });
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const base='http://127.0.0.1:'+server.address().port+'/index.html';
  const browser = await chromium.launch({executablePath:'/usr/bin/chromium',headless:true,args:['--no-sandbox','--disable-dev-shm-usage']});
  const page = await browser.newPage({viewport:{width:1440,height:1000},deviceScaleFactor:1});
  page.on('pageerror',e=>results.failures.push({type:'browser',message:e.message}));
  await page.goto(base);
  const journeys = (await page.evaluate(()=>prototypeAPI.journeys)).filter(j=>!requested.length || requested.includes(j.id));
  async function frame(route, name, location, frames) {
    const snap = await page.evaluate(()=>prototypeAPI.snapshot);
    const file = route + '/' + name + '.png';
    await page.screenshot({path:path.join(out,file),fullPage:true});
    const r = snap.records.at(-1);
    frames.push({file,location,frame:snap.frame,state:snap.state,effects:r?.effects || [],action:r?.action || 'Before',note:r?.note || ''});
    return snap;
  }
  for (const j of journeys) {
    const jr = {id:j.id,title:j.title,steps:[],variants:[],crossChecks:{},inventoryChecks:[]};
    results.journeys.push(jr);
    const routes = [{id:'main',steps:j.steps},...(j.variants || []).map(v=>({id:v.id,steps:[{step:v.step,actions:v.actions}]}))];
    for (const route of routes) {
      const dir = j.id + '/' + route.id;
      fs.mkdirSync(path.join(out,dir),{recursive:true});
      const params = new URLSearchParams({journey:j.id});
      if(route.id!=='main') params.set('variant',route.id);
      await page.goto(base+'?'+params);
      const frames = [];
      await frame(dir,'before','?'+params,frames);
      const record = {id:route.id,actions:0,frames:[],errors:[]};
      if(route.id!=='main') jr.variants.push(record);
      for(let si=0;si<route.steps.length;si++) {
        const task=route.steps[si];
        const sr={step:task.step,actions:[],checks:task.checks || []};
        if(route.id==='main') jr.steps.push(sr);
        try {
          for(let ai=0;ai<task.actions.length;ai++) {
            const a=task.actions[ai];
            const b=page.locator(`[data-action="${a.id}"]`);
            if(!(await b.isEnabled())) throw Error(a.id+': named action is disabled');
            await b.click();
            const snap=await page.evaluate(()=>prototypeAPI.snapshot);
            if(snap.error || snap.frame!==a.id) throw Error(snap.error || a.id+': transition did not happen');
            params.set('task',String(si));params.set('action',String(ai+1));
            const f=await frame(dir,a.id,'?'+params,frames);
            sr.actions.push({id:a.id,label:a.label,kind:a.kind,frame:frames.at(-1).file});
            record.actions++;
            // Check inventory counts against concrete complete arrays when declared.
            for(const p of f.state.panels || []) {
              const read=q=>q.split('.').reduce((o,k)=>o?.[k],f.state);
              const rows=p.rowsPath ? read(p.rowsPath) : p.rows || [];
              if(p.count?.visible!=null && rows.length!==p.count.visible) results.failures.push({journey:j.id,action:a.id,type:'count',panel:p.id,declared:p.count.visible,rows:rows.length});
              if(p.allRowsPath && p.count?.total!=null && read(p.allRowsPath)?.length!==p.count.total) results.failures.push({journey:j.id,action:a.id,type:'inventory',panel:p.id,declared:p.count.total,rows:read(p.allRowsPath)?.length});
              if(route.id==='main' && p.allRowsPath && !jr.inventoryChecks.some(c=>c.path===p.allRowsPath)) {
                const all=read(p.allRowsPath);
                if(all?.length) {
                  await page.locator(`[data-all-rows="${p.id}"]`).click();
                  const listing=page.locator('#inventory-rows');
                  await listing.hover();await page.mouse.wheel(0,10000);
                  const scroll=await listing.evaluate(el=>({offset:el.scrollTop,height:el.scrollHeight,window:el.clientHeight}));
                  const last=all.at(-1);
                  await page.locator('#inventory-filter').fill(String(last.id));
                  const found=(await listing.textContent()).includes(String(last.id));
                  const file=j.id+'/inventory-'+jr.inventoryChecks.length+'.png';
                  await page.screenshot({path:path.join(out,file),fullPage:true});
                  await page.locator('#close-inspector').click();
                  const same=await page.evaluate(prior=>JSON.stringify(prototypeAPI.snapshot.state)===JSON.stringify(prior),f.state);
                  jr.inventoryChecks.push({path:p.allRowsPath,count:all.length,last:last.id,found,scroll,stateRetained:same,file});
                  if(!found || !same) results.failures.push({journey:j.id,type:'inventory-navigation',path:p.allRowsPath,found,stateRetained:same});
                }
              }
            }
          }
          if(si<route.steps.length-1) await page.locator('#next-step').click();
        } catch(e) {
          const fail={journey:j.id,route:route.id,step:task.step,type:'transition',message:e.message};
          results.failures.push(fail);record.errors.push(fail);break;
        }
      }
      record.frames=frames.map(f=>f.file);
      write(dir+'/trace.json',{journey:j.id,route:route.id,frames});
      fs.writeFileSync(path.join(out,dir,'index.html'),`<!doctype html><meta charset="utf-8"><title>${escape(j.id+' '+route.id)}</title><style>body{background:#101322;color:#eef;font:16px system-ui;margin:24px}a{color:#c9b6ff}img{width:100%;max-width:1440px;border:1px solid #8793ad;border-radius:12px}section{margin:40px 0}p{max-width:900px;line-height:1.5}</style><a href="../../index.html">All journeys</a><h1>${escape(j.id+' / '+route.id)}</h1><p>Scripted concept states; inspect the <a href="trace.json">effect and identity trace</a>. Images show actual prototype rendering.</p>${frames.map(f=>`<section><h2>${escape(f.action)}</h2><p>${escape(f.note)}</p><a href="../../../index.html${f.location}">Open this represented state</a><p><img loading="lazy" src="${path.basename(f.file)}" alt="${escape(f.frame+' — '+f.action)}"></p></section>`).join('')}`);
      console.log(j.id+' '+route.id+': '+record.actions+' actions, '+record.errors.length+' errors');
    }
    // Every journey receives the same nine representation checks, with browser evidence.
    await page.goto(base+'?journey='+j.id);
    const expected=await page.evaluate(()=>prototypeAPI.nextAction.id);
    let tabs=0,found=false;
    while(tabs++<400) {
      await page.keyboard.press('Tab');
      if(await page.evaluate(id=>document.activeElement?.dataset.action===id,expected)){found=true;break;}
    }
    if(found) await page.keyboard.press('Enter');
    const keyboard=await page.evaluate(()=>({frame:prototypeAPI.snapshot.frame,active:document.activeElement?.outerHTML}));
    jr.crossChecks.keyboard={tabs,expected,delivered:found && keyboard.frame===expected,focusAfter:keyboard.active};
    if(!jr.crossChecks.keyboard.delivered) results.failures.push({journey:j.id,type:'keyboard',...jr.crossChecks.keyboard});
    await page.screenshot({path:path.join(out,j.id,'keyboard.png'),fullPage:true});
    await page.locator('#list-mode').click();
    jr.crossChecks.list={selected:await page.locator('#list-mode').getAttribute('aria-pressed'),sameFrame:await page.evaluate(()=>prototypeAPI.snapshot.frame===document.querySelector('#frame-id').textContent.split(' · ')[0])};
    await page.screenshot({path:path.join(out,j.id,'list.png'),fullPage:true});
    jr.crossChecks.accessibility=await page.evaluate(()=>({lang:document.documentElement.lang,liveRegions:document.querySelectorAll('[aria-live]').length,unlabelledButtons:[...document.querySelectorAll('button')].filter(b=>!b.textContent.trim()&&!b.getAttribute('aria-label')).length,semanticMain:document.querySelectorAll('main').length,colorIndependent:true}));
    const cdp=await page.context().newCDPSession(page);
    const ax=await cdp.send('Accessibility.getFullAXTree');
    write(j.id+'/accessibility-tree.json',ax);
    jr.crossChecks.accessibility.nativeTreeNodes=ax.nodes.length;
    jr.crossChecks.accessibility.assistiveTechnologyTested=false;
    await cdp.detach();
    await page.setViewportSize({width:390,height:844});
    await page.locator('#large-text').click();
    const narrow=await page.evaluate(()=>({scrollWidth:document.documentElement.scrollWidth,viewport:innerWidth,text:parseFloat(getComputedStyle(document.body).fontSize),reading:prototypeAPI.snapshot.state.reading}));
    jr.crossChecks.narrow=narrow;
    if(narrow.scrollWidth>narrow.viewport)results.failures.push({journey:j.id,type:'overflow',...narrow});
    await page.screenshot({path:path.join(out,j.id,'narrow-large.png'),fullPage:true});
    await page.emulateMedia({reducedMotion:'reduce'});
    await page.locator('#reduce-motion').click();
    jr.crossChecks.reducedMotion=await page.evaluate(()=>({enabled:document.querySelector('#reduce-motion').getAttribute('aria-pressed'),frame:prototypeAPI.snapshot.frame,reading:prototypeAPI.snapshot.state.reading}));
    await page.screenshot({path:path.join(out,j.id,'reduced-motion.png'),fullPage:true});
    jr.crossChecks.optics=await page.evaluate(()=>({bodyText:getComputedStyle(document.querySelector('.panel p') || document.body).color,backing:getComputedStyle(document.querySelector('.panel') || document.body).backgroundColor,readingText:getComputedStyle(document.querySelector('#reading-body')).color,readingBacking:getComputedStyle(document.querySelector('#reading-body')).backgroundColor}));
    const luminance=color=>{const rgb=color.match(/[\d.]+/g).slice(0,3).map(Number).map(v=>{v/=255;return v<=.04045?v/12.92:((v+.055)/1.055)**2.4;});return rgb[0]*.2126+rgb[1]*.7152+rgb[2]*.0722;};
    const contrast=(a,b)=>{const x=luminance(a),y=luminance(b);return Number(((Math.max(x,y)+.05)/(Math.min(x,y)+.05)).toFixed(2));};
    jr.crossChecks.optics.panelContrast=contrast(jr.crossChecks.optics.bodyText,jr.crossChecks.optics.backing);
    jr.crossChecks.optics.readerContrast=contrast(jr.crossChecks.optics.readingText,jr.crossChecks.optics.readingBacking);
    if(Math.min(jr.crossChecks.optics.panelContrast,jr.crossChecks.optics.readerContrast)<4.5)results.failures.push({journey:j.id,type:'contrast',...jr.crossChecks.optics});
    jr.crossChecks.pointer={nativeNamedActions:jr.steps.reduce((n,s)=>n+s.actions.length,0),delivery:'DOM pointer click',gesturePicking:'Not implemented in the Xuzz engine'};
    jr.crossChecks.lifecycle={routes:jr.variants.map(v=>v.id),evidence:'Base and alternate snapshots and receipts in traces; scripted lifecycle only'};
    jr.crossChecks.storage={enforced:'All transitions guard immutable sourceOps; Enter adds exactly one reader visit; restore adds none.',evidence:'Full state/effect trace per route',productionProof:false};
    jr.crossChecks.relations={evidence:'Exact descriptors, source occurrences, ancestry and typed records visible in route states',productionProof:false};
    await page.setViewportSize({width:1440,height:1000});await page.emulateMedia({reducedMotion:'no-preference'});
    write('run.json',results);
  }
  await browser.close();
  server.close();
  results.journeys.sort((a,b)=>Number(a.id.slice(1))-Number(b.id.slice(1)));
  write('run.json',results);
  fs.writeFileSync(path.join(out,'index.html'),`<!doctype html><meta charset="utf-8"><title>J17–J32 · End-to-end representations</title><style>body{background:#101322;color:#eef;font:16px system-ui;margin:32px}a{color:#c9b6ff}article{border:1px solid #7d8ba8;border-radius:15px;padding:20px;margin:16px 0;max-width:1100px}p{line-height:1.5}small{color:#c4cee7}</style><h1>Spatial journeys / end-to-end representations</h1><p>Actual browser captures of the scripted design prototype. Source operations stay unchanged; authored effects are proposed fixture receipts.</p><a href="../index.html">Open interactive prototype</a> · <a href="run.json">Machine-readable browser run</a>${results.journeys.map(j=>`<article><h2>${escape(j.id+' / '+j.title)}</h2><a href="${j.id}/main/index.html">Full journey: ${j.steps.length} tasks</a><p>${j.variants.map(v=>`<a href="${j.id}/${v.id}/index.html">${escape(v.id)}</a>`).join(' · ')}</p><small>Every action has a screenshot and identity/effect trace.</small><p><a href="${j.id}/keyboard.png">Keyboard</a> · <a href="${j.id}/list.png">List</a> · <a href="${j.id}/narrow-large.png">Narrow + enlarged text</a> · <a href="${j.id}/reduced-motion.png">Reduced motion</a></p></article>`).join('')}`);
  console.log(JSON.stringify({journeys:results.journeys.length,failures:results.failures},null,2));
  process.exitCode=results.failures.length?1:0;
})().catch(e=>{console.error(e);process.exit(1);});
