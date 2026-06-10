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
};

extern const mrb_data_type Sprite3DSType;
void spriteBindingInit(mrb_state *mrb);
void spriteBindingReinit(mrb_state *mrb); /* FIX3: re-bind apos scripts Ruby */
void sprites_draw_all();
