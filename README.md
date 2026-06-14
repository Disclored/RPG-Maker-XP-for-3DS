mkxp-3ds — Pokémon Essentials on 3DS

⚠️ Status: work in progress. Pokémon fan games are not fully playable yet — boot and rendering work, but performance and some plugins are still being fixed.

A fork of mkxp-z that runs RPG Maker XP / RGSS1 games (including Pokémon Essentials fan games) natively on Nintendo 3DS, powered by mruby.
Setup

Project (compiled with devkitARM): C:\Users\USER\Desktop\Emulator\mkxp-3ds
Game + SD card (Azahar emulator / real 3DS homebrew): C:\Users\USER\AppData\Roaming\Azahar\sdmc\mkxp

Progress

13/06/2026 — First successful full boot (engine reaches the title/map and renders).
14/06/2026 — Boot time cut from ~15 minutes to ~10 seconds (scripts + plugins + .rxdata loading).

What was done

Root-caused the main bottleneck: mruby's symbol table is O(N²) on large symbol sets (only 256 hash buckets + a linear-scan fallback). Patched it to O(1) → scripts 92s → 3.6s, plugins 80s → 1.4s, data files ~10–30× faster.
Bytecode caching (.mrb) for both base scripts and plugins, so reboots skip recompilation.
Faster Marshal loader: class-resolution cache + symbol pre-computation (e.g. Animations.rxdata 14s → 0.17s).
Fixed invisible dialogue boxes: windowskin/image path resolution now works on the 3DS filesystem (full-path lookup).
Fixed menu input: Input.repeat? now follows proper RGSS timing (one tap = one move, hold = smooth scroll).
GC tuning: generational GC during gameplay to prevent out-of-memory on the map.
Built-in profiling to measure load times and per-frame cost (drove every fix above with real numbers).

Known issues / next

Map runs ~15 FPS (target 30) — per-frame optimization ongoing.
Some plugins fail to load (Elite Battle: DX skipped, Following Pokémon partial).
Audio: sound effects only; BGM/BGS streaming not implemented yet.
