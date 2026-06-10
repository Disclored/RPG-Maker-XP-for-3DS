#include <mruby.h>
#include <mruby/class.h>
#include <mruby/variable.h>
#include <mruby/string.h>
#include <3ds.h>
#include "display_3ds.h"
#include "input_3ds.h"
#include "sprite_binding_3ds.h"
#include "tilemap_binding_3ds.h"
extern FILE *g_dbglog;
#define printf(fmt, ...) do { if (g_dbglog) { fprintf(g_dbglog, fmt, ##__VA_ARGS__); fflush(g_dbglog); } } while(0)

extern void inputBindingUpdate();

static int  s_frame_rate  = 40;
static int  s_frame_count = 0;
static bool s_frozen      = false;

static int  s_freeze_count      = 0;
static int  s_transition_count  = 0;
static int  s_skipped_frames    = 0;

static bool s_watchdog_active  = false;
static int  s_watchdog_count   = 0;
static int  s_watchdog_max     = 600;

/* ═══════════════════════════════════════════════════════════════════════════
 * [DBG] PROBE UTILS
 * Macros para log com ficheiro + linha exactos, nunca silenciosos.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define DBG(fmt, ...) printf("[GFX|%s:%d] " fmt "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define DBG_ONCE(fmt, ...) do { static bool _once=false; if(!_once){_once=true; DBG(fmt, ##__VA_ARGS__);} } while(0)

/* [DBG] Dump completo do estado interno do módulo Graphics */
static void dbg_dump_state(const char* label) {
    printf("[GFX|STATE|%s] frame=%d frozen=%d freeze_count=%d "
           "transition_count=%d skipped=%d watchdog=%s(%d/%d)\n",
           label,
           s_frame_count, s_frozen,
           s_freeze_count, s_transition_count, s_skipped_frames,
           s_watchdog_active ? "ON" : "off",
           s_watchdog_count, s_watchdog_max);
}

/* [DBG] Dump do $scene Ruby actual sem crashar se for nil */
static void dbg_dump_scene(mrb_state *mrb, const char* label) {
    mrb_value scene = mrb_gv_get(mrb, mrb_intern_cstr(mrb, "$scene"));
    if (mrb_nil_p(scene)) {
        printf("[GFX|SCENE|%s] $scene=NIL  ← PROBLEMA: sem cena activa\n", label);
        return;
    }
    mrb_value klass = mrb_funcall(mrb, scene, "class", 0);
    mrb_value kname = mrb_funcall(mrb, klass, "to_s",  0);
    printf("[GFX|SCENE|%s] $scene=%s\n",
           label, mrb_string_p(kname) ? RSTRING_PTR(kname) : "?");
}

/* ─────────────────────────────────────────────────────────────────────────── */

