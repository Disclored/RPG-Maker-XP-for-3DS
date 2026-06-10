#include "sprite_binding_3ds.h"

#include "display_3ds.h"
/* FIX BUG 5: display_3ds_blit_ex aceita draw_w/draw_h separados de src_w/src_h */
extern void display_3ds_blit_ex(DS3Texture* t,
                                 float dx, float dy, float dw, float dh,
                                 float sx, float sy, float sw, float sh,
                                 float alpha);
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/value.h>
#include <mruby/variable.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>   /* std::stable_sort */

/* global sprite list for rendering */
static std::vector<Sprite3DS*> g_sprites;

static void sprite_free(mrb_state*, void *p) {
    Sprite3DS *s = (Sprite3DS*)p;
    if (!s) return;
    for (size_t i = 0; i < g_sprites.size(); i++)
        if (g_sprites[i] == s) { g_sprites.erase(g_sprites.begin() + i); break; }
    free(s);
}
const mrb_data_type Sprite3DSType = { "Sprite", sprite_free };

#define GET_SPR(s) ((Sprite3DS*)DATA_PTR(s))

#include "tilemap_binding_3ds.h"

void sprites_draw_all() {
    /* ── DIAGNÓSTICO: dump completo nos primeiros 3 frames ── */
    {
        static int s_diag_frame = 0;
        s_diag_frame++;
        if (s_diag_frame <= 10) {
            extern FILE *g_dbglog;
            int total    = (int)g_sprites.size();
            int disposed = 0, invisible = 0, no_bitmap = 0, ready = 0;
            for (size_t i = 0; i < g_sprites.size(); i++) {
                Sprite3DS *s = g_sprites[i];
                if (!s || s->disposed)  { disposed++;   continue; }
                if (!s->visible)        { invisible++;  continue; }
                if (!s->bitmap)         { no_bitmap++;  continue; }
                ready++;
            }
            if (g_dbglog) {
                fprintf(g_dbglog,
                    "[SPR|DUMP] diag_frame=%d g_sprites=%d "
                    "(disposed=%d invisible=%d no_bitmap=%d ready=%d)\n",
                    s_diag_frame, total, disposed, invisible, no_bitmap, ready);
                /* Se g_sprites=0 nas primeiras frames, spr_init nunca foi chamado --
                 * confirmar com [SPR|INIT] acima. Se g_sprites>0 mas ready=0 e
                 * no_bitmap>0, o DATA_PTR ainda está nullptr (MRB_TT_DATA em falta). */
                if (total == 0 && s_diag_frame <= 3) {
                    fprintf(g_dbglog,
                        "[SPR|DUMP]   AVISO: g_sprites=0 -- spr_init C++ nao foi chamado!\n"
                        "[SPR|DUMP]   Sprite#initialize foi substituido por versao Ruby.\n"
                        "[SPR|DUMP]   Ver [SPRITE_CHECK] no MKXPDebug.log do probe.\n");
                } else if (total > 0 && no_bitmap == total && s_diag_frame <= 3) {
                    fprintf(g_dbglog,
                        "[SPR|DUMP]   AVISO: %d sprites mas todos sem bitmap!\n"
                        "[SPR|DUMP]   DATA_PTR=nullptr -> MRB_TT_DATA nao foi setado.\n"
                        "[SPR|DUMP]   Ver [FIX2] no log -- BitmapWrapper MRB_TT_DATA.\n",
                        total);
                }
                fflush(g_dbglog);
            }
        }
    }

    /* Ordenar por z -- stable_sort preserva ordem de inserção para z iguais */
    std::stable_sort(g_sprites.begin(), g_sprites.end(),
        [](const Sprite3DS *a, const Sprite3DS *b) { return a->z < b->z; });

    for (size_t i = 0; i < g_sprites.size(); i++) {
        Sprite3DS *s = g_sprites[i];
        if (!s || s->disposed || !s->visible || !s->bitmap) continue;
        if (s->bitmap->disposed) continue;

        /* FIX BUG 3: fazer upload sempre que tex_dirty=true OU tex=nullptr.
         * tex pode ser nullptr mesmo com tex_dirty=false se o bitmap foi
         * recriado externamente (ex: bmp_clear seguido de draw sem flush). */
        if (s->bitmap->tex_dirty || !s->bitmap->tex) {
            if (s->bitmap->tex) {
                display_3ds_free_texture(s->bitmap->tex);
                s->bitmap->tex = nullptr;
            }
            if (s->bitmap->pixels && s->bitmap->width > 0 && s->bitmap->height > 0) {
                s->bitmap->tex = display_3ds_create_texture(
                    s->bitmap->width, s->bitmap->height, s->bitmap->pixels);
            }
            s->bitmap->tex_dirty = false;
        }
        if (!s->bitmap->tex || !s->bitmap->tex->valid) continue;

        int src_w = s->src_w > 0 ? s->src_w : s->bitmap->width;
        int src_h = s->src_h > 0 ? s->src_h : s->bitmap->height;

        /* FIX BUG 5: tamanho de destino tem em conta zoom.
         * zoom_x/zoom_y por omissão são 1.0, por isso não afecta o caso normal.
         * draw_w/draw_h são as dimensões no ecra (pixels destino). */
        float draw_w = src_w * s->zoom_x;
        float draw_h = src_h * s->zoom_y;
        float alpha  = s->opacity / 255.0f;

        /* ox/oy são o ponto de origem do sprite -- scaled pelo zoom */
        float dx = (float)s->x - s->ox * s->zoom_x;
        float dy = (float)s->y - s->oy * s->zoom_y;

        display_3ds_blit_ex(s->bitmap->tex,
            dx, dy, draw_w, draw_h,
            (float)s->src_x, (float)s->src_y,
            (float)src_w, (float)src_h,
            alpha);
    }
}

