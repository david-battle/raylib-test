# Ideas & Notes

Future experiment ideas from design discussions. Not build instructions.

## Verifying the UDP echo server (quick)

Single external round-trip test (this is the reliable fast check):

```bash
timeout 5 python3 -c "import socket;s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.settimeout(3);s.sendto(b'ping',('34.3.109.195',7777));print('ECHO OK:',s.recvfrom(1024)[0])"
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

## Custom cursor on WSLg — UNRESOLVED (rat hole)

Goal: hide the system cursor in `main.c` and draw a custom one. **Nothing
worked; cursor stays visible.** Status quo: `main.c` calls `HideCursor()` every
frame and `hide_cursor_x11.c` defines an invisible X11 cursor on the window.
Build with `gcc -I ~/raylib/src main.c hide_cursor_x11.c -o net_test
~/raylib/src/libraylib.a -lm -lpthread -ldl -lX11`.

- Environment: WSL2 + WSLg. raylib uses GLFW over X11 (Xwayland). The Windows
  compositor draws the cursor, so X11/GLFW cursor hiding is ignored.
- Tried: `HideCursor()` (reports `IsCursorHidden()=1` but no visual change),
  calling it every frame, and a 1x1 transparent cursor via `XDefineCursor`
  (XCreatePixmapCursor). All no-ops visually.
- The bundled raylib GLFW fork (`rglfw`) does NOT export `glfwSetCursor` (it's
  an undefined symbol), so the classic "attach empty cursor via glfwSetCursor"
  trick is unavailable without compiling the full GLFW sources.
- `raylib.h` and Xlib both define `Font`, so X11 code must live in its own
  translation unit (`hide_cursor_x11.c`) — you can't `#include <X11/Xlib.h>`
  in the same file as `raylib.h`.
- Uninvestigated ideas for later: bump WSLg/WSL (bug may be version-specific),
  Xwayland `-cursor` option, Wayland-native raylib backend, or draw the custom
  cursor and accept the system one on top.

## Sprites / Textures

- raylib has no built-in sprite or animation manager. A sprite is just a
  `Texture2D` drawn as a sub-rect: load a sheet once, then `DrawTextureRec()`
  (or `DrawTexturePro()` for rotation/scale) with a frame rect, advancing a
  timer with `GetFrameTime()`.
- `LoadImageAnim()` loads animated images (e.g. GIF) as a frame sequence.
- Procedural animation: analyze the sprite's pixel geometry once, build a
  horizontal frame sheet in RAM with `ImageDrawImage()` + `ImageDrawPixel()`,
  then cycle with `DrawTextureRec()`. `main.c` does this for the sprite's eyes.
- Gotcha: the sprite's eyes are **solid white ovals with no visible pupils**;
  the black gap between them is the face, not a pupil. Animating that gap just
  looks like a moving glasses bridge. Correct approach: draw 2x2 black pupils
  inside the ovals and shift them (plus a blink frame), with a permanent white
  bridge between the eyes for a "glasses" look.
- Pre-made options:
  - DIY (~40 lines: sheet + frame timer), per official sprite-animation example.
  - `raylib-extras` org: `raytilemap` (Tiled tilemaps), `examples-cpp`
    platformer for sprite animation + state management patterns.
  - `rres` (raysan5/rres): official asset-packing system; rTexGen packs many
    sprites into a single atlas file loaded in one call.
  - Language bindings sometimes add wrappers (e.g. raylib-ruby `Sprite`).