static mrb_value safe_funcall(mrb_state *mrb, mrb_value obj,
                               const char *name, int argc, mrb_value *argv) {
    mrb_sym sym = mrb_intern_cstr(mrb, name);
    mrb_value result = mrb_nil_value();
    mrb->exc = 0;
    result = mrb_funcall_argv(mrb, obj, sym, argc, argv);
    if (mrb->exc) {
        mrb_value exc = mrb_obj_value(mrb->exc);
        mrb_value msg = mrb_funcall(mrb, exc, "message", 0);
        /* [DBG] safe_funcall: nome do método + mensagem de erro exacta */
        printf("[GFX|CRASH] safe_funcall '%s' falhou: %s\n",
               name, mrb_string_p(msg) ? RSTRING_PTR(msg) : "(sem mensagem)");
        mrb->exc = 0;
    }
    return result;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * grph_update — chamado a cada frame pelo loop Ruby
 *
 * PONTOS DE FALHA DIAGNOSTICADOS:
 *   [A] s_frozen=true sem transition → ecrã preto para sempre
 *   [B] sprites_draw_all não desenha nada → ecrã preto mas sem erro
 *   [C] display_3ds_begin/end_frame com s_top=NULL → crash silencioso
 * ═══════════════════════════════════════════════════════════════════════════ */
static mrb_value grph_update(mrb_state *mrb, mrb_value self) {
	printf("[GFX|ENTER] grph_update entrou\n");
	printf("[GFX] grph_update CALLED frame=%d frozen=%d\n", s_frame_count, s_frozen);
    (void)self;

    if (!aptMainLoop()) {
        DBG("aptMainLoop() devolveu false → quit");
        mrb_raise(mrb, E_RUNTIME_ERROR, "aptMainLoop: quit requested");
    }

    hidScanInput();
    inputBindingUpdate();

    /* [DBG-A] Primeiros 3 frames: dump completo para confirmar estado inicial */
    if (s_frame_count < 3) {
        dbg_dump_state("update-early");
        dbg_dump_scene(mrb, "update-early");
    }

    /* [DBG-A] Detectar frozen logo no frame 1 — se já começa frozen, é bug de init */
    if (s_frame_count == 0 && s_frozen) {
        printf("[GFX|BUG] frame 0 já está frozen=true ANTES de qualquer freeze()!\n");
        printf("[GFX|BUG]   → Graphics.freeze() foi chamado durante init, sem transition() depois.\n");
    }

    if (!s_frozen) {
        /* [DBG-B] Antes de draw: confirmar que o render target existe */
        if (s_frame_count < 3 || s_frame_count % 300 == 0) {
            printf("[GFX|RENDER] frame %d → begin_frame + sprites_draw_all + end_frame\n",
                   s_frame_count);
        }

        display_3ds_begin_frame();

        /* [DBG-B] sprites_draw_all é uma caixa negra — meter probe antes/depois */
        int pre_blit = display_3ds_blit_count();   /* ← adicionar accessor em display_3ds */
        tilemaps_draw_all();
        sprites_draw_all();
        int post_blit = display_3ds_blit_count();

        if (s_frame_count < 3 || s_frame_count % 300 == 0) {
            printf("[GFX|RENDER] frame %d → sprites_draw_all fez %d blits novos\n",
                   s_frame_count, post_blit - pre_blit);
            if (post_blit == pre_blit) {
                printf("[GFX|WARN]   → ZERO blits! Sprites não registados ou todos invisíveis.\n");
                printf("[GFX|WARN]     Verificar: sprite.visible=true, sprite.bitmap!=nil, z correcto\n");
                dbg_dump_scene(mrb, "zero-blits");
            }
        }

        display_3ds_end_frame();

    } else {
        /* [DBG-A] Frozen: log imediato no frame 1 frozen, depois a cada 60 */
        s_skipped_frames++;
        if (s_skipped_frames == 1) {
            printf("[GFX|FROZEN] render BLOQUEADO — s_frozen=true desde frame %d\n",
                   s_frame_count);
            printf("[GFX|FROZEN]   freeze_count=%d transition_count=%d\n",
                   s_freeze_count, s_transition_count);
            printf("[GFX|FROZEN]   → Graphics.transition() NÃO foi chamado após freeze!\n");
            dbg_dump_scene(mrb, "frozen-frame1");
        }
        if (s_skipped_frames % 60 == 0) {
            printf("[GFX|FROZEN] ainda frozen há %d frames (frame=%d) — falta transition()\n",
                   s_skipped_frames, s_frame_count);
            dbg_dump_scene(mrb, "frozen-persist");
        }
    }

    s_frame_count++;

    /* ── Watchdog ── */
    if (s_watchdog_active) {
        s_watchdog_count++;
        if (s_watchdog_count % 60 == 0) {
            printf("[WD] frame %d/%d frozen=%d\n",
                   s_watchdog_count, s_watchdog_max, s_frozen);
            dbg_dump_scene(mrb, "watchdog");
        }
        if (s_watchdog_count >= s_watchdog_max) {
            printf("[WD] TIMEOUT %d frames\n", s_watchdog_max);
            dbg_dump_state("watchdog-timeout");
            /* Só abortar se o ecrã estiver genuinamente frozen sem progresso.
             * Se o jogo está a renderizar normalmente (frozen=false), o watchdog
             * faz reset silencioso — Scene_Map corre indefinidamente por design. */
            if (s_frozen && s_skipped_frames >= s_watchdog_max) {
                s_watchdog_active = false;
                s_watchdog_count  = 0;
                mrb_raise(mrb, E_RUNTIME_ERROR,
                          "[WD] Graphics.update watchdog timeout: frozen sem transition");
            } else {
                /* Jogo a correr normalmente — reset e continuar */
                printf("[WD] jogo activo (frozen=%d skipped=%d) — reset silencioso\n",
                       s_frozen, s_skipped_frames);
                s_watchdog_count = 0;
            }
        }
    }

    gspWaitForVBlank();
    if (s_frame_rate < 60 && s_frame_rate > 0) {
        long long target_ns = 1000000000LL / s_frame_rate;
        long long vblank_ns = 1000000000LL / 60;
        long long residual  = target_ns - vblank_ns;
        if (residual > 0) svcSleepThread(residual);
    }

    return mrb_nil_value();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * grph_freeze
 * [DBG] Log com callstack Ruby simulada: frame actual + $scene
 * ═══════════════════════════════════════════════════════════════════════════ */
static mrb_value grph_freeze(mrb_state *mrb, mrb_value self) {
	printf("[GFX|ENTER] grph_freeze entrou\n");
    (void)self;
    s_frozen = true;
    s_freeze_count++;
    s_skipped_frames = 0;
    printf("[GFX|FREEZE] freeze #%d chamado no frame %d\n",
           s_freeze_count, s_frame_count);
    dbg_dump_scene(mrb, "freeze");
    printf("[GFX|FREEZE]   → ecrã ficará preto até Graphics.transition() ser chamado!\n");
    return mrb_nil_value();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * grph_transition
 * [DBG] Confirma desbloqueio. Se nunca aparecer no log → bug de freeze eterno.
 * ═══════════════════════════════════════════════════════════════════════════ */
static mrb_value grph_transition(mrb_state *mrb, mrb_value self) {
    (void)self;
    s_transition_count++;
    printf("[GFX|TRANSITION] transition #%d chamado no frame %d "
           "(estava frozen=%d, skipped=%d frames)\n",
           s_transition_count, s_frame_count, s_frozen, s_skipped_frames);
    dbg_dump_scene(mrb, "transition");
    if (!s_frozen) {
        printf("[GFX|TRANSITION]   AVISO: transition() chamado mas frozen já era false!\n");
    }
    s_frozen = false;
    s_skipped_frames = 0;
    return mrb_nil_value();
}

static mrb_value grph_frame_rate_get(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_int_value(mrb, s_frame_rate);
}
static mrb_value grph_frame_rate_set(mrb_state *mrb, mrb_value self) {
    (void)self; mrb_int v; mrb_get_args(mrb, "i", &v);
    int old = s_frame_rate;
    s_frame_rate = (v < 1) ? 1 : (v > 120) ? 120 : (int)v;
    printf("[GFX] frame_rate %d → %d\n", old, s_frame_rate);
    return mrb_int_value(mrb, s_frame_rate);
}
static mrb_value grph_frame_count_get(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_int_value(mrb, s_frame_count);
}
static mrb_value grph_frame_count_set(mrb_state *mrb, mrb_value self) {
    (void)self; mrb_int v; mrb_get_args(mrb, "i", &v);
    s_frame_count = (int)v; return mrb_int_value(mrb, s_frame_count);
}
static mrb_value grph_width(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_int_value(mrb, display_3ds_screen_width());
}
static mrb_value grph_height(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_int_value(mrb, display_3ds_screen_height());
}
static mrb_value grph_snap_to_bitmap(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_nil_value();
}
static mrb_value grph_fadeout(mrb_state *mrb, mrb_value self) {
    (void)self;
    printf("[GFX|FADE] fadeout frame=%d frozen=%d\n", s_frame_count, s_frozen);
    return mrb_nil_value();
}
static mrb_value grph_fadein(mrb_state *mrb, mrb_value self) {
    (void)self;
    printf("[GFX|FADE] fadein frame=%d frozen=%d\n", s_frame_count, s_frozen);
    return mrb_nil_value();
}

static mrb_value grph_wait(mrb_state *mrb, mrb_value self) {
    (void)self;
    mrb_int d;
    mrb_get_args(mrb, "i", &d);
    printf("[GFX|WAIT] wait(%lld) frozen=%d frame=%d\n",
           (long long)d, s_frozen, s_frame_count);
    for (mrb_int i = 0; i < d; i++) {
        if (!aptMainLoop()) {
            mrb_raise(mrb, E_RUNTIME_ERROR, "aptMainLoop: quit requested");
        }
        hidScanInput();
        inputBindingUpdate();
        if (!s_frozen) {
            display_3ds_begin_frame();
            tilemaps_draw_all();
            sprites_draw_all();
            display_3ds_end_frame();
        } else {
            s_skipped_frames++;
        }
        s_frame_count++;
        gspWaitForVBlank();
    }
    printf("[GFX|WAIT] wait() concluído frame=%d\n", s_frame_count);
    return mrb_nil_value();
}

static mrb_value grph_playing(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_false_value();
}
static mrb_value grph_brightness_get(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_int_value(mrb, 255);
}
static mrb_value grph_brightness_set(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_nil_value();
}
static mrb_value grph_resize_screen(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_nil_value();
}
static mrb_value grph_frame_reset(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_nil_value();
}
static mrb_value grph_delta(mrb_state *mrb, mrb_value self) {
    (void)self;
    return mrb_int_value(mrb, (mrb_int)(1000000 / (s_frame_rate > 0 ? s_frame_rate : 40)));
}
static mrb_value grph_delta_s(mrb_state *mrb, mrb_value self) {
    (void)self;
    return mrb_float_value(mrb, 1.0 / (s_frame_rate > 0 ? s_frame_rate : 40));
}

static mrb_value grph_watchdog_enable(mrb_state *mrb, mrb_value self) {
    (void)self;
    mrb_int max = s_watchdog_max;
    mrb_get_args(mrb, "|i", &max);
    s_watchdog_max    = (int)max;
    s_watchdog_count  = 0;
    s_watchdog_active = true;
    printf("[WD] watchdog activado max=%d frames\n", s_watchdog_max);
    return mrb_nil_value();
}
static mrb_value grph_watchdog_disable(mrb_state *mrb, mrb_value self) {
    (void)self;
    s_watchdog_active = false;
    printf("[WD] watchdog desactivado após %d frames\n", s_watchdog_count);
    s_watchdog_count  = 0;
    return mrb_nil_value();
}
static mrb_value grph_watchdog_count(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_int_value(mrb, s_watchdog_count);
}

static mrb_value grph_kgc_special_transition(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_nil_value();
}
static mrb_value grph_kgc_screen_capture(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_nil_value();
}
static mrb_value grph_kgc_transition(mrb_state *mrb, mrb_value self) {
    (void)self;
    s_transition_count++;
    printf("[GFX|KGC] kgc_transition #%d frame=%d frozen_era=%d\n",
           s_transition_count, s_frame_count, s_frozen);
    s_frozen = false;
    s_skipped_frames = 0;
    return mrb_nil_value();
}

static mrb_value grph_poke_width(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_int_value(mrb, display_3ds_screen_width());
}
static mrb_value grph_poke_height(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_int_value(mrb, display_3ds_screen_height());
}
static mrb_value grph_poke_snap(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_nil_value();
}
static mrb_value grph_poke_resize(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_nil_value();
}
static mrb_value grph_haveresizescreen(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_true_value();
}

void graphicsBindingInit(mrb_state *mrb) {
    RClass *mod = mrb_define_module(mrb, "Graphics");

    mrb_define_module_function(mrb, mod, "update",               grph_update,          MRB_ARGS_NONE());
	printf("[GFXINIT] update bound to grph_update\n");
    mrb_define_module_function(mrb, mod, "update_3ds_internal",  grph_update,          MRB_ARGS_NONE());
    mrb_define_module_function(mrb, mod, "freeze",               grph_freeze,          MRB_ARGS_NONE());
    mrb_define_module_function(mrb, mod, "transition",           grph_transition,      MRB_ARGS_OPT(3));
    mrb_define_module_function(mrb, mod, "frame_rate",           grph_frame_rate_get,  MRB_ARGS_NONE());
    mrb_define_module_function(mrb, mod, "frame_rate=",          grph_frame_rate_set,  MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, mod, "frame_count",          grph_frame_count_get, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, mod, "frame_count=",         grph_frame_count_set, MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, mod, "width",                grph_width,           MRB_ARGS_NONE());
    mrb_define_module_function(mrb, mod, "height",               grph_height,          MRB_ARGS_NONE());
    mrb_define_module_function(mrb, mod, "snap_to_bitmap",       grph_snap_to_bitmap,  MRB_ARGS_OPT(1));
    mrb_define_module_function(mrb, mod, "fadeout",              grph_fadeout,         MRB_ARGS_OPT(1));
    mrb_define_module_function(mrb, mod, "fadein",               grph_fadein,          MRB_ARGS_OPT(1));
    mrb_define_module_function(mrb, mod, "wait",                 grph_wait,            MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, mod, "playing?",             grph_playing,         MRB_ARGS_NONE());
    mrb_define_module_function(mrb, mod, "brightness",           grph_brightness_get,  MRB_ARGS_NONE());
    mrb_define_module_function(mrb, mod, "brightness=",          grph_brightness_set,  MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, mod, "resize_screen",        grph_resize_screen,   MRB_ARGS_OPT(2));
    mrb_define_module_function(mrb, mod, "frame_reset",          grph_frame_reset,     MRB_ARGS_NONE());
    mrb_define_module_function(mrb, mod, "delta",                grph_delta,           MRB_ARGS_NONE());
    mrb_define_module_function(mrb, mod, "delta_s",              grph_delta_s,         MRB_ARGS_NONE());

    mrb_define_module_function(mrb, mod, "watchdog_enable",      grph_watchdog_enable, MRB_ARGS_OPT(1));
    mrb_define_module_function(mrb, mod, "watchdog_disable",     grph_watchdog_disable,MRB_ARGS_NONE());
    mrb_define_module_function(mrb, mod, "watchdog_count",       grph_watchdog_count,  MRB_ARGS_NONE());

    mrb_define_module_function(mrb, mod, "update_KGC_SpecialTransition",
                               grph_kgc_special_transition, MRB_ARGS_OPT(1));
    mrb_define_module_function(mrb, mod, "update_KGC_ScreenCapture",
                               grph_kgc_screen_capture,     MRB_ARGS_OPT(1));
    mrb_define_module_function(mrb, mod, "transition_KGC_SpecialTransition",
                               grph_kgc_transition,         MRB_ARGS_OPT(3));

    mrb_define_module_function(mrb, mod, "poke_width",           grph_poke_width,       MRB_ARGS_NONE());
    mrb_define_module_function(mrb, mod, "poke_height",          grph_poke_height,      MRB_ARGS_NONE());
    mrb_define_module_function(mrb, mod, "poke_snap_to_bitmap",  grph_poke_snap,        MRB_ARGS_OPT(1));
    mrb_define_module_function(mrb, mod, "mkxp_snap_to_bitmap",  grph_poke_snap,        MRB_ARGS_OPT(1));
    mrb_define_module_function(mrb, mod, "poke_resize_screen",   grph_poke_resize,      MRB_ARGS_OPT(2));
    mrb_define_module_function(mrb, mod, "haveresizescreen",     grph_haveresizescreen, MRB_ARGS_NONE());

    printf("[GFX] graphicsBindingInit OK\n");
    dbg_dump_state("init");
}