# raylib-test

Personal raylib experiment playground. Small single-file C programs testing
audio, networking, and fullscreen behavior. Keep changes small.

## Files

- `main.c` — fullscreen UDP ping test: sends "ping" on SPACE, plays `coin.wav`
  on reply, plays `buttonfx.wav` on a clickable box. Server address is hardcoded.
- `audio_test.c` — audio device init and sound playback check.
- `resources/` — sound effects used by the above.

## Building

Binaries are compiled by statically linking against the sibling raylib clone at
`~/raylib` (`src/raylib.h`, `src/libraylib.a`). Compiled binaries
(`audio_test`, `fullscreen_test`, `net_test`, `*.o`) are gitignored; commit
source only.

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

## Sibling repos

- `~/raylib` — upstream `raysan5/raylib` clone, not personal, do not push.
- `~/factorio-rcon-bot` — public; Jimbo Factorio bot with its own AGENTS.md.
- `~/system-administration` — private; host-level notes, `agent-guidance/COMMON.md`
  (source of personal working agreements), and the `push` script at `bin/push`.
- `~/project-ideas` — public.

`~/.local/bin/push` (symlinked to `system-administration/bin/push`) pushes every
repo whose origin is `github.com/david-battle/*`. Do not push unless asked.
