#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

struct DS3Texture {
    C3D_Tex tex;
    int     width;
    int     height;
    bool    valid;
};

void        display_3ds_init();
void        display_3ds_begin_frame();
void        display_3ds_end_frame();
DS3Texture* display_3ds_create_texture(int w, int h, const unsigned char* rgba);
void        display_3ds_free_texture(DS3Texture* t);
void        display_3ds_blit(DS3Texture* t,
                             float dst_x, float dst_y,
                             float src_x, float src_y,
                             float src_w, float src_h,
                             float alpha);
/* display_3ds_blit_ex: versão extendida com tamanho de destino (dw/dh)
 * separado do tamanho de source (sw/sh) -- necessário para zoom correcto. */
void        display_3ds_blit_ex(DS3Texture* t,
                                float dst_x, float dst_y,
                                float dst_w, float dst_h,
                                float src_x, float src_y,
                                float src_w, float src_h,
                                float alpha);
void        display_3ds_fill_rect(float x, float y, float w, float h,
                                  unsigned char r, unsigned char g,
                                  unsigned char b, unsigned char a);
int         display_3ds_screen_width();
int         display_3ds_screen_height();
int         display_3ds_blit_count(void); 
