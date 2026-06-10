#pragma once
#include <mruby.h>
mrb_value bmp_init(mrb_state *mrb, mrb_value self);
#include <mruby/data.h>
#include "display_3ds.h"

void bitmapBindingReinit(mrb_state *mrb);

/* ── Bitmap3DS: estrutura interna de cada objecto Bitmap Ruby ──────────────── */
struct Bitmap3DS {
    int            width;
    int            height;
    unsigned char *pixels;    /* buffer RGBA linear em RAM */
    DS3Texture    *tex;       /* textura GPU (nullptr se não fez flush) */
    bool           tex_dirty; /* true = pixels modificados desde o último flush */
    bool           disposed;  /* true = disposed(), não usar */
};

extern const mrb_data_type Bitmap3DSType;

/* ── API pública ───────────────────────────────────────────────────────────── */

/* Inicializa o binding Ruby (chamar antes de correr scripts) */
void bitmapBindingInit(mrb_state *mrb);

/* Garante que a textura GPU está actualizada a partir de pixels em RAM.
 * Chama antes de qualquer draw que use b->tex.
 * Se b->tex_dirty == false, é no-op (barato). */
void bmp_flush(Bitmap3DS *b);

/* Alias mais descritivo para uso no sprite/graphics binding */
void bmp_ensure_texture(Bitmap3DS *b);
