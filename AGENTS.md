# raylib-test

Personal raylib experiment playground. Small single-file C programs testing
audio, networking, and fullscreen behavior. Keep changes small.

## Files

- `main.c` — fullscreen mini-game (catch-the-sprite) + UDP ping test: sprite
  flees the mouse and shoots dots at it; click the sprite to score. First to 12
  wins. Each win raises the level and dots home harder (resets on a loss or a
  shutout). Win screen plays `resources/country.mp3` until a key is pressed;
  shutouts ("Sprite: 0") get extra confetti. Ping and click-box UI is compiled
  out by default; build with `-DSHOW_UI` to restore. Plays `resources/` sounds
  for shooting, hits, clicks, and UDP echo replies (server `34.3.109.195:7777`).
  Sprite animation (pupils + blink) is generated procedurally at load.
- `audio_test.c` — plays a sound file given as a path argument, capped at 5s
  (no window needed; avoids `WaitTime` which hangs without one).
- `hide_cursor_x11.c` — attempts to hide the system cursor on WSLg (see
  `NOTES.md`; currently unresolved, cursor stays visible).
- `play_all.sh` — plays every sound in `resources/` via `audio_test`.
- `resources/` — sound effects (copied from raylib examples and used by
  `main.c`) and `sprite.png`.

## Building

Binaries are compiled by statically linking against the sibling raylib clone at
`~/raylib` (`src/raylib.h`, `src/libraylib.a`). Compiled binaries
(`audio_test`, `fullscreen_test`, `net_test`, `*.o`) are gitignored; commit
source only. `net_test` also compiles `hide_cursor_x11.c` (see `NOTES.md`).

## Echo server (for `main.c`)

The UDP echo server `main.c` pings lives on the gcloud instance `udp-test`
(zone `us-west1-a`, project `plasma-sol-276402`) at `34.3.109.195:7777`.
Firewall rule `allow-udp-7777` opens the port.

- Deployed without SSH via instance metadata `startup-script` (runs as root on
  every boot): rebuilds `/home/dlbattle/.ssh/authorized_keys` from instance +
  project metadata keys, then installs the `udp-echo` systemd service running
  `/opt/udp_echo.py` (a simple UDP echo loopback on 7777).
- SSH: `gcloud compute ssh udp-test --zone=us-west1-a` (or the `udpgc` helper
  in `~/.local/bin`). Uses the ed25519 key at `~/.ssh/google_compute_engine`
  (the old RSA key is backed up at `~/.ssh/google_compute_engine.rsa.bak`).
  The Debian 12 sshd rejects plain `ssh-rsa` SHA-1 signatures, so the key must
  be ed25519/ecdsa.
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

## Wrap-up procedure

End of a session: commit source, stop the cloud instance.

1. Check state: `git status` / `git diff` (binaries are gitignored — source
   only).
2. Commit (`git add -A` then `git commit`). Do not push unless asked.
3. Stop the instance to avoid billing (command in "Start / stop" above); note
   the ping in `main.c` won't respond until it's started again.

## Sibling repos

- `~/raylib` — upstream `raysan5/raylib` clone, not personal, do not push.
- `~/factorio-rcon-bot` — public; Jimbo Factorio bot with its own AGENTS.md.
- `~/system-administration` — private; host-level notes, `agent-guidance/COMMON.md`
  (source of personal working agreements), and the `push` script at `bin/push`.
- `~/project-ideas` — public.

`~/.local/bin/push` (symlinked to `system-administration/bin/push`) pushes every
repo whose origin is `github.com/david-battle/*`. Do not push unless asked.
