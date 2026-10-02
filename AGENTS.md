# raylib-test

Personal raylib experiment playground. Small single-file C programs testing
audio, networking, and fullscreen behavior. Keep changes small.

## Conventions

- If the user sends any message after the agent started a program (e.g.
  `net_test`), assume they exited it — verify with `pgrep` instead of assuming
  it's still running. If a fresh copy needs to run and the old one is still
  alive, silently kill it first; never ask, and never offer to kill it. The
  user can kill programs themselves and won't request permission.

## Files

- `main.c` — fullscreen mini-game (catch-the-sprite) + UDP ping test: sprite
  flees the mouse and shoots dots at it; click the sprite to score. First to 12
  wins. Each win raises the level and dots home harder (resets on a loss or a
  shutout). Win screen plays `resources/country.mp3` until a key is pressed;
  shutouts ("Sprite: 0") get extra confetti. Ping and click-box UI is compiled
  out by default; build with `-DSHOW_UI` to restore. Plays `resources/` sounds
  for shooting, hits, clicks, UDP pings, and UDP echo replies. Rendering goes through an
  embedded GLSL 330 bloom pipeline (shader strings in main.c; HUD draws after
  composite). WSLg presents render-target chains Y-mirrored — the un-flip in
  the composite shader must stay (see NOTES.md). The echo server IP is
  ephemeral and changes on each instance start; run via `./run_net_test.sh`,
  which resolves the current external IP from gcloud and passes it as
  `argv[1]` (`./net_test <ip>` works directly too; default is the old IP).
  Sprite animation (pupils, blink, and chomping mouth) is generated
  procedurally at load.
- `audio_test.c` — plays a sound file given as a path argument, capped at 5s
  (no window needed; avoids `WaitTime` which hangs without one).
