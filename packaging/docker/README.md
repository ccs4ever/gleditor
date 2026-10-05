# Local swarm test image

Build the initialized checkout, including uncommitted source changes:

```sh
make -j$(nproc) swarm-image
```

The host needs Git and a running Docker engine. Native development packages are installed inside the
image. `SWARM_IMAGE` changes the tag, `SWARM_IMAGE_BASE` can pin a Debian base digest, and
`CONTAINER_ENGINE=podman` uses a Docker-compatible alternative. The default base is Debian forky,
which supplies SDL3, RNP and the newer Poppler API used by the PDF importer; pin a digest for a
recorded run. The allowlist in `.dockerignore` excludes host builds, local user stores and
identity/configuration directories.

The image uses the standard distribution build toolchain, selected through the Makefile's compiler
probe, and treats Poppler's external headers as system headers. Project compiler warnings remain
enabled; the image does not force a compiler or language standard.

The image includes `xuzz` (and its compatibility aliases), `xudu-dump`, `xudu-swarm-peer`, engine
tests, GnuPG, Python, iproute2 and software OpenGL. Build-time smoke tests exercise publication, V5
history, complete-store inventory restoration, durable outbox and signed link-package primitives.
The default container command repeats those tests:

```sh
docker run --rm --network none gleditor-swarm-test:local
```

For multiple peers, create an internal Docker network and use one named container and private data
volume per user. Do not publish peer ports to the host. Bootstrap only other containers on this
network. Seeder content needs a writable private data directory: copy fixture files there before
starting `xudu-swarm-peer`, and pass its container IP as the listen address. Keep fixture inputs
read-only separately. Xuzz accepts `--test-publication-swarm HOST:PORT` with repeatable
`--dht-node HOST:PORT` for fixture provisioning. This enables the form's explicit mock verification
and Test swarm destination. The Publish and status/retry steps remain keyboard actions in Xuzz.
Topics are signed metadata; catalog ingestion and topic rendezvous are still pending.

```sh
docker network create --internal publication-test
docker volume create publication-alice
docker run --rm -it --name publication-alice --network publication-test \
  --mount source=publication-alice,target=/work \
  gleditor-swarm-test:local sh
```

Repeat with distinct names/volumes for Bob, Carl, Devin and bootstrap peers. Inside each container,
generate temporary keys with `GNUPGHOME=/work/gnupg`; create the directory with mode 700 first. Keep
that variable set when launching Xuzz. Never bake test keys into the image. The default XDG paths
are under `/work`, with headless video/audio and software rendering already set. Save captures and
logs there, use `--backend opengl`, and explicitly pass the container's `/work/store` path when
opening a profile. The image does not itself implement a four-user journey runner or mock Oracle;
missing product controls remain findings under the
[publication contract](../../design/ux_workflow_publication.md).

The existing two-peer namespace prerequisite can also run inside the image:

```sh
docker run --rm --network none --cap-add NET_ADMIN --cap-add SYS_ADMIN \
  gleditor-swarm-test:local sh tools/swarm-netns-test.sh
```

Those capabilities allow the script to create namespaces and mount their handles. Use separate
containers on the internal network for journeys that do not need nested namespaces. Containers share
the host kernel: the namespace test still requires host veth support. An image cannot fix
`Error: Unknown device type.`; the host administrator must make veth available. Docker daemon
availability and kernel support are separate from a passing publication test.
