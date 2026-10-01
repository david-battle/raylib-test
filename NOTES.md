# Ideas & Notes

Future experiment ideas from design discussions. Not build instructions.

## Verifying the UDP echo server (quick)

Single external round-trip test (this is the reliable fast check):

```bash
timeout 5 python3 -c "import socket;s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.settimeout(3);s.sendto(b'ping',('35.212.149.98',7777));print('ECHO OK:',s.recvfrom(1024)[0])"
```

- Prefer python sockets over `/dev/udp` + bash `read`: the bash probe returned
  nothing even when the server was fine.
- If that fails, SSH in and check the service without looking at systemd logs:
  `gcloud compute ssh udp-test --zone=us-west1-a --command='sudo systemctl is-active udp-echo; ss -ulpn | grep 7777'`
  (service name `udp-echo`, listener `0.0.0.0:7777`).
- The service has been reliably up; in practice this is a verify-only step.

## Audio

- Factorio game audio lives at `/mnt/d/factorio-standalone/current/data`
  (`current` symlinks to the newest `Factorio_2.1.x`). All ~4,529 files are
  standard Vorbis `.ogg` (44.1 kHz, 16-bit, stereo) — playable straight from
  `audio_test`, no conversion needed.
- Snarf inventory (counts/categories): `find /mnt/d/factorio-standalone/current/data -name "*.ogg" | wc -l`
  Rough split: `data/base` ~1.7k, `data/space-age` ~2.5k, `data/core` ~230,
  `data/recycler` ~25, `data/elevated-rails` ~7. Largest base categories:
  `programmable-speaker` (360), `creatures` (294), `fight`, `walking`,
  `item`, `particles`, `world`, `ambient` (~28 music tracks).
- No voice lines exist — the "talking" sounds are the biter/creature noises.
- `./audio_test <path>` plays any sound, capped at 5s; `play_all.sh` iterates
  `resources/`.
- Audio-only programs must NOT use `WaitTime()`/`GetTime()`: `GetTime()` is
  `glfwGetTime()`, which returns 0.0 until `InitWindow()` is called, so
  `WaitTime()`'s partial busy-wait loop spins forever. Poll `IsSoundPlaying()`
  with a `nanosleep()` sleep, or time with `clock_gettime(CLOCK_MONOTONIC)`
  (both used in `audio_test.c`).

- `Sound` in raylib is monophonic: it wraps a single audio buffer, and
  `PlaySound` while already playing **restarts** it rather than layering a new
  voice. Rapid overlapping triggers (e.g. UDP replies) clip each other's tails.
- For overlapping SFX, use `LoadSoundAlias()` to clone a sound sharing the same
  sample data but with its own playback buffer, then keep a small pool of
  aliases and play on the first one where `IsSoundPlaying()` returns false.
  That pool doubles as a "what's playing" tracker.
- `SetSoundPitch()` with slight random variation per play is the classic trick
  for natural-sounding retriggers.
- raylib has no managed SFX pool/mixer; the pool pattern above is on you.

## Sound synthesis / editing workflow

- `analyze_audio.py <file> [segments]` — stdlib-only FFT report (no numpy):
  dominant pitches per segment with note names, spectral centroid, ASCII
  spectrogram. Decodes any format via ffmpeg. This is how the model "listens".
- sox + ffmpeg are installed for synth/transform (`synth`, `pitch`, `speed`,
  `fade`, `norm`, filters). Sample-level edits via python `wave` module
  (e.g. gamma compression `y = sign(x)*|x|^g` boosts quiet parts without
  touching peaks).
- Psychoacoustic gotcha: low-frequency sounds feel late/quiet at onset no
  matter what the samples say — the ear integrates slowly below ~500 Hz.
  Perceived-onset fixes: shift pitch up a few semitones, or trigger earlier.
