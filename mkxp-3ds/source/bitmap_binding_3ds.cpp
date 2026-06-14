#include "bitmap_binding_3ds.h"
#include "display_3ds.h"
#include "debug_3ds.h"
#include <3ds.h>
#include <citro2d.h>
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/value.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"

/* ── FIX CRÍTICO: usar mrb_get_argc em vez de mrb_get_args("*") ──────────────
 * Em mruby 3.2, chamar mrb_get_args(mrb, "*", &argv, &argc) pode "consumir"
 * os argumentos do frame actual, fazendo com que chamadas subsequentes a
 * mrb_get_args(mrb, "o"/"ii", ...) vejam argc=0 e lancem
 * ArgumentError: wrong number of arguments (given N, expected 0).
 *
 * mrb_get_argc(mrb) lê o contador sem modificar o estado do frame, sendo
 * seguro para usar antes de qualquer outra chamada a mrb_get_args.
 * ─────────────────────────────────────────────────────────────────────────── */

struct Color3 { float r, g, b, a; };
struct Rect3  { int   x, y, w, h; };

static bool extract_rect(mrb_state *mrb, mrb_value obj, int *x, int *y, int *w, int *h) {
    void *ptr = DATA_PTR(obj);
    if (ptr) {
        Rect3 *r = (Rect3*)ptr;
        *x = r->x; *y = r->y; *w = r->w; *h = r->h;
        return true;
    }
    mrb_value vx = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "@x"));
    mrb_value vy = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "@y"));
    mrb_value vw = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "@width"));
    mrb_value vh = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "@height"));
    if (mrb_nil_p(vx)) return false;
    *x = (int)mrb_integer(mrb_to_int(mrb, vx));
    *y = (int)mrb_integer(mrb_to_int(mrb, vy));
    *w = (int)mrb_integer(mrb_to_int(mrb, vw));
    *h = (int)mrb_integer(mrb_to_int(mrb, vh));
    return true;
}

static bool extract_color(mrb_state *mrb, mrb_value obj, float *r, float *g, float *b, float *a) {
    void *ptr = DATA_PTR(obj);
    if (ptr) {
        Color3 *c = (Color3*)ptr;
        *r = c->r; *g = c->g; *b = c->b; *a = c->a;
        return true;
    }
    mrb_value vr = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "@red"));
    mrb_value vg = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "@green"));
    mrb_value vb = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "@blue"));
    mrb_value va = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "@alpha"));
    if (mrb_nil_p(vr)) return false;
    *r = (float)mrb_float(mrb_to_float(mrb, vr));
    *g = (float)mrb_float(mrb_to_float(mrb, vg));
    *b = (float)mrb_float(mrb_to_float(mrb, vb));
    *a = (float)mrb_float(mrb_to_float(mrb, va));
    return true;
}

static void bitmap_free(mrb_state*, void *p) {
    Bitmap3DS *b = (Bitmap3DS*)p;
    if (!b) return;
    if (b->pixels) free(b->pixels);
    if (b->tex)    display_3ds_free_texture(b->tex);
    free(b);
}
const mrb_data_type Bitmap3DSType = { "Bitmap", bitmap_free };

#define GET_BMP(s) ((Bitmap3DS*)DATA_PTR(s))

static Bitmap3DS *bmp_alloc(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    Bitmap3DS *b  = (Bitmap3DS*)calloc(1, sizeof(Bitmap3DS));
    b->width      = w;
    b->height     = h;
    b->pixels     = (unsigned char*)calloc(w * h, 4);
    b->tex_dirty  = true;
    b->tex        = nullptr;
    return b;
}

void __attribute__((used)) bmp_flush(Bitmap3DS *b) {
    if (!b || !b->tex_dirty || b->disposed) return;
    if (b->tex) {
        display_3ds_free_texture(b->tex);
        b->tex = nullptr;
    }
    if (b->pixels && b->width > 0 && b->height > 0) {
        b->tex = display_3ds_create_texture(b->width, b->height, b->pixels);
        if (!b->tex)
            printf("[BMP] AVISO: create_texture falhou %dx%d\n", b->width, b->height);
        else if (b->has_text) {
            /* texto: LINEAR no downscale (GPU, custo zero) -> legivel em vez de papa */
            display_3ds_set_texture_filter(b->tex, true);
            b->tex->has_text = true;   /* DIAG: marca a textura p/ log TEXFLOW no ecra */
        }
    }
    b->tex_dirty = false;
}

void bmp_ensure_texture(Bitmap3DS *b) {
    bmp_flush(b);
}

static const char *s_game_roots[] = {
    "sdmc:/mkxp/game/",
    "sdmc:/mkxp/",
    "romfs:/",
    "",
    nullptr
};

static void str_tolower(char *s) {
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32;
}

