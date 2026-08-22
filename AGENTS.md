# raylib-test

Personal raylib experiment playground. Small single-file C programs testing
audio, networking, and fullscreen behavior. Keep changes small.

## Conventions

- If the user sends any message after the agent started `net_test`, assume
  they killed it — verify with `pgrep -x net_test` instead of assuming it's
  still running.

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
(`audio_test`, `fullscreen_test`, `net_test`, `*.o`) are gitignored; commit
source only. `net_test` also compiles `hide_cursor_x11.c` (see `NOTES.md`).

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
