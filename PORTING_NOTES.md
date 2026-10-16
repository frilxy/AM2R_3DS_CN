# Nintendo 3DS port (PLATFORM=n3ds)

From upstream [ButterscotchRunner/Butterscotch](https://github.com/ButterscotchRunner/Butterscotch) `main` at `d8575ec`
(this repository's `main` was the `port/n3ds` branch). 3DS backend from
[Project-Sunshine-Native/cinnamon](https://github.com/Project-Sunshine-Native/cinnamon) `UNDERTALE-3DS` (`f139a60`).
Target: AM2R Community Updates 1.5.5 (GameMaker: Studio 1.4.1763, WAD 15/16; the main target since 2026-10-07) and
AM2R 1.1 (WAD 14); the AM2R build enables both. New 3DS (804 MHz, L2 cache, 124 MB mode, core 2).

## Build and run

```bash
tools/n3ds/build.sh ../build-n3ds           # -> am2r.cia + am2r.3dsx (BS_N3DS_GAME=generic: plain runner)
tools/n3ds/make_sd.sh <game dir> <out>      # -> <out>/3ds/am2r/: data.win, lang/, gfx/, audio/, config.ini seed
tools/web/build.sh                          # -> build-web/index.html: the SD card maker page (needs emcc and node)
```

The program ships in the CIA (title `AM2R`, unique ID `0xA2E21`); the SD folder `sdmc:/3ds/am2r/` holds the game's
static files (`data.win`, `lang/`, preprocessed `gfx/` and `audio/`) plus saves and `log.txt`. The CIA checks the
folder's `sd_data_rev.txt` against `tools/n3ds/sd_data_rev.txt` and stops with a message if the card is stale, so a
preprocessor change means a new revision and a new SD folder.

CI (`.github/workflows/build-n3ds.yml`): every push builds the AM2R profile in the `devkitpro/devkitarm` container
(makerom built from 3DSGuy/Project_CTR at a pinned tag) and uploads `am2r.cia`, `am2r.3dsx` and `am2r.elf` as the
`am2r-3ds` artifact. Every passing run except pull requests and `claude/*` branches is also a GitHub release with those files and a QR code
of the CIA's download link for FBI (Remote Install -> Scan QR Code): branch pushes as `am2r-3ds-b<run number>`
(pre-releases except on `main`, so `releases/latest/download/am2r.cia` is the newest `main` build), pushed
tags and releases made on GitHub under their own tag.

Emulator loop (Windows host, WSL build): `tools/n3ds/run_in_azahar.ps1` installs the CIA into Azahar (New 3DS mode,
Vulkan) and collects `log.txt`, screenshots and Azahar's log (`-Harness`, `-Save`, `-ClearSaves`, `-Inputs`,
`-WaitSeconds` hard limit, `-CpuClock`: **100 approximates hardware**; 300 hides CPU-bound problems).
`harness.txt` = scripted presses / self-screenshots / exit, with `after_room` anchors (`hwcheck.txt`: title, load,
pause, walk; `pause.txt`; `explore.txt`); `goto <frame> <room>` and `place <frame> <x> <y>` (moves Samus) reach a
room to test without playing to it; `seed <n>` fixes the random seed (`randomize()` then does nothing), so two runs
draw the same frames. `profrooms_cu.txt` (1.5.5: new game in slot A, intro, then the big rooms with a screenshot each;
`profrooms_cu_seeded.txt` with a seed) and `profrooms.txt` / `bigrooms.txt` (1.1, from slot A's save) are the
performance runs. For 1.5.5, delete `save*` in Azahar's `sdmc/3ds/am2r/` before a run (`-ClearSaves` also deletes
`config.ini`, which changes 1.5's first boot). `inputs.json` = keyboard playback in the desktop `--playback-inputs`
format with a fixed seed; `tools/n3ds/run_tas.ps1` (and `run_tas.bat`, drag a `.ltm` onto it) replays a libTAS
movie via `ltm_to_inputs.py`.

Profiling: `tools/n3ds/build.sh ../build-n3ds-prof -DENABLE_VM_GML_PROFILER=ON -DN3DS_FRAME_PROFILER=ON` logs, on
every room change, a `Room perf:` line and a `Profile:` breakdown of the room's frames: exclusive CPU time per zone
(GML step, frame start, view drawing, GUI, overlays, and inside them sprites, text, surfaces, GPU flushes, texture
loads; tiles are counted, not timed: reading the clock is a syscall and Azahar charges 150 cycles for each), then the
40 heaviest GML scripts/events with their builtins. Without the frame profiler, `ENABLE_VM_GML_PROFILER` logs a GML
profile every 120 frames. `-DN3DS_NO_TILE_CULL=ON` draws every tile (to check culling against). The texture cache
logs page imports and evictions in the profiler build.
`tools/n3ds/compare_rooms.py` screenshots every room (`--match`/`--rooms`) on the desktop runner (under Xvfb, with
`--goto-room`) and in Azahar (Linux AppImage, screen mode 1x), pairs the shots by the room each was really taken in,
and writes `report.html` ranked by difference (desktop | 3DS | difference). `harness_to_desktop.py` turns `goto`
into the desktop's `--goto-room <frame>:<room>`.

## Interface adaptations (Cinnamon fork-era -> upstream)

- Vtable: `endFrame` -> `endFrameInit`/`endFrameEnd`; `drawTiled` -> `drawSpriteTiled`; `prewarmRoom` slot gone
  (main calls `N3DSRenderer_prewarmRoom` on room change); `drawTriangle` takes per-vertex colours; `drawText*`
  take `lineSeparation`; `beginGUI` takes a target surface; `gpuSetBlendModeExt` has alpha factors; surface API
  rewritten (`setRenderTarget`, `ensureApplicationSurface`, `drawSurface` with a source rect, ...).
- View mapping: computed from upstream's world-view-projection matrix and the viewport (x and y map
  independently; rotated views are logged as UNIMPL). `MATRIX_*` start as identity.
- Surfaces: citro3d render targets in VRAM (citro3d can't render into linear memory). Freed ones go back to a
  small pool two frames after `surface_free` (citro3d panics on in-frame deletes) and are reused by the next
  `surface_create` of that size: AM2R recreates some every frame, which fragmented VRAM. VRAM is two 3 MB banks
  and a target must fit in one; a surface that doesn't fit at RGBA8 drops to RGBA5551, then to a smaller stored
  area (top-left part). Over 1024 only the top-left 1024 is stored (PICA limit). AM2R's 2048x1024 pause map ends
  up as 1024x512 RGBA5551, which holds the whole map (it uses about 600x464).
- Screen: AM2R lays out its own picture in the window it is told about (application surface and HUD at its own
  offsets), so the window the game sees is the game's size and the renderer scales that to the screen. In AM2R the
  mode is picked in Options -> Display -> Screen: Stretch (default), 1x (centred 1:1), 2x (doubled and centred) or
  Wide (upstream's widescreen hack); 1x/2x use `N3DSRenderer_setFixedScale`. AM2R 1.5 has its own widescreen
  (`oControl.widescreen`, on by default: a 426x240 view into `widescreen_surface`, centred by the game in the window):
  while it is on, the window is 426 wide (`N3DSAm2r_gameWidth`), the GUI layer is the window's size (1.5 draws its
  HUD in window coordinates) and Wide adds no hack on top. 1.5 makes `widescreen_surface` once, on its first frame,
  320 wide (before the widescreen width is set); on a PC it is lost and remade at startup, here the front end frees a
  wrong-sized one so the game remakes it. 426 columns do not fit the 400-wide top screen, so
  `N3DSAm2r_fitWidescreenToScreen` (once a frame, after the game's step and before its draw) clamps
  `oControl.widescreen_space` from 106 to 80 = 400 - 320 and pins `display_scale`/`displayx`/`displayy` to 1/0/0:
  the window becomes exactly 400 wide and the picture fills the top screen 1:1, with no cropping and no scaling.
  Everything downstream of the space (HUD offsets, lava/water strips, the pause layout) follows the clamped value.
- Pause screen (`n3ds_pause.c`): AM2R's `rm_subscreen` is the pause menu and the game draws and operates it like any
  other room - on the top screen, with its own Menu1/Menu2 (A/B) and D-pad bindings. The front end does nothing for
  it: `N3DSPause_update` only tracks whether the game is in the pause room (following `rm_transition`'s `targetroom`
  into `rm_subscreen`), which `main.c` uses to pin the window height (the pause screen lays itself out for a tall
  320x568 surface, which would letterbox the picture away). There is no touch handling, no queued presses, no frozen
  frame and no bottom screen.
- Bottom screen: black and nothing else. The screen's render target is still created, cleared to black and bound once
  a frame (`N3DSRenderer_beginFrame`): a screen the GPU is never told to draw to keeps its previous framebuffer, which
  is not reliably black, so simply not making a target is not the same as a black screen. No GUI, map, snapshot or
  frozen frame draws to it any more.
- AM2R adjustments from the front end (`n3ds_am2r.c`; the game's files are unchanged): calls to the game's
  `get_text`, `get_xjoybtnname` and `get_xjoybtnsprite` go through C hooks that call the game's script and adjust
  the result (Nintendo button names, A/B and X/Y icons swapped by position, "XBox 360 Joypad" -> "Nintendo 3DS",
  Display tip). The Control page's Keyboard row is hidden (`canedit` 0, rows below moved up); the Display page is
  replaced by a 3DS one (Screen, Frameskip, Fast scripts, Cheats, Exit) and a Cheats page under it, built from the game's own row objects
  (`oMenuLabel`, `oOptionLR`, `oPauseOption`) and driven from C (`oControl.k*`); cheats and the screen mode live in
  `sdmc:/3ds/am2r/cheats.ini`. The water/lava
  ripple filter `oWaterFXV2` (a copy of the screen redrawn in 1-pixel strips every frame) is removed.
- Start+Select toggles the debug monitor (on the top screen while paused). Start and Select reach the game 3 frames
  late, press for press, so the chord is caught before the game sees either.
- "Saving..." (small, top screen, bottom-right) while the game's files have changes not yet on the SD card.
- Builtins vs game scripts: the VM prefers a builtin over a game script of the same name (for GMS 2.3+ games'
  compatibility scripts). For games older than 2.3 the 3DS front end points those calls back at the game's script
  (AM2R's `string_split(str, sep, index)`; the GMS2 builtin returns an array and every button hint read "<array>").
- Main loop: own loop in `src/n3ds/main.c` following `loop.c`'s sequence (`loop.c` is GL/windowing specific).
  The GPU frame is opened before `Runner_step`, since GML draws to surfaces from any event. Pacing is
  `C3D_FrameRate` at the room speed only.
- File system: upstream `OverlayFileSystem` wrapped by `N3DSCachedFileSystem`, which keeps the SD card out of the
  frame loop: existence checks cached; binary files are memory buffers between open and close; files the game
  reads or writes are held in memory and written back a second after the game stops changing them, only if they
  differ from the card (flushed at exit too), by a writer thread from a snapshot. AM2R's `crypt` script XORs its 236 KB save in place a byte at a time
  with a seek per byte, and decrypts/re-encrypts on every save read: seconds per read through stdio, nothing now.
- Input: 3DS buttons -> gamepad slot 0 by position (B = face1, A = face2, Y = face3, X = face4, L/R, ZL/ZR,
  Select, Start, D-pad, circle pad/C-stick axes); `N3DS_KEYBOARD_MIRROR` (off for AM2R) also presses the keyboard's
  arrows/Enter/Escape: AM2R sets `global.controltype` to keyboard whenever a keyboard binding is held and then shows
  keyboard button prompts. AM2R binds its actions in its
  own `config.ini`; `res/n3ds/am2r/sd/config.ini` seeds a 3DS layout (B jump, Y fire, A morph, L/ZL aim, R
  missiles, ZR aim lock, Select weapon, Start pause; menus A = OK, B = back) when the card has none.
- Audio: NDSP (Cinnamon's system on upstream's interface). Sound effects: a DSP-ADPCM bank read once into linear
  memory and played in place. Music (GameMaker streamed sounds, `AUDIO_ENTRY_FLAG_IS_EMBEDDED` clear): BCWAV
  DSP-ADPCM files streamed from the SD by a worker thread on core 2. `audio_sound_gain(snd, gain, time)` fades over
  `time` (AM2R crossfades music over 3 s).
- Textures: the preprocessor packs atlas pages (ETC1A4/RGBA5551/LA4/L4, RGBA8 where an image has partial alpha)
  into `gfx/atlas.bin`; sprite sheets into `gfx/direct_assets.bin`; `room_manifest.bin` lists each room's pages.
  On a room change the room's pages are queued for a background loader thread (own handle on `atlas.bin`, core 2,
  priority above the main thread: below it, it starved); a page needed before its turn is read or waited for.
- Tiles: the runner keeps a GMS1 room's tiles per depth in a grid of 128 px cells (`TileRun`) and asks the
  renderer for the visible room rectangle (`getVisibleRoomRect`), so a frame visits the tiles near the view (54 of
  3744 in `rm_a3h04`) instead of handing over every one; `drawTile` still culls each.
- Texture cache: atlas pages live in linear memory (not VRAM). Past the 5 MB budget, a page drawn within the last
  second is kept while linear memory has 3 MB left, instead of evicting pages still in use (a room whose pages didn't
  fit re-imported ten of them every few frames: a 14 ms hitch).
- `surface_set_target` sets the viewport to the whole surface (it kept the game's 320-wide view port, squeezing any
  wider surface, such as 1.5's 426-wide picture, into 320 columns).
- Dropped Undertale-only code: borders, battle top-screen layout, sprite-name hacks, GMS2 tile-layer cache,
  `sdmc:/3ds/UNDERTALE` paths.
- `n3ds-preprocess`: builds against upstream (`src/image/image_decoder.c`, bzip2, log shim, const-safe `repeat`).
  It writes the `.t3x` files itself (`t3x.c`): a C port of devkitPro tex3ds 2.3's `[--atlas] -f <fmt> -z none`
  (packer, swizzle, formats; ETC1 through the same rg_etc1, `vendor/rg_etc1`), down to libstdc++'s `std::sort`
  order for equal-sized frames. A full AM2R 1.5.5 conversion gives the same bytes as the tex3ds build in every asset
  file; `t3x-cli` (CMake target) takes tex3ds's command line for comparing the two. So it no longer needs devkitPro
  or Linux, and it builds with Emscripten. `--parts textures,sfx,music` and `--music-shard <i>/<n>` split a run.
- SD card maker (`web/`, built by `tools/web/build.sh` into one self-contained `index.html`, hosted on Codeberg
  Pages): checks the AM2R 1.1 zip's `data.win`, gets the Community Updates patch (`web/patch-manifest.json`: the
  AM2R-Autopatcher-Windows commit and file hashes; downloaded from raw.githubusercontent.com, or a zip the user picks),
  applies `data.xdelta` with xdelta3 (WebAssembly, with liblzma: the patch uses LZMA secondary compression), copies
  `files_to_copy`, then runs `n3ds-preprocess` (WebAssembly) in workers: one for the textures and sound bank, the
  rest share the music. Inputs reach it through WORKERFS; outputs leave at each `outputCheckpoint()` and go into a
  stored zip of `3ds/am2r/` (File System Access save, or a download). About a minute on a 12-thread PC. Against
  `make_sd.sh` with a launcher-made 1.5.5: same files; text files have LF where a Windows checkout of the patch has
  CRLF, and three audio files differ in a few bytes (WebAssembly's libm vs glibc in stb_vorbis). The page's QR code is
  the newest Codeberg release's `am2r.cia` when it was built (`AM2R_CIA_URL` overrides), so a saved page keeps
  pointing at a CIA that reads the SD data it makes.

## Built-in scripts ("Fast scripts")

`src/n3ds/n3ds_am2r_native.c` (+ `n3ds_am2r_native_hud.inc`): C versions of AM2R 1.5.5's busiest scripts and events,
each following the game's bytecode instruction for instruction (same values read, same variables written through the
VM's own setters, same builtins called in the same order, GML's epsilon comparisons, `random()` sequence kept):
`string_split`, `approximatelyZero`, `calculateCollisionBounds`, `isCollisionLeft/Right/Top/Bottom/Rectangle`,
`draw_mapblock`, `draw_gui_map`, `draw_gui` (HUD) and the events `oEnemy` Draw, `oA6Dust` Draw and `oLightEngine`
User Event 1 (the light map; a room with `oCoreX`, whose block nests further with-blocks, runs the game's code).

- Scripts replace the FUNC call cache entry (`funcCallCache[i].builtin`), events go through
  `Runner.nativeEventCode` (checked in `executeCode`); turning the option off restores both, so off costs nothing.
- Each is pinned to the FNV-1a hash of its bytecode (`n3ds_am2r_native_hashes.h`): other versions (1.1, mods) keep the
  game's code; a missing hash is logged at boot with the actual one. A native that meets something unexpected (a
  variable missing or of another type) calls the game's code instead, before drawing or writing anything.
- Variables are resolved from the VARI entry of their kind (instance or global): the VM's name map also holds locals,
  and 1.5.5 has a local `scale` that shadowed `oA6Dust.scale`.
- Checked with `profrooms_cu_seeded.txt`: top-screen screenshots pixel-identical with the option on and off.

## Diagnostics

Top screen (toggle Start+Select): FPS, average frame ms and the worst in the last quarter second; `S` step (GML),
`W` waiting for the GPU/VBlank, `D` building the GPU frame, `P` room prewarm; `IO` SD reads by the renderer,
`TX` texture uploads/surface allocations, `FS` the game's file I/O, `AU` audio calls (all ms per frame, main
thread only); VRAM, heap used/total, linear free, UNIMPL count, room. `log.txt`: a `Perf:` line every 5 s, every
room's first frame and any frame over 100 ms with the same breakdown.

## Upstream engine changes

Performance (all general, kept behaviour-identical):

- `instance_activate_region`: inactive instances' bounding boxes in a packed array (`Runner.inactiveBBoxes`, kept by
  `Runner_setActiveState`, rebuilt after room changes) instead of computing every inactive instance's box per call.
  AM2R deactivates most of the room every step and activates regions around the view and every enemy: 8 ms a frame
  in `rm_a3h04`.
- `collision_line`, `collision_rectangle`, `collision_point`, `instance_position`: candidates from the spatial grid
  when the cells in range hold fewer instances than the target object has (`pushGridCandidates`), as the `_list`
  variants and `instance_place` already did.
- `SpatialGrid_removeInstance`: the whole-grid scan for stale entries only runs when an instance's own cells didn't
  hold it; a grid generation number tells an instance whether its cells belong to the current room's grid. Every
  moving instance re-filed in a crowded room scanned the whole grid.
- Draw list: tiles/layers are kept sorted on their own (`cachedStaticDrawables`); creating or destroying instances
  sorts only the instances and merges (lava bubbles made every frame re-sorted ~2500 drawables).
- Tile layer lookups are cached across a run of same-depth tiles; `tile_layer_depth` / `tile_set_scale` rebuild the
  static part.
- Script locals and arguments, and event locals, come from per-size free lists (`vmAllocSlots`) instead of a calloc and
  free per call; nested events keep the caller's stack values on the C stack.

Fixes:

- `runner.c` `persistRoomState`: deactivated instances are kept with a persistent room (GameMaker keeps them,
  still deactivated); they were freed. AM2R deactivates every solid away from the view each step and keeps the room
  you pause in as persistent, so every pause emptied the room (142 -> 44 -> 14 instances): Samus fell through the
  world, and could vanish on the next room change.

## Cinnamon engine changes re-applied

None so far: AM2R runs on upstream's engine unmodified (no AUDO header parsing, `normalizeFixedArrayScope` or
`strengthenReturnValue` needed yet; `parseAudo=false` + per-chunk loading keep memory down).

## Open UNIMPL items

None hit in title, menus, file select, intro, the pause screen or the first area (`rm_a0h01`..`rm_a0h06`).
Stubbed and logged if hit: shaders (AM2R has 2, not used so far), primitives/vertex buffers, gradient sprite
parts/surfaces, `surface_get_pixels`, `sprite_create_from_surface`, fog, colour write masks, rotated views.
AM2R 1.5.5 hits two: `surface_get_pixels` (9 calls at boot) and colour write masks (`draw_gui_map` draws Samus's cell
without writing alpha).

## Performance (Azahar, New 3DS, CPU clock 100%)

Big rooms, AM2R 1.5.5, release build (2026-10-07; each run includes one screenshot stall per room):

| room | before | now | step + draw now |
|---|---|---|---|
| `rm_a3h04` | 28 fps | 51 fps | 14.3 ms |
| `rm_a6b14` | 29 fps | 49 fps | 15.4 ms |
| `rm_a5h04` | 26 fps | 51 fps | 12.6 ms |
| `rm_a2h02` | 47 fps | 52 fps | 8.8 ms |

Azahar measures about 57.9 fps even on the title screen with 3.5 ms of work a frame, probably its own timing (it
advances the clock 150 cycles on every `svcGetSystemTick`); hardware numbers settle it.

Earlier work:

| | before | now |
|---|---|---|
| save select opens | 4.9 s | 0.6 s |
| loading a save | 5.1 s | 0.7 s |
| pause screen | 5 fps | 56 fps |
| first area, gameplay draw | 20 ms | 8-10 ms |
| first frame of `rm_a0h01` / `rm_a0h02` | 1056 / 230 ms | 666 / 78 ms |

Hardware (before these changes): gameplay 37 fps with 13.7 ms draw; freezes of seconds to minutes at the save
screens and room changes, pause screen ~7.5 fps.

Hardware log, 1.5.5 release build (2026-10-07, before the three changes below): most rooms 58-60 fps; the big
rooms 48-53 fps with only ~9 ms of CPU work a frame (step ~4-5 ms, drawing ~4 ms) but ~11 ms waiting for the GPU;
title screen 35 fps; a 75-100 ms stall at every music change (up to 700 ms for a boss theme).

- **Frame overlap.** The loop opened the GPU frame (`C3D_FrameBegin`: waits for the previous frame's GPU work and
  the vblank) before the step, so the step and the GPU never ran at the same time. The frame is now opened lazily
  (`N3DSRenderer_frameGate`): the renderer vtable is wrapped (`src/n3ds/n3ds_gate_vtable.inc`, generated by
  `tools/n3ds/gen_gate_vtable.py` from `renderer.h`; re-run it when the vtable changes) so the first call that may
  touch the GPU opens it, wherever it comes from (Samus is drawn to a surface in End Step); screen captures open
  it before reading the finished frame. Azahar can't show the gain (its GPU takes 0.5 ms).
- **Sprite read-ahead.** Drawing a sprite frame queues the next 4 frames' atlas pages on the background page
  loader; one arrived page a frame becomes a texture before drawing starts (`N3DSRenderer_importReadAhead`). The
  page-data cache (32 MB on New 3DS) ages pages by use, not by import. The title animation streams a 512 KB page
  every 8 frames (~7.7 MB/s; the hardware card read ~8.3 MB/s, Azahar's ~5.5 MB/s).
- **Music in the background.** The stream worker opens a track and reads its header; the next audio update sets
  up its channels; one-shot music streams too. The worker runs above the main thread and the page loader.

## Known issues

- Area 6 tiles cost about 3.6 us each to draw (729 visible in `rm_a6b14`, 2.6 ms).
- A room change on hardware once left the game in the transition room (Samus gone, old view shown, game still
  running); not reproduced in Azahar.
- Start/Select reach the game 3 frames (50 ms) late (Start+Select chord detection).
