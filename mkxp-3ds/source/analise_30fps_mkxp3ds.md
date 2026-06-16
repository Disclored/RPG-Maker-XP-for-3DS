# Análise: chegar a 30 FPS constantes no mkxp-3DS

Baseado em (a) leitura do código que enviaste (`source.zip`), (b) os logs/screenshots das rondas anteriores e (c) pesquisa em 3dbrew, GitHub (mkxp-z, mruby, Luma3DS), PokéCommunity, fóruns e wikis. Onde digo "medido" é do teu log; onde digo "estimado" é raciocínio sobre o código — o profiler vai confirmar.

---

## 1. Veredicto honesto antes de tudo

- **Old 3DS/2DS**: ARM11 **dual-core a 268 MHz**, VFPv2 (sem NEON), 1 core para a app (appcore) + 1 para o sistema (syscore), 128 MB FCRAM, 6 MB VRAM, GPU PICA200 a 268 MHz.
- **Facto incómodo**: a *maioria dos jogos nativos* do 3DS nem mantém 30 fps constantes. Nós estamos a correr um **interpretador mruby** (10–50× mais lento que C nativo) por cima, a executar **Pokémon Essentials** — que é dos projetos RGSS mais pesados que existem.
- **Conclusão realista**: 30 fps constantes em *todos* os cenários é muito improvável. **30 fps estáveis na exploração normal de mapas pequenos/médios é alcançável** com o trabalho certo. Os piores casos (Gardenia: 54 eventos + 2 spritesets + água animada + cutscene longa) vão sempre dar luta.
- **Meta sã**: 30 fps estáveis na exploração; aceitar quedas controladas em transições, cutscenes e batalhas, e atacá-las uma a uma.

Isto não é pessimismo — é para não gastarmos energia a perseguir um número impossível num canto e ignorarmos os ganhos grandes e fáceis no meio.

---

## 2. O número que importa: o orçamento por frame

No `graphics_binding.cpp`, o `grph_update` faz **`gspWaitForVBlank()` uma vez por frame**. Consequência técnica: **o tempo de frame é sempre arredondado para cima a múltiplos de 16,67 ms** (1 vblank).

| update+render | frame real | fps |
|---|---|---|
| < 16,7 ms | 16,7 ms | 60 |
| 16,7–33,3 ms | 33,3 ms | 30 |
| 33,3–50 ms | 50 ms | 20 |
| ~68 ms (atual) | ~83 ms | **~12** |

**Logo, a meta concreta é: `update + render` por baixo de 33 ms (idealmente < 30 ms para folga).**

Estado medido:
- `render` = **0,3–6 ms** → o desenho (citro2d/PICA200) está **ótimo**, sem fugas, eviction de texturas a funcionar.
- `update` = **68 ms estável** no mapa, com **picos de 180–305 ms** em transições/cutscenes.

**É preciso cortar o `update` de ~68 ms para < ~27 ms (≈2,5×) e matar os picos.** Todo o resto desta análise é sobre isto. Mexer no render não dá nada — já está no chão.

---

## 3. Onde vão os 68 ms (do código + logs)

Ordenado por impacto **estimado**. O profiler (ver §6) dá a ordem **real**.

### 3.1 Múltiplos spritesets ativos — provavelmente o MAIOR
- O `$MapFactory` (PokemonMapFactory) carrega os mapas **vizinhos** ligados por conexões; o `Scene_Map` mantém **um `Spriteset_Map` por mapa** num hash.
- Log de Gardenia: `@spritesets=Hash(2)` → **DOIS mapas a fazer `update` completo por frame**. Cada um corre: `@character_sprites.each(&:update)` + `@tilemap.update` + panorama + fog + weather.
- Em 268 MHz, atualizar dois spritesets inteiros por frame ~ **dobra** o custo do mapa. Isto é estrutural e específico de Essentials.
- Confirmado pela comunidade: dividir/gerir mapas ligados é das primeiras coisas que reduz lag em Essentials.

### 3.2 Custo por evento/sprite × 54, antilag insuficiente em diálogo
- Essentials **já tem antilag** (só atualiza eventos perto do ecrã + autorun/parallel; `should_update?` com margem de 2 tiles). Por isso só 8 de 54 sprites têm bitmap carregado (`com_bitmap=8`).
- **Mas**: os piores FPS medidos são **durante a cutscene/diálogo longo** (a Xenia a falar). Durante mensagens o jogador não anda, mas continuam a correr: **Following Pokemon EX, Voltseon's A-Star Pathfinding, Overworld Shadows EX, Spot Step Animation**.
- E o próprio `should_update?` ainda custa **1 chamada + cálculo de distância por sprite, por frame** (54×).
- Técnica provada (devlog de rotinas de NPC, itch.io): **checar distância 1×/segundo** em vez de por frame = −95% desses cálculos; **pausar updates durante diálogo/cutscene** = −100% nesses momentos; **retry de movimento bloqueado a cada 30–90 frames** em vez de tentar todos os frames.