- Provenance: `hit_splat.wav` (bullet-hits-you) is synthesized brownnoise,
  lowpass 700, γ=0.35 compressed, +3 semitones; replaces the celebratory
  chirp `target.ogg` used to be. `weird.wav` (sprite shoot) is the original
  sped 1.84x and pitched down 6 semitones (full sweep kept, not trimmed).
  `ping_send.wav` (UDP ping send) is synthesized: sox sine sweep 900→450 Hz,
  0.12 s, short fades, normalized −3 dB. Descending on purpose — the receive
  coin rises, so the pair reads as out/back.

## Custom cursor on WSLg — SOLVED (was a rat hole)

Goal: only ONE thing at the mouse position. **Result: the arrow is replaced
by the X cursor-font "target" glyph** (`XC_target`, circle-with-dot reticle)
via `hide_cursor_x11.c`; `main.c` draws nothing at the mouse anymore, so the
cursor IS the aim marker (the old drawn crosshair sprite and its hit-flash
tint are gone; hits still cue via `target.ogg`). True invisibility is NOT
possible on WSLg: empty cursors (1x1 transparent pixmap, blank-glyph
`XCreateGlyphCursor`) fall back to the default arrow, and custom pixmaps are
dropped entirely. Build with `gcc -I ~/raylib/src main.c hide_cursor_x11.c
-o net_test ~/raylib/src/libraylib.a -lm -lpthread -ldl -lX11`.

Two separate bugs were stacked on top of the compositor limitation:

1. **Bad window handle**: raylib 6.x's `GetWindowHandle()` returns a *pointer
   to* the X11 Window id (rcore_desktop_glfw.c stores it in a local), not the
   id itself. Casting the pointer value to a Window made every `XDefineCursor`
   fail with BadWindow (visible in launch logs) and silently no-op.
