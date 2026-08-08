# Ideas & Notes

Future experiment ideas from design discussions. Not build instructions.

## Audio

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

## Sprites / Textures

- raylib has no built-in sprite or animation manager. A sprite is just a
  `Texture2D` drawn as a sub-rect: load a sheet once, then `DrawTextureRec()`
  (or `DrawTexturePro()` for rotation/scale) with a frame rect, advancing a
  timer with `GetFrameTime()`.
- `LoadImageAnim()` loads animated images (e.g. GIF) as a frame sequence.
- Pre-made options:
  - DIY (~40 lines: sheet + frame timer), per official sprite-animation example.
  - `raylib-extras` org: `raytilemap` (Tiled tilemaps), `examples-cpp`
    platformer for sprite animation + state management patterns.
  - `rres` (raysan5/rres): official asset-packing system; rTexGen packs many
    sprites into a single atlas file loaded in one call.
  - Language bindings sometimes add wrappers (e.g. raylib-ruby `Sprite`).
