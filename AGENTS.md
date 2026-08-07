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

## Sibling repos

- `~/raylib` — upstream `raysan5/raylib` clone, not personal, do not push.
- `~/factorio-rcon-bot` — public; Jimbo Factorio bot with its own AGENTS.md.
- `~/system-administration` — private; host-level notes, `agent-guidance/COMMON.md`
  (source of personal working agreements), and the `push` script at `bin/push`.
- `~/project-ideas` — public.

`~/.local/bin/push` (symlinked to `system-administration/bin/push`) pushes every
repo whose origin is `github.com/david-battle/*`. Do not push unless asked.
