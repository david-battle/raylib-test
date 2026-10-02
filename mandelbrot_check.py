#!/usr/bin/env python3
"""Diff a mandelbrot_ref.c render against a mandelbrot_harness.py render.

    ./mandelbrot_ref -cx C -cy C -span S -iter N -x X -y Y -cw W -ch H -mode 2 > ref.rgb
    python3 mandelbrot_check.py ref.rgb gpu.raw <x> <y> <cw> <ch> <fw> <fh>

The reference renders just the crop window as RGB, bottom-up (row 0 is the
bottom). The harness dumps the whole framebuffer as RGBA, top-down, because that
is what raylib's rlReadScreenPixels hands back - it flips the rows for you. So
the checker does the single flip between them. Stdlib only on purpose: no numpy,
no PIL, nothing to install.

If the reference file is int16-sized it is read as escape counts instead of
colour, which is the only honest way to compare deep views (see below).

Read the numbers like this:
  mem   membership mismatches, out of cw*ch. This is the correctness number -
        interior-vs-exterior must agree everywhere. Expect 0.
  mean  mean per-pixel channel error over pixels both sides coloured. Only a
        precision number while the crop is shallow enough for the depth-faded
        colour bands to be off (nu < 400, roughly span <= 1e-4). Past that a hue
        band is only ~25 iterations wide, so +/-1 iteration of chaotic
        divergence shows up as a visible band and mean climbs into the tens
        even while mem stays 0. Use count mode there.
"""
import sys
from statistics import fmean

ref_path, gpu_path = sys.argv[1], sys.argv[2]
x, y, cw, ch, fw, fh = (int(v) for v in sys.argv[3:9])
ref = open(ref_path, 'rb').read()
gpu = open(gpu_path, 'rb').read()
n = cw*ch
crop = " at x=%d y=%d" % (x, y)

if len(ref) == n*2:                       # count mode: both files are int16 counts
    a = [int.from_bytes(ref[i:i+2], 'little', signed=True) for i in range(0, len(ref), 2)]
    if len(gpu) != n*2:
        sys.exit("ref is counts (%d bytes) but gpu is %d" % (len(ref), len(gpu)))
    d = [abs(u-int.from_bytes(gpu[i:i+2], 'little', signed=True))
         for i, u in zip(range(0, len(gpu), 2), a)]
    capped = [abs(u-int.from_bytes(gpu[i:i+2], 'little', signed=True))
              for i, u in zip(range(0, len(gpu), 2), a) if u >= 32000]
    print("%s vs %s%s  counts exact %d/%d (%.1f%%)  |d| med %d mean %.1f max %d"
          % (ref_path, gpu_path, crop, d.count(0), n, 100.0*d.count(0)/n,
             sorted(d)[n//2], fmean(d), max(d)))
    if capped:
        print("    interior pixels (capped at 32000): disagree %d/%d" % (sum(1 for v in capped if v), len(capped)))
    raise SystemExit(0)

if len(ref) != n*3:
    sys.exit("ref is %d bytes, expected %d for a %dx%d crop" % (len(ref), n*3, cw, ch))
# The other side is normally a full RGBA framebuffer dump; a crop-sized RGB file
# (two reference renders, say) is compared directly, same convention.
full = len(gpu) == fw*fh*4
if not full and len(gpu) != n*3:
    sys.exit("gpu dump is %d bytes, expected %d for a %dx%d RGBA framebuffer or %d for a crop"
             % (len(gpu), fw*fh*4, fw, fh, n*3))

mism = 0
errs = []
interior = 0
for j in range(ch):
    for i in range(cw):
        o = ((fh - 1 - (y + j))*fw + x + i)*4 if full else (j*cw + i)*3
        k = (j*cw + i)*3
        r1, g1, b1 = ref[k], ref[k+1], ref[k+2]
        r2, g2, b2 = gpu[o], gpu[o+1], gpu[o+2]
        # a pixel is "interior" when the shader's dither leaves it near black;
        # 12 is well clear of the +/-1 dither and well under any hue
        t1, t2 = r1+g1+b1 > 12, r2+g2+b2 > 12
        if not t1:
            interior += 1
        if t1 != t2:
            mism += 1
        elif t1:
            errs.extend((abs(r1-r2), abs(g1-g2), abs(b1-b2)))
mean = fmean(errs) if errs else -1.0
print("%s vs %s%s  mem %d/%d  interior %d/%d  mean|d| %.2f"
      % (ref_path, gpu_path, crop, mism, n, interior, n, mean))
if not interior:
    print("    note: crop has no interior pixels, so mem is trivially 0 - pick a crop"
          " that straddles the boundary before believing it")
if mism:
    print("    FAIL: membership disagreement - the two renders are not the same set")
if mean > 8:
    print("    note: large mean colour error is expected at depth (bands are ~25 iterations"
          " wide); trust mem, or re-run shallower to read it as precision")
