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
    mrb_value rb_self; /* DUMP: o objecto Ruby do sprite, para o dump-de-frame
                          (botao Y) poder ler a classe e ivars (@viewport, etc).
                          Preenchido no spr_init. */
};

extern const mrb_data_type Sprite3DSType;
void spriteBindingInit(mrb_state *mrb);
void spriteBindingReinit(mrb_state *mrb); /* FIX3: re-bind apos scripts Ruby */
void sprites_draw_all();
void sprites_dump_frame(mrb_state *mrb, const char *motivo); /* DUMP: despeja
    TODOS os sprites do frame actual (classe, geometria, opacity, viewport,
    bitmap). Chamado quando o utilizador carrega no botao Y. */
