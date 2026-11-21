#!/usr/bin/env python3
"""Compares two sets of regression captures (scripts/regress.sh): <baseline> and <new>, one folder per scene, the
frames paired in capture order.

For each pair: the mean absolute difference over RGB (0..255) and the share of pixels that moved by more than 8 levels.
A pair fails past --mean or --share; a side-by-side sheet (baseline | new | difference x4) is written next to the new
captures for each failing scene. Exit status 1 when anything failed or is missing."""
import argparse
import os
import sys

from PIL import Image, ImageChops


def frames(folder):
    return sorted(p for p in os.listdir(folder) if p.endswith(".png")) if os.path.isdir(folder) else []


def compare(a, b):
    a = a.convert("RGB")
    b = b.convert("RGB").resize(a.size)
    diff = ImageChops.difference(a, b)
    hist = diff.convert("L").histogram()
    pixels = a.size[0] * a.size[1]
    moved = sum(hist[9:]) / pixels
    total = sum(sum(i * n for i, n in enumerate(band.histogram())) for band in diff.split())
    return total / (pixels * 3), moved, diff


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("baseline")
    ap.add_argument("new")
    ap.add_argument("scenes", nargs="*", help="only these scenes (default: every folder in <new>)")
    ap.add_argument("--mean", type=float, default=1.0, help="largest mean difference allowed (levels)")
    ap.add_argument("--share", type=float, default=0.02, help="largest share of moved pixels allowed")
    args = ap.parse_args()

    failed = False
    for scene in args.scenes or sorted(os.listdir(args.new)):
        new_dir = os.path.join(args.new, scene)
        if not os.path.isdir(new_dir):
            continue
        base_dir = os.path.join(args.baseline, scene)
        old, new = frames(base_dir), frames(new_dir)
        if not old:
            print(f"{scene}: no baseline (scripts/regress.sh --update)")
            failed = True
            continue
        if len(old) != len(new):
            print(f"{scene}: {len(new)} captures, the baseline has {len(old)}")
            failed = True
        sheet = []
        for o, n in zip(old, new):
            a, b = Image.open(os.path.join(base_dir, o)), Image.open(os.path.join(new_dir, n))
            mean, moved, diff = compare(a, b)
            bad = mean > args.mean or moved > args.share
            failed |= bad
            print(f"{scene} {n}: mean {mean:.2f}, moved {moved * 100:.2f}%{'  FAIL' if bad else ''}")
            if bad:
                sheet.append((a.convert("RGB"), b.convert("RGB"), diff.point(lambda v: min(255, v * 4))))
        if sheet:
            w, h = 320, 240
            out = Image.new("RGB", (3 * w, h * len(sheet)))
            for row, images in enumerate(sheet):
                for col, im in enumerate(images):
                    out.paste(im.resize((w, h)), (col * w, row * h))
            path = os.path.join(args.new, f"{scene}_diff.png")
            out.save(path)
            print(f"{scene}: differences in {path}")
    print("regression: FAILED" if failed else "regression: all scenes match")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