static Bitmap3DS *bmp_load_file(const char *path) {
    if (!path || path[0] == '\0') {
        printf("[BMP] AVISO: bmp_load_file chamado com path VAZIO\n");
        fflush(stdout);
        Bitmap3DS *b = bmp_alloc(32, 32);
        for (int i = 0; i < 32 * 32; i++) {
            b->pixels[i*4+0] = 255;
            b->pixels[i*4+1] = 0;
            b->pixels[i*4+2] = 255;
            b->pixels[i*4+3] = 255;
        }
        b->tex_dirty = true;
        bmp_flush(b);
        return b;
    }

    /* FIX/PREVENCAO imagens que nao carregam: a lista de extensoes tinha de
     * cobrir TODAS as que o Essentials usa. Faltava .gif (usado em varias
     * imagens; o pbResolveBitmap tenta .png E .gif). Sem .gif aqui, uma imagem
     * .gif resolvia o nome mas o stbi_load falhava -> quadrado magenta. Tambem
     * incluimos variantes em MAIUSCULAS (.PNG/.GIF) porque alguns ficheiros do
     * jogo usam extensao maiuscula (ex: uparrow.PNG) e o filesystem do 3DS e'
     * sensivel a maiusculas/minusculas. */
    static const char *exts[] = { "", ".png", ".gif", ".jpg", ".jpeg", ".bmp",
                                  ".PNG", ".GIF", ".JPG", ".BMP", nullptr };

    for (int ri = 0; s_game_roots[ri] != nullptr; ri++) {
        for (int ei = 0; exts[ei] != nullptr; ei++) {
            char full[512];
            const char *root = s_game_roots[ri];
            if (root[0] != '\0' && path[0] == '/') {
                snprintf(full, sizeof(full), "%s%s%s", root, path + 1, exts[ei]);
            } else {
                snprintf(full, sizeof(full), "%s%s%s", root, path, exts[ei]);
            }

            int w, h, ch;
            unsigned char *px = stbi_load(full, &w, &h, &ch, 4);
            if (px) {
                /* diagnostico: para imagens do titulo (MODTS/Titles), mostra
                 * que caminho EXATO resolveu e o tamanho. Ajuda a perceber porque
                 * start.png / pokemon.png apareciam a 1x1. */
                if (strstr(path, "MODTS") || strstr(path, "Titles") ||
                    strstr(path, "start") || strstr(path, "pokemon")) {
                    printf("[BMP|OK] '%s' -> '%s' (%dx%d)\n", path, full, w, h);
                    fflush(stdout);
                }
                Bitmap3DS *b = bmp_alloc(w, h);
                memcpy(b->pixels, px, (size_t)(w * h * 4));
                stbi_image_free(px);
                b->tex_dirty = true;
                bmp_flush(b);
                return b;
            }

            char full_lc[512];
            snprintf(full_lc, sizeof(full_lc), "%s", full);
            char *dot = nullptr;
            for (char *p = full_lc; *p; p++) if (*p == '.') dot = p;
            if (dot) str_tolower(dot);
            if (dot && strcmp(full_lc, full) != 0) {
                px = stbi_load(full_lc, &w, &h, &ch, 4);
                if (px) {
                    Bitmap3DS *b = bmp_alloc(w, h);
                    memcpy(b->pixels, px, (size_t)(w * h * 4));
                    stbi_image_free(px);
                    b->tex_dirty = true;
                    bmp_flush(b);
                    return b;
                }
            }
        }
    }

    /* LOGGING DETALHADO de falha (prevencao/diagnostico futuro): regista o nome
     * pedido E todas as raizes/extensoes tentadas, para que qualquer imagem que
     * venha a falhar fique identificada no log com precisao (caminho exato que
     * faltou). Sem isto, so' se via "magenta" no ecra sem saber o ficheiro.
     * Casos previstos a falhar (do Graphics.zip analisado): Graphics/System/*
     * (pasta inexistente), Graphics/Battlers/eggCracks (em falta).
     * IMPORTANTE: escreve no stdout E no ficheiro debug_binding.log (g_dbglog),
     * para o diagnostico ficar no log persistente como o resto. */
    {
        extern FILE *g_dbglog;
        printf("[BMP|MISS] FALHOU carregar: '%s' -- fallback 32x32 magenta\n", path);
        if (g_dbglog) fprintf(g_dbglog, "[BMP|MISS] FALHOU carregar: '%s' -- fallback 32x32 magenta\n", path);
        int logged = 0;
        for (int ri = 0; s_game_roots[ri] != nullptr && logged < 3; ri++) {
            char shown[512];
            const char *root = s_game_roots[ri];
            if (root[0] != '\0' && path[0] == '/')
                snprintf(shown, sizeof(shown), "%s%s", root, path + 1);
            else
                snprintf(shown, sizeof(shown), "%s%s", root, path);
            printf("[BMP|MISS]   tentou raiz[%d]: '%s' (+ .png/.gif/.PNG/...)\n", ri, shown);
            if (g_dbglog) fprintf(g_dbglog, "[BMP|MISS]   tentou raiz[%d]: '%s' (+ .png/.gif/.PNG/...)\n", ri, shown);
            logged++;
        }
        if (g_dbglog) fflush(g_dbglog);
    }
    fflush(stdout);
    Bitmap3DS *b = bmp_alloc(32, 32);
    for (int i = 0; i < 32 * 32; i++) {
        b->pixels[i*4+0] = 255;
        b->pixels[i*4+1] = 0;
        b->pixels[i*4+2] = 255;
        b->pixels[i*4+3] = 255;
    }
    b->tex_dirty = true;
    bmp_flush(b);
    return b;
}

/* ── bmp_init ─────────────────────────────────────────────────────────────────
 * CORRECÇÃO CRÍTICA: usar mrb_get_argc() para contar argumentos SEM modificar
 * o estado do frame de chamada.
 *
 * A versão anterior usava mrb_argc_compat() que chamava mrb_get_args(mrb,"*",...)
 * internamente. Em mruby 3.2, mrb_get_args("*") consome os argumentos do frame
 * actual, deixando argc=0 para todas as chamadas subsequentes a mrb_get_args.
 * Isso causava o erro "wrong number of arguments (given 2, expected 1)" quando
 * o código tentava mrb_get_args(mrb,"o",&arg) depois de mrb_get_args(mrb,"*",...).
 *
 * mrb_get_argc(mrb) é seguro: lê apenas o contador sem tocar nos argumentos.
 * ─────────────────────────────────────────────────────────────────────────── */
mrb_value bmp_init(mrb_state *mrb, mrb_value self) {
    mrb_data_init(self, nullptr, &Bitmap3DSType);

    /* FIX: usar mrb_get_argc em vez de mrb_argc_compat para evitar
     * o problema de double-consume de argumentos em mruby 3.2 */
    mrb_int   argc = mrb_get_argc(mrb);
    Bitmap3DS *b   = nullptr;

    if (argc >= 2) {
        /* Forma (width, height) -- mais frequente no PE */
        mrb_int w, h;
        if (mrb_get_args(mrb, "ii", &w, &h) == 2) {
            b = bmp_alloc((int)w, (int)h);
        } else {
            /* Tipo não era inteiro -- tentar como objecto */
            mrb_value a1, a2;
            mrb_get_args(mrb, "oo", &a1, &a2);
            if (mrb_integer_p(a1) && mrb_integer_p(a2)) {
                b = bmp_alloc((int)mrb_integer(a1), (int)mrb_integer(a2));
            } else {
                b = bmp_alloc(1, 1);
            }
        }
    } else if (argc == 1) {
        mrb_value arg;
        mrb_get_args(mrb, "o", &arg);
        if (mrb_string_p(arg)) {
            const char *req_path = mrb_str_to_cstr(mrb, arg);
            b = bmp_load_file(req_path);
        } else if (mrb_integer_p(arg)) {
            /* Só um inteiro -- usar como dimensão quadrada */
            mrb_int sz = mrb_integer(arg);
            b = bmp_alloc((int)sz, (int)sz);
        } else if (!mrb_nil_p(arg)) {
            /* Objecto Bitmap passado directamente (estilo BitmapWrapper original PE).
             * Copiar pixel data se disponível, ou criar 1x1. */
            Bitmap3DS *src = (Bitmap3DS*)DATA_PTR(arg);
            if (src && src->width > 0 && src->height > 0 && src->pixels) {
                b = bmp_alloc(src->width, src->height);
                memcpy(b->pixels, src->pixels, (size_t)(src->width * src->height * 4));
                b->tex_dirty = true;
                bmp_flush(b);
            } else {
                /* Tentar obter via ivar @bitmap (BitmapWrapper aninhado) */
                mrb_value inner = mrb_iv_get(mrb, arg, mrb_intern_lit(mrb, "@bitmap"));
                if (!mrb_nil_p(inner)) {
                    src = (Bitmap3DS*)DATA_PTR(inner);
                }
                if (src && src->width > 0 && src->height > 0 && src->pixels) {
                    b = bmp_alloc(src->width, src->height);
                    memcpy(b->pixels, src->pixels, (size_t)(src->width * src->height * 4));
                    b->tex_dirty = true;
                    bmp_flush(b);
                } else {
                    b = bmp_alloc(1, 1);
                }
            }
        } else {
            b = bmp_alloc(1, 1);
        }
    } else {
        b = bmp_alloc(1, 1);
    }

    DATA_PTR(self)  = b;
    DATA_TYPE(self) = &Bitmap3DSType;
    return self;
}