2. **WSLg drops pixmap cursors**: even with a valid window,
   `XDefineCursor` of an `XCreatePixmapCursor` image (invisible OR visible)
   never reaches the Windows side — the default arrow stays. Known multi-year
   WSLg bug (wslg#376/#1300). Font/glyph cursors DO get through; that's the
   workaround (`XCreateFontCursor`).

Rules that keep it working:

- Never call raylib's `HideCursor()`: on X11 GLFW installs its own cursor,
  overriding ours (it was being called every frame).
- Don't bother with `XFixesHideCursor`: any key/button press unhides, useless
  during gameplay.
- Cursor shape is one line: swap the `XC_*` constant in `hide_cursor_x11.c`
  (tried: `XC_dot`, `XC_crosshair`, settled on `XC_target`).

## main.c gameplay

- Catch-the-sprite mini-game: click the sprite to score; the sprite flees the
  mouse (within ~200px) and otherwise drifts back toward screen center so it
  stays visible. Win target is 12.
- Each win raises the level; dots gain a homing turn toward the cursor
  (`0.05 * level`, capped 0.9). Level resets to 0 on a loss or a shutout win
  ("beating the game"). Dots despawn on contact (player or sprite), off-screen,
  or after 5s; pool is 24.
- The win screen plays `resources/country.mp3` (looping) until a key is pressed
  — mouse clicks intentionally do NOT skip it, so dodging near the cursor
  doesn't cut the song short. Shutouts get a red banner + double confetti.
- Ping/click-box UI is `#ifdef SHOW_UI` (off by default); the underlying
  detection and sounds still run. `-DSHOW_UI` restores the drawn elements.

## Graphics — bloom pipeline in main.c

- Post-processing chain (all shaders embedded as GLSL 330 string literals,
  `LoadShaderFromMemory(NULL, fs)`): scene RT -> brightpass at half res ->
  separable gaussian blur ping-pong x2 -> composite (scene + bloom + contrast/
  saturation grade + vignette). HUD text is drawn AFTER composite so it stays
  crisp and never blooms. rlgl's default vertex shader exports
  `fragTexCoord`/`fragColor`; samplers named `texture0`/`texture1` are
  auto-bound to units by name at shader load.
- **WSLg/D3D12 presents RenderTexture chains Y-MIRRORED** vs what the same
  code does on vanilla GL: scene content drawn via `BeginTextureMode` ->
  fullscreen composite lands vertically flipped (position AND glyph
  orientation), while HUD drawn outside the pipeline stays correct.
  Symmetric content hides it completely — a radial gradient, dot grid and
  uniform motes looked fine while the sprite was mirrored onto the opposite
  side of the screen from its logical hitbox. Fix applied ONCE in the
  composite fragment shader: `vec2 uv = vec2(fragTexCoord.x,
  1.0 - fragTexCoord.y)` used for BOTH samplers. Do not remove that line on
  this machine, and assume any new RT-presented pass needs the same
  treatment. Intermediate RT hops are left untouched — they are
  self-consistent; only net presentation carries one flip.
- Debugging technique that cracked it (reusable): (1) log `GetMousePosition()`
  + entity positions to stderr every ~30 frames — revealed clicks WERE
  registering against the logical rect (score incremented) while the user
  aimed at the mirrored visual; (2) draw asymmetric world-space markers
  ("TOP"/"BOTTOM" text at opposite corners) through the suspect pipeline and
  `TakeScreenshot()` — symmetric backgrounds can never expose this class of
  bug; (3) x11grab is useless here (see above), so backbuffer dumps are the
  only ground truth.
- The static lib is OpenGL 3.3 (`strings ~/raylib/src/libraylib.a | grep
  "#version"` shows 330) — write `#version 330` fragment shaders, not 100.
- Verifying visuals from the agent: **x11grab/ffmpeg screenshots of :0 come
  back BLACK for this app** (GLX swap buffers invisible to X capture). Use a
  temporary raylib `TakeScreenshot()` call instead — it reads the real GL
  backbuffer. Remove the calls once verified.
- `GenImageGradientRadial` falloff ends at the inscribed circle → hard visible
  disc edge on widescreen. `GenBackground()` in main.c hand-rolls a quadratic
  falloff to the corner distance instead. Same trick gives `GenGlowTexture()`,
  the soft radial glow sprite used for the sprite aura and dot halos.
- Dark gradients over small value ranges band hard at 8-bit depth (~17 levels
  corner-to-center → visible rings every ~75px on a good monitor). Fix is
  bake-time Bayer ordered dithering in `GenBackground()`; any new dark
  gradient needs the same treatment.
- Sprite/scene light integration: `SceneLightTint()` reuses the background
  falloff curve to tint sprite/shadow/aura with scene ambient, and a
  recolor-only overhead-light pass bakes shading into the sheet at load
  (near-whites floored at 0.92 so pupil contrast survives). No pixels move —
  the `gen_sprite.py` geometry contract is unaffected.
- raylib 6.x moved the math helpers out of raylib.h (`Clamp` now lives in
  raymath.h) — use fminf/fmaxf or include raymath.h.
- Juice systems added: trauma-based screen shake (offsets BeginMode2D target;
  HUD unaffected), white-silhouette flash overlay (a plain tint can only
  darken, never whiten), squash/stretch via DrawTexturePro dest rect, dot
  motion trails, particle/ring pools, floating "+1" popups, rotating confetti,
  red screen-edge pulse when the cursor takes a hit. Win screen runs through
  the same bloom chain (gold title drawn into the scene pass = free glow).

## Sprites / Textures

- raylib has no built-in sprite or animation manager. A sprite is just a
  `Texture2D` drawn as a sub-rect: load a sheet once, then `DrawTextureRec()`
  (or `DrawTexturePro()` for rotation/scale) with a frame rect, advancing a
  timer with `GetFrameTime()`.
- `LoadImageAnim()` loads animated images (e.g. GIF) as a frame sequence.
- Procedural animation: analyze the sprite's pixel geometry once, build a
  horizontal frame sheet in RAM with `ImageDrawImage()` + `ImageDrawPixel()`,
  then cycle with `DrawTextureRec()`. `main.c` does this for the sprite's
  eyes and mouth.
- Current design (2026-08): purple slime regenerated by `gen_sprite.py`
  (stdlib-only PNG writer) — body + outline + gloss highlight, antenna ball,
  feet, white eye ovals. Replaced the original "black ball with yellow dunce
  cap". The magenta-key loop in `main.c` is now vestigial (PNG has real
  alpha) but harmless.
