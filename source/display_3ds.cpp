#include "display_3ds.h"
#include <cstring>
#include <stdio.h>
extern FILE *g_dbglog;
#define printf(fmt, ...) do { if (g_dbglog) { fprintf(g_dbglog, fmt, ##__VA_ARGS__); fflush(g_dbglog); } } while(0)

#define TOP_W 400
#define TOP_H 240

static C3D_RenderTarget* s_top    = nullptr;
static C3D_RenderTarget* s_bottom = nullptr;

static int s_frame_count   = 0;
static int s_blit_count    = 0;   /* blits neste frame */
static int s_blit_total    = 0;   /* total histórico de blits */
static int s_tex_created   = 0;
static int s_tex_failed    = 0;
static int s_blit_skipped  = 0;

/* ── Accessor público para grph_update comparar antes/depois de sprites_draw_all ── */
int display_3ds_blit_count() { return s_blit_total; }

/* ═══════════════════════════════════════════════════════════════════════════
 * display_3ds_init
 * [DBG] Verifica se render targets foram criados. NULL aqui = ecrã preto garantido.
 * ═══════════════════════════════════════════════════════════════════════════ */
void display_3ds_init() {
    printf("[DISPLAY|INIT] a criar render targets...\n");
    s_top    = C2D_CreateScreenTarget(GFX_TOP,    GFX_LEFT);
    s_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    printf("[DISPLAY|INIT] top=%p bottom=%p\n", (void*)s_top, (void*)s_bottom);

    if (!s_top) {
        printf("[DISPLAY|INIT] ERRO CRÍTICO: s_top=NULL!\n");
        printf("[DISPLAY|INIT]   → C2D_CreateScreenTarget(GFX_TOP) falhou\n");
        printf("[DISPLAY|INIT]   → Verificar: C3D_Init / C2D_Init foram chamados antes?\n");
    }
    if (!s_bottom) {
        printf("[DISPLAY|INIT] ERRO CRÍTICO: s_bottom=NULL!\n");
    }
    if (s_top && s_bottom) {
        printf("[DISPLAY|INIT] OK — render targets prontos\n");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * display_3ds_begin_frame
 * [DBG] Verifica render target antes de usar. Detecta se init falhou.
 * ═══════════════════════════════════════════════════════════════════════════ */
void display_3ds_begin_frame() {
    /* [DBG] Frame 0,1,2 + cada 300: confirmar que o render está a arrancar */
    if (s_frame_count < 3 || s_frame_count % 300 == 0) {
        printf("[DISPLAY|FRAME] begin #%d "
               "(tex_ok=%d tex_fail=%d blit_total=%d)\n",
               s_frame_count, s_tex_created, s_tex_failed, s_blit_total);
    }

    /* [DBG] CRÍTICO: render target nulo = crash silencioso ou ecrã preto */
    if (!s_top) {
        printf("[DISPLAY|FRAME] ERRO CRÍTICO frame %d: s_top=NULL!\n", s_frame_count);
        printf("[DISPLAY|FRAME]   → display_3ds_init() não foi chamado ou falhou\n");
        printf("[DISPLAY|FRAME]   → C3D_FrameBegin vai crashar ou não fazer nada\n");
        return;
    }

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(s_top,    C2D_Color32(0, 0, 0, 255));
    C2D_TargetClear(s_bottom, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(s_top);
    s_blit_count   = 0;
    s_blit_skipped = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * display_3ds_end_frame
 * [DBG] Diagnóstico pós-draw: quantos blits aconteceram e porquê ficou preto.
 * ═══════════════════════════════════════════════════════════════════════════ */
void display_3ds_end_frame() {
    s_frame_count++;

    /* [DBG] Frame 1,2,3 + cada 300: relatório de blits */
    bool do_log = (s_frame_count <= 3 || s_frame_count % 300 == 0);

    if (do_log) {
        printf("[DISPLAY|FRAME] end #%d: blits=%d skipped=%d\n",
               s_frame_count, s_blit_count, s_blit_skipped);
    }

    /* [DBG] DIAGNÓSTICO: razão exacta do ecrã preto */
    if (s_blit_count == 0) {
        if (s_blit_skipped > 0) {
            /* texturas inválidas — tex->valid=false */
            printf("[DISPLAY|BLACK] frame %d: ecrã preto — %d texturas inválidas (valid=false)\n",
                   s_frame_count, s_blit_skipped);
            printf("[DISPLAY|BLACK]   → display_3ds_create_texture falhou ou tex->valid não foi posto a true\n");
            printf("[DISPLAY|BLACK]   → tex_ok=%d tex_fail=%d\n", s_tex_created, s_tex_failed);
        } else {
            /* sprites_draw_all não chamou blit nenhum */
            printf("[DISPLAY|BLACK] frame %d: ecrã preto — ZERO sprites desenhados\n",
                   s_frame_count);
            printf("[DISPLAY|BLACK]   Causas possíveis:\n");
            printf("[DISPLAY|BLACK]     1. sprites_draw_all lista está vazia (sem sprites registados)\n");
            printf("[DISPLAY|BLACK]     2. todos os sprites têm visible=false\n");
            printf("[DISPLAY|BLACK]     3. todos os sprites têm bitmap=nil\n");
            printf("[DISPLAY|BLACK]     4. Tilemap não usa o sistema de sprites (render separado?)\n");
        }
    } else if (do_log) {
        printf("[DISPLAY|FRAME] frame %d OK — %d sprites desenhados\n",
               s_frame_count, s_blit_count);
    }

    if (!s_bottom) {
        printf("[DISPLAY|FRAME] ERRO: s_bottom=NULL no end_frame %d\n", s_frame_count);
        return;
    }

    C2D_SceneBegin(s_bottom);
    C3D_FrameEnd(0);
}

int display_3ds_screen_width()  { return TOP_W; }
int display_3ds_screen_height() { return TOP_H; }

/* ── Morton tiling ─────────────────────────────────────────────────────────── */
static inline int morton_offset(int x, int y, int tw) {
    static const int t[8] = {0,1,4,5,16,17,20,21};
    return ((y>>3)*(tw>>3)+(x>>3))*64 + t[x&7] + (t[y&7]<<1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * display_3ds_create_texture
 * [DBG] Log com dimensões, POT, resultado do TexInit e valid=true/false.
 *       Cada falha indica a causa exacta.
 * ═══════════════════════════════════════════════════════════════════════════ */
DS3Texture* display_3ds_create_texture(int w, int h, const unsigned char* rgba) {
    /* [DBG] Argumento inválido */
    if (!rgba || w <= 0 || h <= 0) {
        printf("[TEX|CREATE] ERRO argumento inválido: w=%d h=%d rgba=%p\n",
               w, h, (const void*)rgba);
        printf("[TEX|CREATE]   → bitmap chegou vazio ao create_texture\n");
        s_tex_failed++;
        return nullptr;
    }

    DS3Texture* tex = new DS3Texture();
    tex->width  = w;
    tex->height = h;
    tex->valid  = false;

    int tw = 8, th = 8;
    while (tw < w) tw <<= 1;
    while (th < h) th <<= 1;
    if (tw > 1024) tw = 1024;
    if (th > 1024) th = 1024;

    /* [DBG] Log das primeiras 10 texturas sempre */
    static int s_tex_log_count = 0;
    bool do_log = (s_tex_log_count < 10);
    if (do_log) {
        printf("[TEX|CREATE] #%d: src=%dx%d → POT=%dx%d (%.1fKB VRAM)\n",
               s_tex_log_count + 1, w, h, tw, th, (tw*th*4)/1024.0f);
    }

    /* [DBG] C3D_TexInit: falha mais comum = falta de VRAM */
    if (!C3D_TexInit(&tex->tex, (u16)tw, (u16)th, GPU_RGBA8)) {
        printf("[TEX|CREATE] ERRO C3D_TexInit FALHOU: src=%dx%d POT=%dx%d\n",
               w, h, tw, th);
        printf("[TEX|CREATE]   → Causas: VRAM esgotada, dimensão inválida, ou C3D não iniciado\n");
        printf("[TEX|CREATE]   → tex_ok=%d tex_fail=%d até agora\n",
               s_tex_created, s_tex_failed + 1);
        s_tex_failed++;
        delete tex;
        return nullptr;
    }

    u8* dst = static_cast<u8*>(tex->tex.data);
    if (!dst) {
        printf("[TEX|CREATE] ERRO tex->tex.data=NULL após TexInit (%dx%d)\n", tw, th);
        printf("[TEX|CREATE]   → C3D_TexInit devolveu true mas data é NULL (bug citro3d?)\n");
        s_tex_failed++;
        C3D_TexDelete(&tex->tex);
        delete tex;
        return nullptr;
    }

    memset(dst, 0, (size_t)(tw * th * 4));

    for (int y = 0; y < h && y < th; y++) {
        for (int x = 0; x < w && x < tw; x++) {
            int off = morton_offset(x, th - 1 - y, tw);
            const u8* src = rgba + (y * w + x) * 4;
            dst[off*4+0] = src[3]; /* A */
            dst[off*4+1] = src[2]; /* B */
            dst[off*4+2] = src[1]; /* G */
            dst[off*4+3] = src[0]; /* R */
        }
    }

    C3D_TexFlush(&tex->tex);

    tex->valid = true;
    s_tex_created++;
    s_blit_total += 0;  /* não conta aqui, só em blit() */
    s_tex_log_count++;

    if (do_log) {
        printf("[TEX|CREATE] #%d OK valid=true total_ok=%d total_fail=%d\n",
               s_tex_log_count, s_tex_created, s_tex_failed);
    }

    return tex;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * display_3ds_free_texture
 * [DBG] Confirma que a textura era válida antes de libertar.
 *       Se free é chamado com valid=false, confirma bug upstream.
 * ═══════════════════════════════════════════════════════════════════════════ */
void display_3ds_free_texture(DS3Texture* t) {
    if (!t) {
        printf("[TEX|FREE] chamado com t=NULL (double-free?)\n");
        return;
    }
    printf("[TEX|FREE] valid=%d ptr=%p w=%d h=%d\n",
           t->valid, (void*)t, t->width, t->height);
    if (!t->valid) {
        printf("[TEX|FREE]   AVISO: a libertar textura que nunca foi válida\n");
        printf("[TEX|FREE]   → create_texture falhou e caller não verificou nullptr?\n");
    }
    if (t->valid) C3D_TexDelete(&t->tex);
    delete t;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * display_3ds_blit
 * [DBG] Skip detalhado: porquê é que este blit foi ignorado?
 * ═══════════════════════════════════════════════════════════════════════════ */
void display_3ds_blit(DS3Texture* t,
                      float dx, float dy,
                      float sx, float sy, float sw, float sh,
                      float alpha) {
    if (!t) {
        s_blit_skipped++;
        if (s_blit_skipped <= 5) {
            printf("[BLIT] SKIP frame=%d: t=NULL dx=%.0f dy=%.0f\n",
                   s_frame_count, dx, dy);
            printf("[BLIT]   → sprite.bitmap=nil ou create_texture devolveu nullptr\n");
        }
        return;
    }
    if (!t->valid) {
        s_blit_skipped++;
        if (s_blit_skipped <= 5) {
            printf("[BLIT] SKIP frame=%d: valid=false w=%d h=%d dx=%.0f dy=%.0f\n",
                   s_frame_count, t->width, t->height, dx, dy);
            printf("[BLIT]   → create_texture não pôs valid=true (FIX #1 aplicado?)\n");
        }
        return;
    }

    s_blit_count++;
    s_blit_total++;

    float tw = (float)t->tex.width;
    float th = (float)t->tex.height;

    float u0 = sx / tw;
    float u1 = (sx + sw) / tw;
    float v_top    = 1.0f - (sy / th);
    float v_bottom = 1.0f - ((sy + sh) / th);

    Tex3DS_SubTexture sub = {
        (u16)(int)(sw + 0.5f), (u16)(int)(sh + 0.5f),
        u0, v_top, u1, v_bottom
    };
    C2D_Image img = { &t->tex, &sub };
    C2D_DrawParams p = { {dx, dy, sw, sh}, {0.0f, 0.0f}, 0.5f, 0.0f };

    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, C2D_Color32f(1.0f, 1.0f, 1.0f, alpha), 1.0f);
    C2D_DrawImage(img, &p, &tint);
}

void display_3ds_fill_rect(float x, float y, float w, float h,
                           unsigned char r, unsigned char g,
                           unsigned char b, unsigned char a) {
    C2D_DrawRectSolid(x, y, 0.5f, w, h, C2D_Color32(r, g, b, a));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * display_3ds_blit_ex — versão com destino dw/dh separado (zoom != 1.0)
 * ═══════════════════════════════════════════════════════════════════════════ */
void display_3ds_blit_ex(DS3Texture* t,
                          float dx, float dy, float dw, float dh,
                          float sx, float sy, float sw, float sh,
                          float alpha) {
    if (!t) {
        s_blit_skipped++;
        if (s_blit_skipped <= 5) {
            printf("[BLIT_EX] SKIP frame=%d: t=NULL dx=%.0f dy=%.0f dw=%.0f dh=%.0f\n",
                   s_frame_count, dx, dy, dw, dh);
        }
        return;
    }
    if (!t->valid) {
        s_blit_skipped++;
        if (s_blit_skipped <= 5) {
            printf("[BLIT_EX] SKIP frame=%d: valid=false w=%d h=%d\n",
                   s_frame_count, t->width, t->height);
        }
        return;
    }

    s_blit_count++;
    s_blit_total++;

    float tw = (float)t->tex.width;
    float th = (float)t->tex.height;

    float u0 = sx / tw;
    float u1 = (sx + sw) / tw;
    float v_top    = 1.0f - (sy / th);
    float v_bottom = 1.0f - ((sy + sh) / th);

    Tex3DS_SubTexture sub = {
        (u16)(int)(sw + 0.5f), (u16)(int)(sh + 0.5f),
        u0, v_top, u1, v_bottom
    };
    C2D_Image img = { &t->tex, &sub };
    C2D_DrawParams p = { {dx, dy, dw, dh}, {0.0f, 0.0f}, 0.5f, 0.0f };

    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, C2D_Color32f(1.0f, 1.0f, 1.0f, alpha), 1.0f);
    C2D_DrawImage(img, &p, &tint);
}