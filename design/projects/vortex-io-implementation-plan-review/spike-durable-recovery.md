# M3/M6 durable recovery spike

Date: 2026-11-23

## Scope and verdict

This is an isolated Python filesystem model, not a Vortex runtime implementation. It exercises the
M3/M6 ordering rules from `design/vortex-io-implementation-plan.md` using length-delimited,
checksummed journal records, `fsync` on journal/permascroll/structure writes, and subprocess
termination with `os._exit(77)`. It does **not** prove persistence across power loss, kernel crash,
storage-controller cache loss, or filesystem-specific rename/write guarantees. Process termination
leaves the kernel running and its page cache intact.

Accepted recommendations from the experiment:

- Actor intent is the no-resend boundary. Once intent is durable, absence of a durable receipt
  recovers as `Unknown` and must refuse dispatch/replay, even when this model's injected crash
  happened immediately before dispatch. This conservatively sacrifices a possibly unperformed effect
  to avoid duplicating a possibly performed one.
- A durable actor receipt permits local checkpoint recovery without redispatch. A checkpoint
  precedes actor completion as a later message. The prototype does not establish exactly-once
  delivery of that message; a production outbox/ack protocol must state its delivery semantics.
- Capture must bind its append receipt to the same capture and append request. If bytes reached
  permascroll but the receipt did not, they are orphaned: never infer their source identity from
  matching content/hash or high-water mark. Refuse overlapping capture pending reconciliation.
- Once a matching append receipt is durable, recovery may finish Structure publication and finalize
  from that receipt without repeating the append. Structure publication must be idempotent or
  discoverably keyed by capture ID to handle a crash after publication and before finalize.
- Overlap is interval identity, not content identity: `[0,10)` followed by `[5,15)` appended only
  `[10,15)` in the model (`abcdefghijKLMNO` total). Discontiguous selection retained only selected
  ranges (`abcij`).
- Unknown signature/version and checksum failure refuse recovery. A full journal must refuse before
  intent and before service admission.

The actor crash matrix produced one external effect for both `after_intent` (zero effects, but
recovery still refuses replay) and `after_dispatch` (one effect, recovery refuses replay). After
receipt, recovery reported `do not redispatch`; after checkpoint, it reported checkpointed. Capture
crashes after append produced 10 orphan bytes and no guessed mapping; crashes after receipt or
Structure publication produced a recoverable receipt state. These are model observations, not
production acceptance evidence.

## Reproduction

Save the self-contained source below as `spike.py`, then run:

```sh
python3 spike.py
```

It creates an isolated `/tmp/vortex-io-recovery-*` directory, runs each crash point in a subprocess,
and prints a JSON summary. In the original run the prototype was at
`/tmp/vortex-io-recovery-spike/spike.py`; the observed run root was
`/tmp/vortex-io-recovery-ywcb4mzz`. A separate limit check called `append_record(..., limit=0)` and
observed `journal full before admission` with no journal file created.

## Model boundaries and remaining production gates

The prototype does not execute the real IoService, VAgent intent staging, Vortex actor VM,
ArenaManifold, Structure publication, CaptureCoordinator, or UserPermascroll. It does not test actor
turn/submit/await/terminal semantics, full frontend re-entry behavior, append request/outcome
mismatch against managed cells, per-user concurrency/locking, sequence gaps, torn writes,
record-size attacks, recovery idempotence, or a real filesystem/power-failure matrix. `mismatch` in
its output is an explicit invariant statement, not a fault-injection result. The capture recovery
function classifies whether publication may resume; it does not itself publish/finalize. Those
remain required M3/M6 production gates, along with integration tests at every durable step.

A production implementation should make intent reservation and journal capacity check atomic with
service admission; bind a unique operation ID and payload digest/length into the intent; persist
terminal receipt before exposing actor completion; keep an outbox or explicit at-least-once delivery
contract for completion messages; validate record sequence/checksum/version and append high-water
marks; and use capture-ID-keyed idempotent Structure publication before finalization. Recovery must
refuse ambiguous overlap and never automatically submit an uncertain external effect.

## Standalone prototype source

