# M8 I/O budget pressure spike

Date: 2026-11-23

## Scope

This is an executable deterministic accounting model for the M8 pressure questions in §11 of
`design/vortex-io-implementation-plan.md`, following the reserve-before-admit, owner-pump, progress
coalescing, and release-ack contract in §4. It is a preparatory spike only: the actual `IoService`,
scheduler, adapters, and host configuration do not exist yet. The small capacities below are
scenario inputs chosen to force pressure and ordering cases; they are not candidate production
defaults and say nothing about real workload rates or byte sizes.

The model treats queue entries, in-flight worker slots, pinned/backend leases, terminal result
reservations, and control metadata as separate counted classes. Admission atomically reserves queue,
pinned, result, and control capacity before the request is visible. An in-flight slot is acquired
only when queued work starts, since it represents active worker concurrency rather than total
admitted requests. Work that exceeds a reservation refuses before admission. Finishing backend work
creates a terminal candidate; owner publication waits for both that candidate and release
acknowledgement. Those reservations remain charged until the owner pump applies the terminal result.
Progress is one replaceable latest-value record per request and shares the configured owner work cap
with terminal delivery. Read and write directions are tracked independently.

## Reproduction

Requires Python 3.10+; no repository build or runtime dependencies.

Extract the included source into its unique temporary path, then run it:

````sh
python3 - <<'PY'
from pathlib import Path
report = Path('design/projects/vortex-io-implementation-plan-review/spike-budget-pressure.md').read_text()
source = report.split('## Executable model source\n\n```python\n', 1)[1].split('\n```', 1)[0]
Path('/tmp/vortex_io_pressure_spike_repro.py').write_text(source)
PY
python3 /tmp/vortex_io_pressure_spike_repro.py
````

The run produced:

```text
    delayed backend: admitted=3 refused=3 completed=0 peak_terminal_backlog=0 progress_pending=0 charges={'queue': 3, 'inflight': 0, 'pinned': 3, 'result': 3, 'control': 3}
    burst completions: admitted=6 refused=0 completed=6 peak_terminal_backlog=6 progress_pending=0 charges={'queue': 0, 'inflight': 0, 'pinned': 0, 'result': 0, 'control': 0}
      pump (terminal, progress, terminal backlog): [(2, 2, 0, 4), (3, 2, 0, 2), (4, 2, 0, 0), (5, 0, 2, 0), (6, 0, 2, 0), (7, 0, 2, 0)]
      independent directions: {'read': 3, 'write': 3}
    delayed release acknowledgement: terminal held until ack; charges released only after pump
    pump cap/backpressure: admitted=4 refused=0 completed=1 peak_terminal_backlog=3 progress_pending=3 charges={'queue': 1, 'inflight': 0, 'pinned': 3, 'result': 3, 'control': 3}
      pump (terminal, progress, terminal backlog): [(2, 1, 0, 2)]
    all invariant assertions passed
```

## Scenarios and observations

- **Delayed backend / reserve refusal:** with 3 slots per admission charge class and a single worker
  slot, six offers produced three admitted queued requests and three refusals. No backend was
  started. Queue, pinned, result, and control charges each remained 3. This checks that pressure is
  visible before admission and that delayed work is bounded.
- **Burst completion / bounded owner turn:** six in-flight requests completed together, while every
  request emitted three progress updates. All six terminal records were retained (peak backlog 6);
  progress coalesced to six records, rather than eighteen. A pump cap of 2 processed at most two
  units per turn. Terminal delivery drained over three turns, followed by progress over three turns.
  Queue, in-flight, pinned, result, and control charges returned to zero after terminal publication.
  Three reads and three writes were accounted independently.
- **Release acknowledgement later than result:** a terminal candidate arrived before its release
  ack. The terminal was not made pump-eligible and its pinned/result reservations stayed charged
  until ack; owner pump then released them.
- **Pump pressure:** three completions plus three coalesced progress records were pending under a
  one-unit owner cap. The first pump applied only one terminal; two terminal reservations and all
  three progress records remained accounted. One further admission fit because capacity remained.
  This illustrates bounded turn work and retained results under slow owner consumption.

Assertions check that every charge remains within its configured capacity, terminal delivery is
bounded by result capacity, ack without candidate is not publishable, all delivered terminal records
have both candidate and ack, admission charges release only at owner publication, progress has one
latest value per request, and the exercised directions preserve their separate counts.

## Limits and gates not closed

The model does not exercise C++ synchronization, actual byte-valued charging, partial reservation
rollback, enqueue invariant failure, cancellation races, backend cleanup failure, allocator
pressure, endpoint FIFO/duplex scheduling, adapter preparation cost, or configuration-generation
resolution. In particular, terminal and ack sets are IDs in a deterministic single-threaded
simulation; they do not establish thread safety or real-time bounds. The release-order case models
candidate-first; the symmetric ack-first behavior is represented by the same readiness predicate but
was not separately scheduled. Per-turn work is an abstract unit count, not measured CPU time.

This does **not** close M8. M8 depends on M1–M7 evidence and still requires representative workloads
against the implemented service, byte and lease accounting, setting schema/default/purpose/notes
links, generation-snapshot resolution, and pressure tests. No production numeric defaults should be
selected from this model.

## Executable model source