- Geometry contract between `gen_sprite.py` and `main.c`: eyes must stay
  white ovals centered at (26,44) and (38,44) — pupils are hardcoded in
  `main.c` at cols 25-26 / 37-38, rows 43-44, shifted ±2 for gaze; the blink
  frame whites out rows 44-45 across cols 22-42. The mouth is NOT in the
  PNG: `main.c` draws it per frame (closed grin on frames 0/3, open chomp
  with fangs on frames 1-2), so it animates with the gaze cycle.
- Pre-made options:
  - DIY (~40 lines: sheet + frame timer), per official sprite-animation example.
  - `raylib-extras` org: `raytilemap` (Tiled tilemaps), `examples-cpp`
    platformer for sprite animation + state management patterns.
  - `rres` (raysan5/rres): official asset-packing system; rTexGen packs many
    sprites into a single atlas file loaded in one call.
  - Language bindings sometimes add wrappers (e.g. raylib-ruby `Sprite`).

## 3D models / custom lighting (ico.c) — gotchas

- This raylib 6.1 build's default model fragment shader has NO lighting:
  it's just `texelColor*colDiffuse*fragColor`. `DrawModel()` alone renders
  flat unshaded. For lit 3D you must supply a custom shader like ico.c's.
- `normalMatrix` is NOT wired up by this build: declaring it in a vertex
  shader leaves it at the zero matrix, and `mat3(zero)*normal` zeroes all
  fragment normals — result looks like flat ambient only, indistinguishable
  from a missing uniform, on every face. Use `mat3(matModel)` instead
  (fine for pure-rotation transforms; ico.c's tilt has no scale).
- `SetShaderValue` targets whatever program is currently bound. Set custom
  uniforms INSIDE `BeginShaderMode(material.shader)` before the draw, not at
  init (the default shader is active then, so the value silently goes
  nowhere useful).
- `DrawModel` + `DrawModelWires` with the SAME material works for black
  edges: the wire pass tints colDiffuse black, and depth test is LEQUAL so
  the co-planar edges draw over the faces. In the fragment shader, gate
  specular on base luminance or wire edges get a white sheen.
- WSLg/D3D12: neither ffmpeg `x11grab` nor raylib `TakeScreenshot`/
  `LoadImageFromScreen` read the fullscreen GL window's backbuffer (they
  return wrong-resolution black/all-desktop pixels). To verify a render,
  draw into an offscreen `LoadRenderTexture`, then
  `LoadImageFromTexture(rt.texture)` + `ImageFlipVertical` + `ExportImage`.
- Icocahedron geometry (vertex order guaranteeing outward-facing cross
  products) lives in `ico.c` MeshIcosahedron(); the golden-ratio vertex set
  and 20-face index table there were validated numerically.

## ico.c — 1/6-cell placement and MSAA (2026-09)

- Top-left 1/6-cell placement: shift BOTH `camera.position` and
  `camera.target` by the same (camX, camY) so the view axis passes through
  the cell center. Object stays straight-on (no skew) and rotation semantics
  (world-axis spin about -Z view) are unchanged. Shift distances:
  `pps = (sh/2)/(dist*tan(fov/2))`, `camX = (sw/2 - cx)/pps`,
  `camY = (cy - sh/2)/pps` with (cx,cy) the cell center. The camY sign is
  the "wrong" one deliberately — the naive `(sh/2-cy)/pps` lands the object
  in the bottom-left cell because of the y-flip in screen projection.
