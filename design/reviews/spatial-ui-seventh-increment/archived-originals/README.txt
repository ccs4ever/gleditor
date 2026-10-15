Archived seventh-checkpoint source preservation

The .txt files in this directory are byte-for-byte copies of the source files listed in originals.sha256.tsv. The saved root-SHA256SUMS.txt is the original archive-wide inventory before the quality-gate repairs. The existing nested SHA256SUMS files are left unchanged and remain historical records of their original evidence runs. Their script hashes refer to the preserved originals, not the repaired current scripts.

After repair, the top-level spatial-ui-seventh-increment/SHA256SUMS is regenerated as the current archive inventory (excluding itself). SHA256-CHANGES.txt records original and repaired hashes for each edited path, so current files are not presented as if they match historical hashes.
