# State-pack authoring schema

Write plain JSON under packs/ for your assigned journeys. Each journey starts from one initial
state, then each numbered step has concrete named actions; the state flows across steps. Multiple
actions per step are expected. Include all task subparts, negative variants and explicit outcomes.
Do not replace a task with a generic Next/Continue button or only an explanatory paragraph.

Read contracts.md and the unchanged J17–J32 contract. This is a design prototype, not product code.
Do not edit the journey contract, baseline mockups or old review. Use real hypothetical identities
and ranges with a stable fixture ID; keep independent fixtures separate. No actual keys or network.

Top-level: {"schema":1,"runner":"luna-1","journeys":[...]}.

Journey:

- id, title, fixture, initial (state object), steps, variants.
- initial must include reading {visit,subject,store,version,occurrence,offset}, visits (nodes),
  selection (subject/state/locator), sourceOps (map of visited source authorities to count),
  readerEffects {visits,collections,annotations}, destinationEffects {}, and panels.
- Other fields can hold query, packages, pouch, draft, commit, media, loom, comparison etc.
- panels is an array of readable surface objects:
  {id,title,kind:"document|cell|list|inspector|comparison|media|history|notice", body:\[short
  paragraphs\],facts:[{label,value}],rows:[{id,label,detail,status,ordinal}],
  tone:"neutral|selected|comment|sharing",count:{total,filtered,visible}}. Prefer 2–4 panels; facts
  and rows carry exact identity/evidence. Avoid wall-of-text or tiny fields. Nested values are
  permitted in state, but facts should be strings/numbers for readable UI.
- Put reading content first, side inspectors next, transient notices last. Add type-specific
  details.

Step: {step:1,goal:"...",actions:[...],checks:[short explicit observations],coverage:\[task
subparts\]} Each action: {id:"J17-s1-a1",label:"Inspect
publisher",kind:"inspect|view|enter|restore|save|author|fixture", patch:{"selection.subject":"...",
"query.request":"Q2", "panels":[...]}, effects:\[{domain:"reader collection|reader activity|private
staging|authored destination|none", summary:"...",identity:"..."}\], expect:{"reading.visit":"01"},
note:"Short visible result/receipt; what changed and what did not", branches:optional []} Dotted
patch keys set exact values; arrays replace, objects are values. expect checks the BEFORE state. Do
not guess expectations: omit if not needed. Inspection must preserve reading/sourceOps. Enter adds
exactly one visit; restore does not. Successful author changes destinationEffects only.

All action labels are visible buttons in the prototype. The renderer displays panels after each
action, along with human-readable effects and exact fixture/frame IDs. Fixture-kind actions are
separate simulator events (e.g. pointer advance, loss of reply); not product controls. Action.result
is not used; put every represented result in patch.panels and relevant state fields. Always replace
panels with the result of that action so a prior caption cannot contradict state.

Variant: {id:"J17-invalid",step:4,title:"Invalid payload",start:optional state snapshot,
actions:[...],checks:[...],coverage:[...]} Variant actions are complete independent
negative/alternative routes, starting from variant.start or the journey.initial. Each must have
stable before/after descriptors and visible recovery/cancel. Represent alternatives beyond the
successful main path, not just labels saying they exist.

Keep source/count effects coherent. If a source was authored earlier, its starting count is a
fixture baseline and may never grow from navigation. All counts must match rows/data where shown. If
listing all 146/100 members, use a deterministic JSON array and count metadata consistently. Provide
distinct query recipe vs captured result snapshot and type-specific reference inspectors.

The lead renders and tests every action and adds common pointer/list/keyboard/focus, enlarged,
narrow and reduced-motion controls to all journeys. You needn't implement those common controls. Add
runtime-specific cases in J32. All variants must be reachable via the visible fixture-variant
chooser. Leave no TBD, unspecified policy, missing-state stub or expected future implementation
inside your represented routes. Cite declared proposed contracts where required.

Your output should cover every assigned base step in order. Include a coverage map from prior runner
findings/variants to new step/action IDs in a separate pack-coverage.md. Do not generate images or
product implementation; write only your assigned pack JSON and coverage Markdown.