static mrb_value spr_init(mrb_state *mrb, mrb_value self) {
    /* aceita viewport opcional — ignorado por agora */
    Sprite3DS *s = (Sprite3DS*)calloc(1, sizeof(Sprite3DS));
    s->opacity = 255;
    s->zoom_x  = 1.0f;
    s->zoom_y  = 1.0f;
    s->visible = true;
    DATA_PTR(self)  = s;
    DATA_TYPE(self) = &Sprite3DSType;
    g_sprites.push_back(s);

    /* DIAGNÓSTICO: log nas primeiras 10 chamadas para confirmar que spr_init C++ está activo */
    {
        static int s_spr_init_count = 0;
        s_spr_init_count++;
        if (s_spr_init_count <= 10) {
            extern FILE *g_dbglog;
            if (g_dbglog) {
                fprintf(g_dbglog,
                    "[SPR|INIT] #%d spr_init C++ chamado: sprite=%p g_sprites.size=%d\n",
                    s_spr_init_count, (void*)s, (int)g_sprites.size());
                fflush(g_dbglog);
            }
        }
    }
    return self;
}

static mrb_value spr_dispose(mrb_state*, mrb_value self) {
    Sprite3DS *s = GET_SPR(self);
    if (s) s->disposed = true;
    return mrb_nil_value();
}
static mrb_value spr_disposed(mrb_state*, mrb_value self) {
    Sprite3DS *s = GET_SPR(self);
    return mrb_bool_value(!s || s->disposed);
}
static mrb_value spr_update(mrb_state*, mrb_value) { return mrb_nil_value(); }
static mrb_value spr_flash (mrb_state *mrb, mrb_value) { (void)mrb; return mrb_nil_value(); }

/* bitmap get/set */
static mrb_value spr_get_bitmap(mrb_state *mrb, mrb_value self) {
    return mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@bitmap"));
}
/* Resolve um objecto Ruby Bitmap ou BitmapWrapper para o Bitmap3DS* C++ subjacente.
 *
 * BitmapWrapper é criado pelos scripts com initialize Ruby puro que não chama
 * super, por isso DATA_PTR da instância wrapper fica nullptr mesmo com
 * MRB_TT_DATA na classe.  Estratégia de fallback:
 *   1. DATA_PTR directo (Bitmap puro criado por bmp_init C++)
 *   2. ivar @bitmap  (BitmapWrapper guarda o Bitmap base em @bitmap)
 *   3. ivar @__bmp__ (alternativa usada nalguns wrappers)
 *   4. método #bitmap (chamada Ruby -- mais lenta mas robusta)
 */
