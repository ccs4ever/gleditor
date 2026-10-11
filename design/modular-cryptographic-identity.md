# Modular Cryptographic Identity & Publication System (Replacing GPG)

## 1. Executive Summary

This specification outlines the complete replacement of legacy GnuPG (`gpg` fork/exec CLI and
`librnp` C-FFI) with a **modular, in-process cryptographic identity and publication system**.

The concrete implementation leverages modern, industry-standard primitives:

- **Asymmetric Signatures**: Ed25519 (RFC 8032) via OpenSSL 3.0+ (already a core dependency in
  `XUDU_PKGS`), operating entirely in-process without spawning external shell processes. Strict
  canonical verification ($S < L$) and domain separation prefixes prevent signature malleability and
  cross-protocol replay.
- **Standard Certificates & Key Formats**: Standard **X.509 v3 certificates (RFC 5280 / RFC 8410 for
  Ed25519)** encoded as DER / PEM, PKCS#8 (`BEGIN PRIVATE KEY`), and SubjectPublicKeyInfo
  (`BEGIN PUBLIC KEY`).
- **Hierarchical Device Keys via Decoupled X.509 Delegation**: Master Root Identity (offline / cold
  storage CA) delegates operational signing authority to ephemeral workstation Device Keys. Signing
  uses decoupled PKCS#10 / RFC 8410 Certificate Signing Requests (CSR), enabling pure air-gapped
  device enrollment without bringing the master private key online.
- **Permascroll & BEP 46 Anchoring to Master Identity**: Permascroll global addressing
  (`btpk:<master_fingerprint>:permascroll`) and BEP 46 DHT mutable torrent roots anchor strictly to
  the **Master Identity Fingerprint**. Device key rotation does not fracture the author's scroll
  space or break citations across versions.
- **First-Class Revocation with RFC 5280 Semantics & Two-Tier Propagation**: Revocations distinguish
  between routine retirement (`Superseded`, `CessationOfOperation`)—which preserve historical
  citations signed prior to the revocation date—and active breaches (`KeyCompromise`). Propagation
  uses a Two-Tier system: Tier 1 real-time BEP 10 gossip (`DeviceRevocationBroadcast = 0x06`) into a
  Transient Quarantine Cache (TQC), and Tier 2 permanent consensus batching in `MerkleLedger`.
- **Verifiable Non-Revocation Architecture**: Local verification uses an $O(1)$ set lookup against
  the synced ledger. Remote / lightweight verification relies on state-authenticated dictionaries
  (Sorted Merkle Tree adjacent-leaf proofs or quorum-signed epoch accumulators), resolving the
  inherent absence-proof limitation of append-only trees.
- **Publication Manifest & Bootstrap Permascroll**: `AUTHORSHIP.tsv` remains the standalone
  publication manifest sealed into torrents alongside `primedia` and `ops`, carrying the author's
  globalized permascroll key required to bootstrap the `d.scrolls` and `d.source` ranks in the
  ZigZag store slice. Manifests use single-line Base64 DER for `device_cert` and raw 64-byte binary
  for `AUTHORSHIP.sig`, hardened against TSV injection. `store.tables` remains minimal (local
  on-disk metadata only).
- **Progressive Identity Disclosure ("Zero-Friction to Sovereign")**: New users launch `xuzz` with
  zero cryptographic ceremony (silent workstation key generation). Master identity setup occurs only
  upon first publication, accompanied by a 12-word BIP-39 recovery seed and transparent 1-click
  signing.

______________________________________________________________________

## 2. Architectural Principles & Boundaries

### Eliminating External Tooling (`gpg`) & C-FFI (`librnp`)

- Publishing previously shelled out to `gpg --detach-sign` via child process pipes, failing if `gpg`
  was not installed on the host system.
- Identity verification previously linked `librnp`.
- The new system executes all signing, verification, and certificate validation in-process using
  OpenSSL 3.0+ EVP and X.509 APIs, eliminating `gpg` process execution and enabling complete removal
  of `librnp` from the build toolchain and packaging scripts.

### Clean Break: No Legacy Formats or Backward Validation

- Per project design principles (pre-production system) and architectural rules, there is **no need
  to retain legacy formats or maintain backward compatibility**.
- Legacy 40-character OpenPGP fingerprints, ASCII-armored PGP signatures (`AUTHORSHIP.tsv.asc`), and
  OpenPGP key blocks are completely eliminated without backward shims or dual-path parsers.
- Fingerprints across [`identity_layout.hpp`](apps/common/xanadu/identity/identity_layout.hpp),
  [`merkle_ledger.hpp`](apps/common/xanadu/merkle_ledger.hpp), and
  [`user_permascroll.hpp`](apps/common/xanadu/user_permascroll.hpp) will exclusively be standard
  **64-hexadecimal (32-byte SHA-256)** digests of Ed25519 SubjectPublicKeyInfo.
- OpenSSL 3.0+ is confirmed as the singular cryptographic dependency; LibreSSL was evaluated and
  rejected due to package availability gaps in major distributions and symbol collisions with
  `libtorrent-rasterbar`.

### Naming & Scope Governance: `xuzz` for Application, `xanadu` for Layer

- Eliminate legacy `xudu` application references across touched code, configuration, and
  documentation in favor of **`xuzz`** for the application and **`xanadu`** for the engine layer as
  a whole.
- User & local machine paths: `$XDG_CONFIG_HOME/xuzz/config.tsv` (env `XUZZ_CONFIG`),
  `$XDG_DATA_HOME/xuzz/permascroll`, and `$XDG_DATA_HOME/xuzz/device.key`.
