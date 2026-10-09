# mkxp-3ds — Pokémon Essentials on 3DS

> **Status:** Work in progress. Pokémon Essentials fan games boot, load saves and play on the map. On the
> Old 3DS (measured in Azahar) the map now averages **~26 FPS** (27–29 FPS when walking around); the target
> is a stable 30 FPS.

A port of **mkxp-z** that runs **RPG Maker XP / RGSS1** games (including **Pokémon Essentials** fan games)
natively on **Nintendo 3DS**, powered by **mruby 3.2** and **citro2d/citro3d**. The main target is the
**Old 3DS**. The test game is Pokémon Solar Eclipse (Essentials v19.1 + EBDX + plugins).

It is a generic emulator: there are no patches for any specific game. Everything is done at the engine or
interpreter level, and stays compatible with what MRI (the reference Ruby) does.

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
* **No more crashes when entering maps.**
  * Large images (up to 256×32000 tilesets) are kept in 16 KB bands.
  * Very tall tilesets are packed to the SD card and only the blocks in use stay in RAM: a 4 MB tileset
    drops to ~90 KB.
* **First boot after a cache change** recompiles every script, which leaves the heap fragmented. The game
  now saves the cache and restarts itself once, cleanly.
* **Map: 16–19 → 18–21 FPS indoors.** The work behind this is listed under "Interpreter and Ruby core" below.
* **New smart sprite mode** (launcher option, on by default on Old 3DS). On the PC it cuts 30–36% of the
  frame time; it is not yet validated on the console. See "Smart sprite mode" below.

### 08/10/2026 — mkxp-z compatibility review and the map at 26 FPS

* Four side-by-side reviews against mkxp-z fixed engine bugs that looked like game bugs: `Input.*ex?`, `Reset`,
  `SystemExit`, `Thread.exit`, negative `src_rect`, `Viewport#dispose`, `eval` scope and more.
* Two mruby GC bugs fixed:
  * infinite marking after switching to generational mode;
  * a full GC at the end of every incremental major GC (~340 ms on Old 3DS).
* `Array#==`, `eql?` and `hash` moved to C.
* Blocks passed to core iterators no longer allocate.
* Native versions of the hottest Essentials functions.
* **Map in Azahar (Old 3DS mode):** 21.9 → **26.2 FPS** on average; the worst frames (p95) went from
  87 ms to 61 ms; allocations per frame dropped by 70%.

---

### 09/10/2026 — Essentials map functions in C, smoother GC

* `Game_Map#passable?`, `Game_Map#terrain_tag` and `Interpreter#setup_starting_event` run in C. A
  pathfinding plugin that checks every tile on map entry used to freeze the game for ~4 s on Old 3DS.
* Tile drawing (`Bitmap#blt` from tilesets) is ~20% faster.
* The GC's "clear old objects" pass and the first collection after boot are spread across frames:
  * the clear pass used to stop the game for 66–85 ms;
  * the first collection after boot used to stop it for 188 ms.
* Every change was replayed on PC against three recorded console sessions, and the final saves are
  identical. Not yet measured on the console.

---

## What was done

### Boot and loading

* **Symbol table:** mruby's symbol table was **O(N²)** on large symbol sets (only 256 hash buckets plus a
  linear scan). Patched to **O(1)**.

  | Component  | Before | After          |
  | ---------- | ------ | -------------- |
  | Scripts    | 92s    | 3.6s           |
  | Plugins    | 80s    | 1.4s           |
  | Data files | 1100s  | ~10–30× faster |

* **Bytecode cache:** all scripts and plugins (~1000 entries) are compiled once and stored in a single
  `scripts.pack` with a CRC per entry. A damaged entry is detected and recompiled, and later boots skip
  compilation. The cache folder is tied to the compiler version, so an old cache is never reused by mistake.
* **Faster Marshal loading:** class-resolution cache and symbol pre-computation (`Animations.rxdata`:
  **14s → 0.17s**).
* **`Game.rgssad`** is extracted automatically on the first run.

### Interpreter and Ruby core (mruby patches, all checked against MRI)

* **Method calls:**
  * per-call-site method cache;
  * instance-variable hints;
  * trivial C functions done inside the call itself, e.g. `size`, `empty?`, `is_a?`, `==`, `round`, `floor`,
    `first`, `last`;
  * faster method entry/return.
* **`Hash#keys` without copying:** for hashes with more than 10 keys, the key array is cached and shared until
  the hash changes. Essentials and plugins call `keys.include?` every frame.
* **Regular expressions:**
  * `str[/re/]` runs in C: no allocations when there is no match, **3.7× faster** on the common case;
  * `Regexp` literal compilation is cached in C.
* **Correctness fixes:**
  * float literals are now parsed exactly (`0.37` used to be off by one bit);
  * `$!` works;
  * keyword arguments, `module_function`, generic instance variables and other differences from MRI are fixed.
* **Faster GC:**
  * generational GC during gameplay;
  * the sweep is split across frames;
  * a mruby GC bug (`clear_all_old`) is fixed.
* A latent mruby GC bug (infinite marking after switching to generational mode) is fixed.
* mruby ran a full GC at the end of *every* incremental major GC (~340 ms on Old 3DS); removed.
  `GC.start` during gameplay (every map transfer in Essentials) is now an incremental major GC.
* **Blocks without allocation:** blocks passed to audited core iterators (`each`, `times`, `map`, ...)
  take their Proc/Env from a fixed pool instead of the heap (~25% fewer allocations on the map).
