#!/usr/bin/env python3
"""Recompute the panel-band proof from the committed eighth-checkpoint PNGs."""

import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", type=Path)
    args = parser.parse_args()
    root = args.repository / "design/reviews/spatial-ui-eighth-increment"
    names = [
        "pointer/png/11-back.png",
        "pointer/png/12-origin.png",
        "candidate-pointer/png/11-back.png",
        "candidate-pointer/png/12-origin.png",
        "text-gl-link/evidence/png/11-back.png",
        "text-gl-link/evidence/png/12-origin.png",
    ]
    rows = []
    pixels = []
    for name in names:
        path = root / name
        image = Image.open(path).convert("RGB")
        assert image.size == (800, 600), name
        band = image.crop((0, 316, 800, 551)).tobytes()
        pixels.append(band)
        colors = [tuple(band[i : i + 3]) for i in range(0, len(band), 3)]
        rows.append({
            "path": name,
            "file_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "panel_rgb_sha256": hashlib.sha256(band).hexdigest(),
            "panel_surface_pixels": colors.count((30, 41, 59)),
            "unique_rgb_colors": len(set(colors)),
        })
    assert all(band == pixels[0] for band in pixels[1:])
    assert all(row["panel_surface_pixels"] == 112850 for row in rows)
    print(json.dumps({"crop": [0, 316, 800, 551], "all_pixels_equal": True,
                      "captures": rows}, indent=2))


if __name__ == "__main__":
    main()