static Bitmap3DS *resolve_bitmap(mrb_state *mrb, mrb_value bobj) {
    if (mrb_nil_p(bobj)) return nullptr;

    /* Tentativa 1: DATA_PTR directo */
    Bitmap3DS *bmp = (Bitmap3DS*)DATA_PTR(bobj);
    if (bmp) return bmp;

    extern FILE *g_dbglog;

    /* Tentativa 2: ivar @bitmap */
    mrb_value iv = mrb_iv_get(mrb, bobj, mrb_intern_lit(mrb, "@bitmap"));
    if (!mrb_nil_p(iv) && !mrb_equal(mrb, iv, bobj)) {
        bmp = (Bitmap3DS*)DATA_PTR(iv);
        if (bmp) {
            if (g_dbglog) { fprintf(g_dbglog,
                "[BMP|UNWRAP] resolvido via @bitmap: %dx%d\n", bmp->width, bmp->height);
                fflush(g_dbglog); }
            return bmp;
        }
        /* @bitmap pode ser outro wrapper -- recursão protegida */
        bmp = resolve_bitmap(mrb, iv);
        if (bmp) return bmp;
    }

    /* Tentativa 3: ivar @__bmp__ */
    iv = mrb_iv_get(mrb, bobj, mrb_intern_lit(mrb, "@__bmp__"));
    if (!mrb_nil_p(iv) && !mrb_equal(mrb, iv, bobj)) {
        bmp = (Bitmap3DS*)DATA_PTR(iv);
        if (bmp) return bmp;
    }

    /* Tentativa 4: chamar método #bitmap em Ruby */
    if (!mrb->exc && mrb_respond_to(mrb, bobj, mrb_intern_lit(mrb, "bitmap"))) {
        iv = mrb_funcall(mrb, bobj, "bitmap", 0);
        if (mrb->exc) { mrb->exc = 0; }
        else if (!mrb_nil_p(iv) && !mrb_equal(mrb, iv, bobj)) {
            bmp = (Bitmap3DS*)DATA_PTR(iv);
            if (bmp) {
                if (g_dbglog) { fprintf(g_dbglog,
                    "[BMP|UNWRAP] resolvido via #bitmap(): %dx%d\n", bmp->width, bmp->height);
                    fflush(g_dbglog); }
                return bmp;
            }
        }
    }

    if (g_dbglog) {
        fprintf(g_dbglog,
            "[BMP|UNWRAP] FALHOU: DATA_PTR=NULL e nenhum fallback resultou "
            "(type=%d) -- BitmapWrapper.initialize nao chama super?\n",
            (int)mrb_type(bobj));
        fflush(g_dbglog);
    }
    return nullptr;
}

static mrb_value spr_set_bitmap(mrb_state *mrb, mrb_value self) {
    mrb_value bobj; mrb_get_args(mrb, "o", &bobj);
    mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@bitmap"), bobj);
    Sprite3DS *s = GET_SPR(self);
    if (s) {
        Bitmap3DS *bmp = resolve_bitmap(mrb, bobj);
        s->bitmap = bmp;
        /* DIAGNÓSTICO: log nas primeiras 20 chamadas */
        static int s_set_bmp_count = 0;
        s_set_bmp_count++;
        if (s_set_bmp_count <= 20) {
            extern FILE *g_dbglog;
            if (g_dbglog) {
                int tt = mrb_nil_p(bobj) ? -1 : (int)mrb_type(bobj);
                void *dp = mrb_nil_p(bobj) ? nullptr : DATA_PTR(bobj);
                fprintf(g_dbglog,
                    "[SPR|SET_BMP] #%d sprite=%p bobj_type=%d DATA_PTR=%p bmp=%p\n",
                    s_set_bmp_count, (void*)s, tt, dp, (void*)bmp);
                if (bmp) {
                    fprintf(g_dbglog,
                        "[SPR|SET_BMP]   bmp OK via resolve: %dx%d tex_dirty=%d\n",
                        bmp->width, bmp->height, bmp->tex_dirty);
                } else if (!mrb_nil_p(bobj)) {
                    fprintf(g_dbglog,
                        "[SPR|SET_BMP]   resolve_bitmap FALHOU (type=%d)"
                        " -- ver [BMP|UNWRAP] acima\n", tt);
                }
                fflush(g_dbglog);
            }
        }
    }
    return bobj;
}

/* src_rect get/set */
static mrb_value spr_get_src_rect(mrb_state *mrb, mrb_value self) {
    return mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@src_rect"));
}

/* Extrai Rect de objecto Ruby puro (ivars @x,@y,@width,@height) ou C-struct.
 * DATA_PTR sobre Rect Ruby puro devolve NULL/lixo -- nunca fazer cast directo. */