### 3.3 ~2,4 milhões de objetos alocados → churn de GC
- `objs_criados_total` sobe de ~311 k (intro) para **~2,4 M** já dentro do mapa; `frees_total` acompanha. Princípio de otimização Ruby: **"80% do ganho vem da memória"**.
- Cada `Game_Character#update` / `Sprite_Character#update` aloca objetos temporários (Rects, Colors, arrays). Mesmo com GC geracional (já ativado no boot), o coletor varre.
- Atacar: **reusar objetos** em hot paths (um `Rect`/`Color` reutilizado em vez de `Rect.new`/`Color.new` por sprite/frame), métodos `!` (in-place), `<<` em vez de `+=` em strings.

### 3.4 Pacing por vblank (§2) — restrição dura, não custo
Não é "tempo gasto", é o teto: tens de ficar < 33 ms ou cais logo para 20 fps.

### 3.5 CustomTilemap (desenho manual de tiles)
- O `pokemon_tilemap_fix.rb` (VWrap) tem o **fast-path certo**: pré-desenha a região visível para um buffer `@layer0` e depois só faz **scroll** (ajusta `ox/oy`), só re-renderiza ao passar a borda. Por isso o `render` é pequeno.
- **Mas** o refresh de **autotiles animados** (água) corre periodicamente — e Gardenia tem cascata/água. `$DONTREFRESHAUTOTILES = false`. Em mapas com muita água isto pesa no `update`.
- Nota da comunidade: o CustomTilemap desenha tiles *manualmente* em Ruby, o que é **mais lento que o Tilemap nativo do RGSS**. É um custo intrínseco de Essentials.

### 3.6 Picos de init de Spriteset (transições) — mata a fluidez, não o steady-state
- Init de um `Spriteset_Map` de mapa Gen4 = **~3800 ms** (medido). Inclui criar 54 character sprites + tentar carregar o tileset.
- Desses, **~768 ms são desperdiçados** num `stbi_load` de um PNG de 30 MB que **falha sempre** (o teu `[BMP|BIGFAIL]`) antes de cair no magenta. Já tens o `stbi_info` a correr (mostra `256x31040`) — dá para **curto-circuitar por dimensão antes do decode**.

---

## 4. Plano concreto (priorizado, com impacto e risco)

### Lado MOTOR (C++ — livre de mexer)

**M1 — Curto-circuitar o `stbi_load` fútil dos tilesets gigantes.**
Antes do decode, usar o `stbi_info` (já corre) e, se `altura > limite_seguro` (ex.: a altura que nem com VWrap cabe), saltar direto para o fallback magenta sem tentar alocar/decodificar 30 MB.
- Impacto: **−768 ms × N** nos picos de transição dos mapas Gen4. Mede-se já.
- Risco: **baixo**. Só afeta tilesets que já falham.

**M2 — Afinação fina do GC mruby.**
Já é geracional. Testar: tornar o `mrb_full_gc` do freeze menos agressivo; ajustar `interval_ratio`/`step_ratio` para o coletor incremental fazer passos mais pequenos por frame (espalha a pausa); avaliar `mrb_gc_arena_restore` nos pontos certos das funções C.
- Impacto: reduz as pausas de GC dentro do `update`. Risco: **médio** (mal feito → OOM ou pausas piores).

**M3 — (Avançado) Offload do áudio para o core1 (syscore).**
3dbrew/Luma confirmam: em Old 3DS dá para usar o core1 chamando `APT_SetAppCpuTimeLimit` primeiro (o syscore "empresta" até 30%, e o Luma 3DSX subiu para 89%). Mover o **decode de OGG** (stb_vorbis, cada `SE #N` no log custa tempo) para uma thread no core1 liberta o appcore para o VM Ruby.
- Impacto: **médio** (tira o decode de áudio do caminho crítico). Risco: **alto** (scheduling FIFO, risco de deadlock, depende do payload do CFW; rotinas time-sensitive não gostam do syscore).

**M4 — (Grande, arriscado) Tilemap nativo em C++.**
O `tilemap_binding_3ds.cpp` tem a classe `Tilemap` mas é um **stub** (`tilemaps_draw_all` só faz blit do tileset inteiro). Implementar o render tile-a-tile real em C++ e fazer Essentials usá-lo em vez do CustomTilemap pouparia o desenho manual em Ruby.
- Impacto: **potencialmente grande**. Risco: **muito alto** — Essentials está colado ao CustomTilemap; é muita integração e fácil partir prioridades/autotiles/camadas.

