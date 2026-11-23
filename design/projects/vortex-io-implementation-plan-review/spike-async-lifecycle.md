# Async I/O lifecycle and stream spike

Date: 2026-11-23

## Scope and boundary

Read the M1, M2, M4, and stream/framing clauses in `design/vortex-io-implementation-plan.md`. This
is an executable protocol-model/OS-semantics spike, not an implementation test: this checkout has no
Vortex I/O service, scheduler, adapters, or VM continuation implementation to exercise. It does not
claim any M1/M2/M4 gate has passed. It intentionally excludes M0 schema/identity and M8 budget
measurement.

The lifecycle model exercises the plan's linearization points: completion becomes visible only on
owner pump; waiter registration and terminal observation serialize at that boundary; waiter
cancellation does not cancel a request; a request is reclaimable only after both terminal candidate
and release acknowledgement; re-await resolves the same request without another admission. The model
is deterministic and single-process. It does not test concurrency memory ordering or native backend
cleanup.

## Result

Passed the following executable checks:

- Candidate queued before waiter registration, then owner pump: the waiter wakes (no lost wakeup).
- Waiter-only cancellation leaves its request pending; another waiter observes completion of that
  same request.
- Duplicate await/pump does not duplicate a wake; completed re-await returns Ready.
- Candidate without release acknowledgement does not publish terminal state or release the request;
  both signals together do.
- Resume/re-await keeps the same request identifier; request count stays unchanged.
- Real loopback UDP delivers a zero-byte datagram as a message. Real loopback TCP `recv()` after
  orderly write shutdown returns zero bytes (EOF). A length-prefixed zero-length frame remains
  explicitly representable.
- A writer constrained to three bytes per call completes a ten-byte output only by tracking
  short-write progress.
- A temporary file demonstrates that flush exposes the preceding write and a later flush exposes
  subsequent bytes. This is a userspace flush visibility check, not a durability (`fsync`) claim.

M4 source-generation conflicts and projection publication were not executable because the
adapter/lens/service implementation is absent. No localhost service protocol or application adapter
was available.

## Reproduce

Requires Python 3 and local loopback sockets. Save the following as `spike_async_lifecycle.py`, then
run `python3 spike_async_lifecycle.py`. It uses only the Python standard library and temporary
files; no repository source/build outputs are touched.

```python
import socket, queue, os, tempfile

class Service:
    def __init__(self):
        self.events = queue.Queue()
        self.reqs = {}
        self.waiters = {}
        self.wakes = []
        self.released = set()
    def admit(self, rid):
        assert rid not in self.reqs
        self.reqs[rid] = {'terminal': None, 'candidate': None, 'ack': False}
        return rid
    def candidate(self, rid, outcome):
        self.events.put(('candidate', rid, outcome))
    def ack(self, rid):
        self.events.put(('ack', rid))
    def pump(self):
        while not self.events.empty():
            kind, rid, *rest = self.events.get_nowait()
            r = self.reqs[rid]
            if kind == 'candidate':
                if r['candidate'] is None:
                    r['candidate'] = rest[0]
            else:
                r['ack'] = True
        for rid, r in self.reqs.items():
            if r['candidate'] is not None and r['ack'] and r['terminal'] is None:
                r['terminal'] = r['candidate']
                for key in sorted(self.waiters.get(rid, set())):
                    if key not in self.wakes:
                        self.wakes.append(key)
                self.released.add(rid)
    def await_(self, rid, cursor, continuation):
        if self.reqs[rid]['terminal'] is not None:
            return ('Ready', self.reqs[rid]['terminal'])
        key = (rid, cursor, continuation)
        self.waiters.setdefault(rid, set()).add(key)
        return ('Pending', key)
    def cancel_waiter(self, key):
        self.waiters[key[0]].discard(key)

s = Service()
r = s.admit('r1')
s.candidate(r, 'Complete')       # queued, not yet owner-published
key = s.await_(r, 'c1', 'k1')[1]
s.ack(r)
s.pump()
assert s.wakes == [key] and s.reqs[r]['terminal'] == 'Complete'
assert s.await_(r, 'c1', 'k1')[0] == 'Ready'
s.pump()
assert s.wakes == [key]           # idempotent duplicate observation

r2 = s.admit('r2')
w1 = s.await_(r2, 'c2', 'k2')[1]
s.cancel_waiter(w1)                # waiter cancellation only
w2 = s.await_(r2, 'c3', 'k3')[1]
s.candidate(r2, 'Complete')
s.ack(r2)
s.pump()
assert s.reqs[r2]['terminal'] == 'Complete'
assert s.wakes == [key, w2] and r2 in s.released

r3 = s.admit('r3')
s.candidate(r3, 'Cancelled')
s.pump()
assert s.reqs[r3]['terminal'] is None and r3 not in s.released
s.ack(r3)
s.pump()
assert s.reqs[r3]['terminal'] == 'Cancelled' and r3 in s.released
assert s.await_(r2, 'c3', 'k3') == ('Ready', 'Complete')
assert len(s.reqs) == 3              # no resubmit on re-await

# Empty UDP message and TCP EOF are distinct native observations.
a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
b.bind(('127.0.0.1', 0))
a.sendto(b'', b.getsockname())
data, _ = b.recvfrom(1)
assert data == b''
ls = socket.socket()
ls.bind(('127.0.0.1', 0))
ls.listen(1)
peer = socket.socket()
peer.connect(ls.getsockname())
conn, _ = ls.accept()
peer.shutdown(socket.SHUT_WR)
assert conn.recv(8) == b''         # stream EOF
frame = (0).to_bytes(4, 'big')
assert int.from_bytes(frame, 'big') == 0 and frame[4:] == b''

class ShortWriter:
    def __init__(self):
        self.out = bytearray()
    def write(self, data):
        n = min(3, len(data))
        self.out.extend(data[:n])
        return n
w = ShortWriter()
payload = b'abcdefghij'
accepted = 0
while accepted < len(payload):
    accepted += w.write(payload[accepted:])
assert accepted == len(payload) and bytes(w.out) == payload

with tempfile.TemporaryFile() as f:
    f.write(b'pre')
    f.flush()
    assert os.pread(f.fileno(), 3, 0) == b'pre'
    f.write(b'post')
    assert os.pread(f.fileno(), 7, 0) == b'pre'
    f.flush()
    assert os.pread(f.fileno(), 7, 0) == b'prepost'

print('PASS: owner lifecycle, waiter cancellation, duplicate wake, ack gating, same-request resume')
print('PASS: zero-byte UDP message distinct from TCP EOF; zero-length frame representation')
print('PASS: forced short-write progress and flush fence visibility')
for sock in (a, b, peer, conn, ls):
    sock.close()
```
