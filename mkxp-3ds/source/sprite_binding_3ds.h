#pragma once
#include <mruby.h>
#include <mruby/data.h>
#include "bitmap_binding_3ds.h"

struct Sprite3DS {
    Bitmap3DS *bitmap;
    int x, y, ox, oy, z;
    int opacity, blend_type;
    float zoom_x, zoom_y, angle;
    bool visible, mirror, disposed;
    int src_x, src_y, src_w, src_h; /* src_rect */
    int vp_z;    /* P3: z do viewport. Chave primaria SO' quando has_vp=true. */
    bool has_vp; /* P3: true se o sprite tem viewport. Sprites SEM viewport
                    ordenam pelo proprio z no espaco global (regra RGSS): a
                    chave primaria e' (has_vp ? vp_z : z). Sem isto, sprites
                    sem viewport (ex. janela de mensagem com z=99999) caiam
                    atras de qualquer sprite com viewport.z>0 (intro pictures). */
    unsigned int reg_seq; /* [ZSORT-REGTIE] ordem de criacao (contador monotono,
                    atribuido no spr_init). Usado SO' como desempate quando a
                    chave primaria e' >= 99990 (topo: menus vp.z=99999 e janelas
                    de mensagem no-vp z=99999 empatam). Resolve o menu/roupeiro a
                    aparecer atras de sprites-fantasma SEM partir a mensagem (que,
                    criada depois, mantem-se por cima -- comportamento RGSS). */
    mrb_value rb_self; /* DUMP: o objecto Ruby do sprite, para o dump-de-frame
                          (botao Y) poder ler a classe e ivars (@viewport, etc).
                          Preenchido no spr_init. */
    /* ── FIX-NSYNC: assinatura do @character para o sync nativo de estaticos ──
     * Tudo calloc'd (0/false) no spr_init: ns_init=false marca a 1a chamada.
     * Elegibilidade (@reflection/@surfbase) e' verificada a cada chamada na
     * funcao, nao cachada (o follower so' recebe @reflection apos o initialize). */
    bool        ns_init;          /* false ate' a 1a chamada de mkxp_static_sync */
    int         ns_static_frames; /* frames consecutivos em fast-path (refresh forcado) */
    double      ns_rx, ns_ry;     /* @real_x / @real_y guardados (podem ser float) */
    int         ns_dir, ns_pat, ns_op, ns_blend, ns_tile, ns_bob, ns_offx, ns_offy, ns_bush;
    bool        ns_transp;        /* @transparent guardado */
};

extern const mrb_data_type Sprite3DSType;
void spriteBindingInit(mrb_state *mrb);
void spriteBindingReinit(mrb_state *mrb); /* FIX3: re-bind apos scripts Ruby */
void sprites_draw_all();
void sprites_dump_frame(mrb_state *mrb, const char *motivo); /* DUMP: despeja
    TODOS os sprites do frame actual (classe, geometria, opacity, viewport,
    bitmap). Chamado quando o utilizador carrega no botao Y. */