```python
from collections import deque
from dataclasses import dataclass

@dataclass(frozen=True)
class Limits:
    queue: int = 4
    inflight: int = 2
    pinned: int = 4
    result: int = 4
    control: int = 4
    pump_work: int = 2

class Model:
    def __init__(self, limits):
        self.lim=limits; self.q=deque(); self.running={}; self.done=deque()
        self.resources=dict(queue=0,inflight=0,pinned=0,result=0,control=0)
        self.terminal=set(); self.acks=set(); self.ack_pending={}; self.progress={}
        self.admitted=self.refused=self.completed=self.peak_done=0
        self.pump_events=[]; self.per_dir={'read':0,'write':0}
    def invariant(self):
        for k,v in self.resources.items(): assert 0 <= v <= getattr(self.lim,k)
        assert len(self.done) <= self.lim.result
        assert self.acks <= self.terminal
    def admit(self,rid,direction,start,finish):
        # Reserve all classes atomically before admitted becomes visible.
        if any(self.resources[k] >= getattr(self.lim,k) for k in self.resources if k != 'inflight'):
            self.refused+=1; return False
        for k in self.resources:
            if k != 'inflight': self.resources[k]+=1
        self.q.append((rid,direction,start,finish)); self.admitted+=1
        self.invariant(); return True
    def start_ready(self,t):
        deferred=deque()
        while self.q:
            item=self.q.popleft(); rid,direction,start,finish=item
            if start<=t and len(self.running)<self.lim.inflight:
                self.resources['queue']-=1; self.resources['inflight']+=1
                self.running[rid]=(direction,finish)
            else: deferred.append(item)
        self.q=deferred
    def backend(self,t,ack_delay=0):
        # Terminal candidate and release acknowledgement arrive together here,
        # but remain modeled as separately required facts for owner publication.
        for rid,(direction,finish) in list(self.running.items()):
            if finish<=t:
                del self.running[rid]; self.resources['inflight']-=1
                self.terminal.add(rid)
                if ack_delay == 0:
                    self.acks.add(rid); self.done.append((rid,direction))
                    self.peak_done=max(self.peak_done,len(self.done))
                else:
                    self.ack_pending[rid]=(direction,t+ack_delay)
        for rid,(direction,at) in list(self.ack_pending.items()):
            if at<=t:
                del self.ack_pending[rid]; self.acks.add(rid); self.done.append((rid,direction))
                self.peak_done=max(self.peak_done,len(self.done))
        self.invariant()
    def progress_update(self,rid,n): self.progress[rid]=n
    def pump(self,t):
        n=min(self.lim.pump_work,len(self.done))
        for _ in range(n):
            rid,direction=self.done.popleft()
            assert rid in self.terminal and rid in self.acks
            self.per_dir[direction]+=1
            for k in ('pinned','result','control'): self.resources[k]-=1
            self.terminal.remove(rid); self.acks.remove(rid); self.completed+=1
        p=min(self.lim.pump_work-n,len(self.progress))
        for rid in list(self.progress)[:p]: del self.progress[rid]
        self.pump_events.append((t,n,p,len(self.done))); self.invariant(); return n+p
    def summary(self):
        return (f'admitted={self.admitted} refused={self.refused} completed={self.completed} '
                f'peak_terminal_backlog={self.peak_done} progress_pending={len(self.progress)} charges={self.resources}')

def delayed():
    m=Model(Limits(queue=3,inflight=1,pinned=3,result=3,control=3,pump_work=1))
    for i in range(6): m.admit(i,'read',10,20)
    assert (m.admitted,m.refused,len(m.q),m.resources['pinned'])==(3,3,3,3)
    m.invariant(); print('delayed backend:',m.summary())
def burst():
    m=Model(Limits(queue=6,inflight=6,pinned=6,result=6,control=6,pump_work=2))
    for i in range(6):
        assert m.admit(i,'read' if i%2==0 else 'write',0,1)
        for p in (1,2,3): m.progress_update(i,p)
    m.start_ready(0); m.backend(1)
    assert len(m.done)==6 and len(m.progress)==6
    for t in range(2,8): m.pump(t)
    assert m.completed==6 and m.peak_done==6
    assert all(a+b<=2 for _,a,b,_ in m.pump_events)
    assert m.resources==dict(queue=0,inflight=0,pinned=0,result=0,control=0)
    assert not m.progress and m.per_dir=={'read':3,'write':3}
    print('burst completions:',m.summary())
    print('  pump (terminal, progress, terminal backlog):',m.pump_events)
    print('  independent directions:',m.per_dir)
def release_ack():
    m=Model(Limits(queue=2,inflight=1,pinned=2,result=2,control=2,pump_work=1))
    assert m.admit(0,'read',0,1)
    m.start_ready(0); m.backend(1,ack_delay=2)
    assert len(m.terminal)==1 and len(m.acks)==0 and not m.done
    assert m.resources['pinned']==1 and m.resources['result']==1
    m.backend(3); assert len(m.done)==1
    m.pump(3); assert m.resources['pinned']==0
    print('delayed release acknowledgement: terminal held until ack; charges released only after pump')
def slow_pump():
    m=Model(Limits(queue=5,inflight=3,pinned=5,result=5,control=5,pump_work=1))
    for i in range(3):
        assert m.admit(i,'write',0,1)
        m.progress_update(i,1); m.progress_update(i,2)
    m.start_ready(0); m.backend(1,ack_delay=3)
    assert len(m.done)==0 and m.resources['pinned']==3
    m.backend(2); assert len(m.done)==0
    m.backend(4); assert len(m.done)==3
    assert len(m.done)==3 and m.resources['result']==3
    assert m.admit(3,'write',1,2)  # reservations remain available
    m.pump(2); m.invariant()
    print('pump cap/backpressure:',m.summary())
    print('  pump (terminal, progress, terminal backlog):',m.pump_events)
if __name__=='__main__':
    delayed(); burst(); release_ack(); slow_pump(); print('all invariant assertions passed')

```