static mrb_value bmp_dispose(mrb_state*, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    if (b) b->disposed = true;
    return mrb_nil_value();
}
static mrb_value bmp_disposed(mrb_state*, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    return mrb_bool_value(!b || b->disposed);
}
static mrb_value bmp_width(mrb_state*, mrb_value s) {
    Bitmap3DS *b = GET_BMP(s); return mrb_fixnum_value(b ? b->width  : 0); }
static mrb_value bmp_height(mrb_state*, mrb_value s) {
    Bitmap3DS *b = GET_BMP(s); return mrb_fixnum_value(b ? b->height : 0); }

static mrb_value bmp_rect(mrb_state *mrb, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    RClass *rk = mrb_class_get(mrb, "Rect");
    mrb_value argv[4] = {
        mrb_fixnum_value(0), mrb_fixnum_value(0),
        mrb_fixnum_value(b ? b->width : 0), mrb_fixnum_value(b ? b->height : 0)
    };
    return mrb_obj_new(mrb, rk, 4, argv);
}

static mrb_value bmp_clear(mrb_state*, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    if (b && b->pixels) {
        memset(b->pixels, 0, (size_t)(b->width * b->height * 4));
        b->tex_dirty = true;
    }
    return mrb_nil_value();
}

static mrb_value bmp_fill_rect(mrb_state *mrb, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    if (!b) return mrb_nil_value();

    int x = 0, y = 0, w = 0, h = 0;
    float cr = 255, cg = 255, cb = 255, ca = 255;

    mrb_int argc = mrb_get_argc(mrb);
    if (argc == 2) {
        mrb_value ro, co; mrb_get_args(mrb, "oo", &ro, &co);
        if (!extract_rect(mrb, ro, &x, &y, &w, &h))      return mrb_nil_value();
        if (!extract_color(mrb, co, &cr, &cg, &cb, &ca)) return mrb_nil_value();
    } else {
        mrb_int xi, yi, wi, hi; mrb_value co;
        mrb_get_args(mrb, "iiiio", &xi, &yi, &wi, &hi, &co);
        x = (int)xi; y = (int)yi; w = (int)wi; h = (int)hi;
        if (!extract_color(mrb, co, &cr, &cg, &cb, &ca)) return mrb_nil_value();
    }

    unsigned char R = (unsigned char)cr, G = (unsigned char)cg,
                  B = (unsigned char)cb, A = (unsigned char)ca;
    for (int py = y; py < y + h && py < b->height; py++) {
        if (py < 0) continue;
        for (int px2 = x; px2 < x + w && px2 < b->width; px2++) {
            if (px2 < 0) continue;
            int idx = (py * b->width + px2) * 4;
            b->pixels[idx]   = R;
            b->pixels[idx+1] = G;
            b->pixels[idx+2] = B;
            b->pixels[idx+3] = A;
        }
    }
    b->tex_dirty = true;
    return mrb_nil_value();
}

static mrb_value bmp_blt(mrb_state *mrb, mrb_value self) {
    Bitmap3DS *dst = GET_BMP(self);
    if (!dst) return mrb_nil_value();
    mrb_int dx, dy, opacity = 255;
    mrb_value srco, sro;
    mrb_get_args(mrb, "iioo|i", &dx, &dy, &srco, &sro, &opacity);
    Bitmap3DS *src = (Bitmap3DS*)DATA_PTR(srco);
    if (!src || !src->pixels) return mrb_nil_value();
    int sx0 = 0, sy0 = 0, sw = 0, sh = 0;
    if (!extract_rect(mrb, sro, &sx0, &sy0, &sw, &sh)) return mrb_nil_value();
    float alpha = (float)opacity / 255.0f;
    for (int sy = sy0, dy2 = (int)dy; sy < sy0 + sh; sy++, dy2++) {
        if (sy < 0 || sy >= src->height || dy2 < 0 || dy2 >= dst->height) continue;
        for (int sxx = sx0, dx2 = (int)dx; sxx < sx0 + sw; sxx++, dx2++) {
            if (sxx < 0 || sxx >= src->width || dx2 < 0 || dx2 >= dst->width) continue;
            int si = (sy * src->width + sxx) * 4;
            int di = (dy2 * dst->width + dx2) * 4;
            float sa = src->pixels[si+3] / 255.0f * alpha;
            float da = 1.0f - sa;
            dst->pixels[di]   = (unsigned char)(src->pixels[si]   * sa + dst->pixels[di]   * da);
            dst->pixels[di+1] = (unsigned char)(src->pixels[si+1] * sa + dst->pixels[di+1] * da);
            dst->pixels[di+2] = (unsigned char)(src->pixels[si+2] * sa + dst->pixels[di+2] * da);
            dst->pixels[di+3] = (unsigned char)(sa * 255.0f       + dst->pixels[di+3]      * da);
        }
    }
    dst->tex_dirty = true;
    /* DIAG TEXFLOW: copia que ENVOLVE texto (origem ou destino). Mostra a cadeia:
     * bitmap-de-texto -> contents/janela. Incondicional (so' p/ bitmaps de texto,
     * nao inunda com o tilemap). Se a origem tem texto, o destino passa a ter
     * (segue a cadeia + faz o destino usar LINEAR). */
    if (src->has_text || dst->has_text) {
        printf("[TEXFLOW|BLT] src=%dx%d(txt=%d) rect=(%d,%d,%d,%d) -> dst=%dx%d(txt=%d) @(%d,%d) op=%d\n",
            src->width, src->height, src->has_text?1:0, sx0, sy0, sw, sh,
            dst->width, dst->height, dst->has_text?1:0, (int)dx, (int)dy, (int)opacity);
        if (src->has_text) dst->has_text = true;
    }
    /* LOG (DBG_DIALOG): imagem copiada (retrato, moldura) -- origem WxH + rect,
     * destino WxH + posicao. Revela tamanhos das imagens das caixas. */
    DBG(DBG_DIALOG, "[BLT] src=%dx%d rect=(%d,%d,%d,%d) -> dst=%dx%d @(%d,%d) op=%d",
        src->width, src->height, sx0, sy0, sw, sh,
        dst->width, dst->height, (int)dx, (int)dy, (int)opacity);
    return mrb_nil_value();
}