* `Array#==`/`eql?`/`hash` in C (Hashes keyed by `[x, y]` are ~6x faster); 64 cached tileset blocks.
* **GC pauses spread across frames:**
  * the "clear old objects" pass before a major GC (66–85 ms) now runs in steps;
  * the first generational collection runs in steps too (188 ms before);
  * the longest GC pause per frame on the map is now the 8 ms budget.
* Every change is checked by **500 tests** plus **20 semantics tests** compared against MRI's output,
  and by replaying three recorded console sessions on PC (the final save must match exactly).

### Native versions of hot Essentials functions (HLE)

* Some functions from the Essentials core are called thousands of times per frame. For these, the engine
  carries a C version. One very common plugin library is also covered: Luka's Scripting Utilities, a
  dependency of EBDX.
* **Safety:**
  * when the game starts, the original Ruby source is compiled and its bytecode is compared with the
    game's own method;
  * the C version is used only if they are identical, so a game or plugin that changed the function keeps
    its own version;
  * any unusual case (subclasses, redefined methods, unexpected values) falls back to the original Ruby
    method, so results and errors stay the same.
* **Functions covered:**
  * `validate`;
  * `GameData` `get`/`try_get`/`exists?`;
  * `TerrainTag.try_get`;
  * `Game_Event#should_update?`;
  * `Game_Map#passable?` and `terrain_tag`;
  * `Interpreter#setup_starting_event`;
  * `Hash#has_key?` (Luka's Scripting Utilities).

### Smart sprite mode (launcher option "Sprites fora do ecrã")

* The player and moving characters always update.
* Characters standing still on screen update every 2nd frame; off-screen characters every 8th frame.
* Empty pictures skip their update entirely.
* **Self-calibrating:** it checks its coordinate maths against the player's sprite and turns itself off if
  a game uses different units.
* Measured on PC: **−30% indoors, −36% outdoors** frame time.

### Rendering / compatibility

* Fixed invisible dialogue boxes.
* Windowskin and image paths resolve correctly on the **3DS filesystem**.
* **Textures:**
  * tile atlas for tilesets;
  * RGBA4 fallback when texture memory is low;
  * fast Morton (tiled) uploads;
  * textures are freed or uploaded only when the GPU is done with them.
* Ruby compatibility layer for Essentials v17–v21: missing Ruby methods, mkxp-z's `System` API, `Win32API` input.

### Input

* Fixed menu controls.
* `Input.repeat?`, `count`, `dir4`/`dir8` behave like mkxp-z (only the last pressed button repeats;
  diagonals pick the "other" direction).
* mkxp-z keyboard extensions (`pressex?`, `triggerex?`, `repeatex?`, `releaseex?`) map keys to the 3DS
  buttons through mkxp-z's default bindings, so "Configure Controls" plugins keep working.
* `Input.update` inside wait loops (without `Graphics.update`) still reads the buttons, HOME and START+SELECT.

### mkxp-z compatibility review (Oct 2026)

* Full side-by-side review of Sprite/Viewport/Graphics, Input/Audio/FS, Bitmap/Font and the script runtime
  against mkxp-z (see `docs/revisao/comparacao-mkxp-z.md`). Fixed, among others:
  * `System/Graphics/Input.delta` in microseconds for Essentials v19/v20 (as in mkxp-z 2.2/2.3);
  * `Reset` restarts the game, `SystemExit` exits cleanly, `Thread.exit` works;
  * `eval` sees the caller's scope (locals, `def`, constants);
  * negative `src_rect` shifts the sprite (EBDX), `Viewport#dispose` disposes its children;
  * text squeeze and `hue_change` direction now match mkxp-z.

### Audio

* BGM/BGS/ME streaming (OGG, WAV) on its own thread, with a cache for sound effects.
* Fades, pitch and loop points (LOOPSTART/LOOPLENGTH) are supported, following mkxp-z's behaviour.

### Profiling and debugging

* Built-in profiler: Ruby sampling profile, per-frame breakdown, slow-frame reports, GC phases,
  allocations per frame, and memory and heap-fragmentation reports. The diagnostic log is a launcher option.
* PC tools:
  * a simulator that **replays a console session** (save + recorded input);
  * opcode and method histograms;
  * paired A/B benchmarking.

All optimizations above were driven by real profiling data.

---

## Known issues / next steps

* **Map performance on Old 3DS (Azahar):**
  * the last measured build (08/10) averages **26 FPS** on the map (27–29 FPS when walking around);
  * the 09/10 build is not measured on the console yet;
  * **target: a stable 30 FPS.** What still costs the most is plugin code for sprites (footprints,
    shadows, water bubbles) and the custom tilemap redraw.
* **Game speed:** Essentials runs its logic at 60 updates per second, so 30 FPS still means half speed.
  Whether logic could run at full speed while drawing at 30 FPS will be evaluated after the interpreter work.
* **Plugins:** EBDX and Following Pokémon load and run on the map. Battles are not fully validated yet.
* **Sound:** cannot be tested in Azahar without `dspfirm.cdc`, so audio's memory and CPU cost is not
  measured yet.
* **Boot:** a warm boot (cache already built) still takes ~35 s on Old 3DS. About 14 s of it is the GC
  running almost non-stop while scripts and plugins load. This keeps memory low, but it is the next thing
  to tune.
* **Map loading:** entering some maps still takes 1–5 s (tileset/autotile redraw by the game's custom
  tilemap, map connection data).
