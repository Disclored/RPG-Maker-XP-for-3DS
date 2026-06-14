# mkxp-3ds — Pokémon Essentials on 3DS

> **Status:** Work in progress. Pokémon fan games are not fully playable yet — boot and rendering work, but performance and some plugins are still being fixed.

A fork of **mkxp-z** that runs **RPG Maker XP / RGSS1** games (including **Pokémon Essentials** fan games) natively on **Nintendo 3DS**, powered by **mruby**.

---

## Setup

### Project (compiled with devkitARM)

```text
C:\Users\USER\Desktop\Emulator\mkxp-3ds
```

### Game + SD card (Azahar emulator / real 3DS homebrew)

```text
C:\Users\USER\AppData\Roaming\Azahar\sdmc\mkxp
```

---

## Progress

### 13/06/2026 — First successful full boot

Engine reaches the title/map and renders.

![Boot screenshot](https://github.com/user-attachments/assets/56ca29c0-6746-46aa-af3f-6f11f35c3973)

---

### 14/06/2026 — Massive boot-time improvement

Boot time reduced from **~15 minutes → ~10 seconds**
(scripts + plugins + `.rxdata` loading).

---

## What was done

### Performance fixes

* Root-caused the main bottleneck:

  * mruby's symbol table was **O(N²)** on large symbol sets
  * only **256 hash buckets** + linear-scan fallback
* Patched lookup to **O(1)**

Results:

| Component  | Before | After          |
| ---------- | ------ | -------------- |
| Scripts    | 92s    | 3.6s           |
| Plugins    | 80s    | 1.4s           |
| Data files | 1100s  | ~10–30× faster |

### Bytecode cache

* Added **`.mrb` bytecode caching**
* Works for:

  * base scripts
  * plugins
* Reboots now skip recompilation

### Faster Marshal loading

* Added:

  * class-resolution cache
  * symbol pre-computation

Example:

* `Animations.rxdata`

  * **14s → 0.17s**

### Rendering / compatibility fixes

* Fixed invisible dialogue boxes
* Windowskin and image path resolution now works correctly on the **3DS filesystem**

### Input fixes

* Fixed menu controls
* `Input.repeat?` now follows proper **RGSS timing**

  * single tap → one move
  * hold → smooth scrolling

### Memory improvements

* Tuned GC:

  * generational garbage collection during gameplay
* Prevents out-of-memory while on maps

### Profiling

* Added built-in profiling:

  * load-time measurement
  * per-frame cost analysis

All optimizations above were driven by real profiling data.

---

## Known issues / next steps

* Map performance currently around **15 FPS**

  * target: **30 FPS**
  * per-frame optimization ongoing

* Some plugins still fail to load:

  * Elite Battle: DX → skipped
  * Following Pokémon → partial support

* Audio:

  * sound effects work
  * BGM/BGS streaming not implemented yet