static mrb_value bmp_stretch_blt(mrb_state *mrb, mrb_value self) {
    Bitmap3DS *dst = GET_BMP(self);
    if (!dst) return mrb_nil_value();

    mrb_value dro, srco, sro;
    mrb_int opacity = 255;
    mrb_int argc = mrb_get_argc(mrb);
    if (argc >= 3) {
        if (argc >= 4)
            mrb_get_args(mrb, "ooo|i", &dro, &srco, &sro, &opacity);
        else
            mrb_get_args(mrb, "ooo", &dro, &srco, &sro);
    } else {
        return mrb_nil_value();
    }

    Bitmap3DS *src = (Bitmap3DS*)DATA_PTR(srco);
    if (!src || !src->pixels) return mrb_nil_value();

    int dx, dy, dw, dh, sx0, sy0, sw, sh;
    if (!extract_rect(mrb, dro, &dx, &dy, &dw, &dh)) return mrb_nil_value();
    if (!extract_rect(mrb, sro, &sx0, &sy0, &sw, &sh)) return mrb_nil_value();
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return mrb_nil_value();

    float alpha   = (float)opacity / 255.0f;
    float sx_step = (float)sw / (float)dw;
    float sy_step = (float)sh / (float)dh;

    for (int py = 0; py < dh; py++) {
        int dpy = dy + py;
        if (dpy < 0 || dpy >= dst->height) continue;
        int spy = sy0 + (int)(py * sy_step);
        if (spy < 0 || spy >= src->height) continue;
        for (int px2 = 0; px2 < dw; px2++) {
            int dpx = dx + px2;
            if (dpx < 0 || dpx >= dst->width) continue;
            int spx = sx0 + (int)(px2 * sx_step);
            if (spx < 0 || spx >= src->width) continue;
            int si = (spy * src->width + spx) * 4;
            int di = (dpy * dst->width + dpx) * 4;
            float sa = src->pixels[si+3] / 255.0f * alpha;
            float da = 1.0f - sa;
            dst->pixels[di]   = (unsigned char)(src->pixels[si]   * sa + dst->pixels[di]   * da);
            dst->pixels[di+1] = (unsigned char)(src->pixels[si+1] * sa + dst->pixels[di+1] * da);
            dst->pixels[di+2] = (unsigned char)(src->pixels[si+2] * sa + dst->pixels[di+2] * da);
            dst->pixels[di+3] = (unsigned char)(sa * 255.0f       + dst->pixels[di+3]      * da);
        }
    }
    dst->tex_dirty = true;
    /* DIAG TEXFLOW: ESTICAMENTO que envolve texto. ESTE e' o suspeito principal:
     * se o bitmap de texto (ex: 216x32) for esticado para a janela (ex: 512x96),
     * o fator de stretch >1 explica o borrao. Incondicional p/ bitmaps de texto. */
    if (src->has_text || dst->has_text) {
        printf("[TEXFLOW|STRETCH] src=%dx%d(txt=%d) s=(%d,%d,%d,%d) -> dst=%dx%d(txt=%d) d=(%d,%d,%d,%d) stretch=x%.2f,y%.2f\n",
            src->width, src->height, src->has_text?1:0, sx0, sy0, sw, sh,
            dst->width, dst->height, dst->has_text?1:0, dx, dy, dw, dh,
            (sw>0?(float)dw/sw:0.0f), (sh>0?(float)dh/sh:0.0f));
        if (src->has_text) dst->has_text = true;
    }
    /* LOG (DBG_DIALOG): esticamento -- e' assim que a windowskin (ex: 96x48) e'
     * redimensionada para a moldura da caixa. O retangulo DESTINO (dx,dy,dw,dh)
     * = dimensoes reais da janela desenhada. Compara com o tamanho esperado. */
    DBG(DBG_DIALOG, "[STRETCH] src=%dx%d s=(%d,%d,%d,%d) -> dst=%dx%d d=(%d,%d,%d,%d)",
        src->width, src->height, sx0, sy0, sw, sh,
        dst->width, dst->height, dx, dy, dw, dh);
    return mrb_nil_value();
}

/* ============================================================================
 * RENDERIZACAO DE TEXTO — fonte do sistema 3DS (BCFNT) rasterizada para o
 * buffer de pixeis do Bitmap.
 *
 * Porque CPU/rasterizacao e nao C2D_DrawText: o draw_text do RGSS desenha para
 * a TEXTURA de um Bitmap (ex.: o `contents` de uma janela), que o jogo depois
 * compoe via Sprite. C2D_DrawText desenha no render target do ecra durante um
 * frame -- nao serve para escrever num bitmap arbitrario. Por isso lemos os
 * glifos da sheet BCFNT (formato tiled/morton, alpha) e fazemos alpha-blend no
 * buffer RGBA do bitmap, na cor do texto. Da' texto com acentos (latino
 * completo da fonte do sistema) e encaixa no pipeline de sprites existente.
 * ========================================================================== */

/* Des-tiling Morton (igual ao do display_3ds.cpp): dado (x,y) numa textura de
 * largura tw (em pixeis, multipla de 8), devolve o indice linear do pixel no
 * buffer tiled em blocos de 8x8. */
/* Des-tiling Morton: usa exatamente a mesma formula comprovada do
 * display_3ds.cpp (tabela de swizzle), para garantir consistencia com o resto
 * do pipeline de texturas. (x,y) em pixeis, tw = largura da sheet (mult. de 8). */
