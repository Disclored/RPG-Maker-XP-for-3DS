#include "display_3ds.h"
#include "debug_3ds.h"
#include "audio_3ds.h"
#include "profile_3ds.h"
#include <cstring>
#include <stdio.h>
extern FILE *g_dbglog;
/* OTIMIZACAO 3DS: flush periodico (cada 64 escritas) em vez de a cada printf,
 * para evitar I/O sincrono ao cartao SD por frame. Contador global partilhado. */
extern int g_dbglog_flushc;
#define printf(fmt, ...) do { if (g_dbglog) { fprintf(g_dbglog, fmt, ##__VA_ARGS__); \
    if (++g_dbglog_flushc >= 512) { g_dbglog_flushc = 0; fflush(g_dbglog); } } } while(0)

#define TOP_W 400
#define TOP_H 240

/* ═══════════════════════════════════════════════════════════════════════════
 * ESCALA GLOBAL JOGO -> ECRA  (Opcao A: sem distorcao, centrado, barras laterais)
 *
 * O jogo desenha em coordenadas logicas de GAME_W x GAME_H (Settings::SCREEN_*).
 * Este fan-game usa 512x384. O ecra de cima do 3DS e' 400x240.
 * Para caber SEM distorcao nem cortes, usamos o menor fator de escala:
 *   escala_altura = 240/384 = 0.625
 *   escala_largura = 400/512 = 0.781  -> usamos 0.625 (o menor) para nada cortar
 * Resultado: 512*0.625=320 de largura, 384*0.625=240 de altura.
 *   -> 320x240 centrado: barra preta de (400-320)/2 = 40px de cada lado.
 *
 * Se algum dia mudares a resolucao do jogo, ajusta GAME_W/GAME_H e recalcula.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define GAME_W   512.0f
#define GAME_H   384.0f
#define DISP_SCALE  ( (float)TOP_H / GAME_H )                 /* 240/384 = 0.625 */
#define DISP_OFF_X  ( ( (float)TOP_W - GAME_W * DISP_SCALE ) * 0.5f )  /* (400-320)/2 = 40 */
#define DISP_OFF_Y  ( ( (float)TOP_H - GAME_H * DISP_SCALE ) * 0.5f )  /* (240-240)/2 = 0  */

static C3D_RenderTarget* s_top    = nullptr;
static C3D_RenderTarget* s_bottom = nullptr;

/* Indica se estamos DENTRO de um bloco C3D_FrameBegin/FrameEnd. CRITICO:
 * C3D_TexInit (alocacao de VRAM) NAO pode ser chamado dentro de um frame --
 * corrompe o estado do GPU e o Azahar deixa de apresentar (ecra preto apesar
 * dos blits). Como o sprite_binding cria texturas a meio do desenho, usamos
 * esta flag para, no create_texture, fechar o frame temporariamente, criar a
 * textura em seguranca, e reabrir o frame. */