static bool spr_extract_rect(mrb_state *mrb, mrb_value ro,
                              int *x, int *y, int *w, int *h) {
    if (mrb_nil_p(ro)) return false;
    /* Tentar ivars Ruby primeiro (Rect definido em compat_stubs.h) */
    mrb_value vx = mrb_iv_get(mrb, ro, mrb_intern_lit(mrb, "@x"));
    mrb_value vy = mrb_iv_get(mrb, ro, mrb_intern_lit(mrb, "@y"));
    mrb_value vw = mrb_iv_get(mrb, ro, mrb_intern_lit(mrb, "@width"));
    mrb_value vh = mrb_iv_get(mrb, ro, mrb_intern_lit(mrb, "@height"));
    if (!mrb_nil_p(vx)) {
        *x = (int)mrb_integer(mrb_to_int(mrb, vx));
        *y = (int)mrb_integer(mrb_to_int(mrb, vy));
        *w = (int)mrb_integer(mrb_to_int(mrb, vw));
        *h = (int)mrb_integer(mrb_to_int(mrb, vh));
        return true;
    }
    return false;
}

static mrb_value spr_set_src_rect(mrb_state *mrb, mrb_value self) {
    mrb_value ro; mrb_get_args(mrb, "o", &ro);
    mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@src_rect"), ro);
    Sprite3DS *s = GET_SPR(self);
    if (s) {
        int x = 0, y = 0, w = 0, h = 0;
        if (spr_extract_rect(mrb, ro, &x, &y, &w, &h)) {
            s->src_x = x; s->src_y = y; s->src_w = w; s->src_h = h;
        }
    }
    return ro;
}

/* int properties */
#define INT_PROP(name, field) \
static mrb_value spr_get_##name(mrb_state *mrb, mrb_value s) { \
    (void)mrb; Sprite3DS *p = GET_SPR(s); return mrb_fixnum_value(p ? p->field : 0); } \
static mrb_value spr_set_##name(mrb_state *mrb, mrb_value s) { \
    mrb_int v; mrb_get_args(mrb, "i", &v); \
    Sprite3DS *p = GET_SPR(s); if (p) p->field = (int)v; \
    return mrb_fixnum_value(v); }

INT_PROP(x, x) INT_PROP(y, y) INT_PROP(ox, ox) INT_PROP(oy, oy)
INT_PROP(z, z) INT_PROP(opacity, opacity) INT_PROP(blend_type, blend_type)

/* float properties */
#define FLOAT_PROP(name, field) \
static mrb_value spr_get_##name(mrb_state *mrb, mrb_value s) { \
    Sprite3DS *p = GET_SPR(s); return mrb_float_value(mrb, p ? p->field : 0.0f); } \
static mrb_value spr_set_##name(mrb_state *mrb, mrb_value s) { \
    mrb_float v; mrb_get_args(mrb, "f", &v); \
    Sprite3DS *p = GET_SPR(s); if (p) p->field = (float)v; \
    return mrb_float_value(mrb, v); }

FLOAT_PROP(zoom_x, zoom_x) FLOAT_PROP(zoom_y, zoom_y) FLOAT_PROP(angle, angle)

/* bool properties */
#define BOOL_PROP(name, field) \
static mrb_value spr_get_##name(mrb_state *mrb, mrb_value s) { \
    (void)mrb; Sprite3DS *p = GET_SPR(s); return mrb_bool_value(p && p->field); } \
static mrb_value spr_set_##name(mrb_state *mrb, mrb_value s) { \
    mrb_bool v; mrb_get_args(mrb, "b", &v); \
    Sprite3DS *p = GET_SPR(s); if (p) p->field = (bool)v; \
    return mrb_bool_value(v); }

BOOL_PROP(visible, visible) BOOL_PROP(mirror, mirror)

/* width/height from bitmap */
static mrb_value spr_width(mrb_state *mrb, mrb_value s) {
    (void)mrb;
    Sprite3DS *p = GET_SPR(s);
    return mrb_fixnum_value(p && p->bitmap ? (p->src_w > 0 ? p->src_w : p->bitmap->width) : 0);
}
static mrb_value spr_height(mrb_state *mrb, mrb_value s) {
    (void)mrb;
    Sprite3DS *p = GET_SPR(s);
    return mrb_fixnum_value(p && p->bitmap ? (p->src_h > 0 ? p->src_h : p->bitmap->height) : 0);
}

/* color/tone — guardados como ivars */
static mrb_value spr_get_color(mrb_state *mrb, mrb_value s) {
    return mrb_iv_get(mrb, s, mrb_intern_lit(mrb, "@color")); }
static mrb_value spr_set_color(mrb_state *mrb, mrb_value s) {
    mrb_value v; mrb_get_args(mrb, "o", &v);
    mrb_iv_set(mrb, s, mrb_intern_lit(mrb, "@color"), v); return v; }