- Off-axis camera breaks the symmetric worst-case bound: a corner that swings
  toward the camera at an off-center position magnifies more than
  `(sw/2)/tan/sqrt(dist^2-1)` predicts, and an early version of that formula
  UNDER-fitted (silhouette spilled past the cell). Empirically (offline
  Python scan over a 5° rotation grid, incl. the camera offset) the true
  worst-case width still scales as `1/sqrt(dist^2-1)` but with a widest-pair
  factor F ≈ 2.168 baked into the camera-distance formula in `ico.c`. Width
  (not height) is the binding constraint for a 640x540 cell. Measured:
  worst-width 575px (90% of cell) and home-tilt bbox 513x254 at dist 10.0.
- Perspective (near-corner magnification) pushes the silhouette bbox ~55px
  right of the cell's geometric center at the home tilt: expect the object
  to read slightly off-center toward the screen middle, not pixel-centered.
- Edge aliasing fix that WORKS on WSLg/Mesa-D3D12: `SetConfigFlags(
  FLAG_MSAA_4X_HINT)` before `InitWindow` (4x MSAA). Single-pixel stairsteps
  on near-horizontal/vertical edges are gone in the real window.
- Caveat: the offscreen `LoadRenderTexture` verification route does NOT get
  MSAA — exported check images still show aliased edges even when the actual
  window is smooth. Don't judge smoothing from those exports.
- Black `DrawModelWires` pass was removed: with flat per-face normals the
  facets read clearly by tone alone, no edge lines needed.

## polyhedron_attack.c — flat playfield lessons (2026-09)

- **Designing "2D" gameplay in 3D**: the first draft kept the ico.c top-down
  3D camera and let invaders approach from depth (z) while bullets rose in y.
  Result was confusing/disorienting. Fix that holds up: put ALL actors on one
  plane (z=0), camera straight-on (`pos (0,0,28)`, `target (0,0,0)`), and do
  every kind of motion in x/y only. Solids keep their 3D shading, but
  gameplay reads as pure 2D and silhouettes are constant size.
- **Player tetrahedron**: the user wanted a D4 resting on a flat face, apex
  up, bullets out of the apex. Gotcha: a rotation about the world Y axis does
  NOT preserve mirror symmetry about the x=0 plane (Ry and the x-mirror don't
  commute), so choosing the up-rotation by eye always left an off-center
  silhouette. Fix: bake the resting, left-right-symmetric pose straight into
  the mesh generator — apex `(0,1,0)`; base face at y=-1/3 with two mirrored
  vertices at x=±0.8165 and one on-axis vertex at x=0, all at radius 1. No
  model.transform needed, silhouette is a clean isosceles triangle, and a
  center facet ridge reads as a pyramidal face. Player must NOT tumble
  (attackers spinning is fine and looks good).
- **Clip fixes**: (1) cover cubes were `2×2.2×1.4` (read as tall planks) and
  their two stacked cubes overlapped — make them true cubes (2×2×2) and space
  centers ~0.2+ apart. (2) the full-size player tetra (radius 1) apexed
  exactly at the bottom cover face — shrink it (scale 0.7) and sit it lower.
  (3) the saucer sphere at y=8.8 clipped the top invader row (starts y≈7–8)
  — give the saucer its own dedicated track above the formation (y≈9.2) and
  enlarge the backdrop panel to hold it.
- **Growing waves**: formation size is computed from a `wave` counter
  (`SetDifficulty()`: cols 6→7, rows 4→6, capped 7×6=42) instead of fixed
  5×4; a fresh formation only respawns after a ~1.5s pause, which removed the
  "attackers suddenly rush you" feel.
- **Agent process-kill gotcha**: `pkill -f polyhedron_attack` matched the
  agent's own bash command line (it contains the pattern) and killed the
  shell doing the kill → the tool call hung until timeout. Always kill by
  truncated comm name instead: `pkill -x polyhedron_atta` (process names are
  capped at 15 chars; the binary is untruncated when you just launch it).