- `ico.c` / `ico` — fullscreen display of all five platonic solids plus a
  sphere, one per cell of an implicit 3x2 grid (6 sections). Ordered by
  increasing face count left-to-right / top-to-bottom: tetrahedron (4), cube
  (6), octahedron (8), dodecahedron (12), icosahedron (20), sphere. Each has a
  distinct jewel tone (ruby, emerald, sapphire, amethyst, topaz, citrine).
  Meshes are procedurally generated flat-shaded polyhedra (tetra/cube/octa/
  icosa vertex-face tables in source; dodecahedron built at runtime as the
  icosahedron's dual); custom GLSL 330 lighting shader. Uses the `cell.c`-style
  external-monitor fullscreen setup; each cell gets its own on-axis camera so
  objects scale identically. `Tab` cycles which section the arrow keys rotate
  (left/right spin about the vertical axis, up/down tip about the horizontal);
  a thick dark box marks the active cell. ESC exits.
- `polyhedron_attack.c` / `polyhedron_attack` — fullscreen Space Invaders-style
  shooter reusing the `ico.c` shader + mesh machinery. A 5x4 formation marches
  side to side and descends; the top two rows are dodecahedrons (50 pts), the
  bottom two octahedrons (30 pts). A golden sphere saucer occasionally flies
  across the top (200 pts). The player is a blue tetrahedron at the bottom,
  moved with left/right arrows, firing a single cylinder bullet with SPACE;
  enemy cylinders drop from surviving attackers. Steel-grey cube bunkers (four
  sites, two cubes tall, 2 hp each) block and erode. 3 lives with a 2s respawn
  shield; clearing a wave pays +500; ENTER restarts after game over. Borrows
  `resources/` sounds like `main.c` (`weird.wav` while firing, `hit_splat.wav`
  on kills, `buttonfx.wav` on cover dents, `boom.wav` on player hit, a quiet
  `ping_send.wav` per enemy shot, `coin.wav` when the saucer spawns). Build
  with the same `gcc -I ~/raylib/src ... libraylib.a -lm -lpthread -ldl -lX11`
  line as `ico`.
- `mandelbrot.c` / `mandelbrot` — fullscreen Mandelbrot explorer computed per
  pixel by one fullscreen fragment shader drawn straight into the backbuffer
  (no RenderTexture, so the WSLg RT Y-mirror doesn't apply; raylib's OpenGL
  backend has no compute shaders). Smooth/continuous escape-time hue, interior
  black. Wheel zooms toward the cursor, left-drag pans, SPACE resets to the
  whole set, F1 toggles the HUD, +/- override the iteration limit, `A` hands it
  back to auto, `P` forces the precision path. Drawn view glides toward the
  target view. Two things NOT to undo: the shader maps pixels via
  `gl_FragCoord/res`, never `fragTexCoord` (raylib 6's `DrawRectangle` emits
  shapes-atlas UVs), and the framebuffer size comes from
  `GetRenderWidth/Height()` per frame, not `GetScreenWidth()`. Colour is
  escape-time hue (`nu/maxIter`) plus a depth-faded per-iteration band term
  keyed on `nu` itself — key it on `nu/maxIter` instead and every view washes
  out, because the escape counts on screen sit far below the cap. Don't
  normalise it away; reasoning and measurements in `NOTES.md`.
  Precision: shallow views run in plain float32 (~60 fps); past
  `PRECISE_ZOOM` (2e4 zoom) the same shader runs the orbit in real fp64
  (`dvec2`, `#version 400 core`, `P` forces it) and is membership-exact to
  1e-12 span at ~130-230 ms/frame. Do NOT "optimise" that path back into
  double-single hi/lo float pairs — this GPU's GLSL compiler folds the
  `twoSum` error-free transform away, so DS silently degrades to float32.
  Don't raise `PRECISE_ITER` past ~20000 either: interior pixels pay the whole
  cap, and a deep fp64 view with an interior-heavy frame hits the ~2 s GPU
  watchdog, loses the context, and goes permanently black (looks like a shader
  bug; see `NOTES.md`).
  raylib has no double uniform API: the file includes `external/glad.h` and
  sets the `dvec2` uniforms with `glUniform2d`. The orbit is exact (double and
  `__float128` agree bit-for-bit); what limits deep views is the last
  coordinate add rounding to ulp(0.74), ~7e-4 px at span 1e-10. Don't go
  hunting for more precision. Measurements in `NOTES.md`.
  Builds with the same gcc line as `ico`; run as `./mandelbrot`.
- `mandelbrot_ref.c` / `mandelbrot_harness.py` / `mandelbrot_check.py` —
  verification trio for the shader, use before believing any change to it.
  `mandelbrot_ref.c` is a CPU oracle (same orbit + colouring, in float32 /
  double / `__float128` via `-mode`, `-counts 1` for raw escape counts),
  built with `gcc -O2 mandelbrot_ref.c -o mandelbrot_ref -lquadmath -lm`.
  `mandelbrot_harness.py <src.c> <out.c>` rewrites the frame loop of a *copy* of
  `mandelbrot.c` into a headless one-shot renderer (string surgery: it fails
  loudly at compile time if that loop changes, which is the intended signal) and
  dumps `out.png` plus `out.raw`; compile that copy with the `ico` gcc line.
  `mandelbrot_check.py` diffs the two dumps with the stdlib only and reports
  membership errors plus the mean channel error. Read `mem`, not the colour
  number, on deep views: see `NOTES.md` for why, and for the RGBA/top-down vs
  RGB/bottom-up row trap.

- `cell.c` / `cell` — fullscreen Conway's Life editor with a sparse, unbounded
  grid (only non-empty cells stored; max ~2^53 coordinate range via double
  camera in `main.c`-style fullscreen setup). 3-color states (red/green/blue)
  cycled per channel on left/middle/right click; color is a passive layer.
  Left-drag selects a rect (animated dashed box), Ctrl+C/X copy/cut it into an
  internal clipboard, and Ctrl+V or a bare left click paste it (paste writes
  only live cells). Press `G` to load the Gosper glider gun (36x9) into the
  clipboard. Numpad +/- scales gens/sec (cap 2400), space pauses, `.` single-
  steps, Q clears clipboard/selection, F1 toggles HUD. Compiles against
  `~/raylib` and `hide_cursor_x11.c`, run as `./cell`.
- `hide_cursor_x11.c` — replaces the system cursor with the X cursor-font
  `XC_target` reticle on WSLg (true invisibility impossible; see `NOTES.md`).
  Must not call raylib's `HideCursor()`, which would override it. `main.c`
  draws nothing at the mouse — the cursor is the aim marker.
- `analyze_audio.py` — stdlib-only spectral analysis (FFT report + ASCII
  spectrogram) of any sound file via ffmpeg; used to "listen" to audio.
- `play_all.sh` — plays every sound in `resources/` via `audio_test`.
- `gen_sprite.py` — regenerates `resources/sprite.png` (body/antenna/feet/
  eye whites only; pupils and mouth are drawn per-frame by `main.c`, so the
  eye geometry in the script must not move — see NOTES.md).
- `resources/` — sound effects (`hit_splat.wav`, `weird.wav` are synthesized;
  see NOTES.md provenance), `sprite.png`, and `C5_512Hz.wav` (test tone).

## Building

Binaries are compiled by statically linking against the sibling raylib clone at
`~/raylib` (`src/raylib.h`, `src/libraylib.a`). Compiled binaries
(`audio_test`, `fullscreen_test`, `net_test`, `mandelbrot`, `mandelbrot_ref`,
`*.o`) are gitignored; commit source only. `net_test` also compiles `hide_cursor_x11.c`
(see `NOTES.md`).

## Echo server (for `main.c`)

The UDP echo server `main.c` pings lives on the gcloud instance `udp-test`
(zone `us-west1-a`, project `plasma-sol-276402`). Its external IP is ephemeral
(currently `35.212.149.98`); resolve it with `./run_net_test.sh` or
`gcloud compute instances list ... --format="get(networkInterfaces[0].accessConfigs[0].natIP)"`.
Firewall rule `allow-udp-7777` opens the port.

- Deployed without SSH via instance metadata `startup-script` (runs as root on
  every boot): rebuilds `/home/dlbattle/.ssh/authorized_keys` from instance +
  project metadata keys, then installs the `udp-echo` systemd service running
  `/opt/udp_echo.py` (a simple UDP echo loopback on 7777) and the `http-echo`
  systemd service running `/opt/http_echo.py` (HTTP "date" server on port 80,
  replies with the output of the linux `date` command for any request).
  Firewall rules `allow-udp-7777` and `allow-tcp-80` open the ports.
- SSH: `gcloud compute ssh udp-test --zone=us-west1-a` (or the `udpgc` helper
  in `~/.local/bin`). Uses the ed25519 key at `~/.ssh/google_compute_engine`
  (the old RSA key is backed up at `~/.ssh/google_compute_engine.rsa.bak`).
  The Debian 12 sshd rejects plain `ssh-rsa` SHA-1 signatures, so the key must
  be ed25519/ecdsa.
- Non-interactive remote commands from the agent work fine — no startup-script
  hacks needed. Exact procedure:
  ```
  gcloud compute ssh udp-test --zone=us-west1-a --project=plasma-sol-276402 \
    --command="systemctl is-active udp-echo"
  ```
  Notes: requires the instance to be running and fully booted (~20s after
  start; sshd refuses connections until then — "connection refused" right
  after a start just means boot isn't done). If output looks noisy, filter
  with `grep -v "^Warning\|^Updating\|^Waiting"`. Serial console for
  debugging boot/startup-script issues:
  `gcloud compute instances get-serial-port-output udp-test --zone=us-west1-a
  --project=plasma-sol-276402`.
- If the VM is ever recreated, re-add the `startup-script` metadata (script in
  `~/raylib-test` history) to restore SSH keys and the echo server.
- Quick verify steps (round-trip test + SSH checks) are in `NOTES.md`.

## Start / stop the instance

`udp-test` is a billable e2-small; leave it **stopped** when not needed. The
echo server only responds while it runs, so `main.c`'s ping needs it started.

- Start: `gcloud compute instances start udp-test --zone=us-west1-a --project=plasma-sol-276402`
  (the `startup-script` metadata re-installs the `udp-echo` service on boot).
- Stop: `gcloud compute instances stop udp-test --zone=us-west1-a --project=plasma-sol-276402`
- State: `gcloud compute instances list --project=plasma-sol-276402 --filter="name=udp-test" --format="table(name,status,zone)"`
- As of 2026-08, the instance is currently **stopped (TERMINATED)**.

## Handoff procedure

Triggered by the user saying "handoff". End of a session. The agent does
everything here; the user pushes out of context afterward — **never push**.

1. Kill stray processes: `pkill -x net_test` (holds an X window + audio
   device).
2. Triage stray files: check `git status` for untracked files and leftovers.
   For EACH one make an executive decision without asking the user:
   - `git add` it if it's real content (source, docs, resources)
   - add a `.gitignore` rule if it's a recurring build artifact or local
     scratch that should stay on disk
   - delete it if it's leftover junk (temp files, failed experiments with no
     future, stale backups)
   Never leave untracked files unclassified, and never ask which to do.
3. Verify clean source: `git status` / `git diff`; commit source only.
4. Sanity-build if any C files changed (the command in `run_net_test.sh`) so
   committed source always compiles.
5. Commit (`git commit`, message style: short imperative summary, e.g. "Turn
   network test into catch-the-sprite game").
6. Stop the cloud instance (command in "Start / stop" above) to avoid billing.
   The external IP is ephemeral; next session resolves it via
   `./run_net_test.sh`.
7. Leave breadcrumbs BEFORE committing: non-obvious findings go in NOTES.md
   (asset provenance, rat-hole conclusions, gotchas). AGENTS.md gets only
   facts that change how a future session works.

## Sibling repos

- `~/raylib` — upstream `raysan5/raylib` clone, not personal, do not push.
- `~/factorio-rcon-bot` — public; Jimbo Factorio bot with its own AGENTS.md.
- `~/system-administration` — private; host-level notes, `agent-guidance/COMMON.md`
  (source of personal working agreements), and the `push` script at `bin/push`.
- `~/project-ideas` — public.

`~/.local/bin/push` (symlinked to `system-administration/bin/push`) pushes every
repo whose origin is `github.com/david-battle/*`. Do not push unless asked.