static mrb_value spr_get_tone(mrb_state *mrb, mrb_value s) {
    return mrb_iv_get(mrb, s, mrb_intern_lit(mrb, "@tone")); }
static mrb_value spr_set_tone(mrb_state *mrb, mrb_value s) {
    mrb_value v; mrb_get_args(mrb, "o", &v);
    mrb_iv_set(mrb, s, mrb_intern_lit(mrb, "@tone"), v); return v; }

void spriteBindingInit(mrb_state *mrb) {
    RClass *k = mrb_define_class(mrb, "Sprite", mrb->object_class);
    MRB_SET_INSTANCE_TT(k, MRB_TT_DATA);
    mrb_define_method(mrb, k, "initialize", spr_init,         MRB_ARGS_OPT(1));
    mrb_define_method(mrb, k, "dispose",    spr_dispose,      MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "disposed?",  spr_disposed,     MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "update",     spr_update,       MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "flash",      spr_flash,        MRB_ARGS_REQ(2));
    mrb_define_method(mrb, k, "bitmap",     spr_get_bitmap,   MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "bitmap=",    spr_set_bitmap,   MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "src_rect",   spr_get_src_rect, MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "src_rect=",  spr_set_src_rect, MRB_ARGS_REQ(1));
#define MP(n) \
    mrb_define_method(mrb, k, #n,    spr_get_##n, MRB_ARGS_NONE()); \
    mrb_define_method(mrb, k, #n"=", spr_set_##n, MRB_ARGS_REQ(1))
    MP(x); MP(y); MP(ox); MP(oy); MP(z);
    MP(opacity); MP(blend_type);
    MP(zoom_x); MP(zoom_y); MP(angle);
    MP(visible); MP(mirror);
    MP(color); MP(tone);
#undef MP
    mrb_define_method(mrb, k, "width",  spr_width,  MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "height", spr_height, MRB_ARGS_NONE());
    // viewport, bush_depth, bush_opacity, wave_* -- guardados como ivars Ruby
    // Os scripts acedem mas o render 3DS não usa (simplificação aceitável)
    #define IVAR_PROP(nm) \
    mrb_define_method(mrb, k, nm, \
        [](mrb_state *mrb, mrb_value self) -> mrb_value { \
            return mrb_iv_get(mrb, self, mrb_intern_cstr(mrb, "@" nm)); }, \
        MRB_ARGS_NONE()); \
    mrb_define_method(mrb, k, nm "=", \
        [](mrb_state *mrb, mrb_value self) -> mrb_value { \
            mrb_value v; mrb_get_args(mrb, "o", &v); \
            mrb_iv_set(mrb, self, mrb_intern_cstr(mrb, "@" nm), v); return v; }, \
        MRB_ARGS_REQ(1));
    IVAR_PROP("viewport")
    IVAR_PROP("bush_depth")
    IVAR_PROP("bush_opacity")
    IVAR_PROP("wave_amp")
    IVAR_PROP("wave_length")
    IVAR_PROP("wave_speed")
    IVAR_PROP("wave_phase")
    #undef IVAR_PROP
}

/* FIX3: Re-bind Sprite#initialize para spr_init C++ após os scripts Ruby terem corrido.
 * Alguns scripts fazem "class Sprite; def initialize..." sobrescrevendo o binding C++.
 * Chamar esta função em check_entry_methods, antes do loop principal. */
void spriteBindingReinit(mrb_state *mrb) {
    extern FILE *g_dbglog;

    struct RClass *spr_cls = mrb_class_get(mrb, "Sprite");
    if (!spr_cls) {
        if (g_dbglog) {
            fprintf(g_dbglog, "[FIX3] ERRO: classe Sprite nao encontrada\n");
            fflush(g_dbglog);
        }
        return;
    }

    /* Garantir MRB_TT_DATA */
    MRB_SET_INSTANCE_TT(spr_cls, MRB_TT_DATA);

    /* Re-bind initialize para spr_init C++ -- sobrescreve qualquer def Ruby */
    mrb_define_method(mrb, spr_cls, "initialize", spr_init, MRB_ARGS_OPT(1));

    /* Re-bind bitmap= também porque alguns scripts podem tê-lo sobrescrito */
    mrb_define_method(mrb, spr_cls, "bitmap=", spr_set_bitmap, MRB_ARGS_REQ(1));

    if (g_dbglog) {
        fprintf(g_dbglog,
            "[FIX3] Sprite#initialize re-bound para spr_init C++ OK "
            "(MRB_TT_DATA garantido)\n");
        fflush(g_dbglog);
    }
}