- Engine protocol & extension naming: Namespace `xanadu`, protocol prefixes
  `xanadu-device-delegation-v2\n` and `xanadu-peer-auth-v2:`, and extension names
  `xanadu_identity_lookup`, `xanadu_oracle_vote`, `xanadu_oracle_verify`, `xanadu_transcopyright`.

______________________________________________________________________

## 3. Authorship Storage Analysis: `store.tables` vs. `AUTHORSHIP.tsv`

### Context & Background

YAML dependencies were removed from the Xanadu engine (`apps/common/xanadu/`) to eliminate
dependency bloat and enforce native containers. `AUTHORSHIP.yaml` was replaced with
`AUTHORSHIP.tsv`. We evaluated whether storing authorship metadata as an internal table inside
`store.tables` (rendered via `store-dump`) is superior to retaining a dedicated publication manifest
file.

### Key Architectural Findings

1. **`store.tables` Minimal Invariant**: `store.tables` was intentionally reduced to minimal on-disk
   metadata:

   ```cpp
   struct StoreTables {
     DocumentId documentId;
     std::vector<ScrollSegment> localSegments;
   };
   ```

   All hypertime, links, microversions, annotations, and scrolls were moved out of `store.tables`
   into `ops.nodes` as first-class operations and cells. `store.tables` is strictly a local
   workspace cache file: it is **not packaged into publication torrents** (which carry only
   `primedia`, `ops`, and `AUTHORSHIP`).

1. **The Load-Bearing Role of `permascroll` in Bootstrapping the Store Slice**: Spans authored on a
   local machine address local scroll 0. Upon publication, the author's sovereign permascroll is
   globalized, and its global key is recorded in `AUTHORSHIP.tsv` as `permascroll`. When a recipient
   adopts a published xanadoc or constructs its slice (`ArenaManifold::projectProvenance`), this
   `permascroll` key is **required to bootstrap the `d.scrolls` and `d.source` ranks** in the store
   slice, linking the authorship root to the bootstrap cell and mapping local scroll 0 spans to the
   author's global permascroll. If authorship were placed inside `store.tables`, recipient nodes
   would not receive it unless `store.tables` was also transmitted in the torrent, violating the
   minimal local-only role of `store.tables`.

1. **Tooling & Debuggability (`store-dump`)**: `store-dump` (formerly `xudu-dump`) does not require
   `store.tables` to contain authorship to render it. `store-dump` operates directly on raw
   directory files without going through `Store::load()`. Adding `--section=authorship` to
   `store-dump` allows it to inspect `AUTHORSHIP.tsv` (and `AUTHORSHIP.sig`) directly, parse the TSV
   fields, verify the Ed25519 signature and X.509 device delegation certificate, and display a
   structured, human-readable report.

### Conclusion: Retain Dedicated `AUTHORSHIP.tsv` for Publication; Keep `store.tables` Minimal

- **Decision**: Keep `store.tables` minimal (Format Version 3, containing only `documentId` and
  `localSegments`). Do not burden it with publication state.
- **Maintain `AUTHORSHIP.tsv` + `AUTHORSHIP.sig` in Publication Torrents**:
  - Update `AUTHORSHIP.tsv` fields to standard 64-hex SHA-256 fingerprints, Ed25519 device public
    keys, and standard X.509 v3 device delegation certificates (single-line Base64 DER).
  - Retain the critical `permascroll` field to bootstrap `d.scrolls` and `d.source` in the ZigZag
    slice.
  - Sign the canonical TSV in-process with the device's Ed25519 key, writing raw 64-byte signature
    to `AUTHORSHIP.sig` (eliminating `AUTHORSHIP.tsv.asc`).
- **Rename `tools/xudu-dump.cpp` to `tools/store-dump.cpp`**: Clean break with no compatibility
  symlink. Add `--section=authorship` to parse and cryptographically verify `AUTHORSHIP.tsv`
  directly from a store directory or publication archive, updating Makefile with target
  `store-dump`.

______________________________________________________________________

## 4. Architecture of the Modular System

```
                  ┌────────────────────────────────────────────────────────┐
                  │             Modular Identity Abstractions              │
                  │   ISigner, IVerifier, ICertificateEngine, IRevocation  │
                  └───────────────────────────┬────────────────────────────┘
                                              │
                      ┌───────────────────────┴───────────────────────┐
                      ▼                                               ▼
         ┌─────────────────────────┐                     ┌─────────────────────────┐
         │   StandardCryptoEngine  │                     │       MerkleLedger      │
         │   (OpenSSL 3.0+ EVP)    │                     │   (microsoft/merklecpp) │
         │   - Ed25519 RFC 8032    │                     │   - RFC 5280 Revocation │
         │   - X.509 v3 RFC 8410   │                     │   - Fast-Path Set O(1)  │
         │   - PKCS#10 / PKCS#8    │                     │   - Sorted Merkle / SMT │
         └────────────┬────────────┘                     └────────────▲────────────┘
                      │                                               │
                      │ Issues & Signs                                │ Anchors Revocations
                      ▼                                               │
         ┌─────────────────────────┐                                  │
         │  Master Root Key (CA)   ├──────────────────────────────────┘
         │  [Cold Storage Identity]│
         └────────────┬────────────┘
                      │ Authorizes via PKCS#10 CSR -> X.509 Cert
                      ▼
         ┌─────────────────────────┐
         │  Operational Device Key │
         │  [Local Workstation]    │
         └────────────┬────────────┘
                      │ Signs Manifest (Domain Separated)
                      ▼
         ┌─────────────────────────┐
         │     AUTHORSHIP.tsv      │
         │  + AUTHORSHIP.sig       │
         └─────────────────────────┘
```

