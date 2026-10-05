#!/usr/bin/env python3
"""Publish through the UI across three processes with disposable signing keys.

This checks local sealing and restart integrity. DHT discovery, mock Oracle
transport, subscriptions and link-package journeys require the later swarm runner.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def decode(data):
    def item(offset):
        kind = data[offset:offset + 1]
        if kind == b"i":
            end = data.index(b"e", offset)
            return int(data[offset + 1:end]), end + 1
        if kind == b"l":
            values = []
            offset += 1
            while data[offset:offset + 1] != b"e":
                value, offset = item(offset)
                values.append(value)
            return values, offset + 1
        if kind == b"d":
            values = {}
            offset += 1
            while data[offset:offset + 1] != b"e":
                key, offset = item(offset)
                value, offset = item(offset)
                values[key] = value
            return values, offset + 1
        end = data.index(b":", offset)
        length = int(data[offset:end])
        return data[end + 1:end + 1 + length], end + 1 + length

    value, end = item(0)
    assert end == len(data), "Trailing manifest data"
    return value


def run(binary, root):
    home = root / "gnupg"
    home.mkdir(mode=0o700)
    env = os.environ | {
        "GNUPGHOME": str(home),
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_AUDIODRIVER": "dummy",
        "LIBGL_ALWAYS_SOFTWARE": "1",
        "XDG_DATA_HOME": str(root / "data"),
        "XDG_CONFIG_HOME": str(root / "config"),
        "XDG_CACHE_HOME": str(root / "cache"),
    }
    identity = "Alice Publication Test <alice.publication@example.invalid>"
    try:
        subprocess.run(
            ["gpg", "--batch", "--pinentry-mode", "loopback", "--passphrase", "",
             "--quick-generate-key", identity, "ed25519", "sign", "never"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True,
            timeout=60,
        )
        base = [str(binary), str(root / "store"), "--permascroll",
                str(root / "permascroll"), "--backend", "opengl", "--profile"]
        answers = ["--chord", "Ctrl+Shift+S"]
        for value in ["doc:story-ideas", "Story Ideas", "Alice Publication Test",
                      "alice.publication@example.invalid"]:
            if value in ["doc:story-ideas", "Story Ideas"]:
                answers += ["--chord", "Backspace"] * 8
            answers += ["--type", value, "--chord", "Tab"]
        answers += ["--chord", "Tab"] * 5
        answers += ["--type", " Ideas, writing,IDEAS ", "--chord", "Tab"]
        for edition in range(1, 4):
            script = []
            if edition == 1:
                script = ["--type", "Story Ideas", "--chord", "Ctrl+Alt+Shift+N",
                          "--chord", "F6"]
            elif edition == 3:
                script = ["--type", " revised"]
            edition_action = []
            if edition in (1, 3):
                edition_action += ["--chord", "Right"]
            edition_action += ["--chord", "Tab"]
            if edition == 1:
                edition_action += ["--type", "release"]
            edition_action += ["--chord", "Tab", "--dump-a11y", "--capture",
                               str(root / f"publish-form-{edition}.ppm"),
                               "--chord", "Return"]
            command = base + script + answers + edition_action + [
                "--capture", str(root / f"edition-{edition}.ppm")]
            with (root / f"edition-{edition}.log").open("w") as log:
                subprocess.run(command, env=env, stdout=log, stderr=log,
                               timeout=120, check=True)
            shutil.copyfile(root / "store/published/doc:story-ideas.xanadoc",
                            root / f"edition-{edition}.xanadoc")
        with (root / "status.log").open("w") as log:
            subprocess.run(base + ["--chord", "Ctrl+Shift+P", "--chord", "Tab",
                                  "--chord", "Right", "--chord", "Return",
                                  "--chord", "Tab", "--chord", "Return",
                                  "--chord", "Tab", "--chord", "Return",
                                  "--dump-a11y", "--capture", str(root / "status.ppm")],
                           env=env, stdout=log, stderr=log, timeout=120, check=True)
        status = (root / "status.log").read_text()
        assert "Publication status" in status and "Refresh status" in status
        assert "Retry selected publication" in status, "Retry action is inaccessible"
        assert "publication retry queued" in status, "Retry did not execute"
        assert "Local ready" in status, "Prepared publication never became ready"

        assert "Add a named edition" in (root / "edition-1.log").read_text()
        assert "Keep all existing editions" in (root / "edition-2.log").read_text()
        assert "Repoint release" in (root / "edition-3.log").read_text()
        records = list((root / "store/published").rglob("AUTHORSHIP.tsv"))
        assert records, "No signed seed records were produced"
        for record in records:
            subprocess.run(
                ["gpg", "--batch", "--verify", str(record) + ".asc", str(record)],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                check=True, timeout=30,
            )
        validate(root)
        validate_reader(binary, root, env)
    finally:
        try:
            subprocess.run(
                ["gpgconf", "--homedir", str(home), "--kill", "gpg-agent"],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                timeout=30, check=False,
            )
        finally:
            shutil.rmtree(home, ignore_errors=True)
            (root / "store/identity").unlink(missing_ok=True)
            for state in root.rglob("publication-state"):
                shutil.rmtree(state)



def validate(root):
    pubs = [decode((root / f"edition-{i}.xanadoc").read_bytes()) for i in (1, 2, 3)]
    assert all(p[b"format"] == 2 and p[b"topics"] == [b"ideas", b"writing"]
               for p in pubs), "Topics were not normalized and signed by the form"
    assert len({p[b"publisher"] for p in pubs}) == 1, "Publication identity changed"
    assert [p[b"seq"] for p in pubs] == [1, 2, 3], "Sequence did not advance"
    keys = [next(iter(p[b"scrolls"])) for p in pubs]
    assert len(set(keys)) == 1 and keys[0].endswith(b":permascroll"), "Scroll identity changed"
    segments = [p[b"scrolls"][keys[0]][b"segments"] for p in pubs]
    assert len(segments[0]) == 1
    assert segments[1][:-1] == segments[0], "Restart replaced an earlier seal"
    assert segments[2][:-1] == segments[1], "Editing replaced an earlier seal"
    assert segments[2][-1][b"at"] == segments[1][-1][b"at"] + segments[1][-1][b"len"]
    assert sum(s[b"len"] for s in pubs[2][b"pieces"]) - sum(
        s[b"len"] for s in pubs[1][b"pieces"]) == 8
    assert pubs[0][b"ops"] == pubs[1][b"ops"], "Unchanged history was sealed again"
    assert all(p[b"holes"] for p in pubs), "Private settings ranges are missing"
    for pub, current in zip(pubs, segments):
        for segment in current:
            parent = root / "store/published" / segment[b"torrent"].hex() / "permascroll"
            payload = (parent / "primedia").read_bytes()
            record = dict(line.split("\t", 1) for line in
                          (parent / "AUTHORSHIP.tsv").read_text().splitlines())
            assert int(record["content_length"]) == len(payload)
            assert record["content_sha256"] == hashlib.sha256(payload).hexdigest()
            assert int(record["permascroll_at"]) == segment[b"at"]
            for hole in pub[b"holes"]:
                first = max(hole[b"at"], segment[b"at"])
                last = min(hole[b"at"] + hole[b"len"], segment[b"at"] + segment[b"len"])
                if first < last:
                    assert payload[first - segment[b"at"]:last - segment[b"at"]] == bytes(last - first)
        for segment in pub[b"ops"]:
            parent = root / "store/published" / segment[b"torrent"].hex() / "permascroll"
            payload = (parent / "ops").read_bytes()
            record = dict(line.split("\t", 1) for line in
                          (parent / "AUTHORSHIP.tsv").read_text().splitlines())
            assert int(record["content_length"]) == 0
            assert int(record["ops_length"]) == len(payload)
            assert record["ops_sha256"] == hashlib.sha256(payload).hexdigest()
    report = {
        "sequences": [p[b"seq"] for p in pubs],
        "topics_signed": True,
        "edition_creation_review_and_repoint_accessible": True,
        "status_and_retry_accessible": True,
        "same_publisher": True,
        "same_permascroll_key": True,
        "segment_counts": [len(s) for s in segments],
        "third_edition_new_document_bytes": 8,
        "private_metadata_adds_bytes": True,
        "unchanged_edition_reuses_ops": True,
        "payload_digests_match": True,
        "withheld_bytes_zero_filled": True,
        "authorship_signatures_verified": True,
    }
    (root / "results.json").write_text(json.dumps(report, indent=2) + "\n")


def validate_reader(binary, root, author_env):
    """Use the ordinary Open control, then reopen its reader copy offline."""
    env = author_env | {
        "XDG_DATA_HOME": str(root / "reader-data"),
        "XDG_CONFIG_HOME": str(root / "reader-config"),
        "XDG_CACHE_HOME": str(root / "reader-cache"),
    }
    base = [str(binary), str(root / "reader-workspace"), "--permascroll",
            str(root / "reader-permascroll"), "--backend", "opengl", "--profile"]
    manifest = root / "store/published/doc:story-ideas.xanadoc"
    with (root / "reader-open.log").open("w") as log:
        subprocess.run(base + [
            "--chord", "Ctrl+O", "--chord", "Tab", "--type", str(manifest),
            "--dump-a11y", "--capture", str(root / "reader-open-form.ppm"),
            "--chord", "Return", "--dump-a11y", "--capture",
            str(root / "reader-open.ppm")], env=env, stdout=log, stderr=log,
            timeout=120, check=True)
    opened = (root / "reader-open.log").read_text()
    assert "Open Document or System Xanadoc" in opened
    assert "complete store" in opened, "Open did not restore the publication"
    assert "Could not open publication" not in opened
    copies = list((root / "reader-data/xudu/xanadocs").glob("publication-*"))
    assert len(copies) == 1, "Reader installation is missing or duplicated"
    copy = copies[0]
    initial_ops = (copy / "ops.nodes").read_bytes()
    header = (copy / "store.tables").read_bytes()
    assert int.from_bytes(header[12:16], "little") == 4
    tables = decode(header[16:])
    assert tables[b"published_local"] > 0 and tables[b"deployment"]
    # Remove every publisher carrier while reopening through the ordinary
    # path picker. The original manifest remains only to check a rejection.
    seeds = root / "store/published"
    hidden = root / "author-seeds-offline"
    seeds.rename(hidden)
    try:
        with (root / "reader-offline.log").open("w") as log:
            subprocess.run(base + [
                "--chord", "Ctrl+O", "--chord", "Tab", "--type", str(copy),
                "--chord", "Return", "--dump-a11y", "--capture",
                str(root / "reader-offline.ppm")], env=env, stdout=log, stderr=log,
                timeout=120, check=True)
        offline = (root / "reader-offline.log").read_text()
        assert "opened store" in offline and "Could not open xanadoc" not in offline
        assert (copy / "ops.nodes").read_bytes() == initial_ops, "Reading rewrote author history"
        with (root / "reader-rejection.log").open("w") as log:
            subprocess.run(base + [
                "--chord", "Ctrl+O", "--chord", "Tab", "--type",
                str(root / "edition-3.xanadoc"), "--chord", "Return",
                "--dump-a11y", "--capture", str(root / "reader-rejection.ppm")],
                env=env, stdout=log, stderr=log, timeout=120, check=True)
        refused = (root / "reader-rejection.log").read_text()
        assert "Could not open publication" in refused, "Incomplete cache was accepted"
        assert "complete store" not in refused
        assert len(list(copy.parent.glob("publication-*"))) == 1
    finally:
        hidden.rename(seeds)
    report = json.loads((root / "results.json").read_text())
    report.update({"reader_open_keyboard": True, "reader_offline_reopen_keyboard": True,
                   "reader_keeps_authored_ops": True, "reader_refuses_missing_cache": True})
    (root / "results.json").write_text(json.dumps(report, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/xuzz"))
    parser.add_argument("--output", type=Path, default=Path("build/publication-local"),
                        help="Parent for a fresh run directory; earlier evidence is retained")
    args = parser.parse_args()
    for tool in ("gpg", "gpgconf"):
        if not shutil.which(tool):
            parser.error(f"{tool} is required")
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"Build xuzz first: {binary}")
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="run-", dir=args.output)).resolve()
    print(f"Publication local journey evidence: {root}", flush=True)
    run(binary, root)
    print("PASS: three UI publications, signatures, edition choices, cached reader opening and offline reopening")


if __name__ == "__main__":
    main()