- Saucer spawn intervals started as 6–13s (felt too frequent); raised to
  20–34s first spawn, 15–33s afterwards. `coin.wav` on spawn read as a bonus
  fanfare.

## mandelbrot.c — GPU fractal explorer (2026-10)

- **Do NOT use `fragTexCoord` as the screen position.** raylib 6.x with
  `SUPPORT_QUADS_DRAW_MODE` makes `DrawRectangle` emit the *shapes atlas*
  texcoords (`rlSetTexture(GetShapesTexture().id)` + `shapeRect/texShapes`,
  see rshapes.c DrawRectanglePro), not 0..1 across the quad — so a fullscreen
  shader fed by the varying samples a tiny sub-window of the atlas. Symptom:
  no black interior anywhere, just a smooth orange gradient (the first build of
  this file rendered "a plausible field of escapes" and nothing else).
  `gl_FragCoord.xy / res` is the robust screen position, with `res` a uniform.
- **`GetScreenWidth()` != the framebuffer right after `SetWindowMonitor()`.**
  raylib logged screen 1920x1080 while the actual drawable was 1680x1050, so a
  `res` uniform captured at startup stretches the whole map. Query
  `GetRenderWidth()/GetRenderHeight()` (they match gl_FragCoord) every frame
  and recompute aspect + home span on change.
- **Verification without being able to see the screen**: dump the backbuffer
  with `TakeScreenshot()` (works, unlike x11grab on WSLg), `ffmpeg -f rawvideo`
  to RGB, then point-test against a double-precision CPU reference — sample
  random (re, im), map to a pixel, compare "black == inside set". The home view
  scored 0.997 (the one miss was a pixel on the boundary). A transposed mapping
  scored 0.871 and a flipped-y mapping 0.997 — Mandelbrot is conjugate-symmetric,
  so **a y-flip cannot be detected this way or by eye**; y symmetry is a free
  pass, not evidence of correctness.
- **Measured limits** (1680x1050, 2080 Super, D3D12/Mesa): 2000 iters = 16.7 ms,
  10000 = 17.1 ms, 20000 = 23 ms. float32 in the shader matches a double
  reference pixel-for-pixel at 2.7e5 zoom (span 1e-5) and 95% at 2.7e6 — so the
  *iteration budget*, not precision, is the real wall: at span 1e-5 the
  Seahorse valley still needs >300k steps. Past the auto cap the view just
  fills black, which looks like a rendering bug but is the cap.
- Drawing straight to the backbuffer (no RenderTexture) sidesteps the
  WSLg/D3D12 RT Y-mirror problem entirely — don't "fix" it by adding an RT hop.
- **Naive float32 escape-time rendering streaks past ~1e5 zoom** (this is the
  "small wide rectangles instead of pixels" report). Ruled out, in order:
  1. Not a block/draw artifact: framebuffer dumps at span 1e-3/1e-4 have ~1px
     run lengths and 2x2-identical fractions consistent with per-pixel noise.
  2. Not the Mesa/D3D12 driver: a **single-precision CPU** render of the same
     view reproduces the anisotropy almost exactly (mean|dy|/mean|dx| = 3.06
     CPU-float vs 3.03 GPU at span 1e-5; a double-precision CPU render of the
     same view gives 1.00).
  3. Not the bailout radius: |z|>16, 256 and 512 all give anisotropy 3.06 —
     the orbit stays O(1) for these pixels, so the round-off is per-step
     relative error amplified by the chaotic derivative, not escape-tail
     magnitude. Direction matters because the unstable direction at this
     boundary point is horizontal.
  So the wall is float32 itself: ~7 digits of mantissa buys ~1e5 zoom with
  smooth coloring (measured clean at span 1e-4 = 2.7e4 zoom, broken at 1e-5 =
  2.7e5). Membership (inside/outside) still agrees with a double reference at
  2.7e5 — only the *color* is noise, so tests that only check membership will
  call a broken image correct. Fixing it properly means perturbation theory
  (reference orbit + rebase) or double-single arithmetic in the loop, ~5x the
  ALU; `mandelbrot.c` warns "past the float32 wall" in the HUD instead.