### 4.1 Modular Interfaces

The cryptographic architecture defines clear abstract interfaces separating key management, signing,
verification, certificate authority operations, and revocation querying. All operations return
`std::expected<T, ValidationError>` for explicit error handling without exceptions.

#### `ISigner` and `IVerifier`

```cpp
namespace xanadu::identity {

class ISigner {
public:
  virtual ~ISigner() = default;
  [[nodiscard]] virtual std::string algorithmName() const noexcept = 0;
  [[nodiscard]] virtual PubKey32 publicKey() const noexcept = 0;
  [[nodiscard]] virtual std::expected<Signature64, ValidationError> sign(
      std::span<const std::uint8_t> message) const noexcept = 0;
};

class IVerifier {
public:
  virtual ~IVerifier() = default;
  [[nodiscard]] virtual std::string algorithmName() const noexcept = 0;
  [[nodiscard]] virtual std::expected<void, ValidationError> verify(
      const PubKey32 &pubKey,
      std::span<const std::uint8_t> message,
      const Signature64 &signature) const noexcept = 0;
};

} // namespace xanadu::identity
```

#### `ICertificateEngine`

Supports both direct issuance and decoupled PKCS#10 / RFC 8410 CSR workflows for cold-storage
air-gapped workstations:

```cpp
namespace xanadu::identity {

struct CertificateConstraints {
  std::uint64_t serialNumber{1};
  std::uint64_t notBefore{0};
  std::uint64_t notAfter{0};
  std::string deviceId;
  std::string deviceName;
  std::string authorName;
  std::string authorEmail;
};

class ICertificateEngine {
public:
  virtual ~ICertificateEngine() = default;

  /// Generate a PKCS#10 CSR on a workstation using the local device key.
  [[nodiscard]] virtual std::expected<std::vector<std::uint8_t>, ValidationError>
  generateCsr(const ISigner &deviceSigner,
              const std::string &deviceId,
              const std::string &deviceName) const noexcept = 0;

  /// Authorize a CSR using the Master Key (can run offline / air-gapped).
  [[nodiscard]] virtual std::expected<X509Certificate, ValidationError>
  authorizeCsr(const ISigner &masterSigner,
               std::span<const std::uint8_t> csrDer,
               const CertificateConstraints &constraints) const noexcept = 0;

  /// Full X.509 path validation using OpenSSL X509_STORE_CTX, validating
  /// signatures, temporal validity, BasicConstraints (cA=FALSE), and KeyUsage.
  [[nodiscard]] virtual std::expected<void, ValidationError>
  verifyDelegation(const X509Certificate &cert,
                   const PubKey32 &expectedMasterKey,
                   std::uint64_t currentTime) const noexcept = 0;
};

} // namespace xanadu::identity
```

#### `IRevocationRegistry`

```cpp
namespace xanadu::identity {

class IRevocationRegistry {
public:
  virtual ~IRevocationRegistry() = default;

  [[nodiscard]] virtual bool isDeviceRevoked(
      const PubKey32 &deviceKey,
      std::uint64_t timestamp) const noexcept = 0;

  [[nodiscard]] virtual bool isSerialRevoked(
      std::uint64_t serialNumber,
      std::uint64_t timestamp) const noexcept = 0;

  [[nodiscard]] virtual bool isIdentityRevoked(
      const Fingerprint &masterFingerprint) const noexcept = 0;
};

} // namespace xanadu::identity
```

### 4.2 Cryptographic Rigor & Defense in Depth

1. **Strict Canonical Ed25519 Verification**: In compliance with RFC 8032 Section 5.1.7, all Ed25519
   signatures must enforce canonical scalar reduction ($S < L$). Malleable signatures with $S \ge L$
   are rejected to prevent torrent InfoHash mutation split swarms.
1. **Mandatory Domain Separation**: To prevent cross-protocol signature replay, all signing buffers
   prepend a distinct UTF-8 domain separation header:
   - Manifest signing: `"xanadu-authorship-manifest-v2\n"`
   - Device CSR: `"xanadu-device-csr-v2\n"`
   - Delegation certificates: Standard X.509 TBSCertificate ASN.1 structure
   - Device revocation: `"xanadu-device-revocation-v2\n"`
   - Peer authentication / handshake: `"xanadu-peer-auth-v2\n"`
1. **Robust X.509 Path Validation**: Rather than calling bare `X509_verify()` (which only checks raw
   cryptographic signatures and ignores extensions), `StandardCryptoEngine` utilizes OpenSSL's full
   path validation harness:
   - Construct an `X509_STORE` seeded with the Master Public Key as the trusted trust anchor.
   - Initialize `X509_STORE_CTX` with the device certificate and evaluate via `X509_verify_cert()`.
   - Explicitly verify `X509_check_ca(cert) == 0` (ensuring the workstation key cannot act as an
     intermediate CA to issue further subordinate certificates).
   - Validate `KeyUsage` contains `KU_DIGITAL_SIGNATURE` and `KU_NON_REPUDIATION`.
   - Strict DER parsing with zero trailing bytes (`len == d2i_len`).

______________________________________________________________________

## 5. Key Hierarchy, Lifecycle & Built-In Revocation

### 5.1 Master Identity vs. Device Keys via X.509 v3