static inline int fnt_morton_offset(int x, int y, int tw) {
    static const int t[8] = {0,1,4,5,16,17,20,21};
    return ((y>>3)*(tw>>3)+(x>>3))*64 + t[x&7] + (t[y&7]<<1);
}

/* Le o valor alpha (0..255) de um glifo na sheet BCFNT, na coordenada (gx,gy)
 * em pixeis dentro da sheet. As sheets do sistema sao tipicamente A4 (4 bits
 * por pixel, so alpha) mas tratamos tambem A8. tw/th = dimensoes da sheet. */
static inline int fnt_sheet_alpha(const u8 *sheet, int gx, int gy,
                                  int tw, int th, GPU_TEXCOLOR fmt) {
    if (gx < 0 || gy < 0 || gx >= tw || gy >= th) return 0;
    int off = fnt_morton_offset(gx, gy, tw);
    if (fmt == GPU_A4) {
        /* 4 bits por pixel: 2 pixeis por byte */
        int byteOff = off >> 1;
        u8 byte = sheet[byteOff];
        int nib = (off & 1) ? (byte >> 4) : (byte & 0x0F);
        return nib * 17;            /* 0..15 -> 0..255 */
    } else { /* GPU_A8 */
        return sheet[off];
    }
}

/* Decodifica um codepoint UTF-8 de `s` (avanca `*i`). Devolve o codepoint. */
static u32 fnt_utf8_next(const char *s, int len, int *i) {
    unsigned char c = (unsigned char)s[*i];
    if (c < 0x80) { (*i)++; return c; }
    if ((c >> 5) == 0x6 && *i + 1 < len) {
        u32 cp = ((c & 0x1F) << 6) | ((unsigned char)s[*i+1] & 0x3F);
        *i += 2; return cp;
    }
    if ((c >> 4) == 0xE && *i + 2 < len) {
        u32 cp = ((c & 0x0F) << 12) | (((unsigned char)s[*i+1] & 0x3F) << 6) |
                 ((unsigned char)s[*i+2] & 0x3F);
        *i += 3; return cp;
    }
    if ((c >> 3) == 0x1E && *i + 3 < len) {
        u32 cp = ((c & 0x07) << 18) | (((unsigned char)s[*i+1] & 0x3F) << 12) |
                 (((unsigned char)s[*i+2] & 0x3F) << 6) |
                 ((unsigned char)s[*i+3] & 0x3F);
        *i += 4; return cp;
    }
    (*i)++; return c;               /* byte invalido: avanca 1 */
}

/* Escala do texto. A fonte do sistema e' grande (~30px de linha); o RGSS usa
 * tipicamente ~24-32px de altura de linha. Usamos uma escala que da' ~22px de
 * altura, adequada para caber nas caixas do Essentials. */
static const float FNT_SCALE = 0.70f;

/* Garante que a fonte do sistema esta' carregada e mapeada. fontGetSystemFont()
 * pode devolver a fonte mas as sheets de glifos so' ficam acessiveis apos
 * fontEnsureMapped(). Chamado (idempotente) antes do 1o desenho. */
static CFNT_s *fnt_system(void) {
    static bool tried = false;
    static CFNT_s *cached = 0;
    if (!tried) {
        tried = true;
        cached = fontGetSystemFont();
        if (cached) {
            /* mapeia a fonte partilhada para acesso aos glifos (idempotente) */
            fontEnsureMapped();
        }
        if (cached) printf("[FONT] system font OK\n");
        else        printf("[FONT] system font INDISPONIVEL (texto nao desenha)\n");
    }
    return cached;
}

/* Mede a largura (em pixeis) que uma string ocuparia, a' escala FNT_SCALE. */
static int fnt_measure_width(const char *str, int len) {
    CFNT_s *font = fnt_system();
    if (!font) return len * 8;          /* fallback grosseiro */
    float w = 0.0f;
    int i = 0;
    while (i < len) {
        u32 cp = fnt_utf8_next(str, len, &i);
        if (cp == '\n') break;
        int idx = fontGlyphIndexFromCodePoint(font, cp);
        charWidthInfo_s *cwi = fontGetCharWidthInfo(font, idx);
        if (cwi) w += cwi->charWidth * FNT_SCALE;
    }
    return (int)(w + 0.5f);
}

/* Desenha `str` no bitmap `b`, comecando em (x,y) (canto sup-esq), na cor
 * (R,G,B,A). Rasteriza cada glifo da fonte do sistema com alpha-blend. */