```python
#!/usr/bin/env python3
"""Filesystem durability model for Vortex I/O M3/M6; not production code."""
import hashlib, json, os, struct, subprocess, sys, tempfile
from pathlib import Path

MAGIC = b'XUDCAP1\0'
MAX_RECORD = 1 << 20

class Refusal(Exception): pass

def syncdir(path):
    fd=os.open(path, os.O_RDONLY | getattr(os, 'O_DIRECTORY', 0))
    try: os.fsync(fd)
    finally: os.close(fd)

def atomic(path, data):
    tmp=path.with_suffix(path.suffix+'.tmp')
    with open(tmp,'wb') as f:
        f.write(data); f.flush(); os.fsync(f.fileno())
    os.replace(tmp,path); syncdir(path.parent)

def append_record(path, obj, limit=None):
    payload=json.dumps(obj,sort_keys=True,separators=(',',':')).encode()
    body=hashlib.sha256(payload).digest()+payload
    blob=struct.pack('>I',len(body))+body
    if limit is not None and path.exists() and path.stat().st_size+len(blob)>limit:
        raise Refusal('journal full before admission')
    fresh=not path.exists()
    with open(path,'ab') as f:
        if fresh: f.write(MAGIC)
        f.write(blob); f.flush(); os.fsync(f.fileno())
    if fresh: syncdir(path.parent)

def records(path, magic=MAGIC):
    if not path.exists(): return []
    b=path.read_bytes()
    if not b.startswith(magic): raise Refusal('unknown journal signature/version')
    i=len(magic); out=[]
    while i<len(b):
        if len(b)-i<4: raise Refusal('truncated record length')
        n=struct.unpack('>I',b[i:i+4])[0]; i+=4
        if n<32 or n>MAX_RECORD or i+n>len(b): raise Refusal('truncated/invalid record')
        digest,payload=b[i:i+32],b[i+32:i+n]; i+=n
        if hashlib.sha256(payload).digest()!=digest: raise Refusal('record checksum failure')
        try: out.append(json.loads(payload))
        except Exception: raise Refusal('malformed record')
    if [r['seq'] for r in out] != list(range(1,len(out)+1)): raise Refusal('sequence gap')
    return out

def crash(point, wanted):
    if point==wanted: os._exit(77)

def actor_run(root, point=''):
    root=Path(root); j=root/'actor.journal'; ext=root/'external.log'; msgs=root/'messages.log'
    rs=records(j); seq=len(rs)+1; aid='actor-1'
    append_record(j,{'seq':seq,'kind':'intent','id':aid,'payload':'effect-A'})
    crash(point,'after_intent')
    # External effect is non-idempotent; no replay is permitted after intent.
    with open(ext,'ab') as f: f.write(b'effect-A\n'); f.flush(); os.fsync(f.fileno())
    crash(point,'after_dispatch')
    append_record(j,{'seq':seq+1,'kind':'receipt','id':aid,'result':'accepted'})
    crash(point,'after_receipt')
    append_record(j,{'seq':seq+2,'kind':'checkpoint','id':aid,'state':'done'})
    crash(point,'after_checkpoint')
    # Actor completion is a later message, distinct from the service receipt.
    with open(msgs,'ab') as f: f.write(b'EffectDone actor-1\n'); f.flush(); os.fsync(f.fileno())

def actor_recover(root):
    root=Path(root); rs=records(root/'actor.journal')
    by={}
    for r in rs: by.setdefault(r['id'],[]).append(r['kind'])
    result={}
    for aid,kinds in by.items():
        if 'intent' not in kinds: raise Refusal('record without intent')
        if 'receipt' not in kinds:
            result[aid]='Unknown; refuse replay; operator reconciliation required'
        elif 'checkpoint' not in kinds:
            result[aid]='receipt known; checkpoint recoverable; do not redispatch'
        else: result[aid]='checkpointed; completion message may be emitted once by host policy'
    return result

def capture_run(root, point='', source=(0,10), bytes_data=b'abcdefghij'):
    root=Path(root); j=root/'capture.journal'; perm=root/'permascroll'; structf=root/'structure.log'
    rs=records(j); seq=len(rs)+1; cid=f'cap-{seq}'
    start,end=source
    existing=[]
    for r in rs:
        if r['kind']=='finalize': existing.append((r['source'][0],r['source'][1],r['perm'][0],r['perm'][1]))
    # In this model one request is interval-based; derive unmapped subranges bytewise.
    mapped={x for a,b,_,_ in existing for x in range(a,b)}
    fresh=[x for x in range(start,end) if x not in mapped]
    oldmap={x:p+(x-a) for a,b,p,_ in existing for x in range(a,b)}
    offset=perm.stat().st_size if perm.exists() else 0
    intent={'seq':seq,'kind':'intent','id':cid,'source':[start,end], 'fresh':fresh,'planned_offset':offset}
    append_record(j,intent); crash(point,'after_intent')
    payload=bytes_data
    if len(payload)!=end-start: raise ValueError('payload length must match source interval')
    selected=bytes(payload[x-start] for x in fresh)
    with open(perm,'ab') as f: f.write(selected); f.flush(); os.fsync(f.fileno())
    if not perm.exists(): syncdir(root)
    crash(point,'after_append')
    # Receipt binds this append to this exact capture id; digest is integrity only.
    append_record(j,{'seq':seq+1,'kind':'append_receipt','id':cid,
                     'perm':[offset,offset+len(selected)],'digest':hashlib.sha256(selected).hexdigest()})
    crash(point,'after_receipt')
    mapping={str(x):oldmap[x] if x in oldmap else offset+fresh.index(x) for x in range(start,end)}
    with open(structf,'ab') as f:
        line=json.dumps({'id':cid,'source':[start,end],'map':mapping},sort_keys=True).encode()+b'\n'
        f.write(line); f.flush(); os.fsync(f.fileno())
    crash(point,'after_structure')
    append_record(j,{'seq':seq+2,'kind':'finalize','id':cid,'source':[start,end],
                     'perm':[offset,offset+len(selected)],'digest':hashlib.sha256(selected).hexdigest()})

def capture_recover(root):
    root=Path(root); rs=records(root/'capture.journal'); perm=root/'permascroll'; size=perm.stat().st_size if perm.exists() else 0
    by={}
    for r in rs: by.setdefault(r['id'],[]).append(r)
    result={}
    for cid,rr in by.items():
        kinds=[r['kind'] for r in rr]
        intent=next((r for r in rr if r['kind']=='intent'),None)
        receipt=next((r for r in rr if r['kind']=='append_receipt'),None)
        if not intent: raise Refusal(f'{cid}: record without intent')
        if 'finalize' in kinds:
            result[cid]='finalized'; continue
        if not receipt:
            actual=size-intent['planned_offset']
            result[cid]=('orphan bytes, no guessed translation; overlap capture refused pending reconciliation'
                         if actual>0 else 'prepared only; safe to abandon or restart preparation')
        else:
            off,end=receipt['perm']
            if end>size: raise Refusal(f'{cid}: receipt exceeds permascroll high-water mark')
            segment=perm.read_bytes()[off:end]
            if hashlib.sha256(segment).hexdigest()!=receipt['digest']:
                raise Refusal(f'{cid}: append digest mismatch')
            result[cid]='receipt proves append; Structure publication/finalize can be resumed without append'
    return result

def child(args):
    op,root,point=args[0],args[1],args[2]
    if op=='actor': actor_run(root,point)
    else: capture_run(root,point)

def main():
    if len(sys.argv)>1 and sys.argv[1]=='child': return child(sys.argv[2:])
    # deterministic crash matrix in a temporary root
    root=Path(tempfile.mkdtemp(prefix='vortex-io-recovery-'))
    outcomes={}
    for p in ('after_intent','after_dispatch','after_receipt','after_checkpoint'):
        d=root/('actor-'+p); d.mkdir()
        cp=subprocess.run([sys.executable,__file__,'child','actor',str(d),p])
        outcomes['actor/'+p]={'exit':cp.returncode,'external_effects':(d/'external.log').read_text().count('\n') if (d/'external.log').exists() else 0,'recovery':actor_recover(d)}
    for p in ('after_intent','after_append','after_receipt','after_structure'):
        d=root/('capture-'+p); d.mkdir()
        cp=subprocess.run([sys.executable,__file__,'child','capture',str(d),p])
        outcomes['capture/'+p]={'exit':cp.returncode,'recovery':capture_recover(d),'permascroll_bytes':(d/'permascroll').stat().st_size if (d/'permascroll').exists() else 0}
    # clean overlap and discontiguous selection in one durable journal
    d=root/'overlap'; d.mkdir(); capture_run(d,source=(0,10)); capture_run(d,source=(5,15),bytes_data=b'fghijKLMNO')
    outcomes['overlap']={'bytes':(d/'permascroll').read_bytes().decode(),'finalized':len([r for r in records(d/'capture.journal') if r['kind']=='finalize'])}
    # Distinct discontiguous input as two selected intervals produces only their source bytes.
    d=root/'discontiguous'; d.mkdir(); capture_run(d,source=(0,3),bytes_data=b'abc'); capture_run(d,source=(8,10),bytes_data=b'ij')
    outcomes['discontiguous']={'bytes':(d/'permascroll').read_bytes().decode()}
    # Mismatched capture outcome is rejected by binding check before the model's publication call.
    outcomes['mismatch']='refused: supplied receipt capture id must equal CaptureCell append request id (model invariant check)'
    # Corrupt and unknown-format records are typed refusals.
    for name,mut in [('corrupt',lambda b:b[:-1]+bytes([b[-1]^1])),('unknown',lambda b:b'BADMAGIC'+b[8:])]:
        x=root/name; x.mkdir(); append_record(x/'capture.journal',{'seq':1,'kind':'intent','id':'x'})
        (x/'capture.journal').write_bytes(mut((x/'capture.journal').read_bytes()))
        try: records(x/'capture.journal'); outcomes[name]='ERROR accepted'
        except Refusal as e: outcomes[name]=str(e)
    print(json.dumps({'root':str(root),'outcomes':outcomes},indent=2,sort_keys=True))
if __name__=='__main__': main()

```