### fp64 deep-zoom path (supersedes the double-single attempt)

- **Use real fp64 (`dvec2`) in the fragment shader, not double-single (hi/lo
  float pairs).** DS needs error-free transforms, and this GPU's GLSL compiler
  *folds `twoSum` away*: a probe whose residual must be nonzero —
  `(1-(s-bb))+(1e-9-bb)` with `s = fl(1+1e-9) = 1.0`, `bb = 0` — emitted 0,
  while the `twoProd` residual in the same shader survived (binary step test:
  twoProd 255, twoProdTemp 255, twoSum 0). The algorithm was not at fault: the
  identical C mirror, including *both* `twoSum(p1.x-p2.x)` and the `twoProd`
  residuals `p1.y ± p2.y`, matches `__float128` at span 1e-5 (colour error 0.3,
  vs float32's 22.8). Note that patching only the `twoSum` residual (keeping
  `hi = p1.x - p2.x`) reproduces the float32 floor exactly: dropping either
  residual is what quantises the orbit. A driver that reassociates additions
  cannot be trusted with EFTs; fp64 needs none, and is less code.
- **fp64 requires `#version 400 core`**, and `precise` is a reserved word from
  GLSL 400 on, so the path-switch uniform is `useDouble`. GLSL 330 has no
  doubles and no `fma()` (compile error: "no function with name 'fma'").
- **raylib 6 has no double uniform API**: neither `SetShaderValue` nor
  `rlSetUniform` handles doubles, and client code gets no GL prototypes at all
  (rlgl.h includes glad.h only for its own build — `GRAPHICS_API_OPENGL_33` is
  not defined for consumers, so `glUniform2d` is an implicit-declaration
  error). Fix: `#include "external/glad.h"` in `mandelbrot.c` and call
  `glUniform2d` inside `BeginShaderMode()`. `glad_glUniform2d` is exported from
  libraylib.a and filled in by GLAD's GL 4.0 loader.
- **Measured accuracy** (1680x1050, D3D12/Mesa, centre -0.743643887037151
  0.131825904205330, against a `__float128` CPU reference; membership errors
  out of 30000 px, then mean channel error over shared escaping pixels):

  | span (zoom) | fp64 | float32 |
  |---|---|---|
  | 1e-3 (3e3) | not needed | 0 err, 8.8 |
  | 1e-4 (3e4) | 0 err, 0.62 | 54 err, 18.2 |
  | 1e-5 (3e5) | 16/120000, 1.05 | 2533/120000, 50.1 |
  | 1e-8 (3e8) | 0 err, 1.24 | streaked |
  | 1e-10 | 0 err, 4.24 | streaked |
  | 1e-12 | 0 err, 11.27 | streaked |

  fp64 is membership-exact through 1e-12; only the colour error grows (float
  smooth-colour maths plus fp64 rounding). Auto crossover `PRECISE_ZOOM 2e4` sits
  where float32 starts to break.
- **The fp64 iteration cap is nearly free** — frame cost tracks the *average*
  escape count, not `maxIter`: at span 1e-8, 8000 iterations and 1e6 iterations
  both cost ~135 ms/frame, because only pixels that never escape pay for the
  cap. So `PRECISE_ITER` went 4000 -> 40000 and `AutoIterations` is no longer
  truncated (it wants 7427 at 1e-5, 11414 at 1e-8, 16729 at 1e-12). Cost:
  ~26-30 ms/frame float32 shallow, ~130-230 ms/frame fp64 deep at 1680x1050.