static void fnt_draw_into_bitmap(Bitmap3DS *b, int x, int y,
                                 const char *str, int len,
                                 u8 R, u8 G, u8 B, u8 A) {
    if (!b || !b->pixels || len <= 0) return;
    CFNT_s *font = fnt_system();
    if (!font) return;
    TGLP_s *tglp = fontGetGlyphInfo(font);
    if (!tglp) return;

    int sheetW = tglp->sheetWidth;
    int sheetH = tglp->sheetHeight;
    GPU_TEXCOLOR fmt = (GPU_TEXCOLOR)tglp->sheetFmt;
    /* a baseline do texto fica mais abaixo; cellHeight inclui ascender. */
    float penX = (float)x;

    int i = 0;
    while (i < len) {
        u32 cp = fnt_utf8_next(str, len, &i);
        if (cp == '\n') break;          /* uma linha por chamada (RGSS) */

        int gidx = fontGlyphIndexFromCodePoint(font, cp);
        fontGlyphPos_s gp;
        fontCalcGlyphPos(&gp, font, gidx, GLYPH_POS_CALC_VTXCOORD,
                         FNT_SCALE, FNT_SCALE);

        const u8 *sheet = (const u8*)fontGetGlyphSheetTex(font, gp.sheetIndex);
        if (!sheet) { penX += gp.xAdvance; continue; }

        /* UVs do glifo na sheet (0..1) -> pixeis */
        int gx0 = (int)(gp.texcoord.left   * sheetW + 0.5f);
        int gx1 = (int)(gp.texcoord.right  * sheetW + 0.5f);
        /* nota: top/bottom invertidos no atlas (top = maior V) */
        int gy0 = (int)((1.0f - gp.texcoord.top)    * sheetH + 0.5f);
        int gy1 = (int)((1.0f - gp.texcoord.bottom) * sheetH + 0.5f);
        if (gy0 > gy1) { int t = gy0; gy0 = gy1; gy1 = t; }
        if (gx0 > gx1) { int t = gx0; gx0 = gx1; gx1 = t; }

        int gw = gx1 - gx0;
        int gh = gy1 - gy0;
        if (gw <= 0 || gh <= 0) { penX += gp.xAdvance; continue; }

        /* dimensoes de destino (vtx coords ja' escaladas) */
        float dstW = gp.vtxcoord.right  - gp.vtxcoord.left;
        float dstH = gp.vtxcoord.bottom - gp.vtxcoord.top;
        if (dstW <= 0) dstW = gw;
        if (dstH <= 0) dstH = gh;

        int baseX = (int)(penX + gp.vtxcoord.left + 0.5f);
        int baseY = (int)(y    + gp.vtxcoord.top  + 0.5f);

        /* amostragem BILINEAR da sheet para o destino.
         * Antes: nearest (1 pixel) -> bordas em escada + ilegivel ao reduzir.
         * Agora: media ponderada dos 4 vizinhos pela posicao fracionaria ->
         * texto suave e legivel mesmo com FNT_SCALE pequeno. Custa 4 leituras
         * por pixel (so' no desenho de texto, nao por frame inteiro). */
        for (int dy = 0; dy < (int)dstH; dy++) {
            int py = baseY + dy;
            if (py < 0 || py >= b->height) continue;
            float fsy = gy0 + (dy + 0.5f) * (float)gh / dstH - 0.5f;
            int   sy0 = (int)floorf(fsy);
            float wy  = fsy - sy0;
            for (int dx = 0; dx < (int)dstW; dx++) {
                int px = baseX + dx;
                if (px < 0 || px >= b->width) continue;
                float fsx = gx0 + (dx + 0.5f) * (float)gw / dstW - 0.5f;
                int   sx0 = (int)floorf(fsx);
                float wx  = fsx - sx0;

                /* 4 vizinhos (clamp implicito via fnt_sheet_alpha que devolve 0
                 * fora dos limites; aqui usamos os limites do glifo gx0..gx1). */
                int a00 = fnt_sheet_alpha(sheet, sx0,   sy0,   sheetW, sheetH, fmt);
                int a10 = fnt_sheet_alpha(sheet, sx0+1, sy0,   sheetW, sheetH, fmt);
                int a01 = fnt_sheet_alpha(sheet, sx0,   sy0+1, sheetW, sheetH, fmt);
                int a11 = fnt_sheet_alpha(sheet, sx0+1, sy0+1, sheetW, sheetH, fmt);
                float top = a00 * (1.0f - wx) + a10 * wx;
                float bot = a01 * (1.0f - wx) + a11 * wx;
                int a = (int)(top * (1.0f - wy) + bot * wy + 0.5f);
                if (a <= 0) continue;

                /* alpha do glifo * alpha do texto */
                int af = a * A / 255;
                if (af <= 0) continue;
                int idx = (py * b->width + px) * 4;
                int inv = 255 - af;
                b->pixels[idx+0] = (unsigned char)((R * af + b->pixels[idx+0] * inv) / 255);
                b->pixels[idx+1] = (unsigned char)((G * af + b->pixels[idx+1] * inv) / 255);
                b->pixels[idx+2] = (unsigned char)((B * af + b->pixels[idx+2] * inv) / 255);
                int da = b->pixels[idx+3];
                b->pixels[idx+3] = (unsigned char)(af + da * inv / 255);
            }
        }
        penX += gp.xAdvance;
    }
    b->tex_dirty = true;
}

/* draw_text(x, y, width, height, str, align=0)  OU  draw_text(rect, str, align=0)
 * Desenha `str` dentro da area dada, usando a cor da fonte do bitmap
 * (bitmap.font.color) se existir, senao branco. Suporta alinhamento
 * 0=esq, 1=centro, 2=dir. */
static mrb_value bmp_draw_text(mrb_state *mrb, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    if (!b || !b->pixels) return mrb_nil_value();

    int ax = 0, ay = 0, aw = 0, ah = 0, align = 0;
    const char *str = 0; mrb_int slen = 0;

    mrb_int argc = mrb_get_argc(mrb);
    if (argc <= 3) {
        /* forma (rect, str[, align]) */
        mrb_value ro, sv; mrb_int al = 0;
        if (argc == 3) mrb_get_args(mrb, "ooi", &ro, &sv, &al);
        else           mrb_get_args(mrb, "oo",  &ro, &sv);
        if (!extract_rect(mrb, ro, &ax, &ay, &aw, &ah)) return mrb_nil_value();
        align = (int)al;
        mrb_value s2 = (mrb_string_p(sv)) ? sv : mrb_obj_as_string(mrb, sv);
        str = RSTRING_PTR(s2); slen = RSTRING_LEN(s2);
    } else {
        /* forma (x, y, w, h, str[, align]) */
        mrb_int xi, yi, wi, hi, al = 0; mrb_value sv;
        if (argc >= 6) mrb_get_args(mrb, "iiiioi", &xi, &yi, &wi, &hi, &sv, &al);
        else           mrb_get_args(mrb, "iiiio",  &xi, &yi, &wi, &hi, &sv);
        ax = (int)xi; ay = (int)yi; aw = (int)wi; ah = (int)hi; align = (int)al;
        mrb_value s2 = (mrb_string_p(sv)) ? sv : mrb_obj_as_string(mrb, sv);
        str = RSTRING_PTR(s2); slen = RSTRING_LEN(s2);
    }
    if (!str || slen <= 0) return mrb_nil_value();

    /* cor do texto: bitmap.font.color (se existir), senao branco */
    u8 R = 255, G = 255, B = 255, A = 255;
    mrb_value fobj = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@font"));
    if (!mrb_nil_p(fobj)) {
        mrb_value col = mrb_iv_get(mrb, fobj, mrb_intern_lit(mrb, "@color"));
        if (mrb_nil_p(col) && mrb_respond_to(mrb, fobj, mrb_intern_lit(mrb, "color")))
            col = mrb_funcall(mrb, fobj, "color", 0);
        if (!mrb_nil_p(col)) {
            float fr, fg, fb, fa;
            if (extract_color(mrb, col, &fr, &fg, &fb, &fa)) {
                R = (u8)fr; G = (u8)fg; B = (u8)fb; A = (u8)fa;
            }
        }
    }

    /* alinhamento horizontal + centragem vertical aproximada */
    int tw = fnt_measure_width(str, (int)slen);
    int drawX = ax;
    if (align == 1)      drawX = ax + (aw - tw) / 2;   /* centro */
    else if (align == 2) drawX = ax + (aw - tw);       /* direita */
    /* baseline vertical: centra a linha (~22px) na altura da celula */
    int lineH = 22;
    int drawY = ay + ((ah > lineH) ? (ah - lineH) / 2 : 0);

    fnt_draw_into_bitmap(b, drawX, drawY, str, (int)slen, R, G, B, A);
    b->has_text = true;   /* -> textura usa filtro LINEAR (texto legivel no downscale) */
    /* LOG DIAGNOSTICO (DBG_DIALOG): o TEXTO que esta a ser desenhado, em que
     * retangulo, em que bitmap (tamanho), com que cor e largura medida. Mostra
     * o conteudo real das caixas e se o letra-a-letra avanca (strings a crescer). */
    DBG(DBG_DIALOG, "[TXT] '%.50s' rect=(%d,%d,%d,%d) al=%d -> bmp=%dx%d rgba=%d,%d,%d,%d tw=%d",
        str, ax, ay, aw, ah, align, b->width, b->height, R, G, B, A, tw);
    return mrb_nil_value();
}