**M5 — (Estratégico) Avaliar mruby 4.0.**
A 4.0 (abr 2026) põe as method tables em `.rodata` (não no heap), presym sempre on, gray stack de tamanho fixo (menos overhead por objeto no GC) e `mrb_gc_add_region`. Menos memória + GC melhor.
- Impacto: memória + GC. Risco: **alto** (recompilar o binding todo contra a 4.0; APIs podem ter mudado).

### Lado JOGO (patches reversíveis em `compat_stubs.h` — permitido)

**J1 — Antilag agressivo durante mensagens/transições.**
Quando há uma janela de mensagem ativa (`$game_temp.message_window_showing` ou equivalente) ou estamos em transição, **saltar os updates não-essenciais** de `Sprite_Character` e dos plugins de overworld.
- Impacto: ataca **diretamente** o pior FPS medido (a cutscene). Risco: **baixo-médio** (validar que nada visual congela quando não devia).

**J2 — Throttle dos plugins pesados por-frame.**
Following Pokemon EX e Voltseon's A-Star Pathfinding: reduzir a frequência de recálculo de caminho/destino/distância (1×/seg, não por frame), com cooldown em movimento bloqueado.
- Impacto: **alto**. Risco: **médio** (alias reversível ao método de update do plugin).

**J3 — Limitar o update do spriteset VIZINHO.**
Atualizar a fundo só o spriteset do **mapa ativo**; o(s) vizinho(s) a frequência reduzida (ex.: 1 em cada N frames, ou só quando o jogador está perto da borda).
- Impacto: **alto** (ataca o §3.1). Risco: **médio** (possíveis glitches de animação na borda do mapa).

**J4 — Reduzir alocações nos hot paths.**
Reusar `Rect`/`Color` partilhados em vez de criar novos por sprite/frame nos métodos de update mais chamados.
- Impacto: **médio** (alivia o GC do §3.3). Risco: **baixo**.

---

## 5. Sequência recomendada

1. **Correr o profiler** (próximo build, já corrigido) → confirmar a repartição real dos 68 ms (sprite_char vs spriteset_map). **Não avançar às cegas.**
2. **J1** (antilag em diálogo) — maior ganho onde dói mais (cutscenes), mede-se logo no log.
3. **J3** (spriteset vizinho) — ataca o steady-state dos 68 ms.
4. **M1** (curto-circuito dos tilesets) — mata os picos de transição Gen4.
5. **J2 / J4** — afinar pathfinding e alocações.
6. Reavaliar com o log. **Só então** considerar M3/M4/M5 se ainda faltar muito.

Cada passo: alterar → tu compilas → log → comparar `update=Xms` antes/depois. É o fluxo de sempre, e é o único honesto (não consigo compilar nem correr no 3DS).

---

## 6. A medição vem primeiro (já desbloqueada)

O profiler granular (`RUBYPROF` no `compat_stubs.h`) **não corria** porque o gate usava `respond_to?(:__mkxp_ms)`, e o `__mkxp_ms` é um método **privado** de Kernel → `respond_to?` devolve `false` mesmo existindo. Já está corrigido para **detetar por chamada direta**, e agora **loga sempre** se o `__mkxp_ms` está presente ou ausente.

No próximo arranque, uma destas linhas decide tudo:
- `[RUBYPROF] __mkxp_ms PRESENTE … a instalar profiler` + `… instrumentado` + (≈120 frames depois) `[RUBYPROF] f=N | sprite_char=Xms | spriteset_map=Yms | TOTAL_medido=Zms/frame` → **temos a repartição**.
- `[RUBYPROF] SKIP: __mkxp_ms AUSENTE` → aí sim o `binding_3ds.cpp` não foi recompilado.

Essa repartição reordena o §3. Se `sprite_char` dominar → J1/J2/J4 primeiro. Se `spriteset_map` dominar → J3 e o custo de tilemap/autotiles.

---

## 7. Resumo honesto

- O render está ótimo; **o problema é 100% lógica Ruby** (68 ms). Meta: < 33 ms/frame.
- O maior suspeito estrutural é **2 spritesets a atualizar em simultâneo** + **plugins de overworld a correr durante cutscenes**, agravado por **~2,4 M alocações** a alimentar o GC.
- A ordem 3.1 > 3.2 > 3.3 é a minha **melhor estimativa do código**, não medida — o profiler manda.
- **30 fps constantes em Gardenia (pior caso) é improvável**; em mapas normais é alcançável.
- Hardware é o teto: Old 3DS a 268 MHz com um interpretador por cima de Essentials. Não há bala de prata — há somar muitos cortes (J1+J3+M1+J2) e medir cada um.
