# mkxp-3ds — Pokémon Essentials on 3DS

> **Status (14/11/2026):** Work in progress. Pokémon Essentials fan games boot, load saves and play on the map, measured
> on a **real Old 3DS**. On a busy town map (Solar Eclipse, map 43) the median frame went from **~60 ms (Oct 18)** to
> **~22–27 ms (Nov 14)**; the target is a stable 30 FPS with no spikes, then 60 FPS. Round 44 moved the map engine
> to C (event state in a contiguous C registry, sprites loop in C, half the live Ruby objects) and removed most
> memory/GC spikes. The last console run (14/11) crashed after 3.5 minutes on **heap fragmentation**. The cause (a
> tile cache that grew to a 300 KB block) is fixed on PC and waits for the next console build.

A port of **mkxp-z** that runs **RPG Maker XP / RGSS1** games (including **Pokémon Essentials** fan games)
natively on **Nintendo 3DS**, powered by **mruby 3.2** and **citro2d/citro3d**. The main target is the
**Old 3DS**. The test game is Pokémon Solar Eclipse (Essentials v19.1 + EBDX + Following Pokémon + plugins).

It is a generic emulator: there are no patches for any specific game. Everything is done at the engine or
interpreter level, and stays compatible with what MRI (the reference Ruby) does. Every C version of a game
function is only used when the game's bytecode is identical to the original (Essentials core or a public plugin).

---

## Setup

### Project (compiled with devkitARM)

```text
C:\mkxp-3ds
```

### Game + SD card (Azahar emulator / real 3DS homebrew)

```text
C:\Users\USER\AppData\Roaming\Azahar\sdmc\mkxp
```

Games go in `sdmc:/mkxp/games/<game>/`; logs are written to `sdmc:/mkxp/logs/`.
For sound in Azahar, `sdmc:/3ds/dspfirm.cdc` is required (dump it with DSP1).
On a real Old 3DS the **CIA** build is recommended (80 MB of memory instead of 64 MB for a `.3dsx`).
Every optimization can be switched off without a new build: create `sdmc:/mkxp/off/<name>.txt`.

---

## Progress

### 13/06/2026 — First successful full boot

Engine reaches the title/map and renders.

