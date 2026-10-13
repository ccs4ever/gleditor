/* Scripted concept-state replay; deliberately independent of the Xuzz engine. */
(() => {
  'use strict';
  const clone = value => JSON.parse(JSON.stringify(value));
  const safe = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const pretty = value => typeof value === 'string' ? value : JSON.stringify(value);
  const scopeText = value => Array.isArray(value) ? value.map(pretty).join(' · ') : value && typeof value === 'object' ? Object.entries(value).map(([k,v])=>`${k}: ${Array.isArray(v) ? v.join(', ') : pretty(v)}`).join(' · ') : pretty(value);
  const get = (value, path) => path.split('.').reduce((o, k) => o?.[k], value);
  const set = (value, path, next) => {
    const keys = path.split('.'); let obj = value;
    keys.slice(0, -1).forEach(k => { if (!obj[k] || typeof obj[k] !== 'object') obj[k] = {}; obj = obj[k]; });
    obj[keys.at(-1)] = clone(next);
  };
  const stable = value => JSON.stringify(value, (_, v) => v && !Array.isArray(v) && typeof v === 'object' ? Object.fromEntries(Object.entries(v).sort(([a],[b]) => a.localeCompare(b))) : v);
  const $ = id => document.getElementById(id);
  const pack = window.SPATIAL_JOURNEYS;
  if (!pack?.journeys?.length) { $('result-note').textContent = 'State pack is not loaded. Build journey-data.js from the JSON packs.'; return; }
  const journeys = pack.journeys;
  let journey, variant = null, state, stepIndex = 0, actionIndex = 0, records = [], error = null;
  const routeSteps = () => variant ? [{step:variant.step,goal:variant.title,actions:variant.actions,checks:variant.checks}] : journey.steps;
  const step = () => routeSteps()[stepIndex];
  const frameId = () => actionIndex ? step().actions[actionIndex-1].id : `${journey.id}-${variant?.id || 'main'}-s${step().step}-before`;
  function apply(action, record = true) {
    const before = clone(state);
    const candidate = clone(state);
    for (const [path, value] of Object.entries(action.expect || {})) {
      if (stable(get(state, path)) !== stable(value)) throw new Error(`${action.id}: prior ${path} differs from declared fixture`);
    }
    for (const [path, value] of Object.entries(action.patch || {})) set(candidate, path, value);
    if (stable(before.sourceOps) !== stable(candidate.sourceOps)) throw new Error(`${action.id}: visited source operations changed`);
    if (['inspect','view','save','author'].includes(action.kind) && before.reading?.visit !== candidate.reading?.visit) throw new Error(`${action.id}: inspection/save/authoring changed reading visit`);
    if (action.kind === 'restore' && before.readerEffects?.visits !== candidate.readerEffects?.visits) throw new Error(`${action.id}: restore added a visit`);
    if (action.kind === 'enter' && candidate.readerEffects?.visits !== before.readerEffects?.visits + 1) throw new Error(`${action.id}: Enter must record exactly one reader visit`);
    state = candidate;
    const effects = action.effects || [{domain:'none',summary:'Inspection only; current reading place and source states retained.'}];
    if (record) records.push({frame:action.id, action:action.label, kind:action.kind, before, after:clone(state), effects, note:action.note || ''});
    return effects;
  }
  function initialState() { return clone(variant?.start || journey.initial); }
  function reset(id, variantId = '') {
    journey = journeys.find(j => j.id === id) || journeys[0];
    variant = journey.variants?.find(v => v.id === variantId) || null;
    state = initialState(); stepIndex = 0; actionIndex = 0; records = []; error = null;
    $('journey-picker').value = journey.id;
    $('variant-picker').innerHTML = '<option value="">Main journey</option>' + (journey.variants || []).map(v => `<option value="${safe(v.id)}">${safe(v.title)}</option>`).join('');
    $('variant-picker').value = variant?.id || '';
    $('step-picker').innerHTML = routeSteps().map((s,i) => `<option value="${i}">Task ${s.step}: ${safe(s.goal)}</option>`).join('');
    $('result-note').textContent = 'Choose a named action. All effects shown here are scripted fixture effects.';
    $('effect-list').innerHTML = '';
    render();
  }
  function seek(index) {
    state = initialState(); records = []; stepIndex = 0; actionIndex = 0; error = null;
    for (let s = 0; s < index; s++) for (const action of routeSteps()[s].actions) apply(action);
    stepIndex = index; render();
  }
  function next(actionId) {
    const action = step().actions[actionIndex];
    if (!action || action.id !== actionId) return;
    try {
      const effects = apply(action); actionIndex++; error = null;
      $('result-note').textContent = action.note || `${action.label}: represented result is now visible.`;
      $('effect-list').innerHTML = effects.map(e => `<li>${safe(e.domain)} · ${safe(e.summary)}${e.identity ? ` · ${safe(e.identity)}` : ''}</li>`).join('');
      render();
      (document.querySelector('[data-action]:not(:disabled)') || $('next-step'))?.focus({preventScroll:true});
    } catch (e) { error = e.message; $('result-note').textContent = e.message; throw e; }
  }
  function rowsFor(panel, complete = false) {
    if (complete && panel.allRowsPath) return get(state,panel.allRowsPath) || [];
    if (panel.rowsPath) return get(state,panel.rowsPath) || [];
    return panel.rows || [];
  }
  function factRows(facts = []) {
    return `<dl class="facts">${facts.map(f => `<dt>${safe(f.label)}</dt><dd>${safe(pretty(f.value))}</dd>`).join('')}</dl>`;
  }
  function loomDiagram(panel) {
    const loom=state.loom;
    if(!loom?.trunk?.length || panel.id!==(state.panels || []).find(p=>p.kind==='history')?.id) return '';
    const nodes=new Map();
    loom.trunk.forEach((op,i)=>nodes.set(op.id,{x:80+20*Math.sin(i*Math.PI/4),y:22+i*2.1}));
    const pathFor=ops=>ops.map(op=>{const p=nodes.get(op.id);return `${p.x},${p.y}`;}).join(' ');
    let svg=`<polyline class="ancestry" points="${pathFor(loom.trunk)}"/><text x="18" y="263">Main · ${safe(loom.trunk.at(-1).id)}</text>`;
    for(const [i,b] of (loom.branches || []).entries()) {
      const fork=nodes.get(b.forkAt);if(!fork || !b.ops?.length) continue;
      b.ops.forEach((op,n)=>nodes.set(op.id,{x:205+19*Math.sin(n*Math.PI/4),y:fork.y+n*1.2}));
      svg+=`<path class="ancestry" d="M${fork.x},${fork.y} Q140,${fork.y-18} 205,${fork.y}"/><polyline class="ancestry" points="${pathFor(b.ops)}"/><text x="238" y="${Math.min(244,fork.y+17)}">${safe(b.id)} · ${safe(b.head)}</text>`;
    }
    for(const [id,p] of nodes) svg+=`<circle cx="${p.x}" cy="${p.y}" r="${id===loom.selectedAnchor?4:1.4}" fill="${id===loom.selectedAnchor?'#bcabf9':'#a8afc3'}"/>`;
    const parallel=loom.parallelView;
    if(parallel?.sameOperation && nodes.has(parallel.sameOperation)) {
      const p=nodes.get(parallel.sameOperation);
      svg+=`<path class="correspondence" d="M${p.x},${p.y} Q255,${p.y-65} 368,85"/><rect x="322" y="82" width="112" height="30" rx="6" class="filtered"/><text x="330" y="103">${safe(parallel.sameOperation)} copy</text>`;
    }
    return `<figure class="loom-diagram"><svg viewBox="0 0 450 280" role="img" aria-label="Stored main ancestry and three branch exits. Same-operation correspondence is shown only when a parallel view is active."><title>Stored ancestry and view correspondence</title>${svg}</svg><figcaption>Compressed stored ancestry · individually named exits remain in the list.</figcaption><ul class="relation-key"><li><i class="silver"></i>Silver solid · stored parent</li><li><i class="pearl"></i>Pearl · same op, another view</li><li><i class="seamed"></i>Seamed · filtered view order</li><li><i class="gold"></i>Gold · shared source address</li></ul></figure>`;
  }
  function panelHTML(panel) {
    const rows = rowsFor(panel);
    const count = panel.count ? `<div class="count">${safe(panel.count.total)} total · ${safe(panel.count.filtered ?? panel.count.total)} filtered · ${safe(panel.count.visible ?? rows.length)} visible</div>` : '';
    return `<section class="glass panel ${safe(panel.tone || 'neutral')} kind-${safe(panel.kind || 'inspector')}" data-panel="${safe(panel.id)}" aria-label="${safe(panel.title)}">
      <h2>${safe(panel.title)}</h2>${loomDiagram(panel)}${(panel.body || []).map(p => `<p class="body">${safe(p)}</p>`).join('')}${factRows(panel.facts)}${count}
      ${rows.length ? `<ul class="rows">${rows.map((r,i) => `<li data-status="${safe(r.status || '')}"><strong>${r.ordinal != null ? safe(r.ordinal) + ' · ' : ''}${safe(r.label || r.title || r.id)}</strong><span>${safe(pretty(r.detail || r.descriptor || ''))}</span>${r.status ? `<span>${safe(r.status)}</span>` : ''}<button class="row-inspect" data-inspect-panel="${safe(panel.id)}" data-row="${i}" aria-label="Inspect ${safe(r.label || r.id)}">Inspect reference</button></li>`).join('')}</ul>` : ''}
      <div class="row-tools"><button data-panel-details="${safe(panel.id)}">Inspect exact record</button>${panel.allRowsPath ? `<button data-all-rows="${safe(panel.id)}">Browse complete inventory</button>` : ''}</div></section>`;
  }
  function render() {
    const current = step();
    $('journey-title').textContent = `${journey.id} / ${journey.title}`;
    const query = state.query || {};
    $('query-label').textContent = query.text || query.term || 'Exact subjects · pinned states';
    $('scope-label').textContent = query.scope ? scopeText(query.scope) : `${journey.fixture} · scripted fixture`;
    $('goal').textContent = current.goal;
    $('frame-id').textContent = `${frameId()} · ${journey.fixture}${variant ? ' · ' + variant.id : ''}`;
    $('step-picker').value = String(stepIndex);
    $('previous-step').disabled = stepIndex === 0;
    $('next-step').disabled = actionIndex < current.actions.length || stepIndex >= routeSteps().length-1;
    const reading = state.reading || {};
    $('visit-label').textContent = `Current reading place · visit ${reading.visit ?? 'retained'}`;
    $('reading-title').textContent = pretty(reading.subject || 'Reading place retained');
    $('reading-meta').textContent = [reading.store,reading.version,reading.occurrence].filter(Boolean).map(pretty).join(' · ');
    const body = reading.body || ['The selected source remains at its pinned state.', 'Inspection tools preserve this reading place until an explicit entry or restore action.'];
    $('reading-body').innerHTML = body.map(p => `<p>${safe(p)}</p>`).join('');
    $('reading-position').textContent = `Saved position ${reading.offset ?? 0}`;
    const panels = state.panels || [];
    $('left-wing').innerHTML = panels.filter((p,i) => i % 2 === 0).map(panelHTML).join('');
    $('right-wing').innerHTML = panels.filter((p,i) => i % 2 === 1).map(panelHTML).join('');
    function actionButtons(kind) {
      return current.actions.map((a,i) => ({a,i})).filter(({a}) => (a.kind === 'fixture') === (kind === 'fixture')).map(({a,i}) =>
        `<button class="action ${a.kind === 'fixture' ? 'fixture-action' : ''} ${i === actionIndex ? 'pending' : ''}" data-action="${safe(a.id)}" ${i !== actionIndex ? 'disabled' : ''} aria-label="${safe(a.label)}">${i < actionIndex ? '✓ ' : ''}${safe(a.label)}</button>`).join('');
    }
    $('product-actions').innerHTML = actionButtons('product');
    const fixtureActions = actionButtons('fixture');
    $('fixture-actions').innerHTML = fixtureActions ? '<span class="fixture-label">Scripted external events · simulator controls</span>' + fixtureActions : '';
    const visits = state.visits || [];
    $('visit-rail').textContent = Array.isArray(visits) ? visits.map(v => `${v.id || v.visit}: ${v.subject || v.label || ''}${v.parent ? ' ← ' + v.parent : ''}`).join(' / ') : pretty(visits);
    document.querySelectorAll('[data-action]').forEach(b => b.addEventListener('click', () => next(b.dataset.action)));
    document.querySelectorAll('[data-panel-details]').forEach(b => b.addEventListener('click', () => inspect((state.panels || []).find(p=>p.id===b.dataset.panelDetails), 'Exact subject record')));
    document.querySelectorAll('[data-inspect-panel]').forEach(b => b.addEventListener('click', () => inspect(rowsFor((state.panels || []).find(p=>p.id===b.dataset.inspectPanel))[Number(b.dataset.row)], 'Reference descriptor')));
    document.querySelectorAll('[data-all-rows]').forEach(b => b.addEventListener('click', () => inventory((state.panels || []).find(p=>p.id===b.dataset.allRows))));
  }
  function inspect(value, title) {
    $('inspector-title').textContent = title;
    $('inspector-body').innerHTML = `<div class="record"><pre>${safe(JSON.stringify(value,null,2))}</pre></div>`;
    $('inspector').showModal(); $('close-inspector').focus();
  }
  function inventory(panel) {
    $('inspector-title').textContent = `Complete inventory / ${panel.title}`;
    const rows = rowsFor(panel,true);
    $('inspector-body').innerHTML = '<label>Find a reference <input id="inventory-filter" placeholder="Name, ordinal or source"></label><ul class="rows" id="inventory-rows"></ul>';
    const draw = () => {
      const q = $('inventory-filter').value.toLocaleLowerCase();
      $('inventory-rows').innerHTML = rows.filter(r=>JSON.stringify(r).toLocaleLowerCase().includes(q)).map(r=>`<li><strong>${safe(r.ordinal ?? r.id)} · ${safe(r.label || r.title || r.id)}</strong><span>Reference ${safe(r.id)}</span><span>${safe(pretty(r.detail || r.descriptor || r))}</span></li>`).join('');
    };
    $('inventory-filter').addEventListener('input',draw); draw(); $('inspector').showModal(); $('inventory-filter').focus();
  }
  $('close-inspector').onclick = () => $('inspector').close();
  $('inspect-state').onclick = () => inspect({fixture:journey.fixture,frame:frameId(),state,records},'Fixture identity and simulated effect record');
  $('journey-picker').innerHTML = journeys.map(j => `<option value="${safe(j.id)}">${safe(j.id + ' · ' + j.title)}</option>`).join('');
  $('journey-picker').onchange = () => reset($('journey-picker').value);
  $('variant-picker').onchange = () => reset(journey.id,$('variant-picker').value);
  $('step-picker').onchange = () => seek(Number($('step-picker').value));
  $('reset-route').onclick = () => reset(journey.id,variant?.id);
  $('previous-step').onclick = () => seek(stepIndex-1);
  $('next-step').onclick = () => { if (!$('next-step').disabled) { stepIndex++; actionIndex=0; render(); } };
  for (const [id,css] of [['list-mode','list-mode'],['large-text','large-text'],['reduce-motion','reduced-motion']]) {
    $(id).onclick = () => { const on=document.body.classList.toggle(css); if(id==='large-text') document.documentElement.classList.toggle(css,on); $(id).setAttribute('aria-pressed',String(on)); };
  }
  window.prototypeAPI = { get snapshot(){return clone({journey:journey.id,variant:variant?.id || null,step:step().step,actionIndex,frame:frameId(),state,records,error});},reset,seek,next,journeys, get nextAction(){return step().actions[actionIndex] || null;} };
  reset(new URLSearchParams(location.search).get('journey') || journeys[0].id, new URLSearchParams(location.search).get('variant') || '');
  const params = new URLSearchParams(location.search);
  if (params.has('task')) seek(Math.max(0, Math.min(routeSteps().length-1, Number(params.get('task')) || 0)));
  for (let i=0, n=Number(params.get('action')) || 0; i<n && actionIndex<step().actions.length; i++) next(step().actions[actionIndex].id);
})();
