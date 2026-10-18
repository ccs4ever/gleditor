# Distributed language commands

`vpl`, `vquery`, `vprolog` and `vlace` share the same distributed worker and requester commands. The
executable selects the language of a submitted source file. Every worker can execute all four
languages and validated sealed Vortex instruction artifacts.

```sh
vpl --mesh-help
vpl --mesh-worker worker.bencode 3600
vpl --mesh-run requester.bencode AUTHOR:DEVICE:EPOCH program.vpl
vquery --mesh-run requester.bencode AUTHOR:DEVICE:EPOCH query.vql
vprolog --mesh-run requester.bencode AUTHOR:DEVICE:EPOCH rules.pl
vlace --mesh-run requester.bencode AUTHOR:DEVICE:EPOCH program.vlace
```

A worker prints `mesh-node AUTHOR:DEVICE:EPOCH` after reserving a durable incarnation. Copy that
whole identity into the requester command. Restarting the worker reserves a new epoch; obtain its
new identity before submitting more work. Worker duration is explicit, in seconds, up to one day.
Stop the process normally to shut down its network owner and finish bounded in-flight work.

A successful request prints `mesh-store PATH` and one `mesh-result INFOHASH` per verified result.
The store belongs to the requester and uses the configured requester permascroll. Failed or timed
out requests exit nonzero with a reason. Polling and network preparation run on the host's owner
thread; command-line waiting does not run on a graphical application's render thread.

## Deployment configuration

The configuration is a versioned bencode dictionary, limited to 64 KiB. File paths are relative to
its directory unless absolute. This is deployment metadata, outside every document store. Create the
deployment directory before starting: epoch reservation refuses a missing directory or an
interrupted journal instead of resetting its identity.

The following Python example writes the configuration. Its small encoder avoids a package
dependency; replace the values with existing author identities and verified device delegations.

```python
from pathlib import Path


def bencode(value):
    if isinstance(value, str):
        value = value.encode()
    if isinstance(value, bytes):
        return str(len(value)).encode() + b":" + value
    if isinstance(value, int):
        return b"i" + str(value).encode() + b"e"
    if isinstance(value, list):
        return b"l" + b"".join(map(bencode, value)) + b"e"
    return b"d" + b"".join(
        bencode(key) + bencode(value[key]) for key in sorted(value)
    ) + b"e"


config = {
    "version": 1,
    "author": "YOUR_FULL_OPENPGP_FINGERPRINT",
    "device": "YOUR_64_HEX_DEVICE_PUBLIC_KEY",
    "secret": "device.secret",
    "name": "Your Name",
    "email": "you@example.org",
    "gpg_home": "gnupg",
    "gpg_key": "YOUR_OPENPGP_SIGNING_KEY_FINGERPRINT",
    "delegations": [
        {"certificate": "local-device.tsv", "master": "local-master.asc"},
        {"certificate": "peer-device.tsv", "master": "peer-master.asc"},
    ],
    "allow_compute": ["YOUR_FULL_OPENPGP_FINGERPRINT", "TRUSTED_REQUESTER_FINGERPRINT"],
    "deployment": "deployment",
    "permascroll": "permascroll",
    "seeds": "seeds",
    "results": "results",
    "downloads": "downloads",
    "listen": "0.0.0.0:6881",
    "dht": 1,
    "peers": [{"address": "192.0.2.20", "port": 6881}],
    "timeout": 120,
    "budget": {"cycles": 1000000, "cells": 100000, "solutions": 100, "output": 1000000},
}
Path("deployment").mkdir(exist_ok=True)
Path("worker.bencode").write_bytes(bencode(config))
```

`device.secret` contains the existing device secret key as 128 hexadecimal digits, optionally
followed by one newline. Keep it private. Startup verifies that it matches the public key, that the
local device has a valid master-signed delegation, and that every supplied peer delegation verifies.
No test identity or unsigned fallback is installed. `gpg_home` is optional; omit it to use the
normal GnuPG keyring and agent. The author needs a signing key for result provenance. `gpg_key`
optionally selects its OpenPGP fingerprint; it defaults to `author`. The compute identity uses the
verified device delegation, while the signed provenance records the selected OpenPGP signer. These
are distinct identity checks; configuring two different keys does not assert they are the same key.

`allow_compute` is an explicit list of requester fingerprints. Admission also requires a current
verified device delegation, a capability signed by that requester's delegated key, a matching worker
incarnation, an unexpired budget and Hashcash. An empty list refuses computation. Include the
requester's own fingerprint in its requester configuration so it can authorize its submitted
capability. Untrusted discovery announcements grant no permission.

`peers` contains optional literal IP addresses and listener ports. Empty means use DHT discovery.
For a closed swarm, set `dht` to zero and configure reciprocal peer hints. All hosts join the
`vortex-compute` rendezvous automatically; hints are also introduced to newly requested program and
result torrents. They do not bypass authenticated peer or signed-content verification.

`timeout` is a request deadline in seconds, from 1 to 86400. The budget fields bound work, cells,
solutions and output bytes. Source size, parser structure, queue sizes, manifests and downloads have
additional engine limits. Distributed evaluation accepts immutable input and returns a sealed value;
effects and unrestricted native callbacks are refused.