1. **Master Identity Key (`MasterKey`)**:

   - Ed25519 private key generated once by the author.
   - Acts as the Root Certificate Authority (CA) for all author devices.
   - Anchored by a 12-word BIP-39 mnemonic phrase for disaster recovery.
   - Kept in secure local storage (`~/.config/xuzz/master_identity.pem`, passphrase-encrypted via
     PKCS#8 PBKDF2/scrypt).
   - Author's permanent fingerprint is $H_{\text{SHA-256}}(\text{SPKI}(\text{MasterPubKey}))$.
   - **Never signs documents directly.** Only issues X.509 device delegation certificates and
     revocation entries.

1. **Device Operational Key (`DeviceKey`)**:

   - Ed25519 keypair generated automatically on each workstation/device
     (`~/.local/share/xuzz/device.key`, file mode `0600`).
   - Sits in memory during editing sessions; signs `UserPermascroll` segments, live collaborative
     ops, and publication manifests (`AUTHORSHIP.tsv`).

1. **Permascroll & BEP 46 Addressing Anchored to Master Identity**:

   - The global permascroll key and BEP 46 mutable torrent infohashes are anchored strictly to the
     Master Identity Fingerprint (`btpk:<master_fingerprint>:permascroll`).
   - Rotating or replacing a device key does not modify the author's root scroll address or fracture
     citations in existing hyperdocuments. Device keys provide cryptographic authorization; the
     Master Identity owns the address space.

1. **Cold-Storage Air-Gap & Decoupled CSR Workflow**: To support cold-storage security without
   exposing the master key to network-connected devices, device authorization supports a clean
   3-step offline workflow:

   - **Step 1 (Workstation)**: Generate local device key and emit CSR via
     `xuzz identity request-device --name "Laptop" --out device.csr`.
   - **Step 2 (Cold / Air-Gapped Master)**: Transfer `device.csr` via USB or QR code, verify, and
     sign via
     `xuzz identity authorize-device --csr device.csr --validity-days 365 --out device.crt`.
   - **Step 3 (Workstation)**: Import the signed certificate via
     `xuzz identity install-device --cert device.crt`.

1. **Master Disaster Recovery & Key Rotation**:

   - If a master key is lost, the author restores the root identity from their 12-word BIP-39 seed.

   - If a master key is compromised or superseded, a signed `IdentitySupersededRecord` is committed
     to the ledger:

     ```cpp
     struct IdentitySupersededRecord {
       Fingerprint oldMasterFingerprint;
       Fingerprint newMasterFingerprint;
       std::uint64_t supersessionDate{0};
       Signature64 oldMasterSignature;
       Signature64 newMasterSignature;
     };
     ```

     Both old and new keys sign the transition, maintaining continuous provenance chains across
     master identity rotations.

### 5.2 Built-In Revocation Semantics & Non-Revocation Architecture

Rather than relying on brittle, centralized HTTP CRL distribution points, revocation records with
standard RFC 5280 semantics are committed directly into Xanadu's decentralized architecture.

1. **Standard RFC 5280 Reason Codes**:

   ```cpp
   enum class RevocationReason : std::uint8_t {
     Unspecified          = 0,
     KeyCompromise        = 1, // Device stolen or private key leaked
     CACompromise         = 2, // Master identity compromise
     Superseded           = 4, // Device decommissioned or replaced
     CessationOfOperation = 5  // Temporary/testing device retired
   };
   ```

1. **Temporal Revocation Semantics (Preserving Citation Permanence)**: A naive revocation check that
   unconditionally rejects any document signed by a revoked key creates a catastrophic "scorched
   earth" failure, retroactively invalidating all historical documents published by an author over
   years.

   - **Routine Retirement (`Superseded`, `CessationOfOperation`)**: Documents signed and published
     *prior* to `revocationDate` remain **100% valid**. Only signatures created after
     `revocationDate` are rejected.
   - **Security Breach (`KeyCompromise`, `CACompromise`)**: All documents signed by the compromised
     key are treated as untrusted, unless verifiable inclusion in a published Merkle block or
     external timestamp proves publication preceded the compromise timestamp.

1. **Revocation Records (`DeviceRevocationRecord`)**:

   ```cpp
   struct DeviceRevocationRecord {
     std::uint64_t serialNumber{0};
     PubKey32 devicePublicKey;
     Fingerprint masterFingerprint;
     std::uint64_t revocationDate{0};
     RevocationReason reason{RevocationReason::KeyCompromise};
     Signature64 masterSignature;
   };
   ```

   Revocation records use deterministic, length-prefixed canonical serialization:

   ```
   xanadu-device-revocation-v2\n
   serial:8:<binary_u64_le>\n
   device_key:32:<binary_pubkey>\n
   master_fp:32:<binary_fp>\n
   date:8:<binary_u64_le>\n
   reason:1:<binary_u8>\n
   ```

1. **Two-Tier Revocation Gossip**:

   - **Tier 1 (Fast-Path Gossip)**: When a device is revoked, the author node broadcasts an
     immediate BEP 10 extension message (`DeviceRevocationBroadcast = 0x06`) across active swarm
     connections. Connected peers ingest the record into a local in-memory **Transient Quarantine
     Cache (TQC)**, terminating active edit sessions from the compromised device within
     milliseconds.
   - **Tier 2 (Consensus Ledger Commitment)**: Revocation records are batched into the next Merkle
     block committed to `MerkleLedger` by the network Oracle quorum, providing permanent, immutable
     ordering.

1. **Verifiable Non-Revocation Proof Architecture**: An append-only Merkle tree (`merklecpp`) can
   prove *inclusion* of a revocation record in $O(\log N)$, but cannot prove *absence*
   (non-revocation) from tree membership alone.

   - **Local Nodes**: Maintain an in-memory indexed hash table of all revoked keys and serial
     numbers derived from the ledger (`revokedDeviceKeys_`, `revokedSerials_`,
     `revokedIdentities_`). Local verification of non-revocation is an $O(1)$ set lookup against the
     synced local ledger.
   - **Remote / Lightweight Verifiers**: Non-revocation is proven cryptographically via **Sorted
     Merkle Trees (SMT)** or **Adjacent-Leaf Inclusions**: all active revocations are sorted
     lexicographically by `(masterFingerprint, serialNumber)`. An absence proof for serial $S$
     consists of Merkle inclusion proofs for two adjacent leaves $L_i$ and $L_{i+1}$ demonstrating
     that $L_i < S < L_{i+1}$, proving mathematically that $S$ is not revoked in that block epoch.

### 5.3 User Onboarding & Progressive Identity Disclosure

To ensure widespread adoption, the onboarding experience adheres to a **"Zero-Friction to Sovereign"
ladder**, eliminating technical barriers while guaranteeing cryptographic ownership:

1. **Tier 0 (First Software Launch)**:
   - When a user opens `xuzz` for the first time, `device.key` is silently generated in the
     background (`~/.local/share/xuzz/device.key`) with strict `0600` POSIX permissions.
   - Zero dialogs, no passphrase prompts, and no cryptographic queries interrupt the user. The
     editor opens immediately in an ephemeral local workspace.
1. **Tier 1 (First Publication / Sovereign Author)**:
   - When the user selects **File -> Publish Document**:
   - An intuitive inline modal appears requesting an Author Name and Passphrase (optional contact
     email).
   - In the background, `StandardCryptoEngine` generates the Master Identity Key, exports a 12-word
     BIP-39 mnemonic recovery card with a "Copy / Print" prompt, and issues a 1-year X.509 device
     delegation certificate for the current machine.
   - Publishing executes seamlessly with 1-click signing.
1. **Tier 2 (Verified Public Author via Decentralized Oracles)**:
   - Authors seeking public verification submit their author handle to the decentralized Oracle
     quorum.
   - Verification eliminates legacy PGP-encrypted emails in favor of an automated verification code
     (`XUZZ-482-910`) entered directly into the `xuzz` interface or confirmed via standard OAuth /
     DKIM loopback.
1. **Self-Healing Certificate Expiry**:
   - If a workstation's 1-year delegation certificate expires, `xuzz` detects the condition during
     publication and prompts: *"Your workstation authorization has expired. Enter your Master
     Passphrase to renew."*
   - Re-authorization succeeds instantly without manual certificate manipulation.
1. **3-State Trust Badge Component in Xuzz**: The UI renders a non-intrusive status badge reflecting
   document provenance:
   - **Green Badge (Verified Author)**: Master key active, device certificate valid, attested by
     network Oracle quorum.
   - **Blue Badge (Self-Signed Sovereign)**: Valid master key and device certificate, active and
     unrevoked, independent of Oracles.
   - **Red Badge (Untrusted / Revoked)**: Expired delegation, signature mismatch, or revoked device
     key.
1. **No Manual Tab-Editing in `config.tsv`**: All identity preferences and network settings are
   accessible via `xuzz config set <key> <val>` or the graphical Settings overlay, eliminating
   errors from manual editing of tab-separated files.

### 5.4 Hardened Manifest Serialization & TSV Injection Defense

`AUTHORSHIP.tsv` is secured against parser desynchronization and field injection attacks:

1. **Strict Field Name Validation**: Field keys must match the strict regex `^[a-z_][a-z0-9_]*$`.
   Any line with unexpected characters is rejected.
1. **Control Character Prohibition**: Field values containing unescaped tabs (`\t`), newlines
   (`\n`), or carriage returns (`\r`) trigger immediate parse failure.
1. **No Duplicate Keys**: If any field appears more than once, `parseProvenance()` aborts with
   `ProvenanceParseError::DuplicateKey`. Overwriting fields via crafted duplicate entries is
   strictly prevented.
1. **Reserved Field Isolation**: The `extra` dictionary cannot contain reserved keys (`author`,
   `email`, `master_fingerprint`, `device_key`, `device_cert`, `permascroll`, `published`).
1. **Exact Serialization Standards**:
   - `device_cert`: Encoded strictly as a **single-line Base64 DER string**.
   - `AUTHORSHIP.sig`: Encoded strictly as **raw 64-byte binary**.

### 5.5 Engine Pipeline Verification & Oracle Sybil Defense

1. **Cryptographic Validation in Consensus Engine**:
   - In `identity_validation.cpp`, `verifyOracleAttestation` checks the cryptographic signature
     `att.oracleSignature` over `"xanadu-oracle-attestation-v1\n"` using the oracle's public key.
   - In `stageBlock`, all candidate `IdentityEntry` and `VoteEntry` records undergo explicit
     signature verification prior to block admission.
1. **Oracle Quorum & Sybil Resistance**:
   - Attestation of identity claims requires an $M$-of-$N$ threshold (e.g., at least 3 of 5 active
     designated oracles). Single-oracle compromises cannot forge network identity attestations.

### 5.6 Normative Specification: Cross-Device Authoring & Ephemeral Session Governance

This section establishes the normative security requirements for authoring, signing, and publishing
across heterogeneous physical environments, spanning personal hardware, shared terminals, and
untrusted public kiosks.

#### 5.6.1 Hardware Trust Classification

Implementations MUST classify host environments into one of three standardized trust tiers:

| Trust Tier                      | Environment & Threat Profile                                                                           | Key Storage Mechanism                       | Certificate Lifetime Window              | Compromise Containment                                                         |
| :------------------------------ | :----------------------------------------------------------------------------------------------------- | :------------------------------------------ | :--------------------------------------- | :----------------------------------------------------------------------------- |
| **Class 1: Personal Device**    | **Trusted hardware, single user.** Full local disk control, OS biometric keychain.                     | OS Keyring / Secure Enclave (`0600`)        | $\le 365\text{ days}$ (auto-renewing)    | Negligible; workstation key only; revokable via phone.                         |
| **Class 2: Shared Workstation** | **Semi-trusted hardware.** Office/lab with colleagues, shared OS or multi-user accounts.               | Encrypted local user profile                | $\le 30\text{ days}$ (default 7 days)    | Restricted to workstation scope; auto-expires.                                 |
| **Class 3: Public Computer**    | **Zero-trust / Hostile endpoint.** Library kiosk, cybercafe. RAM scrapers, keyloggers, disk forensics. | **RAM-only** (`/dev/shm`, zero disk writes) | $\le 2\text{ hours}$ (ephemeral session) | **Zero.** Key expires in minutes; RAM wiped on exit; Master Key never exposed. |

#### 5.6.2 Normative Security Invariants

1. **Invariant 1: Cold Master Key Isolation (Mandatory)**: The Master Identity private key
   (`MasterKey`) MUST NOT be imported, decrypted, or entered on any Class 2 (Shared Workstation) or
   Class 3 (Public Computer) machine under any circumstance. Authorizing a non-personal machine MUST
   occur exclusively via out-of-band delegation from an external trusted device.

1. **Invariant 2: Zero-Footprint Volatile Execution on Hostile Endpoints**: When operating on a
   Class 3 machine, the application MUST run in Guest/Kiosk mode:

   - All cryptographic keypairs, temporary span buffers, and application state MUST reside strictly
     in volatile memory (`/dev/shm` or RAM-backed heaps).
   - The application MUST NOT persist private keys, active primedia segments, or transaction logs to
     local non-volatile storage (disks, SSDs, swap partitions).

1. **Invariant 3: Cryptographic Memory Sanitization**: Upon explicit session logout, user window
   closure, or 15 minutes of idle inactivity on a Class 3 machine, the application MUST overwrite
   all memory pages holding ephemeral Ed25519 private keys and decrypted buffers using
   `OPENSSL_cleanse` before deallocating memory.

1. **Invariant 4: Permascroll Continuity via Master Identity Anchoring**: All documents authored
   across any device class MUST anchor their root permascroll global address to the Master Identity
   Fingerprint: `btpk:<master_fingerprint>:permascroll`. Primedia bytes typed during an ephemeral
   session MUST use session-salted subscroll descriptors (`permascroll/ephemeral-<session_id>`).
   Upon publication, recipient nodes transclude spans without coordinate collision, and the author's
   primary node adopts them via swarm gossip.

1. **Invariant 5: Real-Time Quarantine via Two-Tier Gossip**: Upon termination of a Class 3 session,
   or when an author issues a remote revocation from their mobile device, the author node MUST emit
   an immediate Tier 1 BEP 10 gossip packet (`DeviceRevocationBroadcast = 0x06`). Connected swarm
   peers MUST ingest this record into their in-memory Transient Quarantine Cache (TQC), immediately
   refusing subsequent incoming operations from that ephemeral key within milliseconds, followed by
   Tier 2 block commitment in `MerkleLedger`.

1. **Invariant 6: Citation Permanence via Temporal Verification**: Because ephemeral session
   certificates carry standard X.509 `[notBefore, notAfter]` validity windows, verifiers MUST treat
   documents published during the active window as valid, even after the ephemeral session concludes
   and the key is decommissioned with reason `CessationOfOperation`. Historical citations in
   published works remain immutable and permanent.

#### 5.6.3 Out-of-Band Pairing Workflow (The "QR-to-Micro-Cert" Pattern)

To enable frictionless authoring without compromising credentials, Class 2 and Class 3 devices MUST
be enrolled via an out-of-band challenge-response ceremony:

```
┌─────────────────────────────────┐                 ┌─────────────────────────────────┐
│   Public / Shared Terminal      │                 │    Author Mobile Device         │
│   (Hostile / Guest Mode)        │                 │    (Secure Enclave / Master CA) │
└────────────────┬────────────────┘                 └────────────────┬────────────────┘
                 │                                                   │
                 │ 1. Generates Ephemeral Keypair (RAM-only)         │
                 │ 2. Displays Animated Pairing QR Code              │
                 │    (Encapsulates PKCS#10 CSR + Nonce)             │
                 │                                                   │
                 │ ────────────────────────────────────────────────> │
                 │              Scans QR via Camera                  │
                 │                                                   │
                 │                                                   │ 3. Displays prompt:
                 │                                                   │    "Authorize Public Terminal
                 │                                                   │     for 2 Hours?"
                 │                                                   │ 4. Signs X.509 v3 Micro-Cert
                 │                                                   │    (validity = 2h, cA=FALSE)
                 │ <──────────────────────────────────────────────── │
                 │          Transmits Micro-Cert via BLE/LAN         │
                 │                                                   │
                 │ 5. Installs Micro-Cert into RAM                   │
                 │ 6. Signs Operations as Authorized Delegate        │
                 │                                                   │
                 │ [Publish Document]                                │
                 │ Emits AUTHORSHIP.tsv with Micro-Cert              │
                 │ Anchored to btpk:<master_fp>:permascroll          │
                 │                                                   │
                 │ [Session Logout / Window Close]                   │
                 │ 7. OPENSSL_cleanse memory                         │
                 │ 8. Broadcasts BEP 10 Gossip Revocation (0x06)     │
                 ▼                                                   ▼
```

______________________________________________________________________

## 6. Target Codebase Changes

Grouped by component layer:

### Component 1: Core Cryptographic Interfaces & OpenSSL 3 Engine

#### `apps/common/xanadu/identity/identity_engine.hpp` [NEW]

- Defines abstract interfaces: `ISigner`, `IVerifier`, `ICertificateEngine`, `IRevocationRegistry`
  returning `std::expected<T, ValidationError>`.
- Defines `X509Certificate` wrapper, `DeviceRevocationRecord`, and `IdentitySupersededRecord`.
- Declares domain separation constants.

#### `apps/common/xanadu/identity/standard_crypto_engine.hpp` [NEW]

#### `apps/common/xanadu/identity/standard_crypto_engine.cpp` [NEW]

- Implements `StandardCryptoEngine` using OpenSSL 3.0+ (`EVP_PKEY`, `EVP_PKEY_ED25519`,
  `EVP_DigestSign`, `EVP_DigestVerify`).
- In-process key generation (`generateEd25519KeyPair`), BIP-39 mnemonic generation/recovery, and
  PKCS#8 encrypted serialization.
- PKCS#10 CSR generation and air-gapped authorization.
- Full X.509 v3 path validation using `X509_STORE`, `X509_STORE_CTX`, `X509_verify_cert`, and
  `X509_check_ca`.
- PEM / Base64 DER certificate conversions.

#### `apps/common/xanadu/identity/identity_layout.hpp` [MODIFY]

- Upgrade `Fingerprint` to 32 bytes / 64 hex characters (SHA-256 of public key SPKI).
- Add support for standard `X509Certificate`, `DeviceRevocationRecord`, and
  `IdentitySupersededRecord`.
- Expand `ValidationError` enum with `KeyRevoked`, `CertificateExpired`, `CertificateNotYetValid`,
  `DelegationSignatureInvalid`, `CsrSignatureInvalid`, `DuplicateKey`, and `PathValidationFailed`.

### Component 2: In-Process Provenance, Permascroll Bootstrapping & `store-dump`

#### `apps/common/xanadu/provenance.hpp` [MODIFY]

#### `apps/common/xanadu/provenance.cpp` [MODIFY]

- Remove `gpgAvailable()`, child process execution, CLI pipes, and legacy `.asc` signature handling.
- Update `Author` to hold `masterFingerprint` (64-hex SHA-256), `deviceKey` (64-hex Ed25519), and
  `deviceCert` (single-line Base64 DER X.509 v3).
- Implement TSV hardening: strict field validation, reject duplicates, reject `\r`.
- Sign canonical TSV with domain separation prefix `"xanadu-authorship-manifest-v2\n"`, emitting raw
  64-byte binary to `AUTHORSHIP.sig`.
- Retain load-bearing `permascroll` field to bootstrap `d.scrolls` and `d.source` in the ZigZag
  slice.

#### `apps/common/xanadu/publication.hpp` [MODIFY]

#### `apps/common/xanadu/publication.cpp` [MODIFY]

- Package `primedia`, `ops`, `AUTHORSHIP.tsv`, and `AUTHORSHIP.sig` into publication torrent.
- Anchor publication BEP 46 mutable torrent targets and global permascroll to Master Identity
  Fingerprint.

#### `tools/store-dump.cpp` [RENAME/MODIFY] (formerly `tools/xudu-dump.cpp`)

- Rename `tools/xudu-dump.cpp` to `tools/store-dump.cpp` with a clean break.
- Add `--section=authorship`: parses `AUTHORSHIP.tsv` and `AUTHORSHIP.sig`, executes in-process
  Ed25519 and X.509 verification, and displays author metadata and trust status.
- Update `Makefile` target to `store-dump: $(OBJDIR)/store-dump`.

#### `apps/common/xanadu/zigzag/arena_manifold.cpp` [MODIFY]

- Update `projectProvenance`: projects the upgraded `AUTHORSHIP.tsv` fields into the `d.authorship`
  rank and uses `store.bootstrapPermascrollKey()` to link `d.source` to the `d.scrolls` rank.

### Component 3: Merkle Ledger Revocation & Consensus Validation

#### `apps/common/xanadu/merkle_ledger.hpp` [MODIFY]

#### `apps/common/xanadu/merkle_ledger.cpp` [MODIFY]

- Add `DeviceRevocationRecord` and `IdentitySupersededRecord` ledger entry types.
- Maintain indexed lookup tables (`revokedDeviceKeys_`, `revokedSerials_`, `revokedIdentities_`).
- Implement temporal revocation logic: preserve publications signed prior to revocation date for
  `Superseded` / `CessationOfOperation`.
- Support adjacent-leaf proofs for cryptographic non-revocation verification.

#### `apps/common/xanadu/identity/identity_validation.cpp` [MODIFY]

- Add missing cryptographic signature verification in `verifyOracleAttestation`
  (`att.oracleSignature`).
- Add signature validation in `stageBlock` for candidate `IdentityEntry` and `VoteEntry` records.

### Component 4: Permascroll & Network Controller Integration

#### `apps/common/xanadu/user_permascroll.hpp` [MODIFY]

#### `apps/common/xanadu/user_permascroll.cpp` [MODIFY]

- Migrate `DeviceDelegation` to use `StandardCryptoEngine` and `X509Certificate`.
- Remove `#include "identity/pgp_verify.hpp"`.
- Migrate storage path from `~/.local/share/xudu/permascroll` to `~/.local/share/xuzz/permascroll`
  (`$XDG_DATA_HOME/xuzz/permascroll`).
- Update delegation wire prefix to `xanadu-device-delegation-v2\n`.

#### `apps/common/xanadu/config.hpp` [MODIFY]

#### `apps/common/xanadu/config.cpp` [MODIFY]

- Update config path to `$XDG_CONFIG_HOME/xuzz/config.tsv` (env `XUZZ_CONFIG`).
- Add programmatic configuration helper methods to eliminate manual tab-delimited file edits.

#### `apps/common/xanadu/identity/identity_network_controller.hpp` [MODIFY]

#### `apps/common/xanadu/identity/identity_network_controller.cpp` [MODIFY]

- Implement Tier 1 Fast-Path BEP 10 gossip (`DeviceRevocationBroadcast = 0x06`).
- Maintain local `TransientQuarantineCache` (TQC) to reject revoked device ops in real time.
- Update protocol prefixes (`xanadu-peer-auth-v2:`) and extension names.

### Component 5: Deprecation and Build Clean-up

#### `apps/common/xanadu/identity/pgp_verify.hpp` [DELETE]

#### `apps/common/xanadu/identity/pgp_verify.cpp` [DELETE]

#### `Makefile` [MODIFY]

- Remove `librnp` from `XUDU_PKGS` and `ZIGZAG_PKGS`.
- Remove `pkg-config --exists librnp` checks.
- Update targets from `xudu-dump` to `store-dump`.

#### Packaging manifests [MODIFY]

- Remove `librnp-dev` / `librnp` from `debian/control`, `packaging/fedora/gleditor.spec`, and
  `PKGBUILD`.

______________________________________________________________________

## 7. Verification Plan

### 7.1 Automated Tests

Run the full headless suite with maximum parallelism:

```bash
make -j$(nproc) test
```

Specific test suites to run and expand:

#### 1. Cryptographic Engine & Key Tests

```bash
./build/xudu_test --gtest_filter='StandardCryptoEngineTest.*'
```

- Verify Ed25519 keypair generation, BIP-39 seed round-trip, and PKCS#8 encrypted serialization.
- Verify canonical $S < L$ enforcement and rejection of non-canonical signatures.
- Verify corrupt signature rejection and wrong-key rejection.

#### 2. Standard X.509 v3 Delegation Certificate & CSR Tests

```bash
./build/xudu_test --gtest_filter='X509CertificateTest.*'
```

- Verify PKCS#10 CSR generation and offline authorization workflow.
- Verify full path validation with `X509_STORE_CTX`, verifying `cA=FALSE` and `KeyUsage`.
- Verify round-trip conversion to standard PEM and single-line Base64-DER.
- Verify rejection of expired certificates (`currentTime > notAfter`).
- Verify rejection of prematurely used certificates (`currentTime < notBefore`).
- Verify external OpenSSL CLI compatibility (`openssl x509 -text -noout`).

#### 3. Merkle Ledger Revocation & Temporal Verification Tests

```bash
./build/xudu_test --gtest_filter='MerkleLedgerRevocationTest.*'
```

- Verify appending `DeviceRevocationRecord` with RFC 5280 reason codes.
- Verify temporal validity: publications dated prior to `revocationDate` for `Superseded` remain
  valid; publications dated after are rejected.
- Verify fast-path TQC gossip ingestion and instant quarantine.
- Verify adjacent-leaf proof generation for non-revocation verification.

#### 4. Manifest Hardening & TSV Injection Defense Tests

```bash
./build/xudu_test --gtest_filter='ProvenanceHardeningTest.*'
```

- Test rejection of field names with invalid characters, spaces, or tabs.
- Test rejection of duplicate keys in `parseProvenance()`.
- Test rejection of lines containing carriage returns (`\r`).
- Test rejection of reserved keys in the `extra` dictionary.

#### 5. Consensus Engine Signature Verification Tests

```bash
./build/xudu_test --gtest_filter='IdentityValidationTest.*'
```

- Verify that `verifyOracleAttestation` rejects invalid `oracleSignature`.
- Verify that `stageBlock` rejects candidate blocks containing unauthenticated `IdentityEntry` or
  `VoteEntry` records.

#### 6. End-to-End Publication & Permascroll Bootstrap Tests

```bash
./build/xudu_test --gtest_filter='ProvenanceTest.*:PublicationTest.*'
```

- Publish a xanadoc with an author identity, emitting Base64-DER X.509 cert and raw 64-byte
  signature.
- Verify that `permascroll` is correctly formatted and bootstraps `d.scrolls` and `d.source` ranks
  in `ArenaManifold`.
- Verify complete in-process execution without `gpg` installed on the system.

### 7.2 Manual & Headless Regression Verification

- Run `make check` (format-check, linter, static analysis) to ensure C++23 standards,
  `.clang-format`, and `.clang-tidy` harmony.
- Verify `tools/store-dump --section=authorship` cleanly parses and verifies sample publication
  records.
- Verify `make test` succeeds completely without `gpg` present in `PATH`.