static bool s_in_frame = false;

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
    /* Super Debug: sincronizar o contador de frame global para todas as
     * categorias poderem carimbar a que frame pertence o evento. */
    dbg_set_frame(s_frame_count);

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
    s_in_frame = true;
    /* PROFILING: o tempo desde o end_frame anterior ate' aqui foi a LOGICA do
     * jogo (update Ruby: Scene#update, eventos, input). A partir daqui ate' ao
     * end_frame e' o RENDER (C++: blits). Medir os dois separadamente diz-nos
     * se os 5 FPS sao por causa da logica Ruby ou do render. */
    PROF_END(PROF_FRAME_UPDATE);     /* fecha o intervalo de update (aberto no end_frame anterior) */
    PROF_BEGIN(PROF_FRAME_RENDER);   /* abre o intervalo de render */
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

    /* HEARTBEAT: a cada 60 frames (~1x/seg) uma linha "vivo" com o nº de frame.
     * Prova que o LOOP PRINCIPAL esta' a correr. Se o log parar de mostrar
     * heartbeats, o jogo travou (e a ultima linha diz em que frame). Custo ~0
     * (1 linha por segundo). Com o log line-buffered, ve-se em tempo real. */
    if ((s_frame_count % 60) == 0) {
        extern FILE *g_dbglog;
        if (g_dbglog) {
            fprintf(g_dbglog, "[HEARTBEAT] vivo -- frame %d\n", s_frame_count);
            fflush(g_dbglog);
        }
        printf("[HEARTBEAT] vivo -- frame %d\n", s_frame_count);
    }

    /* AUDIO (Fase 1): reciclar buffers de SE que ja' terminaram. Barato; corre
     * 1x por frame garantido. (A funcao verifica o estado interno e so' liberta
     * o que terminou; loga apenas quando ha' algo a reciclar.) */
    audio_3ds_update();

    /* ── MEDIDOR DE FPS REAL (intuitivo p/ debug de performance) ──────────────
     * Mede o tempo de parede entre cada 60 frames via svcGetSystemTick (relogio
     * do CPU ARM11 = 268.111856 MHz). Imprime UMA linha [FPS] legivel:
     *   [FPS] 58.7 fps | 60 frames em 1022 ms | media 17.0 ms/frame
     * Se vires "1.x fps" e ms/frame enorme, ha' trabalho a mais por frame (I/O,
     * recriacao de texturas, marshal). Linha unica a cada 60 frames -> custo ~0. */
    {
        static u64 s_fps_tick0 = 0;
        static int s_fps_frames = 0;
        if (s_fps_tick0 == 0) s_fps_tick0 = svcGetSystemTick();
        if (++s_fps_frames >= 60) {
            u64 now   = svcGetSystemTick();
            double ms = (double)(now - s_fps_tick0) / 268111.856; /* ticks -> ms */
            double fps = (ms > 0.0) ? (s_fps_frames * 1000.0 / ms) : 0.0;
            printf("[FPS] %.1f fps | %d frames em %.0f ms | media %.1f ms/frame\n",
                   fps, s_fps_frames, ms, ms / s_fps_frames);
            s_fps_tick0  = now;
            s_fps_frames = 0;
        }
    }

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
        C3D_FrameEnd(0);
        return;
    }

    C2D_SceneBegin(s_bottom);
    C3D_FrameEnd(0);

    /* PROFILING: fim do render deste frame. Acumula blits e, a cada 60 frames,
     * imprime um relatorio [PROFILE|frame] com a divisao update vs render. Isto
     * responde DIRETAMENTE a' pergunta "onde vai o tempo do frame?": se
     * frame.update >> frame.render, o gargalo e' a logica Ruby (ex: GC, eventos);
     * se frame.render >> frame.update, e' o desenho (blits, texturas). */
    PROF_END(PROF_FRAME_RENDER);     /* fecha o render */
    g_prof_blits += (uint64_t)s_blit_count;

    if ((s_frame_count % 60) == 0 && s_frame_count > 0) {
        double upd_ms = prof_ticks_to_ms(g_prof_zones[PROF_FRAME_UPDATE].total_ticks);
        double rnd_ms = prof_ticks_to_ms(g_prof_zones[PROF_FRAME_RENDER].total_ticks);
        uint64_t upd_n = g_prof_zones[PROF_FRAME_UPDATE].count;
        uint64_t rnd_n = g_prof_zones[PROF_FRAME_RENDER].count;
        double upd_avg = upd_n ? upd_ms / (double)upd_n : 0.0;
        double rnd_avg = rnd_n ? rnd_ms / (double)rnd_n : 0.0;
        extern FILE *g_dbglog;
        char line[256];
        snprintf(line, sizeof(line),
            "[PROFILE|frame] media: update=%.1fms render=%.1fms (=> %s domina) "
            "| objs_criados_total=%llu frees_total=%llu\n",
            upd_avg, rnd_avg,
            upd_avg > rnd_avg ? "LOGICA Ruby" : "RENDER C++",
            (unsigned long long)g_prof_objs_created,
            (unsigned long long)g_prof_frees);
        printf("%s", line);
        if (g_dbglog) { fprintf(g_dbglog, "%s", line); fflush(g_dbglog); }
        /* reset das zonas de frame para a media ser dos ULTIMOS 60 frames, nao
         * acumulada desde o boot (senao a media fica "achatada" e nao reflete o
         * estado atual, ex: menu vs mapa). */
        prof_zone_reset(PROF_FRAME_UPDATE);
        prof_zone_reset(PROF_FRAME_RENDER);
    }

    PROF_BEGIN(PROF_FRAME_UPDATE);   /* abre o intervalo de update ate' ao proximo begin_frame */
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

    /* FIX TEXTO DESFOCADO/ILEGIVEL no menu (New Game, etc.):
     * O citro3d, por omissao, usa filtragem GPU_LINEAR (bilinear). O jogo
     * renderiza a 512x384 (logico RMXP) e faz DOWNSCALE para o ecra 400x240
     * (escala ~0.625x). Com filtragem linear, o texto encolhido fica BORRADO.
     * GPU_NEAREST mantem os pixels nitidos no downscale -> texto legivel.
     * Trade-off: sprites grandes ficam ligeiramente mais "duros", mas a
     * legibilidade do texto e' prioritaria (e o RMXP foi desenhado p/ pixels
     * nitidos, nao interpolados). */
    C3D_TexSetFilter(&tex->tex, GPU_NEAREST, GPU_NEAREST);

    memset(dst, 0, (size_t)(tw * th * 4));

    for (int y = 0; y < h && y < th; y++) {
        for (int x = 0; x < w && x < tw; x++) {
            /* ORIENTACAO VERTICAL (causa raiz da imagem upside-down):
             * Ha' DOIS pontos que controlam o flip vertical -- o upload (aqui) e
             * as UVs (v_top/v_bottom no blit). A regra: EXATAMENTE UM deve
             * inverter, nunca os dois, nunca nenhum.
             *  - upload NATURAL (linha y -> linha y): imagem no topo da textura,
             *    alinhada com h (nao desalinha com a POT th).
             *  - UVs invertidas (v = 1.0 - sy/th): compensam o espaco UV do
             *    PICA200 (V=0 base, V=1 topo), pondo o topo da imagem em cima.
             * O codigo antigo invertia em AMBOS (th-1-y E 1.0-) com th errado ->
             * upside-down + cortado. Agora: upload natural + UV invertida = 1 flip. */
            int off = morton_offset(x, y, tw);
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
    /* Este log disparava por-frame no menu de load (texturas 408x222 recriadas
     * a cada frame) -> escrita ao SD constante -> 1 FPS. Movido para a categoria
     * DBG_TEXTURE, DESLIGADA por defeito. Para diagnosticar texturas outra vez:
     * dbg_set_mask(dbg_get_mask() | DBG_TEXTURE);  ou liga DBG_TEXTURE na mascara
     * em debug_3ds.cpp. Os casos de ERRO abaixo continuam sempre visiveis. */
    DBG(DBG_TEXTURE, "free valid=%d ptr=%p w=%d h=%d",
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

    /* GUARDA: opacity do sprite e' INT e pode sair de [0,255] (ex: fade do
     * MODTS faz opacity += float). alpha>1.0 ou <0.0 da cor/transparencia
     * errada no C2D_Color32f. Clampar. */
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    /* [DIAG] Primeiros blits: alpha+posicao+tamanho, para distinguir
     * "transparente (alpha=0)" de "fora do ecra" de "ok mas invisivel". */
    if ((s_frame_count <= 2 || s_frame_count % 600 == 0) && s_blit_count <= 20) {
        printf("[BLIT|DIAG] f=%d #%d dx=%.0f dy=%.0f sw=%.0f sh=%.0f alpha=%.2f tex=%dx%d\n",
               s_frame_count, s_blit_count, dx, dy, sw, sh, alpha,
               (int)t->tex.width, (int)t->tex.height);
    }

    /* ESCALA GLOBAL jogo(512x384) -> ecra(400x240). Ver constantes no topo. */
    float fdx = dx * DISP_SCALE + DISP_OFF_X;
    float fdy = dy * DISP_SCALE + DISP_OFF_Y;
    float fdw = sw * DISP_SCALE;
    float fdh = sh * DISP_SCALE;

    float tw = (float)t->tex.width;
    float th = (float)t->tex.height;

    /* GUARDA: textura de dimensao 0 -> divisao por zero -> UVs NaN -> GPU
     * desenha lixo ou nada. Saltar em vez de arriscar. */
    if (tw < 1.0f || th < 1.0f) {
        s_blit_skipped++;
        return;
    }

    float u0 = sx / tw;
    float u1 = (sx + sw) / tw;
    float v_top    = 1.0f - (sy / th);
    float v_bottom = 1.0f - ((sy + sh) / th);

    /* Super Debug: sprites GRANDES (logo/fundo) -- dimensoes e UVs reais.
     * Diagnostica o "corte a meio": ver se U[..] cobre a imagem toda e se
     * fdw cabe no ecra. ~1x cada 300 frames. */
    if (DBGV(DBG_SCALE) && sw >= 200.0f && (s_frame_count % 300 == 0)) {
        DBG(DBG_SCALE,
            "blit grande: src=%.0fx%.0f @(%.0f,%.0f) tex=%.0fx%.0f "
            "U[%.3f..%.3f] V[%.3f..%.3f] dst=%.1fx%.1f @(%.1f,%.1f)",
            sw, sh, sx, sy, tw, th, u0, u1, v_top, v_bottom,
            fdw, fdh, fdx, fdy);
    }

    /* FIX ESPELHO HORIZONTAL: a imagem aparecia espelhada na horizontal
     * (texto lia-se da direita p/ esquerda). Os campos left/right (U) da
     * SubTexture estavam trocados -> passar u1 como left e u0 como right. */
    Tex3DS_SubTexture sub = {
        (u16)(int)(sw + 0.5f), (u16)(int)(sh + 0.5f),
        u0, v_top, u1, v_bottom
    };
    C2D_Image img = { &t->tex, &sub };
    C2D_DrawParams p = { {fdx, fdy, fdw, fdh}, {0.0f, 0.0f}, 0.5f, 0.0f };

    /* FIX BRANCO: C2D_PlainImageTint(..., 1.0f) usava blend=1.0, o que SUBSTITUI
     * os pixels da imagem pela cor da tinta (branco) -> a imagem aparecia como
     * um retangulo branco com a forma/opacidade certa mas sem o conteudo.
     * Solucao: blend=0.0 em cada canto (mantem RGB original) e usar so o alpha
     * do tint para a opacidade. */
    C2D_ImageTint tint;
    u32 tcol = C2D_Color32f(1.0f, 1.0f, 1.0f, alpha);
    for (int ci = 0; ci < 4; ci++) {
        tint.corners[ci].color = tcol;
        tint.corners[ci].blend = 0.0f;
    }
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

    /* GUARDA: opacity do sprite e' INT e pode sair de [0,255] (ex: fade do
     * MODTS faz opacity += float). alpha>1.0 ou <0.0 da cor/transparencia
     * errada no C2D_Color32f. Clampar. */
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    /* CLAMP de src negativo: o tilemap pode passar src_x/src_y NEGATIVOS quando
     * a camara (@ox/@oy) fica negativa (ex: jogador perto da borda). Um sx<0 gera
     * coordenadas de textura (u0) negativas -> lixo ou nada no ecra. Recortamos a
     * parte negativa: avancamos a origem para 0, encolhemos a largura/altura, e
     * deslocamos o destino para compensar (em coords do JOGO, antes da escala). */
    if (sx < 0.0f) { float d = -sx; dx += d; dw -= d; sx = 0.0f; sw -= d; }
    if (sy < 0.0f) { float d = -sy; dy += d; dh -= d; sy = 0.0f; sh -= d; }
    /* se depois do recorte nao sobra nada, nada a desenhar */
    if (sw <= 0.0f || sh <= 0.0f || dw <= 0.0f || dh <= 0.0f) { return; }

    /* ESCALA GLOBAL: transformar coords do jogo (512x384) -> ecra (400x240).
     * dx,dy,dw,dh vem em coordenadas logicas do jogo; aplicamos escala + offset. */
    float fdx = dx * DISP_SCALE + DISP_OFF_X;
    float fdy = dy * DISP_SCALE + DISP_OFF_Y;
    float fdw = dw * DISP_SCALE;
    float fdh = dh * DISP_SCALE;

    float tw = (float)t->tex.width;
    float th = (float)t->tex.height;

    /* GUARDA: textura de dimensao 0 -> divisao por zero -> UVs NaN -> GPU
     * desenha lixo ou nada. Saltar em vez de arriscar. */
    if (tw < 1.0f || th < 1.0f) {
        s_blit_skipped++;
        return;
    }

    float u0 = sx / tw;
    float u1 = (sx + sw) / tw;
    float v_top    = 1.0f - (sy / th);
    float v_bottom = 1.0f - ((sy + sh) / th);

    /* Super Debug: sprites GRANDES (logo/fundo) -- dimensoes e UVs reais.
     * Diagnostica o "corte a meio": ver se U[..] cobre a imagem toda e se
     * fdw cabe no ecra. ~1x cada 300 frames. */
    if (DBGV(DBG_SCALE) && sw >= 200.0f && (s_frame_count % 300 == 0)) {
        DBG(DBG_SCALE,
            "blit grande: src=%.0fx%.0f @(%.0f,%.0f) tex=%.0fx%.0f "
            "U[%.3f..%.3f] V[%.3f..%.3f] dst=%.1fx%.1f @(%.1f,%.1f)",
            sw, sh, sx, sy, tw, th, u0, u1, v_top, v_bottom,
            fdw, fdh, fdx, fdy);
    }

    /* FIX ESPELHO HORIZONTAL: a imagem aparecia espelhada na horizontal
     * (texto lia-se da direita p/ esquerda). Os campos left/right (U) da
     * SubTexture estavam trocados -> passar u1 como left e u0 como right. */
    Tex3DS_SubTexture sub = {
        (u16)(int)(sw + 0.5f), (u16)(int)(sh + 0.5f),
        u0, v_top, u1, v_bottom
    };
    C2D_Image img = { &t->tex, &sub };
    C2D_DrawParams p = { {fdx, fdy, fdw, fdh}, {0.0f, 0.0f}, 0.5f, 0.0f };

    /* FIX BRANCO: C2D_PlainImageTint(..., 1.0f) usava blend=1.0, o que SUBSTITUI
     * os pixels da imagem pela cor da tinta (branco) -> a imagem aparecia como
     * um retangulo branco com a forma/opacidade certa mas sem o conteudo.
     * Solucao: blend=0.0 em cada canto (mantem RGB original) e usar so o alpha
     * do tint para a opacidade. */
    C2D_ImageTint tint;
    u32 tcol = C2D_Color32f(1.0f, 1.0f, 1.0f, alpha);
    for (int ci = 0; ci < 4; ci++) {
        tint.corners[ci].color = tcol;
        tint.corners[ci].blend = 0.0f;
    }
    C2D_DrawImage(img, &p, &tint);
}