# TODO — `pert.c`, perturbation Mandelbrot

Goal: zoom far past the fp64 wall (span ~1e-13) using perturbation theory, on
this hardware (1680x1050, 2080 Super, D3D12/Mesa, ~2 s GPU watchdog).
`mandelbrot.c` is **not** modified — this is a separate program.

## Design (revised after Phase 0 measurements)

    c = c0 + delta          z_n = Z_n + w_n      Z = orbit of c0 (shared)
    w_{n+1} = 2*Z_n*w_n + w_n^2 + delta          (exact identity)

`delta` enters additively, so it needs only *relative* precision (~1e-7).
Precision stops being the fixed ulp(0.74)=1.1e-16 floor and scales with zoom.
In perturbation mode the shader never forms an absolute `c` at all, which is
what removes that floor: the only depth-dependent quantity is `delta`, which is
always measured in units of the current span.

**fp32 `w` is *usable*, and much faster (measured, corrected).** My first
conclusion here was wrong: it came from tests whose reference orbit escaped
(see Traps), so every pixel was being compared against a truncated loop. Redone
against a reference that survives, against direct quad on 30000-pixel crops,
centred at c=-1.7497:

    span   iter    fp32 w + fp32 ref            fp64 w + float hi/lo ref
                              mem   exact   max|dc|        mem   exact  max|dc|
    1e-3   10408     103/30000  90.9%    9898        43/30000  97.9%   7334
    1e-6   19378       2/30000  99.9%       2         2/30000 100.0%      1
    1e-9   28347       0/30000  99.7%      88         0/30000 100.0%      1
    1e-12  37316       2/30000  99.9%      77         0/30000 100.0%      1
    1e-16  49275       2/30000  94.1%     304         0/30000 100.0%      4

The reason fp32 survives is that the error that matters is not `N*eps` in `z`
but that divided by the orbit's own amplification `2^n`. `w_n` grows like
`2^n`, so `sum_n 2^(N-n) * 2^-24 * |w_n|` comes out as `~2^N * 2^-24`, and back
in c-space that is `2^-24` of a pixel — depth-independent, sub-pixel, invisible.
The same argument makes an fp32 *reference* harmless, which is why the table
needs no hi/lo split either.

What fp32 does cost: count errors reach a few hundred iterations at depth, and
at span 1e-3 ~100 of 30000 pixels flip membership. Since a hue band is only
~25 iterations wide at depth, that is speckle and wrong band phase, not wrong
geometry. So the honest split is by *role*, not by which one is "correct":
fp32 perturbation for interaction, fp64 perturbation for the settled frame.
fp64 is 1/64 rate on a 2080 Super, so that is a ~60x difference and worth
exploiting rather than avoiding.

**No rebasing (measured, and unnecessary).** The textbook rebase `w = delta*zbar_n` is
first-order, valid only while `|delta*zbar_n| << 1` — n <~ log2(1/delta), about
80 steps at span 1e-25. Scaling a tabulated `W_n` instead requires a *complex
multiply* (not componentwise), and inherits the same validity window: once
`|w|` is not `<<|Z|` the table entry has diverged from the pixel's own orbit,
and rescaling it corrupts the orbit permanently. Measured at `-rbase 500`,
span 1e-6: 22.6% of counts exact, mean |dc| 616. So `w` is carried in fp64
from n=0 with no rebase. That is fine because fp64's own error is
N*2^-53 ~ 4e-11 at N=1e5, and it is *relative to `w`*, so it shrinks with depth.

**Reference table**: CPU computes the reference orbit in `__float128`, then
stores `(Zx.hi, Zx.lo, Zy.hi, Zy.lo)` as float hi/lo pairs in one RGBA32F
texture — GLSL has no double sampler, and hi/lo recovers ~48 bits, which is
enough. `-wprec 3` round-trips the reference through those float pairs to prove
it; it matches `-wprec 2` (exact double reference) to within a count of 1-4 on
30000-pixel crops, so the float table is not the accuracy limit. Error in the
reference reaches `z` as ~2*N*eps_ref, and since it is multiplied by `w`
rather than by `2*Z`, it does *not* amplify exponentially; in c-space it works
out to ~N*eps_ref*delta, a fixed fraction of a pixel.

