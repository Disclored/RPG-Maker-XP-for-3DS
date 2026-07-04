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
    bool           has_text;  /* true = teve draw_text -> usar filtro LINEAR no
                               * downscale (texto pequeno fica legivel; o NEAREST
                               * larga pixeis e parte as letras). Filtro de GPU =
                               * custo zero (sem CPU/RAM extra). */
    bool           tex_failed; /* true = a criacao da textura GPU JA' falhou para
                               * este bitmap (ex: imagem >1024 que nao cabe no POT
                               * 1024x1024, ou VRAM esgotada de forma persistente).
                               * Evita re-tentar create_texture TODOS os frames
                               * (que fazia EVICT-thrashing + C3D_TexInit em loop ->
                               * 2.2 fps). Limpo automaticamente quando tex_dirty
                               * volta a true (pixels mudaram -> vale a pena tentar
                               * outra vez). */
    bool           pixels_linear; /* true = b->pixels veio de linearAlloc (fallback
                               * quando o heap de malloc esta' esgotado pela pool do
                               * mruby) -> bitmap_free tem de usar linearFree, nao
                               * free. Default false (malloc/stb/calloc). */
    bool           pixels_4444; /* true = b->pixels esta' em RGBA4444 (2 bytes/px,
                               * um u16 por pixel: RRRRGGGGBBBBAAAA) em vez de RGBA8888
                               * (4 bytes/px). USADO so' p/ TILESETS GIGANTES (o 8888
                               * de 30MB nao cabe; 4444 = 15MB cabe). Quem LE b->pixels
                               * (blt/stretch_blt/get_pixel) tem de expandir via
                               * px_read(). NUNCA e' destino de escrita nem textura. */
    int            tex_scale; /* FATOR DE DOWNSCALE da textura vs dimensoes reportadas.
                               * Default 0/1 = textura 1:1 (caso normal, intocado). >1
                               * = a TEXTURA GPU foi reduzida por este fator (ex: 2 =
                               * metade) para caber no limite de 1024 do PICA200 e/ou
                               * poupar RAM, MAS width/height continuam a reportar as
                               * dimensoes ORIGINAIS ao Ruby (posicionamento/zoom do
                               * jogo inalterados). O desenho divide o rect de origem
                               * por este fator (ver sprite_binding). Usado p/ PICTURES
                               * sobredimensionadas (introPictures 1696px > 1024) que
                               * de outra forma ficavam magenta. NUNCA p/ tilesets (sao
                               * fonte de blt em RAM, nao textura). */
    int            tex_w;     /* largura REAL da textura/buffer quando tex_scale>1
                               * (= width/tex_scale). 0 se nao aplicavel. */
    int            tex_h;     /* altura REAL da textura/buffer quando tex_scale>1. */
    bool           prefer_vram; /* true = preferir alocar a TEXTURA GPU em VRAM (nao no
                               * heap linear). Marcado pelo probe nos TILESETS
                               * COMPACTADOS (512x1024=2MB, estaticos, never_dispose):
                               * cabem num banco de VRAM (3MB) e sao construidos 1x.
                               * Po'-los em VRAM tira-os do heap linear -> deixa de o
                               * fragmentar -> a camada do tilemap (832x704->1024x1024
                               * =4MB, que NAO cabe em VRAM nem em linear fragmentado)
                               * passa a caber no linear contiguo. Default false. */
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
