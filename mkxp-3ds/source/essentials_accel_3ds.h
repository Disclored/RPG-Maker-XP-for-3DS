#pragma once
#include <mruby.h>

/* ──────────────────────────────────────────────────────────────────────────
 * essentials_accel_3ds — MODULO DE ACELERACAO OPCIONAL (opt-in, isolado)
 *
 * NAO faz parte do nucleo generico do emulador. E' um "fast-path" para jogos
 * Essentials: reimplementa em C++ os getters de posicao quentes do
 * Game_Character (screen_x / screen_y / screen_z) que, em Ruby a 268MHz, sao
 * chamados ~90x por frame (30 sprites x 3) no Sprite_Character#update.
 *
 * E' especifico do Essentials (le ivars @real_x/@real_y/@width/@tile_id/... e
 * as constantes RGSS X_SUBPIXELS=4, TILE_WIDTH/HEIGHT=32). Por isso vive aqui,
 * separado, e SO' e' usado quando a flag Ruby $__mkxp_native_spritechar esta'
 * ligada. Com a flag desligada, o nucleo corre 100% como antes.
 *
 * Cada funcao replica a semantica Ruby EXATAMENTE (incluindo divisao inteira
 * floor vs float round -- validado no host contra ruby). Para os casos raros e
 * complexos (salto, tile-sprites, always_on_top) devolve nil: o Ruby trata.
 * ────────────────────────────────────────────────────────────────────────── */

/* Regista __gc_native_screen_x/y/z como metodos globais (Kernel). Chamar uma
 * vez no arranque (a partir de spriteBindingInit). Idempotente. */
void essentialsAccelInit(mrb_state *mrb);