**Coordinate**: `delta` must come from the *exact integer* pixel offset
`(gl_FragCoord.xy - res*0.5)`, never `uv-0.5` (cancels catastrophically at
depth). Compute `delta = ((pixOff + (centre - c0))*uSpan)*uInvRes`, summing the
`(centre - c0)` offset in *double* (two `dvec2` uniforms, as `mandelbrot.c`
already does) before the multiply. fp32 is fine for `uSpan`/`uInvRes`: their
error is relative, so it is a sub-pixel shift.

**Paths kept**: fp64 perturbation (settled frame) + fp32 perturbation
(interaction) + fp64 direct (fallback when no reference orbit exists, and the
in-app A/B oracle). Float32 *direct* is dropped — it is the only one of the
four that no depth helps. Direct fp64 dies at span ~1e-13 from the
ulp(0.74) add; both perturbation paths have no such floor. `P` cycles the
active path.

## Phases

- [x] **Phase 0 — CPU prototype, no GPU** (`pert_ref.c`). Quad reference
      tables, per-pixel fp32/fp64/quad emulation, `-rbase R`, `-wprec 0..3`,
      `-refauto`, `-counts`. Diff vs direct quad.
      *Accept:* 0 membership errors and median |dc| 0 vs direct quad.
      **Result: met for fp64.** `-wprec 2` and `-wprec 3` are 100% exact on
      30000-pixel crops at spans 1e-6..1e-16 (max |dc| 0 and 1-4 respectively),
      `-wprec 1` (pure quad) max |dc| 0. Rebasing rejected; fp32 kept as the
      interactive path rather than rejected, as recorded under Design. Caveat: `-wprec 1` matching is the proof that the
      *recurrence* is right, which is why it is kept as a permanent mode.
- [x] **Phase 1 — shader path** in `pert.c`: `sampler2D refOrb`, exact pixel
      offset, one `texelFetch` per iteration, `shade()` unchanged.
      **Result: met.** fp64 path is membership-exact vs the `__float128`
      oracle at every span tried, 1e-6 through 1e-50 (see Phase 3).
- [x] **Phase 2 — plumbing**: per-frame quad table -> `rlLoadTexture` once +
      `rlUpdateTexture` per frame (raylib 6's `rlSetUniformSampler` takes a
      texture id, not a unit), NEAREST, no mipmaps; escape guard -> fp64.
      **Result: met.** `RefExtend`'s factor-of-two and a shader/C path-enum
      mismatch were the two bugs; the table itself was verified good to the
      last bit by counting every pixel (108521/108521 escape counts equal).
- [x] **Phase 3 — verification**: native `--shot cx cy span iter path out.png`
      mode (no string surgery, unlike `mandelbrot_harness.py`), diff with
      `mandelbrot_check.py`, membership only at depth, watchdog guard.
      **Result: met, and the objective is beaten.** `pert64` membership is
      exact (`mem 0`) at spans 1e-6, 1e-9, 1e-12, 1e-16, 1e-20, 1e-30, 1e-40 and
      1e-50 (150926 iterations), i.e. 50 orders of magnitude past the direct
      fp64 wall of ~1e-13. `pert32` matches to 1e-33 and collapses at 1e-36,
      where `stepF` underflows fp32's normal range — the expected crossover,
      not a bug. Membership is the only number that means anything past ~1e-4:
      the per-iteration hue bands make the colour channel differ wildly between
      any two renderers while the sets agree exactly.
- [x] **Phase 4 — app**: `ITER_SLOPE` 900, `FRAME_BUDGET_MS` 700 (was 1200),
      HUD path readout.
      **Result: met.** Watchdog characterised: silent context loss between 1.0
      and 1.2 s/frame at 1920x1080 fp64. Deep views never approach it, because
      a *repelling* reference lets almost every pixel bail early (1e-50 at
      150926 iterations still renders in ~0.4 s). The case that does overrun is
      an *attracting* reference, where every pixel runs to the cap; 1e-3 at
      6000 iterations loses the context.
