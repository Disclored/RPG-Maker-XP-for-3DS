# mkxp-3ds — Pokémon Essentials on 3DS

> **Status:** Work in progress. Pokémon Essentials fan games boot, load saves and play on the map, now measured
> on a **real Old 3DS**. On a busy town map (Solar Eclipse, map 43) the median frame dropped from **~60 ms
> (Oct 18) to ~25–30 ms (Nov 1)**; the target is a stable 30 FPS with no spikes. The latest build (Nov 1) still
> crashes at a map border and when entering a house, and shows a vertical black line on the map: being fixed.

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

---

## What was done

### Boot and loading

* **Symbol table** patched from O(N²) to **O(1)** (scripts 92 s → 3.6 s, plugins 80 s → 1.4 s).
* **Bytecode cache** in a single `scripts.pack` with a CRC per entry; hot method bodies preloaded at boot.
* **Faster Marshal** (`Animations.rxdata` 14 s → 0.17 s), lazy event pages, streamed `messages.dat`.
* **Graphics pack:** all images in one file (avoids 200–650 ms `fopen` in big folders on the console SD).
* `Game.rgssad` is extracted automatically on the first run.

### Interpreter and Ruby core (75 mruby patches, all checked against MRI)

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
* Each one is tested against MRI with mutations, and by replaying 14 recorded console sessions on PC (the final
  save must match exactly), plus screenshot A/B comparisons.

### Rendering

* Native Tilemap for the Essentials custom tilemap; ground in a GPU texture; tile atlas with repeated borders
  (no seams at 0.625 scale); RGBA4/RGB565/RGBA5551 fallbacks; textures uploaded only when the GPU is done.
* Fixed invisible dialogue boxes, windowskin paths, transitions with masks.

### Input, audio, compatibility

* `Input.repeat?`, `dir4/dir8`, mkxp-z keyboard extensions mapped to 3DS buttons.
* BGM/BGS/ME streaming on its own thread, SE decoded in the background, fades, pitch and loop points.
* Ruby compatibility layer for Essentials v17–v21, mkxp-z `System` API, `Win32API` input.

### Profiling and debugging

* Console log with scenes (p50/p95/p99), per-method profile, slow-frame reports, memory/VRAM, watchdog for stalls.
* PC tools: a simulator that **replays console sessions exactly**, gprof per window, verify modes for each cache,
  screenshot A/B diff, mutation scripts.

---

## Known issues / next steps

* **Latest build (01/11):** crash at a map border (connected maps) and when entering a house; a vertical black
  line in the middle of the map. These come first.
* **Map performance:** median ~25–30 ms on a real Old 3DS, but spikes remain (p95 ~45 ms). Next: Following Pokémon
  in C, the rest of the character update, events in range, the remaining native map engine parts.
* **Menus and loading:** opening the menu 335–545 ms, a new message 115–414 ms, map change 1.7–2.4 s, loading a
  save 3.6–4.2 s, warm boot 51–65 s.
* **Battles (EBDX):** run, but not optimized yet; done after the map is smooth.
* **Other games:** Essentials v20/v21 (TilemapRenderer) and New 3DS (core 2) later.