/* text_size(str) -> Rect(0,0,largura,altura) com a largura REAL do texto. */
static mrb_value bmp_text_size(mrb_state *mrb, mrb_value self) {
    (void)self;
    mrb_value strv;
    int w = 0;
    mrb_int argc = mrb_get_argc(mrb);
    if (argc >= 1) {
        mrb_get_args(mrb, "o", &strv);
        mrb_value s2 = (mrb_string_p(strv)) ? strv : mrb_obj_as_string(mrb, strv);
        w = fnt_measure_width(RSTRING_PTR(s2), (int)RSTRING_LEN(s2));
    }
    RClass *rk = mrb_class_get(mrb, "Rect");
    mrb_value argv[4] = {
        mrb_fixnum_value(0), mrb_fixnum_value(0),
        mrb_fixnum_value(w), mrb_fixnum_value(22)
    };
    return mrb_obj_new(mrb, rk, 4, argv);
}

static mrb_value bmp_get_pixel(mrb_state *mrb, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    mrb_int x, y; mrb_get_args(mrb, "ii", &x, &y);
    float r = 0, g = 0, bl = 0, a = 0;
    if (b && x >= 0 && y >= 0 && x < b->width && y < b->height) {
        int idx = (y * b->width + x) * 4;
        r = b->pixels[idx]; g = b->pixels[idx+1];
        bl = b->pixels[idx+2]; a = b->pixels[idx+3];
    }
    RClass *ck = mrb_class_get(mrb, "Color");
    mrb_value argv[4] = {
        mrb_float_value(mrb, r), mrb_float_value(mrb, g),
        mrb_float_value(mrb, bl), mrb_float_value(mrb, a)
    };
    return mrb_obj_new(mrb, ck, 4, argv);
}

static mrb_value bmp_set_pixel(mrb_state *mrb, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    mrb_int x, y; mrb_value co;
    mrb_get_args(mrb, "iio", &x, &y, &co);
    float cr, cg, cb2, ca;
    if (b && extract_color(mrb, co, &cr, &cg, &cb2, &ca)
         && x >= 0 && y >= 0 && x < b->width && y < b->height) {
        int idx = (y * b->width + x) * 4;
        b->pixels[idx]   = (unsigned char)cr;
        b->pixels[idx+1] = (unsigned char)cg;
        b->pixels[idx+2] = (unsigned char)cb2;
        b->pixels[idx+3] = (unsigned char)ca;
        b->tex_dirty = true;
    }
    return mrb_nil_value();
}

static mrb_value bmp_clear_rect(mrb_state *mrb, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    if (!b) return mrb_nil_value();
    int x = 0, y = 0, w = 0, h = 0;
    mrb_int argc = mrb_get_argc(mrb);
    if (argc == 1) {
        mrb_value ro; mrb_get_args(mrb, "o", &ro);
        if (!extract_rect(mrb, ro, &x, &y, &w, &h)) return mrb_nil_value();
    } else {
        mrb_int xi, yi, wi, hi; mrb_get_args(mrb, "iiii", &xi, &yi, &wi, &hi);
        x = (int)xi; y = (int)yi; w = (int)wi; h = (int)hi;
    }
    for (int py = y; py < y + h && py < b->height; py++) {
        if (py < 0) continue;
        for (int px2 = x; px2 < x + w && px2 < b->width; px2++) {
            if (px2 < 0) continue;
            int idx = (py * b->width + px2) * 4;
            b->pixels[idx] = b->pixels[idx+1] = b->pixels[idx+2] = b->pixels[idx+3] = 0;
        }
    }
    b->tex_dirty = true;
    return mrb_nil_value();
}

#ifdef __GNUC__
#  define MRB_CB __attribute__((used)) static mrb_value
#else
#  define MRB_CB static mrb_value
#endif

MRB_CB bmp_hue_change        (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();   }
MRB_CB bmp_gradient_fill_rect(mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();   }
MRB_CB bmp_blur              (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();   }
MRB_CB bmp_radial_blur       (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();   }

static mrb_value bmp_font(mrb_state *mrb, mrb_value self) {
    mrb_value cached = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@font"));
    if (!mrb_nil_p(cached)) return cached;
    if (mrb_class_defined(mrb, "Font")) {
        RClass *fk = mrb_class_get(mrb, "Font");
        mrb_value fobj = mrb_obj_new(mrb, fk, 0, nullptr);
        mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@font"), fobj);
        return fobj;
    }
    return mrb_nil_value();
}
static mrb_value bmp_set_font(mrb_state *mrb, mrb_value self) {
    mrb_value v; mrb_get_args(mrb, "o", &v);
    mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@font"), v);
    return v;
}

static mrb_value bmp_max_size(mrb_state *mrb, mrb_value self) {
    (void)mrb; (void)self;
    return mrb_fixnum_value(1024);
}