- [ ] **Phase 5 (stretch)**: snap the reference centre to an interior pixel from
      the previous frame, so views centred on the *exterior* side of a filament
      also benefit. Without it, an escaping `c0` means fp64 fallback = 1e-13 wall.

## The one real limitation, measured

At span 1e-3 around c=-1.7497 the reference orbit is *attracting* (the centre is
in the set and the orbit converges), and there the perturbation condition
degrades: 4 of 14400 crop pixels flip membership, and 32 differ in count (all
mirror-paired, and all pixels whose orbits linger thousands of iterations near
the attractor before escaping). At 1e-6 and deeper, where the reference is
repelling and `|w|` grows monotonically, membership is exact.

This is the known glitch regime for perturbation: a pixel glitches once
`|Z_n + w_n|^2` drops below `G|Z_n|^2` for some threshold G, i.e. the
perturbation stops being small next to the reference and the orbit collapses
onto it. It is a reference-*selection* property, not an arithmetic fault, so the
fix would be glitch detection (flag, then re-reference or rebase) rather than
more precision. Not implemented, because it does not affect the deep-zoom
objective: every deep span tested here is membership-exact.

## Known limitation

Perturbation needs the view centre in the set's basin. Exterior-centre views
get the fp64 path and stop at 1e-13 until Phase 5.

## Answered by Phase 0

1. Recurrence correct — `-wprec 1` reproduces direct quad exactly (max |dc| 0).
2. `-rbase` is not needed at all and is actively harmful (see Design).
3. fp32 range is a non-issue, and fp32 *precision* costs only speckle plus
   visible band-phase error, not wrong geometry — see the table under Design.
   That reopens fp32 as the interactive path.
4. `delta*zbar_n` is indeed only good for n <~ log2(1/delta), as derived.

## Traps found while verifying

- **An escaping reference fails silently and reads as a precision result.** The
  per-pixel loop stops at `tabN`, so every pixel comes back with the *same*
  count (measured: all 14504 exterior pixels at 3088) while the interior
  pixels are reported as escapes. `pert_ref` now exits 3 instead. Any test that
  reports "exact 51%" is this, not noise.
- **The reference does not have to be the view centre** — only be in the
  basin. `-refauto 1` spirals out for the nearest surviving point and offsets
  `delta` by `centre - c0`. Necessary because the interesting coordinates are on
  the boundary, where the orbit escapes.
- Deep test points need checking individually: the crop must actually contain
  interior pixels (at c=-1.7497 a 200x150 crop is 99.99% exterior — the set is
  a filament one pixel wide there), and a reference must exist nearby.

## Open defects found while fixing the interactive stalls

- **Path selection ignores capability.** `MODE_AUTO` picks perturbation whenever
  *any* reference exists, including a fallback one that survives only ~500
  iterations, and never compares against what direct fp64 could deliver. At
  span ~1e-10 that throws away a full-quality render (direct fp64 bands at 0.20
  there, see NOTES.md) in favour of a 500-iteration one, which is the "refuses
  to render any more detail" wall. Choose by what each path can afford, not by
  availability. This is the highest-value remaining fix.
- **`RefFind` returns the first survivor, not the best.** It walks outward and
  takes the first point whose orbit survives `need/f`, so it settles for a
  marginal 500-iteration reference when a 30000-iteration one may sit a few
  pixels further out. Preferring long-surviving candidates would raise the
  depth ceiling everywhere, and unlike the fix above it also helps *below*
  1e-13 where direct fp64 cannot help at all.
- **The attraction classifier cannot see the neighbourhood.** It averages
  `log|2Z|` over the reference orbit, but frame cost is set by how much of the
  screen survives. A repelling reference beside a minibrot reports repelling
  while the view is nearly all interior, so the cap disengages exactly when it
  is needed. Needs a measure of screen-wide survival, not orbit growth.

