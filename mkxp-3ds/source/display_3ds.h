#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

struct DS3Texture {
    C3D_Tex tex;
    int     width;
    int     height;
    bool    valid;
    bool    has_text = false;  /* DIAG/fix: bitmap de origem teve draw_text */
    unsigned pot_bytes = 0;    /* DIAG: bytes POT alocados (p/ contabilidade de heap) */
    bool    in_vram = false;   /* fix: textura alocada em VRAM (overflow do heap linear) */
    int     create_frame   = 0;  /* SUPER: frame em que foi criada (idade)        */
    int     last_blit_frame = -1; /* SUPER: ultimo frame em que foi desenhada (necessidade) */
    /* [EVICT] back-pointer para o campo 'tex' do Bitmap dono (ex: &b->tex).
     * Permite que a eviccao on-demand ponha *owner_slot=nullptr antes de
     * libertar a textura, evitando use-after-free: o dono fica a saber que
     * perdeu a textura e recria-a a partir de b->pixels (que vivem em RAM)
     * na proxima vez que precisar. nullptr = textura sem dono registado
     * (nunca sera' evictada, por seguranca). */
    DS3Texture** owner_slot = nullptr;
};

void        display_3ds_init();
void        display_3ds_begin_frame();
void        display_3ds_end_frame();
DS3Texture* display_3ds_create_texture(int w, int h, const unsigned char* rgba, bool prefer_vram = false);
/* [FPS] Atualiza os pixels de uma textura EXISTENTE in-place (sem C3D_TexInit, sem
 * free/evict, SEM fechar o frame GPU). So' quando as dims (w,h) coincidem. Devolve
 * true se atualizou; false -> o chamador cai no create_texture normal. Mata o
 * re-upload de 4MB/frame do @layer0 do tilemap e dos autotiles animados. */
bool        display_3ds_update_texture(DS3Texture* t, int w, int h, const unsigned char* rgba);
void        display_3ds_free_texture(DS3Texture* t);
/* SUPERDBG: contagem/bytes das texturas vivas (out params podem ser NULL). */
void        display_3ds_tex_stats(size_t* out_count, size_t* out_bytes, size_t* out_vram);
/* [EVICT] Regista o slot do Bitmap dono (ex: &b->tex) na textura, para que a
 * eviccao on-demand possa libertar texturas idle em seguranca. Chamar logo
 * apos atribuir a textura ao dono: b->tex = create_texture(...);
 * display_3ds_set_texture_owner(b->tex, &b->tex); */
void        display_3ds_set_texture_owner(DS3Texture* t, DS3Texture** slot);
/* Define o filtro de amostragem da textura na GPU (custo zero).
 * linear=true -> GPU_LINEAR (suave, bom p/ downscale de TEXTO);
 * linear=false -> GPU_NEAREST (nitido, bom p/ pixel-art/tilemaps). */
void        display_3ds_set_texture_filter(DS3Texture* t, bool linear);
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
