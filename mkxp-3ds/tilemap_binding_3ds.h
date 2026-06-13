#pragma once
#include <mruby.h>
#include <mruby/data.h>
#include "bitmap_binding_3ds.h"

struct Tilemap3DS {
    Bitmap3DS *tileset;
    Bitmap3DS *autotiles[7];
    void      *map_data;   // mrb_value guardado como ponteiro
    int        ox, oy;
    bool       visible, disposed;
};

extern const mrb_data_type Tilemap3DSType;
void tilemapBindingInit(mrb_state *mrb);
void tilemaps_draw_all();   // chamado em sprites_draw_all ou separado