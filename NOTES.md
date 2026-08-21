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