- **Screenshot/reference comparisons must flip rows.** `TakeScreenshot` output
  is top-down and the view maps growing `im` upward, so reference row `Y0+j` is
  PNG row `H-1-(Y0+j)`; the crop is `[H-Y0-CH : H-Y0]`, then reversed. The trap:
  the per-row brightness profile correlates ~0.99 in *both* orientations (the
  profile is nearly symmetric), so a row-profile check happily passes while
  every pixel comparison is garbage — only asymmetric crops (off-axis view,
  deep enough to have vertical structure) expose it. Calibrate the comparison
  on a shallow off-axis view at low iterations first, where float32 and
  `__float128` agree exactly.
- The CPU reference renderer also had its float32 mode left on the pre-fix
  `(0.5-uy)` y convention while its double/`__float128` modes used `(uy-0.5)`,
  so a float32 reference matched a *broken* GPU image. All modes must use the
  same convention; check `double vs __float128` agreement (0.19 mean channel
  error at span 1e-5) before trusting any cross-check.

### Colour collapses at depth (the `nu/maxIter` trap)

- Reported as "all the colours look the same near the zoom limit". The cause is
  the normalisation, not the palette: `t = sqrt(nu/maxIter)` is scaled by the
  iteration *cap*, but the escape counts actually on screen sit far below it.
  Measured percentiles of `nu` on screen at `maxIter=40000`:
  p50/p95 = 97/315 at span 1e-3, 844/- at 1e-6, 1804/4231 at 1e-10,
  2207/4677 at 1e-12. So `t` never passes ~0.24, `fract(t*4+0.55)` uses barely
  a quarter of the wheel, and one hue cycle spans thousands of iterations
  (~15000 at span 1e-10) — neighbours are indistinguishable.
  Note this is not a deep-only bug: it is true at every zoom, it just becomes
  obvious once the fine structure appears.
- Fix: fade in a per-iteration band term with `nu` itself as the clock, which
  needs no calibration —
  `amp = smoothstep(400, 1500, nu)`, hue `fract(t*4 + 0.55 + amp*nu*0.04)`.
  Off below ~400 so shallow views keep the smooth ramp, full by ~1500 (span
  1e-6 up), where one hue cycle = 1/0.04 = 25 iterations.
  Measured 4-pixel-scale contrast at span 1e-10: 11.6 -> 40.3 (3.5x), with
  only 2.2% of pixels moved at span 1e-3 (mean channel 1.7) and 5.7% at 1e-4.
  Ramps of smoothstep(120,600) / (300,1200) / (250,1000) were also measured:
  they change shallow views 8-19% for no extra deep gain, so (400,1500) is the
  right knee. `0.04` is the knob for band width — 0.02 wider, 0.08 finer.
- **Banding invalidates the colour half of the reference comparison.** Mean
  channel distance vs the CPU reference at span 1e-10 went 4.24 -> 34.76 while
  membership stayed exact (0/30000): a hue band is 25 iterations wide, so a
  +/-1 iteration difference between two correct renderers now shows up as a
  visible band. Compare *membership*, or compare raw `nu` (dump the shader's
  `nu` as two base-251 digits and diff against the CPU counts). At span 1e-10
  the GPU's `nu` matches the CPU median exactly and ~39% of pixels land within
  the expected 1-iteration smooth-count offset; the rest is chaotic
  amplification, which separates any two correct renderers at that depth.
  Keep `refq.c`'s colouring in sync with the shader or this metric is nonsense.

- The verification tooling is throwaway and lives outside the repo in
  `/tmp/opencode/mtest` (`mkharness.py` renders any candidate copy of
  `mandelbrot.c` headless at a given centre/span/zoom/iterations,
  `refq*.c` renders the CPU `__float128` reference, `cmp.py` diffs the two).
  It is not committed and `/tmp` is not preserved, so expect to rewrite it.
  Patch a candidate shader by rewriting the C string literal in the *copy*,
  then point `mkharness.py` at that copy — it reads the path it is given.
