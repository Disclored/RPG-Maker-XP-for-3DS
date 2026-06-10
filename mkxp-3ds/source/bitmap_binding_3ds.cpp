#include "bitmap_binding_3ds.h"
#include "display_3ds.h"
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/value.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

    static const char *exts[] = { "", ".png", ".jpg", ".jpeg", ".bmp", nullptr };

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

    printf("[BMP] FALHOU carregar: %s -- fallback 32x32 magenta\n", path);
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
    return mrb_nil_value();
}

static mrb_value bmp_draw_text(mrb_state *mrb, mrb_value self) {
    (void)mrb; (void)self;
    return mrb_nil_value();
}

static mrb_value bmp_text_size(mrb_state *mrb, mrb_value self) {
    (void)self;
    int chars = 4;
    mrb_int argc = mrb_get_argc(mrb);
    if (argc >= 1) {
        mrb_value strv;
        mrb_get_args(mrb, "o", &strv);
        if (mrb_string_p(strv))
            chars = (int)RSTRING_LEN(strv);
    }
    RClass *rk = mrb_class_get(mrb, "Rect");
    mrb_value argv[4] = {
        mrb_fixnum_value(0), mrb_fixnum_value(0),
        mrb_fixnum_value(chars * 8), mrb_fixnum_value(20)
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