MRB_CB bmp_animated      (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_false_value();   }
MRB_CB bmp_mega          (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_false_value();   }
MRB_CB bmp_frame_count   (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_fixnum_value(1); }
MRB_CB bmp_current_frame (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_fixnum_value(0); }
MRB_CB bmp_set_cur_frame (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_fixnum_value(0); }
MRB_CB bmp_playing_get   (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_false_value();   }
MRB_CB bmp_playing_set   (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();     }
MRB_CB bmp_play          (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();     }
MRB_CB bmp_stop          (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();     }
MRB_CB bmp_goto_stop     (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();     }
MRB_CB bmp_goto_play     (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();     }
MRB_CB bmp_next_frame    (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_fixnum_value(0); }
MRB_CB bmp_prev_frame    (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_fixnum_value(0); }
MRB_CB bmp_add_frame     (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_fixnum_value(0); }
MRB_CB bmp_remove_frame  (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();     }
MRB_CB bmp_frame_rate_get(mrb_state *mrb, mrb_value s) { (void)s;  return mrb_float_value(mrb, 0.0);     }
MRB_CB bmp_frame_rate_set(mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();     }
MRB_CB bmp_looping_get   (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_false_value();   }
MRB_CB bmp_looping_set   (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();     }
MRB_CB bmp_snap          (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();     }

void bitmapBindingInit(mrb_state *mrb) {
    RClass *k = mrb_define_class(mrb, "Bitmap", mrb->object_class);
    MRB_SET_INSTANCE_TT(k, MRB_TT_DATA);

    /* Pré-criar BitmapWrapper como subclasse C com MRB_TT_DATA ANTES dos scripts.
     * Quando os scripts fizerem "class BitmapWrapper < Bitmap" reabrerem a classe
     * já existente (em vez de criar nova) e MRB_TT_DATA mantém-se correcto. */
    {
        RClass *bw = mrb_define_class(mrb, "BitmapWrapper", k);
        MRB_SET_INSTANCE_TT(bw, MRB_TT_DATA);
        mrb_define_method(mrb, bw, "initialize", bmp_init, MRB_ARGS_ANY());
    }

    mrb_define_method(mrb, k, "initialize",         bmp_init,              MRB_ARGS_ANY());
    mrb_define_method(mrb, k, "dispose",            bmp_dispose,           MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "disposed?",          bmp_disposed,          MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "width",              bmp_width,             MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "height",             bmp_height,            MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "rect",               bmp_rect,              MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "clear",              bmp_clear,             MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "fill_rect",          bmp_fill_rect,         MRB_ARGS_REQ(2)|MRB_ARGS_OPT(3));
    mrb_define_method(mrb, k, "clear_rect",         bmp_clear_rect,        MRB_ARGS_REQ(1)|MRB_ARGS_OPT(3));
    mrb_define_method(mrb, k, "gradient_fill_rect", bmp_gradient_fill_rect,MRB_ARGS_REQ(3)|MRB_ARGS_OPT(2));
    mrb_define_method(mrb, k, "blt",                bmp_blt,               MRB_ARGS_REQ(4)|MRB_ARGS_OPT(1));
    mrb_define_method(mrb, k, "stretch_blt",        bmp_stretch_blt,       MRB_ARGS_REQ(3)|MRB_ARGS_OPT(1));
    mrb_define_method(mrb, k, "draw_text",          bmp_draw_text,         MRB_ARGS_REQ(2)|MRB_ARGS_OPT(4));
    mrb_define_method(mrb, k, "text_size",          bmp_text_size,         MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "get_pixel",          bmp_get_pixel,         MRB_ARGS_REQ(2));
    mrb_define_method(mrb, k, "set_pixel",          bmp_set_pixel,         MRB_ARGS_REQ(3));
    mrb_define_method(mrb, k, "hue_change",         bmp_hue_change,        MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "blur",               bmp_blur,              MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "radial_blur",        bmp_radial_blur,       MRB_ARGS_REQ(2));
    mrb_define_method(mrb, k, "font",               bmp_font,              MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "font=",              bmp_set_font,          MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, k, "max_size",     bmp_max_size,          MRB_ARGS_NONE());

    mrb_define_method(mrb, k, "animated?",          bmp_animated,          MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "mega?",              bmp_mega,              MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "frame_count",        bmp_frame_count,       MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "current_frame",      bmp_current_frame,     MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "current_frame=",     bmp_set_cur_frame,     MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "playing",            bmp_playing_get,       MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "playing=",           bmp_playing_set,       MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "play",               bmp_play,              MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "stop",               bmp_stop,              MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "goto_and_stop",      bmp_goto_stop,         MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "goto_and_play",      bmp_goto_play,         MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "next_frame",         bmp_next_frame,        MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "previous_frame",     bmp_prev_frame,        MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "add_frame",          bmp_add_frame,         MRB_ARGS_REQ(1)|MRB_ARGS_OPT(1));
    mrb_define_method(mrb, k, "remove_frame",       bmp_remove_frame,      MRB_ARGS_OPT(1));
    mrb_define_method(mrb, k, "frame_rate",         bmp_frame_rate_get,    MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "frame_rate=",        bmp_frame_rate_set,    MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "looping",            bmp_looping_get,       MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "looping=",           bmp_looping_set,       MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "snap_to_bitmap",     bmp_snap,              MRB_ARGS_OPT(1));
}

void bitmapBindingReinit(mrb_state *mrb) {
    extern FILE *g_dbglog;

    if (mrb_class_defined(mrb, "Bitmap")) {
        struct RClass *k = mrb_class_get(mrb, "Bitmap");
        MRB_SET_INSTANCE_TT(k, MRB_TT_DATA);
        mrb_define_method(mrb, k, "initialize", bmp_init, MRB_ARGS_ANY());
        if (g_dbglog) {
            fprintf(g_dbglog, "[BMP|REINIT] Bitmap MRB_TT_DATA + bmp_init rebound OK\n");
            fflush(g_dbglog);
        }
    }

    mrb_sym bw_sym = mrb_intern_lit(mrb, "BitmapWrapper");
    if (mrb_const_defined(mrb, mrb_obj_value(mrb->object_class), bw_sym)) {
        mrb_value bw_val = mrb_const_get(mrb, mrb_obj_value(mrb->object_class), bw_sym);
        if (mrb_class_p(bw_val)) {
            struct RClass *bw = mrb_class_ptr(bw_val);
            MRB_SET_INSTANCE_TT(bw, MRB_TT_DATA);
            mrb_define_method(mrb, bw, "initialize", bmp_init, MRB_ARGS_ANY());
            MRB_SET_INSTANCE_TT(bw, MRB_TT_DATA);
            if (g_dbglog) {
                fprintf(g_dbglog,
                    "[BMP|REINIT] BitmapWrapper MRB_TT_DATA + bmp_init rebound OK\n");
                fflush(g_dbglog);
            }
        }
    } else {
        if (g_dbglog) {
            fprintf(g_dbglog,
                "[BMP|REINIT] BitmapWrapper nao encontrado (jogo nao usa PE?)\n");
            fflush(g_dbglog);
        }
    }
}