![Boot screenshot](https://github.com/user-attachments/assets/56ca29c0-6746-46aa-af3f-6f11f35c3973)

---

### 14/06/2026 — Massive boot-time improvement

Script and plugin loading went from **~15 minutes → seconds**
(scripts + plugins + `.rxdata` loading).

---

### 25/06/2026 — Fixing more memory issues and sprite bugs

Some screenshots of the progress:

<img width="538" height="290" alt="Captura de ecrã 2026-06-21 191029" src="https://github.com/user-attachments/assets/82162505-5f0f-435c-a44f-c9f51f0999eb" />
<img width="627" height="449" alt="Captura de ecrã 2026-06-24 140227" src="https://github.com/user-attachments/assets/d10aea49-4914-4353-a24a-fb7d53a31662" />
<img width="658" height="306" alt="Captura de ecrã 2026-06-18 202454" src="https://github.com/user-attachments/assets/96441403-59a2-468a-9da8-03c5d2319455" />
<img width="730" height="512" alt="Captura de ecrã 2026-06-18 210938" src="https://github.com/user-attachments/assets/fe516b1a-59da-4988-a102-5f7a7d870db0" />
<img width="587" height="454" alt="Captura de ecrã 2026-06-21 112132" src="https://github.com/user-attachments/assets/6eabef6f-c241-49d8-9393-a801fda3cb15" />
<img width="613" height="399" alt="Captura de ecrã 2026-06-25 212628" src="https://github.com/user-attachments/assets/5f5965e5-bddf-4e86-9ee7-7a4c798e31f9" />
<img width="550" height="369" alt="Captura de ecrã 2026-06-25 212634" src="https://github.com/user-attachments/assets/effdc0f9-b5fc-43ef-83ba-0f36eb0aec53" />
<img width="593" height="344" alt="Captura de ecrã 2026-06-25 212640" src="https://github.com/user-attachments/assets/89ec40d6-2833-4d54-af40-08700109ea68" />

---

### 04/07/2026 — Fixing menus: Bag, Save/Load, Options, Trainer Card, etc.

<img width="324" height="238" alt="image" src="https://github.com/user-attachments/assets/1665e82a-b189-41f2-a22c-1a9bd352c199" />

---

### 07/10/2026 — Memory stability and map performance

* **Memory corruption on the console fixed.** The cause was the on-demand page allocator. Memory is now
  committed once at boot, like libctru does.
* **No more crashes when entering maps.** Large images (up to 256×32000 tilesets) are kept in 16 KB bands; very
  tall tilesets are packed to the SD card and only the blocks in use stay in RAM (4 MB → ~90 KB).
* **First boot after a cache change** recompiles every script; the game now saves the cache and restarts
  itself once, cleanly.
* **Map: 16–19 → 18–21 FPS indoors** and a new smart sprite mode (see below).

### 08/10/2026 — mkxp-z compatibility review and the map at 26 FPS (Azahar)

* Four side-by-side reviews against mkxp-z fixed engine bugs that looked like game bugs: `Input.*ex?`, `Reset`,
  `SystemExit`, `Thread.exit`, negative `src_rect`, `Viewport#dispose`, `eval` scope and more.
* Two mruby GC bugs fixed (infinite marking after switching to generational mode; a full GC at the end of every
  incremental major GC, ~340 ms on Old 3DS). `Array#==`/`eql?`/`hash` in C, blocks without allocation.
* **Map in Azahar (Old 3DS mode):** 21.9 → **26.2 FPS** on average; p95 87 → 61 ms; allocations −70%.

### 09/10/2026 — Essentials map functions in C, smoother GC

* `Game_Map#passable?`, `terrain_tag` and `Interpreter#setup_starting_event` in C (a pathfinding plugin used to
  freeze the game for ~4 s on map entry). Tile drawing ~20% faster; GC pauses of 66–188 ms spread across frames.

### 10/10 – 17/10/2026 — Battles, memory and many more native functions

* Big allocations in their own arena; emergency GC no longer stalls mid-frame; heap grows into the arena.
* Map: tone applied in one pass, symbols cached, tile layers shifted instead of redrawn.
* Battle memory: the crash on the 2nd trainer battle was fixed (memory on battle entry); EBDX battle scene,
  transitions with masks and the loading screen got native versions.
* Ruby 3 semantics in the core (`===`, `Float#to_s`, `rand(Range)`, big Arrays), `require`/`Thread` like MRI,
  more Essentials v20/v21 functions in C.

### 18/10/2026 — First run on a real Old 3DS

* The real console differs from Azahar: **the CPU cannot touch VRAM** (render targets and VRAM textures are now
  only written by the GPU/DMA) and a `.3dsx` gets 64 MB, not 96. A CIA build with 80 MB was added.
* OpenDSP as a fallback DSP firmware; native versions of the public map plugins (Overworld Shadows EX,
  Water bubles, Marin's Footprints, Following Pokémon EX).

### 23/10 – 25/10/2026 — The optimization plan, measured on the console (rounds 1–9)

* A single plan driven by console logs: every point has an on/off switch and is validated on PC first.
* SD card: each request costs ~25–45 ms on the console, so files are read in big windows, Graphics/ images go to
  one pack file, written in a background thread; message index, map sizes and hot bytecode are cached on the SD.
* "x2" mode (draw 1 frame in 2 when the game is slow), adaptive. Smaller VM tables (the Old 3DS has no L2 cache:
  every cache miss costs ~220 ns), compact instance-variable tables, yield without allocation, local time per second.
* The game author's hottest methods in C too, as exact-bytecode variants after the generic version.

### 30/10 – 31/10/2026 — Ruby → C and the native map engine (rounds 10–13)

* **Generated C:** the hottest Essentials methods are compiled to C ahead of time (only when the fingerprint
  matches), chosen by the console's per-method profile.
* **Native map engine:** the map state lives in C. The Essentials custom tilemap is drawn by a native Tilemap on
  the GPU; the ground goes into a texture; characters that do not change are moved in C instead of running their
  update; terrain tags cached per tile; trainers and step events in C.
* Per-method profile in the console log (time, calls, µs per call, caller), so each step is chosen by real data.

### 01/11/2026 — Rounds 15–19: the map below 30 ms on a real Old 3DS

* Ground texture as a ring (only new rows/columns are drawn when walking), animated autotiles through the atlas,
  atlas pages in linear memory, the ground texture also with the day/night tone, VRAM freed from idle images.
* Characters: idle ones update 1 in 16 frames inside the screen, sprites never positioned are skipped, a new pose
  only runs the core update.
* **The character update chain of the plugins (footprints → shadows → water bubbles → dynamic shadows → core) runs
  in one C pass**, without Ruby calls between plugins. On the Old 3DS each call costs ~30–50 µs, more than most of
  the code it runs.
* mruby patch 0075: C calls with the method cached per site (99% of ~450 calls per frame skip the lookup) and a
  "stable generation" of objects that ignores the variables written every frame.
* **Idle events in range only add 1 to their stop counter** (the event body went from ~2.6–3 ms to 0.27 ms per
  frame on the console); shadow and water-bubble updates that would change nothing are skipped (~90% of bubbles).
* Real Old 3DS, map 43 (busy town): median frame **28.8–29.7 ms** (log of 01/11 18:56) and **25.2 ms** in the
  latest log (one window), Ruby ~22–28 ms, drawing ~6.5–8 ms.

### 06/11/2026 — Round 20: the two crashes, the black line, and more of the map in C

* devkitPro updated (libctru 2.7.1, host gcc 16): clean rebuild and every test/session checked again first.
* **Crashes at a map border and when entering a house:** the ground texture of a second map was created in the
  middle of drawing, with the GPU frame open, after freeing idle images from VRAM. It is now created before the
  frame starts (to be confirmed on the console).
* **Vertical black line:** a ground cell drawn while one of its tile pieces was not ready stayed empty forever; it
  is now redrawn on the next frame (to be confirmed on the console).
* **Emulator bug found by comparing with Ruby (MRI):** a `return` inside a `File.open { }` block did not leave the
  method. Essentials' `pbGetFileChar` then read the whole file instead of 1 byte (menus, load screen). `File.open`
  with a block is now Ruby (yield + ensure), like mruby-io.
* Another one found by a check mode: the "event variables unchanged" fingerprint skipped part of the variable
  table, so an event that moved could stay "out of range".
* Map spikes: the tilemap prepare (~24 ms, 77 of 600 frames) now redoes only the animated autotile cells; events in
  range reuse their cached "should update?" result; wandering NPCs between steps take the idle shortcut; screen
  position calls of the character update go straight to C; shadows skip their update twice as often.
* Memory estimate made safe when memory is low (it was off by 6.4 MB after loading).

### 07/11/2026 — Rounds 21-23: house crash fixed, black stripe worked around, more in C

* **Crash entering a house:** render targets were deleted inside an open GPU frame (citro3d `svcBreak`); now deleted
  before `C3D_FrameBegin`. Confirmed on the console.
* **Black stripe:** comes from the ground render texture on the real console (not on PC). The ground is drawn tile by
  tile on 3DS for now (`chao_tile`); a one-shot render-target self-test (`rt_teste`) is in the log to find the cause.
* Sprites: idle sprites near the edge refresh less often when the screen is still; slide reads the screen position in C.
* `defined?(super)` fixed for modules and subclasses; Essentials v19.1 idle events skipped like v20; 247 methods in
  generated C (was 192).

### 07/11/2026 — Round 24: ground batched by texture, idle sprite re-checks rarer, IMA-ADPCM WAV
- The ground (drawn tile by tile since the stripe workaround) is now drawn layer by layer and grouped by texture, to
  cut the 21-38 texture switches per frame seen on the console (`ordem_tex`).
- Idle character sprites whose periodic full update changed nothing twice now re-check every 64 frames instead of 16
  (`spr_chave`); the day/night tint is applied without a Ruby dispatch (`tint_direto`).
- IMA-ADPCM WAV files play (`wav_adpcm`); negative-width quads are clipped by their viewport (`compat_scissor`).
- Render-target self-test reworked (one target at a time, only when VRAM is free) to find the stripe's cause.

### 07/11 – 12/11/2026 — Rounds 25–43: the character state in C, memory and the VM

* **Character state in C:** an exact per-object write generation (mruby patch 0079) and a native registry of the
  characters kept up to date by the VM's own writes. The interpreter reads position, direction and look from that
  contiguous C table instead of scattered Ruby tables (no cache misses at ~220 ns each on the Old 3DS).
* `update_move`, `screen_x/y/z`, water reflections, `update_screen_position`, `update_event_triggering`,
  `isPassableStrict?`, `move_speed=`, the start of `Interpreter#update` (also with Following Pokémon), map scrolling,
  weather, `Event#trigger` and message text formatting (`getFormattedText`) run in C.
* **Memory:** GC pages and medium malloc blocks in slabs, 64–256 KB pixel buffers outside the heap, the font cache
  survives saving, and the memory of bitmap pixels is counted.
* **VM:** sends to getters/attr without a frame, a 2-way call cache, faster `Integer#% & | ^`, and lazy Essentials
  getters without a frame (patch 0086).
* Load screen 5.7 s → 4.3 s; faster `Marshal.load` of saves (patch 0085).
* Generated C re-targeted by the console profile (371 methods).
* A console run on 11/11 (log 20:26) showed the same map FPS and a memory regression (slab arena full). It was fixed
  in round 43, and that run became the reference session replayed on PC.

### 13/11 – 14/11/2026 — Round 44: the full-frame native map engine (4 console runs)

* **Events:** a push registry (patches 0080/0081): the VM writes instance variables into a C store, and native code
  reads from it.
  * Events out of range are skipped in batch: ~17–18 thousand per 600 frames, without touching them.
  * The event body skips writes that would not change anything.
  * Passability checks go through a C position index instead of scanning every event.
* **Sprites:** the `Spriteset_Map#update` loop is in C. Off-screen sprites are decided without looking up their
  character. Signatures avoid redoing unchanged sprites.
* **Map data outside the Ruby heap:** message tables and map strings are kept in C. Live objects on the map went from
  ~230–270 thousand to ~120 thousand (−50%), so every GC is cheaper.
* **Spikes removed (confirmed on the console):**
  * Periodic emergency GCs: a `Marshal.dump` size hint was kept per class, so small dumps asked for 295 KB.
  * ~450 ms `FileTest.exist?` frames: a recovery path did an SD `stat` for every missing file.
  * A 9 s map load: the map message step.
  * A font crash when memory was low.
  * `bush?`/`deepBush?` results cached per tile (45–88% hits).
  * `GC.start` from plugins deferred when memory is tight.
* `Regexp`/`MatchData` now behave like MRI (class names, `options`, `Regexp.new(regexp)`).
* The tile atlas falls back to RGBA4 when RGBA8 does not fit.
* The ahead-of-time C generation is re-ranked by the console profile: 372 methods, ~900 KB of ARM code.
* **New profiling zones** show where the map frame goes on the Old 3DS (map 43):
  | part | ms/frame |
  |---|---|
  | drawing | 8–10 |
  | events | ~8 |
  | sprites, footprints and shadows | ~6.5 |
  | GC | 1–2, plus spikes |
  Ruby that could still move to C is only ~2.3 ms/frame. The remaining time is in the engine's own C, which still
  reads Ruby objects.
* **Crash found on the console (14/11):** after 3.5 min the largest free block fell from 3.3 MB to 216 KB (with 14 MB
  free in holes), and a tile cache vector asked for 300 KB in one piece. The cache now uses 6 KB pages and simply
  stops caching when memory is short. Fixed on PC (all 1482 checks, 25 replayed sessions), next on the console.

---

## What was done

### Boot and loading

* **Symbol table** patched from O(N²) to **O(1)** (scripts 92 s → 3.6 s, plugins 80 s → 1.4 s).
* **Bytecode cache** in a single `scripts.pack` with a CRC per entry; hot method bodies preloaded at boot.
* **Faster Marshal** (`Animations.rxdata` 14 s → 0.17 s), lazy event pages, streamed `messages.dat`.
* **Graphics pack:** all images in one file (avoids 200–650 ms `fopen` in big folders on the console SD).
* `Game.rgssad` is extracted automatically on the first run.

### Interpreter and Ruby core (91 mruby patches, all checked against MRI)

* Method calls: per-call-site cache, instance-variable hints, trivial C functions inside the call itself, faster
  method entry/return, C sends with a cached method (patch 0075).
* `Hash#keys` without copying, `str[/re/]` in C, cached `Regexp` literals, exact float parsing, `$!`,
  keyword arguments, `module_function`, generic instance variables and other MRI differences fixed.
* GC: generational during gameplay, sweep split across frames, several mruby GC bugs fixed, GC pauses spread.
* Blocks without allocation (fixed pool), per-object write generations for exact caches.

### Native versions of Essentials and plugin functions (HLE + generated C)

* Hundreds of core and public-plugin methods run in C (map, events, characters, sprites, tilemap, messages,
  windows, battle scene). **Safety:** the original source is compiled at boot and compared with the game's
  bytecode; any difference or unusual case falls back to the original Ruby, so results and errors stay the same.
* Each one is tested against MRI with mutations, and by replaying 25 recorded console sessions on PC (the final
  save must match exactly), plus screenshot A/B comparisons. 1482 host checks run before every build.

### Rendering

* Native Tilemap for the Essentials custom tilemap; ground in a GPU texture; tile atlas with repeated borders
  (no seams at 0.625 scale); RGBA4/RGB565/RGBA5551 fallbacks; textures uploaded only when the GPU is done.
* Fixed invisible dialogue boxes, windowskin paths, transitions with masks.

### Input, audio, compatibility

* `Input.repeat?`, `dir4/dir8`, mkxp-z keyboard extensions mapped to 3DS buttons.
* BGM/BGS/ME streaming on its own thread, SE decoded in the background, fades, pitch and loop points.
* Ruby compatibility layer for Essentials v17–v21, mkxp-z `System` API, `Win32API` input.

### Profiling and debugging

* Console log with scenes (p50/p95/p99), per-method profile, profiling zones, slow-frame reports, memory/VRAM
  (including the largest free block), watchdog for stalls, and a checklist script that reads a console log point by point.
* PC tools: a simulator that **replays console sessions exactly**, gprof per window, verify modes for each cache,
  screenshot A/B diff, mutation scripts.

---

## Known issues / next steps (14/11/2026)

* **Memory fragmentation** on long sessions: the heap ends up with many small holes. First: big allocations out of
  the main heap, a refilled emergency reserve, compaction on map change. The crash it caused is already fixed on PC.
* **GC spikes:** minor GCs of ~400 ms and the full GC that Following Pokémon asks for on every map change.
* **Map frame (median ~22–27 ms, p95 42–58 ms):**
  * the whole event update in C (~8 → ≤ 3 ms);
  * sprites, footprints and shadows in C (~6.5 → ≤ 2.5 ms);
  * Following Pokémon in C;
  * drawing 8–10 → ≤ 5 ms (a persistent draw list, batching by texture, static tile layers, skipping unchanged
    frames). The last one is needed for 60 FPS.
* **Loading:**
  * map change 1.7–2.6 s (target ≤ 0.8 s);
  * boot 44 s to the first frame (target ≤ 25 s);
  * load screen up to 5 s (target ≤ 1 s).
* **Menus and messages:** opening a menu 130–210 ms; a new message up to 350–450 ms.
* **Battles (EBDX):** run, but not optimized yet.
* **Other games:** Essentials v20.1/v21.1, v17/v18 and other fan games (Reborn, …) are being checked against
  their own scripts.
* **Later:** audio work (MP3, MIDI) and New 3DS extra cores.
