#include "display_3ds.h"
#include "debug_3ds.h"
#include "audio_3ds.h"
#include "profile_3ds.h"
#include <mruby.h>            /* [PERF-SYNCDRAW] ler o kill-switch $MKXP_SYNCDRAW */
#include <mruby/variable.h>
#include <cstring>
#include <stdio.h>
#include <vector>
#include <algorithm>
#include <malloc.h>   /* mallinfo: reportar memoria livre no heap malloc */
extern FILE *g_dbglog;
/* OTIMIZACAO 3DS: flush periodico (cada 64 escritas) em vez de a cada printf,
 * para evitar I/O sincrono ao cartao SD por frame. Contador global partilhado. */
extern int g_dbglog_flushc;
#include <cstring>
extern bool g_log_verbose;
extern bool g_boot_done;
/* [BOOT-LEAN] durante o boot, os printf deste ficheiro (DISPLAY, TEX, etc.) so'
 * passam se forem plugins/GameData/erros -- o resto e' ruido de arranque. */
static inline bool __raw_suppress(const char *fmt){
    if (g_log_verbose || !fmt) return false;
    /* erros passam sempre */
    if (strstr(fmt,"FALH")||strstr(fmt,"ERRO")||strstr(fmt,"WARN")||strstr(fmt,"CRASH")) return false;
    /* no boot tambem plugins + dados; em RUNTIME (overworld) NADA -> 0 escritas SD */
    if (!g_boot_done && (strstr(fmt,"[PLUGINS]")||strstr(fmt,"[Game.init]"))) return false;
    return true;
}
#define printf(fmt, ...) do { if (!__raw_suppress(fmt) && g_dbglog) { fprintf(g_dbglog, fmt, ##__VA_ARGS__); \
    if (++g_dbglog_flushc >= 512) { g_dbglog_flushc = 0; dbg_flush(); } } } while(0)

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

/* [DIAG] Contabilidade de texturas VIVAS (criadas - libertadas) e bytes POT
 * alocados no heap linear. Permite ver, no momento de uma falha de TexInit,
 * se e' fuga (vivas cresce sem parar) ou pico legitimo (muitas grandes vivas). */
static int    s_tex_alive  = 0;
static size_t s_tex_bytes  = 0;
static int    s_tex_alive_peak = 0;
static size_t s_tex_bytes_peak = 0;

/* [DIAG] Histograma de texturas VIVAS por tamanho (bytes POT). Decide se os
 * 30MB sao POUCAS gigantes (tilemap 1024x1024=4MB -> facil cortar) ou MUITAS
 * medias (working-set -> precisa VRAM/memoria estendida). */
static int s_tex_hist[6] = {0,0,0,0,0,0};
static int s_tex_big_w = 0, s_tex_big_h = 0;   /* maior textura POT ja vista */

/* [fix] Overflow de texturas para VRAM quando o heap linear (FCRAM) enche.
 * O 3DS tem ~6MB de VRAM, dos quais sobram varios MB depois dos framebuffers.
 * So' encaminhamos texturas PEQUENAS (UI, texto, cursores) para nao arriscar
 * esgotar a VRAM com um atlas grande e matar os framebuffers. Generico p/
 * qualquer jogo RMXP que esgote o heap linear. */
static const unsigned VRAM_OVERFLOW_MAX = 512u * 1024;  /* <=512KB por textura */
static const unsigned VRAM_OVERFLOW_KEEP = 256u * 1024; /* deixar 256KB livres */
static int    s_tex_vram_live  = 0;
static size_t s_tex_vram_bytes = 0;

/* ── SUPER LOG (DBG_SUPER) ───────────────────────────────────────────────────
 * Registo global de texturas VIVAS (engine-level, limitado ~ nº vivo, O(1) por
 * create, O(N) por free com swap-pop). Permite, por intervalo, listar peso
 * (bytes/%), tamanho/POT, local (linear/VRAM) e NECESSIDADE (desenhada vs
 * ociosa = candidata a fuga). Por defeito OFF; liga com:
 *   dbg_set_mask(dbg_get_mask() | DBG_SUPER);
 * Cadencia em frames: 1 = TODOS os frames (avisado: muita escrita ao SD, derruba
 * o FPS — so' para sessao de debug). Ajusta s_super_every. */
static std::vector<DS3Texture*> s_live_texs;
static int s_super_every = 120;   /* intervalo (frames) entre snapshots */
static void super_snapshot(void);  /* SUPER LOG: definido mais abaixo */

/* ── [EVICT] Eviccao on-demand de texturas idle ──────────────────────────────
 * PORQUE: o heap linear (FCRAM, 24MB) fragmenta-se com o tempo. Uma textura
 * grande contigua (ex: 1024x1024 = 4MB) pode falhar a alocar mesmo havendo
 * varios MB livres, porque estao espalhados em blocos pequenos. Sintoma real
 * (log): [TEX|CREATE] ERRO C3D_TexInit FALHOU src=832x704, linear_livre=6958KB
 * mas a textura de 4MB nao cabe -> a arte da intro/transicao nunca aparece.
 *
 * COMO: quando o TexInit falha, libertamos texturas que NAO sao desenhadas ha'
 * muitos frames (idle) para compactar o heap, e re-tentamos UMA vez. As texturas
 * idle sao recriadas automaticamente a partir de b->pixels (que vivem no heap
 * malloc, intacto) quando o jogo voltar a precisar delas.
 *
 * SEGURANCA: antes de libertar, pomos *owner_slot=nullptr para o Bitmap dono
 * saber que perdeu a textura (evita use-after-free no proximo blit). Texturas
 * sem owner registado (owner_slot==nullptr) NUNCA sao evictadas.
 *
 * CUSTO: zero no caminho normal -- so' corre quando o TexInit ja' falhou (caso
 * raro). GENERICO (engine-level): gestao de heap de texturas, nao especifico
 * de nenhum jogo. */
static const int TEX_EVICT_IDLE_FRAMES = 120; /* idle = sem blit ha' >=120 frames (~4s a 30fps) */
static bool evict_idle_textures(DS3Texture* self, unsigned want_bytes, int frame_now) {
    int      freed = 0;
    unsigned freed_bytes = 0;
    /* iterar sobre COPIA: free_texture modifica s_live_texs (swap-pop). */
    std::vector<DS3Texture*> snapshot = s_live_texs;
    for (size_t i = 0; i < snapshot.size(); i++) {
        DS3Texture* t = snapshot[i];
        if (t == self) continue;             /* nunca a propria */
        if (!t->valid) continue;
        if (!t->owner_slot) continue;        /* sem dono -> nao mexer (seguranca) */
        /* idle = nunca desenhada (-1) OU sem blit ha' muito tempo */
        if (t->last_blit_frame >= 0 &&
            (frame_now - t->last_blit_frame) < TEX_EVICT_IDLE_FRAMES) continue;
        /* avisar o dono ANTES de libertar (evita use-after-free) */
        *(t->owner_slot) = nullptr;
        freed_bytes += t->pot_bytes;
        display_3ds_free_texture(t);         /* tira de s_live_texs + liberta heap */
        freed++;
        if (freed_bytes >= want_bytes) break; /* ja' chega para a textura nova */
    }
    if (freed > 0) {
        printf("[TEX|EVICT] libertou %d texturas idle (%.0fKB) p/ caber %.0fKB\n",
               freed, freed_bytes/1024.0, want_bytes/1024.0);
    }
    return freed > 0;
}

/* [TEX-PRESSURE 2026-07-05] Valvula PREVENTIVA: o CACHE-SAFE-DISPOSE (fix do
 * Gilbert) deixou os bitmaps de cache vivos -> as TEXTURAS acumulam sem teto
 * (SUPERDBG: 25MB vivas, linear a 1.5MB, FPS 6-9 por asfixia; no 3DS real nem
 * arranca). O evict on-demand so' corria no PANICO de alloc. Aqui: chamada
 * 1x/120 frames pelo grph_update — se linear < 6MB, evicta texturas idle
 * (>=120f sem blit; re-upload lazy, invisivel) ate' libertar 4MB. */
extern "C" void display_3ds_evict_pressure(int frame_now) {
    if (linearSpaceFree() >= 6u * 1024u * 1024u) return;
    bool ok = evict_idle_textures(nullptr, 4u * 1024u * 1024u, frame_now);
    extern FILE *g_dbglog;
    if (g_dbglog && g_log_verbose) {
        fprintf(g_dbglog, "[TEX-PRESSURE] linear=%.0fKB -> evict preventivo %s\n",
                linearSpaceFree() / 1024.0, ok ? "OK" : "(nada idle p/ libertar)");
        fflush(g_dbglog);
    }
}

/* ── [TEXCENSUS] A medicao DECISIVA (advisor): das ~102 texturas vivas, QUAIS sao?
 * Leak (velhas, cena anterior, idle ha' muito) -> evictar resolve. Working-set vivo
 * (recentes, desenhadas agora) -> so' REDUZIR resolve. Dump das top 20 por tamanho com
 * idade (frames desde create) e idle (frames desde ultimo blit). Capado a 1x/120 frames
 * para nao inundar; chamado quando uma textura grande falha (= o momento do chao preto). */
static void tex_census(const char* tag) {
    extern FILE *g_dbglog;
    static int s_last_census = -1000;
    if (s_frame_count - s_last_census < 120) return;   /* throttle */
    s_last_census = s_frame_count;
    if (!g_dbglog) return;
    std::vector<DS3Texture*> snap;
    size_t total_kb = 0; int n_vram = 0; size_t vram_kb = 0;
    for (size_t i = 0; i < s_live_texs.size(); i++) {
        DS3Texture* t = s_live_texs[i];
        if (!t || !t->valid) continue;
        snap.push_back(t);
        total_kb += t->pot_bytes / 1024;
        if (t->in_vram) { n_vram++; vram_kb += t->pot_bytes / 1024; }
    }
    std::sort(snap.begin(), snap.end(),
              [](DS3Texture* a, DS3Texture* b){ return a->pot_bytes > b->pot_bytes; });
    fprintf(g_dbglog, "[TEXCENSUS:%s] frame=%d vivas=%d total=%uKB | VRAM %d tex %uKB | top20:\n",
            tag ? tag : "?", s_frame_count, (int)snap.size(), (unsigned)total_kb, n_vram, (unsigned)vram_kb);
    int lim = (int)snap.size(); if (lim > 20) lim = 20;
    for (int i = 0; i < lim; i++) {
        DS3Texture* t = snap[i];
        int age  = s_frame_count - t->create_frame;
        int idle = (t->last_blit_frame >= 0) ? (s_frame_count - t->last_blit_frame) : -1;
        fprintf(g_dbglog, "[TEXCENSUS]   %dx%d %uKB %s age=%df idle=%df dono=%s\n",
                t->width, t->height, t->pot_bytes / 1024, t->in_vram ? "VRAM" : "lin",
                age, idle, t->owner_slot ? "sim" : "nao");
    }
    dbg_flush();
}

/* ── [MEM] Maior bloco CONTIGUO de heap linear + censo de memoria ────────────
 * O linearSpaceFree() diz o TOTAL livre, mas o decode de um tileset precisa de
 * um bloco CONTIGUO. Com fragmentacao pode haver 14MB livres e mesmo assim um
 * alloc de 8MB falhar. Esta funcao mede o maior bloco real por busca binaria
 * (linearAlloc/linearFree de tamanhos decrescentes; ~8 tentativas, cada uma
 * libertada de imediato). So' e' chamada em momentos pontuais (transferencia de
 * mapa / decode de tileset), nunca por-frame. */
unsigned linear_biggest_block(void) {
    unsigned hi = (unsigned)linearSpaceFree();
    if (hi < 4u*1024u) return 0;
    unsigned lo = 0, best = 0;
    const unsigned GRAN = 256u*1024u;          /* precisao 256KB */
    while (hi - lo > GRAN) {
        unsigned mid = lo + (hi - lo) / 2u;
        void* p = linearAlloc(mid);
        if (p) { linearFree(p); best = mid; lo = mid; }
        else   { hi = mid; }
    }
    return best;
}

/* Censo de memoria num so' bloco, no momento de uma decisao critica (ex: antes
 * de decodificar um tileset). Responde de uma vez: o que FALTA (precisa vs
 * livre vs maior bloco -> diz se e' FRAGMENTACAO ou falta mesmo), o estado dos
 * heaps (linear/malloc/VRAM), e quanto seria EVICTAVEL (texturas da cena
 * anterior). tag identifica o ponto; need_bytes=0 se nao aplicavel. */
void mkxp_mem_census(const char* tag, unsigned need_bytes) {
    unsigned lin_free = (unsigned)linearSpaceFree();
    unsigned lin_big  = linear_biggest_block();
    struct mallinfo mi = mallinfo();
    unsigned mfree = (unsigned)mi.fordblks;
    int evictable = 0; unsigned evict_bytes = 0;   /* desenhadas >=1x, com dono */
    for (size_t i = 0; i < s_live_texs.size(); i++) {
        DS3Texture* t = s_live_texs[i];
        if (t && t->valid && t->owner_slot && t->last_blit_frame >= 0) {
            evictable++; evict_bytes += t->pot_bytes;
        }
    }
    const char* veredito = "";
    if (need_bytes) {
        if (lin_big >= need_bytes)            veredito = " => CABE contiguo";
        else if (lin_free >= need_bytes)      veredito = " => NAO cabe: FRAGMENTADO (ha' total mas nao contiguo)";
        else if (lin_free + evict_bytes >= need_bytes) veredito = " => NAO cabe agora, mas EVICCAO pode chegar";
        else                                  veredito = " => FALTA MESMO (nem com eviccao)";
    }
    char buf[640];
    snprintf(buf, sizeof(buf),
        "[MEM|CENSO:%s] precisa=%.0fKB | LINEAR livre=%.0fKB maior_bloco=%.0fKB | "
        "MALLOC livre=%.0fKB | TEX vivas=%d (%.0fKB) evictaveis=%d (%.0fKB) | VRAM livre=%.0fKB%s\n",
        tag ? tag : "?", need_bytes/1024.0, lin_free/1024.0, lin_big/1024.0,
        mfree/1024.0, s_tex_alive, s_tex_bytes/1024.0,
        evictable, evict_bytes/1024.0, vramSpaceFree()/1024.0, veredito);
    printf("%s", buf);   /* [LOG-LEAN] printf JA' e' macro->g_dbglog; fprintf duplicado removido */
    if (g_dbglog) dbg_flush();
}

/* ── [EVICT-LIN] Eviccao de EMERGENCIA p/ caber um tileset grande (4444) ──────
 * PROBLEMA (visto no log): ao entrar num mapa, o tileset (ex: Gen4 Outside Town
 * 256x31040 -> 15.5MB em 4444) tenta alocar mas linear_livre=5.7MB porque a
 * memoria linear esta' tomada por TEXTURAS vivas da cena anterior (a intro). O
 * mrb_full_gc nao ajuda (essa memoria NAO sao bitmaps Ruby orfaos, sao C3D
 * textures vivas) -> o tileset falha -> magenta -> fica em cache negativa.
 *
 * DIFERENCA p/ evict_idle_textures: durante uma transferencia de mapa o contador
 * de frames esta' PARADO (nao ha' Graphics.update enquanto carrega), logo as
 * texturas da cena anterior nunca parecem "idle" (last_blit ~= frame atual).
 * Por isso aqui IGNORAMOS o limiar de idle. Em vez disso usamos um criterio
 * seguro de QUEM evictar: texturas JA' DESENHADAS pelo menos uma vez
 * (last_blit_frame >= 0) -> sao da cena/mapa ANTERIOR. POUPAMOS as recem-criadas
 * (last_blit_frame == -1) -> essas pertencem ao mapa NOVO a ser construido agora
 * (evicta-las seria desperdicio). Maiores primeiro para libertar o maximo com o
 * minimo de evictacoes.
 *
 * SEGURANCA: identica a' idle -- poe *owner_slot=nullptr antes de libertar; o
 * Bitmap dono recria a textura a partir de b->pixels (intactos em RAM) no
 * proximo blit. So' corre quando um tileset ja' IA ser magenta, logo nunca
 * piora um caso que funcionava. GENERICO (engine-level), nao especifico de jogo.
 * Devolve os bytes (pot) libertados. */
unsigned display_3ds_evict_linear(unsigned want_bytes) {
    extern FILE *g_dbglog;
    float    lin_before = linearSpaceFree();
    unsigned big_before = linear_biggest_block();   /* bloco contiguo ANTES */
    int freed = 0; unsigned freed_bytes = 0;
    /* COPIA ordenada por tamanho desc.: liberta o maximo com menos evictacoes. */
    std::vector<DS3Texture*> snapshot;
    for (size_t i = 0; i < s_live_texs.size(); i++) {
        DS3Texture* t = s_live_texs[i];
        if (!t || !t->valid) continue;
        if (!t->owner_slot) continue;          /* sem dono -> seguranca, nao mexer */
        if (t->last_blit_frame < 0) continue;  /* nunca desenhada = mapa NOVO -> poupar */
        if (t->last_blit_frame == s_frame_count) continue;  /* [ANTI-PRETO] desenhada ESTE
            frame -> NAO sacrificar: era a causa do chao preto/flicker (a eviccao-forte
            evictava o @layer0 do tilemap que e' preciso AGORA, depois recriava-o ->
            thrash). So' evictamos texturas de frames ANTERIORES (fora do ecra atual). */
        snapshot.push_back(t);
    }
    std::sort(snapshot.begin(), snapshot.end(),
              [](DS3Texture* a, DS3Texture* b){ return a->pot_bytes > b->pot_bytes; });
    for (size_t i = 0; i < snapshot.size(); i++) {
        DS3Texture* t = snapshot[i];
        if (!t->valid) continue;               /* pode ter sido libertada entretanto */
        *(t->owner_slot) = nullptr;
        freed_bytes += t->pot_bytes;
        display_3ds_free_texture(t);
        freed++;
        if (freed_bytes >= want_bytes) break;
    }
    float    lin_after = linearSpaceFree();
    unsigned big_after = linear_biggest_block();    /* bloco contiguo DEPOIS */
    const char* ok = (big_after >= want_bytes) ? "CABE agora" : "AINDA NAO cabe (fragmentado)";
    printf("[TEX|EVICT-LIN] evictou %d tex (~%.0fKB) | maior_bloco %.0f->%.0fKB | precisa %.0fKB -> %s\n",
           freed, freed_bytes/1024.0, big_before/1024.0, big_after/1024.0, want_bytes/1024.0, ok);
    if (g_dbglog && g_log_verbose) {
        fprintf(g_dbglog, "[TEX|EVICT-LIN] evictou %d tex (~%.0fKB pot) | linear_livre %.0f->%.0fKB "
                "(ganho=%.0fKB) | maior_bloco %.0f->%.0fKB | precisa %.0fKB -> %s\n",
                freed, freed_bytes/1024.0, lin_before/1024.0, lin_after/1024.0,
                (lin_after-lin_before)/1024.0, big_before/1024.0, big_after/1024.0,
                want_bytes/1024.0, ok);
        dbg_flush();
    }
    return freed_bytes;
}

void display_3ds_set_texture_owner(DS3Texture* t, DS3Texture** slot) {
    if (t) t->owner_slot = slot;
}

static inline int tex_bucket(unsigned b) {
    if (b <=   16u*1024) return 0;
    if (b <=   64u*1024) return 1;
    if (b <=  256u*1024) return 2;
    if (b <= 1024u*1024) return 3;
    if (b <= 4096u*1024) return 4;
    return 5;
}

/* ── Accessor público para grph_update comparar antes/depois de sprites_draw_all ── */
int display_3ds_blit_count() { return s_blit_total; }

/* ═══════════════════════════════════════════════════════════════════════════
 * display_3ds_init
 * [DBG] Verifica se render targets foram criados. NULL aqui = ecrã preto garantido.
 * ═══════════════════════════════════════════════════════════════════════════ */
/* [ATLAS-ETERNO 2026-07-11] a textura do atlas do tilemap (1024x1024 POT, 4MB)
 * e' criada UMA vez aqui (linear virgem -> sucesso garantido) e partilhada por
 * todos os pedidos 832x704 (ver create_texture). Nunca e' C3D_TexDelete'ada. */
C3D_Tex g_atlas_tex;
bool g_atlas_tex_ok = false;
bool g_atlas_tex_busy = false;

void display_3ds_init() {
    printf("[DISPLAY|INIT] a criar render targets...\n");
    g_atlas_tex_ok = C3D_TexInit(&g_atlas_tex, 1024, 1024, GPU_RGBA8);
    printf("[ATLAS-ETERNO] textura 1024x1024 (4MB) criada no boot -> %s\n",
           g_atlas_tex_ok ? "OK" : "FALHOU");
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
int g_bd_battle_frame_flag = 0;   /* [BLITDIAG v4] posto pelo sprite_binding (assinatura real do battler) */
/* [SPRFX] estado por-blit (posto por sprites_draw_all; neutro por omissao) */
float g_blit_angle = 0.0f; bool g_blit_mirror = false;
unsigned char g_blit_color[4] = {0,0,0,0};
float g_blit_tone[3] = {0,0,0};

/* [BLEND-MODE 2026-07-10] blend_type RGSS por blit (0=normal 1=aditivo 2=sub).
 * O campo existia no Sprite3DS mas NUNCA chegava ao GPU -> as animacoes de
 * ataque (glows/beams, blend=1) e sombras custom (blend=2) desenhavam como
 * alpha normal (caixas bacas). C2D bufferiza quads: mudar a equacao exige
 * C2D_Flush() -- so' acontece na MUDANCA de modo (frames normais: 0 flushes).
 * s_gpu_blend=-1 no inicio de frame forca re-aplicar (C2D pode repor estado). */
int g_blit_blend = 0;
static int s_gpu_blend = -1;
static inline void mkxp_apply_blend(void) {
    if (g_blit_blend == s_gpu_blend) return;
    C2D_Flush();
    switch (g_blit_blend) {
    case 1:  /* aditivo: dst + src*alpha */
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                       GPU_SRC_ALPHA, GPU_ONE,
                       GPU_SRC_ALPHA, GPU_ONE);
        break;
    case 2:  /* subtrativo: dst - src*alpha (reverse subtract) */
        C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD,
                       GPU_SRC_ALPHA, GPU_ONE,
                       GPU_ZERO, GPU_ONE);
        break;
    default: /* normal: src-over */
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                       GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                       GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
        break;
    }
    s_gpu_blend = g_blit_blend;
}

void display_3ds_begin_frame() {
    /* [BLEND-MODE] estado GPU desconhecido no inicio do frame -> forca
     * re-aplicacao no 1o blit (e garante neutro herdado de frames anteriores). */
    g_blit_blend = 0; s_gpu_blend = -1;
    /* Super Debug: sincronizar o contador de frame global para todas as
     * categorias poderem carimbar a que frame pertence o evento. */
    dbg_set_frame(s_frame_count);

    /* SUPER LOG (DBG_SUPER, OFF por defeito): censo completo de recursos por
     * intervalo. So' corre se a categoria estiver ligada -> custo zero quando
     * desligada. Ligar: dbg_set_mask(dbg_get_mask() | DBG_SUPER); */
    if (DBGV(DBG_SUPER) && s_super_every > 0 && (s_frame_count % s_super_every) == 0)
        super_snapshot();

    /* [DBG] Frame 0,1,2 + cada 300: confirmar que o render está a arrancar */
    extern bool g_log_verbose;
    if (g_log_verbose && (s_frame_count < 3 || s_frame_count % 1800 == 0)) {   /* [LOG-LEAN] */
        printf("[DISPLAY|FRAME] begin #%d "
               "(tex_ok=%d tex_fail=%d blit_total=%d)\n",
               s_frame_count, s_tex_created, s_tex_failed, s_blit_total);
        printf("[TEX|MEM] VIVAS=%d (pico %d) bytes_tex=%.0fKB (pico %.0fKB) | "
               "linear_livre=%.0fKB vram_livre=%.0fKB vram_tex=%d/%.0fKB\n",
               s_tex_alive, s_tex_alive_peak,
               s_tex_bytes/1024.0, s_tex_bytes_peak/1024.0,
               linearSpaceFree()/1024.0, vramSpaceFree()/1024.0,
               s_tex_vram_live, s_tex_vram_bytes/1024.0);
        printf("[TEX|HIST] vivas: <=16K=%d <=64K=%d <=256K=%d <=1M=%d <=4M=%d >4M=%d | maior_POT=%dx%d\n",
               s_tex_hist[0], s_tex_hist[1], s_tex_hist[2], s_tex_hist[3],
               s_tex_hist[4], s_tex_hist[5], s_tex_big_w, s_tex_big_h);
    }

    /* [DBG] CRÍTICO: render target nulo = crash silencioso ou ecrã preto */
    if (!s_top) {
        printf("[DISPLAY|FRAME] ERRO CRÍTICO frame %d: s_top=NULL!\n", s_frame_count);
        printf("[DISPLAY|FRAME]   → display_3ds_init() não foi chamado ou falhou\n");
        printf("[DISPLAY|FRAME]   → C3D_FrameBegin vai crashar ou não fazer nada\n");
        return;
    }

    /* [PERF-SYNCDRAW 2026-07-04] C3D_FRAME_SYNCDRAW esperava um vblank NOVO em
     * TODOS os frames (C3D_FrameSync no citro3d: loop gspWaitForAnyEvent ate' o
     * frameCounter dos DOIS ecras avancar) = ~8ms de media, ate' 16.7ms, ANTES
     * de comecar o render — invisivel ao DIAG-PERF (corre antes do t0) e em
     * DUPLICADO com o pacing proprio do motor (PACING SEM PRECIPICIO no
     * graphics_binding). Com flags=0 o swap continua vblank-bound na fila do
     * citro3d (sem tearing); o pacing do motor continua a mandar no ritmo.
     * Kill-switch: $MKXP_SYNCDRAW=true repoe o comportamento antigo. */
    {
        static int sd_mode = -1;
        if (sd_mode < 0) {
            extern mrb_state *g_mkxp_mrb;
            sd_mode = 0;
            if (g_mkxp_mrb) {
                mrb_value sv = mrb_gv_get(g_mkxp_mrb, mrb_intern_lit(g_mkxp_mrb, "$MKXP_SYNCDRAW"));
                if (!mrb_nil_p(sv) && mrb_test(sv)) sd_mode = 1;
            }
        }
        C3D_FrameBegin(sd_mode == 1 ? C3D_FRAME_SYNCDRAW : 0);
    }
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

    /* HEARTBEAT: prova de vida do LOOP PRINCIPAL (forense de crash: a ultima
     * linha diz em que frame travou). Reduzido para 1x cada 300 frames (~5s a
     * 60fps) e SEM duplicacao -- antes escrevia 2x (fprintf + macro printf, que
     * tambem vai para o g_dbglog) e dava fflush a cada 60 frames, o que custava
     * escrita SINCRONA ao SD com frequencia. Mantemos UM fflush aqui para a
     * forense (a 1x/300f o custo e' irrelevante). */
    extern bool g_log_verbose;
    if (g_log_verbose && (s_frame_count % 300) == 0) {
        extern FILE *g_dbglog;
        if (g_dbglog) {
            fprintf(g_dbglog, "[HEARTBEAT] vivo -- frame %d\n", s_frame_count);
            dbg_flush();
        }
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
    extern bool g_log_verbose;
    bool do_log = g_log_verbose && (s_frame_count <= 3 || s_frame_count % 1800 == 0);   /* [LOG-LEAN] */

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

    if ((s_frame_count % 300) == 0 && s_frame_count > 0) {
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
        if (g_dbglog) { fprintf(g_dbglog, "%s", line); dbg_flush(); }   /* [LOG-LEAN] printf removido (stdout duplicava no log) */
        /* reset das zonas de frame para a media ser dos ULTIMOS 60 frames, nao
         * acumulada desde o boot (senao a media fica "achatada" e nao reflete o
         * estado atual, ex: menu vs mapa). */
        prof_zone_reset(PROF_FRAME_UPDATE);
        prof_zone_reset(PROF_FRAME_RENDER);
    }

    PROF_BEGIN(PROF_FRAME_UPDATE);   /* abre o intervalo de update ate' ao proximo begin_frame */
}

/* IMPORTANTE: estes alimentam Graphics.width/height (e poke_width/height).
 * Um jogo RMXP usa Graphics.width/height para posicionar TODO o UI (caixas de
 * mensagem, menus...), a' espera da resolucao LOGICA em que desenha -- aqui
 * 512x384 (GAME_W/GAME_H), NAO o ecra fisico do 3DS (400x240). Devolver 400x240
 * fazia a caixa de mensagem cair a meio (y=240-96=144) e so' 400 de largura.
 * O downscale 512x384 -> 400x240 e' feito no blit (DISP_SCALE), transparente
 * para o jogo. NAO usar TOP_W/TOP_H aqui. */
int display_3ds_screen_width()  { return (int)GAME_W; }   /* 512 (logico) */
int display_3ds_screen_height() { return (int)GAME_H; }   /* 384 (logico) */

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
DS3Texture* display_3ds_create_texture(int w, int h, const unsigned char* rgba, bool prefer_vram) {
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
    extern bool g_log_verbose;
    bool do_log = g_log_verbose && (s_tex_log_count < 10);
    if (do_log) {
        printf("[TEX|CREATE] #%d: src=%dx%d → POT=%dx%d (%.1fKB VRAM)\n",
               s_tex_log_count + 1, w, h, tw, th, (tw*th*4)/1024.0f);
    }

    /* [ATLAS-ETERNO 2026-07-11] o ATLAS do tilemap (janela 832x704 -> POT
     * 1024x1024 = 4MB) e' A textura que pintava o chao — e falhava por
     * fragmentacao do linear no meio da sessao (437x KILL-TEXINVALID 832x704 =
     * mapa preto), porque e' recriada a cada mudanca de mapa. Solucao
     * DETERMINISTICA: a textura e' criada UMA vez no display_3ds_init (linear
     * virgem, sucesso garantido) e PARTILHADA: qualquer pedido 832x704 recebe-a
     * (busy-flag evita dupla posse); o destroy liberta a posse SEM C3D_TexDelete
     * -> existe para sempre, imune a fragmentacao. */
    extern C3D_Tex g_atlas_tex; extern bool g_atlas_tex_ok, g_atlas_tex_busy;
    if (w == 832 && h == 704 && g_atlas_tex_ok && !g_atlas_tex_busy) {
        tex->tex = g_atlas_tex;
        tex->is_shared_atlas = true;
        g_atlas_tex_busy = true;
        tex->valid = true;
        tex->pot_bytes = (unsigned)(tw * th * 4);
        tex->create_frame = s_frame_count;
        s_tex_created++; s_tex_alive++;
        if (s_tex_alive > s_tex_alive_peak) s_tex_alive_peak = s_tex_alive;
        s_tex_bytes += tex->pot_bytes;
        if (s_tex_bytes > s_tex_bytes_peak) s_tex_bytes_peak = s_tex_bytes;
        s_tex_hist[tex_bucket(tex->pot_bytes)]++;
        /* upload dos pixels (mesmo caminho in-place do LAYER0-REUSE) */
        display_3ds_update_texture(tex, w, h, rgba);
        return tex;
    }
    /* [ATLAS-HANDOFF] fim do silencio: se um 832x704 chega aqui com o atlas
     * OCUPADO, alguem nao largou a posse (ex.: buffer estacionado no L0CACHE
     * antes do __drop_texture) — nomear o estado evita outra sessao de caca. */
    if (w == 832 && h == 704 && g_atlas_tex_ok && g_atlas_tex_busy) {
        static int s_atlas_busy_logs = 0;
        if (s_atlas_busy_logs < 6) {
            s_atlas_busy_logs++;
            printf("[ATLAS-ETERNO] AVISO: pedido 832x704 com o atlas OCUPADO -> vai tentar textura normal 4MB (quem tem a posse nao a largou)\n");
        }
    }

    /* [fix #5] TILESET COMPACTADO marcado pelo probe (prefer_vram): tentar VRAM
     * PRIMEIRO. Estes bitmaps sao estaticos (construidos 1x, never_dispose) e cabem
     * num banco de VRAM (3MB). Po'-los em VRAM tira-os do heap linear -> este deixa
     * de ser fragmentado por obstaculos pinned -> a camada do tilemap (4MB) passa a
     * caber no linear contiguo. So' p/ texturas que CABEM num banco (<3MB); a camada
     * de 4MB nunca entra aqui (>3MB) e fica no linear, como deve. Se a VRAM falhar
     * (cheia), cai no caminho linear normal abaixo -> nunca pior que o atual. */
    bool init_ok = false;
    {
        const unsigned VRAM_PREFER_MAX = 3072u * 1024; /* <3MB = cabe num banco */
        unsigned want_v = (unsigned)(tw * th * 4);
        if (prefer_vram && want_v < VRAM_PREFER_MAX &&
            vramSpaceFree() >= (want_v + VRAM_OVERFLOW_KEEP)) {
            if (C3D_TexInitVRAM(&tex->tex, (u16)tw, (u16)th, GPU_RGBA8)) {
                init_ok = true;
                tex->in_vram = true;
                printf("[TEX|VRAM-PREFER] tileset %dx%d (%.0fKB) -> VRAM (libera heap linear)\n",
                       tw, th, want_v/1024.0);
            }
        }
    }

    /* Tenta o heap linear (FCRAM) primeiro — caminho normal. */
    if (!init_ok) init_ok = C3D_TexInit(&tex->tex, (u16)tw, (u16)th, GPU_RGBA8);

    /* [fix] Heap linear cheio? Encaminha texturas pequenas para a VRAM livre
     * em vez de falhar (e deixar a UI/texto por desenhar). Aditivo: so' corre
     * quando o caminho normal ja' falhou, logo nunca piora o estado atual. */
    if (!init_ok) {
        unsigned want = (unsigned)(tw * th * 4);
        if (want <= VRAM_OVERFLOW_MAX &&
            vramSpaceFree() >= (want + VRAM_OVERFLOW_KEEP)) {
            if (C3D_TexInitVRAM(&tex->tex, (u16)tw, (u16)th, GPU_RGBA8)) {
                init_ok = true;
                tex->in_vram = true;
            }
        }
    }

    /* [EVICT] Ultima tentativa antes de desistir: o heap linear pode ter espaco
     * suficiente mas FRAGMENTADO (varios MB livres mas nenhum bloco contiguo
     * grande). Libertar texturas idle compacta o heap. Re-tentamos UMA vez no
     * heap linear. So' corre quando tudo o resto ja' falhou -> custo zero normal.
     * Caso tipico: arte grande da intro/transicao (832x704 -> POT 1024x1024,
     * 4MB) que nao cabe enquanto ha' dezenas de texturas de dialogo/mapa idle. */
    if (!init_ok) {
        unsigned want = (unsigned)(tw * th * 4);
        if (evict_idle_textures(tex, want, s_frame_count)) {
            init_ok = C3D_TexInit(&tex->tex, (u16)tw, (u16)th, GPU_RGBA8);
            if (init_ok) {
                printf("[TEX|EVICT] re-tentativa OK: %dx%d criada apos libertar idle\n", tw, th);
            }
            /* se ainda falhou e a textura e' pequena, tentar VRAM tambem */
            if (!init_ok && want <= VRAM_OVERFLOW_MAX &&
                vramSpaceFree() >= (want + VRAM_OVERFLOW_KEEP)) {
                if (C3D_TexInitVRAM(&tex->tex, (u16)tw, (u16)th, GPU_RGBA8)) {
                    init_ok = true;
                    tex->in_vram = true;
                }
            }
        }
    }

    /* [EVICT-FORTE] Anti-CHAO-PRETO: texturas GRANDES (>=1MB; ex.: @layer0 do
     * tilemap 832x704 -> POT 1024x1024 = 4MB) sao criticas. Se a criacao falha,
     * o chao do mapa fica PRETO (e' composto em RAM mas sem textura na GPU). O
     * evict_idle_textures acima so' liberta texturas IDLE (>=120 frames sem blit);
     * numa cena cheia raramente ha' idle suficiente p/ um bloco CONTIGUO de 4MB,
     * por isso o layer0 falha repetidamente (log: tex_fail=101 e a crescer). Aqui,
     * SO' para texturas grandes e SO' depois de tudo falhar, usamos a eviccao FORTE
     * (display_3ds_evict_linear: evicta tambem texturas JA' desenhadas -- recriadas
     * do b->pixels no proximo blit, nunca UAF) e retentamos algumas vezes; em ultimo
     * recurso tentamos VRAM. Trade-off correto: CHAO visivel > manter texturas de
     * dialogo/sprites em cache. Custo zero no caminho normal (so' corre apos falha). */
    if (!init_ok && (unsigned)(tw * th * 4) >= 1024u * 1024u) {
        extern unsigned display_3ds_evict_linear(unsigned want_bytes);
        unsigned want = (unsigned)(tw * th * 4);
        /* [TEXDIAG] ISOLAR o chao preto: estado de memoria ANTES de tentar a textura
         * grande. Mostra se a falha e' VRAM cheia, falta de RAM, ou FRAGMENTACAO
         * (linear livre alto mas maior_bloco < want). g_dbglog SEMPRE (sem throttle).*/
        extern FILE *g_dbglog;
        unsigned vram0 = (unsigned)vramSpaceFree();
        unsigned lin0  = (unsigned)linearSpaceFree();
        unsigned big0  = linear_biggest_block();
        int      tried_vram = 0, evict_rounds = 0;
        /* VRAM PRIMEIRO: a textura grande do chao (4MB) CABE na VRAM se estiver livre.
         * [ANTI-PRETO-VRAM] PROVADO pelo [TEXDIAG|BIG]: o layer0 precisa 4096KB, a VRAM
         * tem 4119KB livres, MAS o check exigia +256KB de KEEP (4352) -> falhava por
         * 233KB e nunca tentava VRAM (tried_vram=0) -> caia no linear FRAGMENTADO ->
         * chao preto. Aqui (ultimo recurso, depois do linear ja' ter falhado) DROPAMOS
         * a margem KEEP: o chao visivel vale mais que 256KB de folga de VRAM. So' p/ a
         * textura grande critica; se nem 'want' couber, cai na eviccao-forte do linear. */
        if (vramSpaceFree() >= want) {
            tried_vram = 1;
            if (C3D_TexInitVRAM(&tex->tex, (u16)tw, (u16)th, GPU_RGBA8)) {
                init_ok = true; tex->in_vram = true;
            }
        }
        for (int attempt = 0; attempt < 4 && !init_ok; attempt++) {
            evict_rounds++;
            if (display_3ds_evict_linear(want + want / 2u) == 0) break; /* nada mais p/ libertar */
            init_ok = C3D_TexInit(&tex->tex, (u16)tw, (u16)th, GPU_RGBA8);
        }
        unsigned big1 = linear_biggest_block();
        if (g_dbglog) {
            fprintf(g_dbglog,
                "[TEXDIAG|BIG] src=%dx%d POT=%dx%d need=%uKB | ANTES vram=%uKB linear=%uKB maior_bloco=%uKB"
                " | tried_vram=%d evict_rounds=%d | DEPOIS maior_bloco=%uKB -> %s (%s)\n",
                w, h, tw, th, want/1024, vram0/1024, lin0/1024, big0/1024,
                tried_vram, evict_rounds, big1/1024,
                init_ok ? "OK" : "FALHOU->CHAO_PRETO",
                init_ok ? (tex->in_vram ? "VRAM" : "linear") :
                  (vram0 < want ? "VRAM-cheia" : (big0 < want ? "FRAGMENTADO" : "?")));
            dbg_flush();
        }
        if (init_ok)
            printf("[TEX|EVICT-FORTE] %dx%d criada (%s) anti-chao-preto\n",
                   tw, th, tex->in_vram ? "VRAM" : "linear apos eviccao");
        else
            tex_census("chao-preto");   /* [TEXCENSUS] no momento exato da falha: leak vs working-set */
    }

    if (!init_ok) {
        if ((s_tex_failed % 100) == 0) {
            printf("[TEX|CREATE] ERRO C3D_TexInit FALHOU: src=%dx%d POT=%dx%d (1 em cada 100)\n",
                   w, h, tw, th);
            printf("[TEX|CREATE]   → Causas: VRAM esgotada, dimensão inválida, ou C3D não iniciado\n");
            printf("[TEX|CREATE]   → tex_ok=%d tex_fail=%d até agora\n",
                   s_tex_created, s_tex_failed + 1);
            printf("[TEX|MEM] VIVAS=%d (pico %d) bytes_tex=%.0fKB (pico %.0fKB) | "
                   "linear_livre=%.0fKB vram_livre=%.0fKB vram_tex=%d/%.0fKB\n",
                   s_tex_alive, s_tex_alive_peak,
                   s_tex_bytes/1024.0, s_tex_bytes_peak/1024.0,
                   linearSpaceFree()/1024.0, vramSpaceFree()/1024.0,
                   s_tex_vram_live, s_tex_vram_bytes/1024.0);
            printf("[TEX|HIST] vivas: <=16K=%d <=64K=%d <=256K=%d <=1M=%d <=4M=%d >4M=%d | maior_POT=%dx%d\n",
                   s_tex_hist[0], s_tex_hist[1], s_tex_hist[2], s_tex_hist[3],
                   s_tex_hist[4], s_tex_hist[5], s_tex_big_w, s_tex_big_h);
        }
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

    /* ORIENTACAO VERTICAL (causa raiz da imagem upside-down):
     * Ha' DOIS pontos que controlam o flip vertical -- o upload (aqui) e as UVs
     * (v_top/v_bottom no blit). A regra: EXATAMENTE UM deve inverter, nunca os
     * dois, nunca nenhum. Aqui o upload e' NATURAL (linha y -> linha y); as UVs
     * invertidas (v = 1.0 - sy/th) compensam o espaco UV do PICA200 (V=0 base).
     * (Antes invertia em AMBOS com th errado -> upside-down + cortado.)
     *
     * P3: swizzle Morton OTIMIZADO. Antes chamava morton_offset(x,y,tw) por pixel
     * -- recalcula `((y>>3)*(tw>>3)+(x>>3))*64` (multiplicacao + dois >>3) e copia
     * 4 bytes um a um. Agora hoistamos a base da LINHA (so' muda a cada pixel em
     * x>>3) para fora, e a troca de canais RGBA->ABGR do PICA200 faz-se com UM
     * __builtin_bswap32 (instrucao REV no ARM) -> 1 load + 1 store por pixel em
     * vez de 4+4. Bytes de saida identicos ao anterior (A,B,G,R). __builtin_memcpy
     * de 4 bytes alinhados e' compilado para um unico load/store (sem UB de
     * aliasing). Corre sempre que um bitmap muda (incl. o @layer0 do tilemap). */
    {
        static const int mt[8] = { 0, 1, 4, 5, 16, 17, 20, 21 };
        const int twb = tw >> 3;
        const int hh  = (h < th) ? h : th;
        const int ww  = (w < tw) ? w : tw;
        for (int y = 0; y < hh; y++) {
            const int row_base = ((y >> 3) * twb) * 64 + (mt[y & 7] << 1);
            const u8* srow = rgba + (size_t)y * w * 4;
            for (int x = 0; x < ww; x++) {
                const int off = row_base + (x >> 3) * 64 + mt[x & 7];
                u32 v;
                __builtin_memcpy(&v, srow + (size_t)x * 4, 4);
                v = __builtin_bswap32(v);
                __builtin_memcpy(dst + (size_t)off * 4, &v, 4);
            }
        }
    }

    /* VRAM e' memoria nao-cacheada: o flush de cache de dados nao e' necessario
     * (e GSPGPU_FlushDataCache nem deve ser chamado sobre enderecos de VRAM).
     * So' o caminho do heap linear (cacheado) precisa de flush. */
    /* [PERF-TEXFLUSH 2026-07-04] cache-clean por-textura REDUNDANTE: o
     * C3D_FrameEnd(0) (fim de frame) faz GSPGPU_FlushDataCache do heap linear
     * INTEIRO antes de o GPU consumir os comandos (citro3d renderqueue.c,
     * verificado) — a coerencia esta' garantida por ele. Este flush explicito
     * custava um cache-clean de ate' 4MB por frame animado (layer0/agua).
     * NAO REMOVER O COMENTARIO: se o FrameEnd mudar para GX_CMDLIST_FLUSH,
     * repor. Kill-switch: $MKXP_TEXFLUSH=true reativa. */
    {
        static int txf_mode = -1;
        if (txf_mode < 0) {
            extern mrb_state *g_mkxp_mrb;
            txf_mode = 0;
            if (g_mkxp_mrb) {
                mrb_value tv = mrb_gv_get(g_mkxp_mrb, mrb_intern_lit(g_mkxp_mrb, "$MKXP_TEXFLUSH"));
                if (!mrb_nil_p(tv) && mrb_test(tv)) txf_mode = 1;
            }
        }
        if (txf_mode == 1 && !tex->in_vram) C3D_TexFlush(&tex->tex);
    }

    tex->valid = true;
    tex->pot_bytes = (unsigned)(tw * th * 4);
    tex->create_frame    = s_frame_count;   /* SUPER: idade */
    tex->last_blit_frame = -1;              /* SUPER: ainda nao desenhada */
    s_live_texs.push_back(tex);             /* SUPER: registo de vivas */
    s_tex_created++;
    s_tex_alive++;
    s_tex_bytes += tex->pot_bytes;
    if (tex->in_vram) { s_tex_vram_live++; s_tex_vram_bytes += tex->pot_bytes; }
    if (s_tex_alive > s_tex_alive_peak) s_tex_alive_peak = s_tex_alive;
    if (s_tex_bytes > s_tex_bytes_peak) s_tex_bytes_peak = s_tex_bytes;
    s_tex_hist[tex_bucket(tex->pot_bytes)]++;
    if ((long)tw * th > (long)s_tex_big_w * s_tex_big_h) { s_tex_big_w = tw; s_tex_big_h = th; }
    s_blit_total += 0;  /* não conta aqui, só em blit() */
    s_tex_log_count++;

    if (do_log) {
        printf("[TEX|CREATE] #%d OK valid=true total_ok=%d total_fail=%d\n",
               s_tex_log_count, s_tex_created, s_tex_failed);
    }

    return tex;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * display_3ds_update_texture  — [FPS] update IN-PLACE (anti re-upload de 4MB/frame)
 * ───────────────────────────────────────────────────────────────────────────
 * CAUSA dos 10-15fps na rua: os autotiles ANIMADOS (agua/FX) re-blitam o @layer0
 * do tilemap (832x704 -> POT 1024x1024 = 4MB) TODOS os frames -> tex_dirty=true ->
 * o sprite_binding fazia free_texture + create_texture por frame. E o create
 * FECHA+REABRE o frame GPU (C3D_FrameEnd/Begin, pq C3D_TexInit nao corre dentro do
 * frame) + memset de 4MB + swizzle -> ~40ms/frame de render.
 * AQUI: se a textura ja' existe com as mesmas dims POT, re-swizzlamos os pixels
 * novos para o buffer tex->tex.data EXISTENTE -- sem TexInit, sem free/evict, SEM
 * fechar o frame GPU, sem memset (a regiao w x h e' toda reescrita; a borda POT
 * fica a 0 da criacao e nunca e' desenhada, src_rect limita a w x h). Sobra so' o
 * swizzle + flush. Swizzle IDENTICO ao do create_texture (mt[8], bswap32). */
bool display_3ds_update_texture(DS3Texture* tex, int w, int h, const unsigned char* rgba) {
    if (!tex || !tex->valid || !rgba || w <= 0 || h <= 0) return false;
    if (tex->width != w || tex->height != h) return false;   /* dims mudaram -> recriar */
    u8* dst = (u8*)tex->tex.data;
    if (!dst) return false;
    int tw = 8, th = 8;
    while (tw < w) tw <<= 1;
    while (th < h) th <<= 1;
    if (tw > 1024) tw = 1024;
    if (th > 1024) th = 1024;
    static const int mt[8] = { 0, 1, 4, 5, 16, 17, 20, 21 };
    const int twb = tw >> 3;
    const int hh  = (h < th) ? h : th;
    const int ww  = (w < tw) ? w : tw;
    for (int y = 0; y < hh; y++) {
        const int row_base = ((y >> 3) * twb) * 64 + (mt[y & 7] << 1);
        const u8* srow = rgba + (size_t)y * w * 4;
        for (int x = 0; x < ww; x++) {
            const int off = row_base + (x >> 3) * 64 + mt[x & 7];
            u32 v;
            __builtin_memcpy(&v, srow + (size_t)x * 4, 4);
            v = __builtin_bswap32(v);
            __builtin_memcpy(dst + (size_t)off * 4, &v, 4);
        }
    }
    /* [PERF-TEXFLUSH 2026-07-04] cache-clean por-textura REDUNDANTE: o
     * C3D_FrameEnd(0) (fim de frame) faz GSPGPU_FlushDataCache do heap linear
     * INTEIRO antes de o GPU consumir os comandos (citro3d renderqueue.c,
     * verificado) — a coerencia esta' garantida por ele. Este flush explicito
     * custava um cache-clean de ate' 4MB por frame animado (layer0/agua).
     * NAO REMOVER O COMENTARIO: se o FrameEnd mudar para GX_CMDLIST_FLUSH,
     * repor. Kill-switch: $MKXP_TEXFLUSH=true reativa. */
    {
        static int txf_mode = -1;
        if (txf_mode < 0) {
            extern mrb_state *g_mkxp_mrb;
            txf_mode = 0;
            if (g_mkxp_mrb) {
                mrb_value tv = mrb_gv_get(g_mkxp_mrb, mrb_intern_lit(g_mkxp_mrb, "$MKXP_TEXFLUSH"));
                if (!mrb_nil_p(tv) && mrb_test(tv)) txf_mode = 1;
            }
        }
        if (txf_mode == 1 && !tex->in_vram) C3D_TexFlush(&tex->tex);
    }
    tex->last_blit_frame = s_frame_count;   /* mantem viva no EVICT */
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * display_3ds_free_texture
 * [DBG] Confirma que a textura era válida antes de libertar.
 *       Se free é chamado com valid=false, confirma bug upstream.
 * ═══════════════════════════════════════════════════════════════════════════ */
/* SUPERDBG: estatisticas das texturas vivas (lido por MKXPDebug.mem_info). */
void display_3ds_tex_stats(size_t* out_count, size_t* out_bytes, size_t* out_vram) {
    size_t n = s_live_texs.size(), bytes = 0, vram = 0;
    for (size_t i = 0; i < s_live_texs.size(); i++) {
        DS3Texture* t = s_live_texs[i];
        if (!t) continue;
        bytes += t->pot_bytes;
        if (t->in_vram) vram += t->pot_bytes;
    }
    if (out_count) *out_count = n;
    if (out_bytes) *out_bytes = bytes;
    if (out_vram)  *out_vram  = vram;
}

void display_3ds_free_texture(DS3Texture* t) {
    if (!t) {
        printf("[TEX|FREE] chamado com t=NULL (double-free?)\n");
        return;
    }
    /* SUPER: tirar do registo de vivas (swap-pop). */
    for (size_t i = 0; i < s_live_texs.size(); i++) {
        if (s_live_texs[i] == t) {
            s_live_texs[i] = s_live_texs.back();
            s_live_texs.pop_back();
            break;
        }
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
    if (t->valid) {
        /* VRAM e linear vêm de alocadores diferentes: libertar pelo caminho
         * certo (vramFree vs linearFree/C3D_TexDelete) evita corrupcao do heap. */
        if (t->is_shared_atlas) {
            /* [ATLAS-ETERNO] a textura pertence ao boot; so' libertar a POSSE. */
            g_atlas_tex_busy = false;
        } else if (t->in_vram) {
            if (t->tex.data) vramFree(t->tex.data);
            t->tex.data = nullptr;
            s_tex_vram_live--;
            if (s_tex_vram_bytes >= t->pot_bytes) s_tex_vram_bytes -= t->pot_bytes;
            else                                  s_tex_vram_bytes = 0;
        } else {
            C3D_TexDelete(&t->tex);
        }
        s_tex_alive--;
        s_tex_hist[tex_bucket(t->pot_bytes)]--;
        if (s_tex_bytes >= t->pot_bytes) s_tex_bytes -= t->pot_bytes;
        else                             s_tex_bytes = 0;
    }
    delete t;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * super_snapshot  (DBG_SUPER, OFF por defeito)
 * Censo COMPLETO de recursos GPU por intervalo, num so' bloco:
 *   - OCUPACAO   : texturas vivas, bytes, heap linear livre, VRAM livre
 *   - THRESHOLD  : limites (POT max, cap de overflow p/ VRAM, pico de linear)
 *   - HIST       : distribuicao por tamanho
 *   - DELTA      : criadas/libertadas e objetos Ruby criados/libertados no intervalo
 *   - CARREGADA  : texturas NOVAS desde o ultimo snapshot (tamanho/POT/local)
 *   - PESO       : top por bytes (% do total) + NECESSIDADE (desenhada vs ociosa)
 *   - NECESSIDADE: agregado de vivas nao desenhadas ha' muito = candidatas a fuga
 * Generico (engine-level), nao game-specific. Custo zero quando DBG_SUPER off.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void super_snapshot(void) {
    static int      prev_frame   = 0;
    static int      prev_created = 0;
    static int      prev_freedc  = 0;   /* libertadas cumulativo = criadas - vivas */
    static uint64_t prev_objs    = 0;
    static uint64_t prev_frees   = 0;

    int fdelta = s_frame_count - prev_frame; if (fdelta <= 0) fdelta = 1;
    int freedc = s_tex_created - s_tex_alive;
    int cre_i  = s_tex_created - prev_created;
    int fre_i  = freedc        - prev_freedc;
    uint64_t obj_i  = (g_prof_objs_created >= prev_objs)  ? (g_prof_objs_created - prev_objs)  : 0;
    uint64_t fre_oi = (g_prof_frees        >= prev_frees) ? (g_prof_frees        - prev_frees) : 0;

    double tex_kb = s_tex_bytes/1024.0, pk_kb = s_tex_bytes_peak/1024.0;

    printf("\n========== [SUPER] frame=%d (+%d frames desde o ultimo snapshot) ==========\n",
           s_frame_count, fdelta);

    printf("[SUPER|OCUPACAO] texturas_vivas=%d (pico %d) | bytes_tex=%.0fKB (pico %.0fKB)\n",
           s_tex_alive, s_tex_alive_peak, tex_kb, pk_kb);
    printf("[SUPER|OCUPACAO] heap_linear_livre=%.0fKB | VRAM_livre=%.0fKB | em_VRAM=%d tex/%.0fKB\n",
           linearSpaceFree()/1024.0, vramSpaceFree()/1024.0,
           s_tex_vram_live, s_tex_vram_bytes/1024.0);

    printf("[SUPER|THRESHOLD] POT_max=1024x1024 (4MB/tex) | overflow_VRAM<=%.0fKB (folga %.0fKB) | pico_linear=%.0fKB\n",
           VRAM_OVERFLOW_MAX/1024.0, VRAM_OVERFLOW_KEEP/1024.0, pk_kb);

    printf("[SUPER|HIST] <=16K=%d <=64K=%d <=256K=%d <=1M=%d <=4M=%d >4M=%d | maior=%dx%d\n",
           s_tex_hist[0],s_tex_hist[1],s_tex_hist[2],s_tex_hist[3],s_tex_hist[4],s_tex_hist[5],
           s_tex_big_w,s_tex_big_h);

    printf("[SUPER|DELTA] texturas: +%d criadas, -%d libertadas (net %+d) | blits_total=%d falhas_tex=%d\n",
           cre_i, fre_i, cre_i - fre_i, s_blit_total, s_tex_failed);
    printf("[SUPER|DELTA] objetos_Ruby: +%llu criados, -%llu libertados (net %+lld) neste intervalo\n",
           (unsigned long long)obj_i, (unsigned long long)fre_oi,
           (long long)((long long)obj_i - (long long)fre_oi));

    /* CARREGADAS neste intervalo */
    {
        int shown = 0, total_new = 0; double new_kb = 0;
        for (size_t i = 0; i < s_live_texs.size(); i++) {
            DS3Texture* t = s_live_texs[i];
            if (t->create_frame > prev_frame) {
                total_new++; new_kb += t->pot_bytes/1024.0;
                if (shown < 20) {
                    printf("[SUPER|CARREGADA] src=%dx%d pot=%.0fKB %s (criada f=%d)\n",
                           t->width, t->height, t->pot_bytes/1024.0,
                           t->in_vram ? "VRAM" : "linear", t->create_frame);
                    shown++;
                }
            }
        }
        printf("[SUPER|CARREGADA] total_novas=%d (%.0fKB)%s\n",
               total_new, new_kb, (total_new>shown) ? " [lista truncada a 20]" : "");
    }

    /* TOP por PESO + NECESSIDADE */
    {
        std::vector<DS3Texture*> tmp(s_live_texs);
        std::sort(tmp.begin(), tmp.end(),
                  [](DS3Texture* a, DS3Texture* b){ return a->pot_bytes > b->pot_bytes; });
        size_t topn = tmp.size() < 12 ? tmp.size() : 12;
        for (size_t i = 0; i < topn; i++) {
            DS3Texture* t = tmp[i];
            double pct = (s_tex_bytes>0) ? (100.0 * t->pot_bytes / (double)s_tex_bytes) : 0.0;
            int idade = s_frame_count - t->create_frame;
            int desde = (t->last_blit_frame < 0) ? -1 : (s_frame_count - t->last_blit_frame);
            const char* tag = (t->last_blit_frame < 0) ? "NUNCA-DESENHADA"
                            : (desde <= fdelta)         ? "DESENHADA"
                            : (desde > 2*s_super_every) ? "OCIOSA(suspeita-fuga)"
                            :                             "ociosa";
            char db[16];
            if (desde < 0) snprintf(db, sizeof(db), "nunca");
            else           snprintf(db, sizeof(db), "%df", desde);
            printf("[SUPER|PESO] #%u src=%dx%d %.0fKB (%.1f%%) %s idade=%df ult_desenho=%s -> %s\n",
                   (unsigned)(i+1), t->width, t->height, t->pot_bytes/1024.0, pct,
                   t->in_vram ? "VRAM" : "lin", idade, db, tag);
        }
    }

    /* NECESSIDADE agregada: vivas nao desenhadas ha' > 1 intervalo = fuga/cache */
    {
        int idle_n = 0; double idle_kb = 0;
        for (size_t i = 0; i < s_live_texs.size(); i++) {
            DS3Texture* t = s_live_texs[i];
            int desde = (t->last_blit_frame < 0) ? (s_frame_count - t->create_frame)
                                                 : (s_frame_count - t->last_blit_frame);
            if (desde > s_super_every) { idle_n++; idle_kb += t->pot_bytes/1024.0; }
        }
        printf("[SUPER|NECESSIDADE] vivas_nao_desenhadas_ha_>%df = %d tex / %.0fKB "
               "(candidatas a fuga/cache; perto de 0 fora de transicoes)\n",
               s_super_every, idle_n, idle_kb);
    }

    printf("==========================================================================\n\n");

    prev_frame = s_frame_count; prev_created = s_tex_created; prev_freedc = freedc;
    prev_objs  = g_prof_objs_created; prev_frees = g_prof_frees;
}

/* Define o filtro de amostragem da textura (mag, min). Custo zero: e' so' um
 * estado de hardware da GPU PICA200. Usado para por TEXTO em GPU_LINEAR (suave
 * no downscale 0.625x -> legivel) sem tocar nos tilemaps/sprites (GPU_NEAREST). */
void display_3ds_set_texture_filter(DS3Texture* t, bool linear) {
    if (!t || !t->valid) return;
    /* inline (sem nomear o enum) para nao depender do nome do tipo entre versoes
     * do libctru; GPU_LINEAR/GPU_NEAREST sao os mesmos valores que a linha ~315. */
    if (linear) C3D_TexSetFilter(&t->tex, GPU_LINEAR,  GPU_LINEAR);
    else        C3D_TexSetFilter(&t->tex, GPU_NEAREST, GPU_NEAREST);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * display_3ds_blit
 * [DBG] Skip detalhado: porquê é que este blit foi ignorado?
 * ═══════════════════════════════════════════════════════════════════════════ */
/* ── Clip à AREA DE JOGO ──────────────────────────────────────────────────────
 * PORQUE: o jogo desenha em 512x384 logico, mas algumas imagens sao MAIORES que
 * o ecra (ex: introPlayerbg 1024x160, uma faixa decorativa centrada). No PC a
 * janela corta o que sai; no 3DS, sem corte, a imagem vaza para as BARRAS PRETAS
 * laterais (os 40px de cada lado) e ate' para fora do ecra. Resultado visivel:
 * "o amarelo passa as bordas pretas".
 * COMO: depois de transformar para coords FISICAS (fd*), intersectamos o retangulo
 * de destino com a area de jogo fisica [OFF_X..OFF_X+320] x [OFF_Y..OFF_Y+240] e
 * encolhemos os UVs (u0/u1/v) na MESMA proporcao do que foi cortado, para a parte
 * visivel continuar a mapear o pedaco certo da textura (sem esticar). Puramente
 * matematico -- nao depende da orientacao da framebuffer (ao contrario do scissor
 * do PICA, que opera na framebuffer rodada 90 e e' arriscado). GENERICO: qualquer
 * sprite que ultrapasse a area logica fica corretamente contido. Custo: ~6 ifs. */
static inline bool clip_to_game_area(float &fdx, float &fdy, float &fdw, float &fdh,
                                     float &u0, float &u1, float &v_top, float &v_bottom) {
    const float gx0 = DISP_OFF_X;
    const float gy0 = DISP_OFF_Y;
    const float gx1 = DISP_OFF_X + GAME_W * DISP_SCALE;   /* 40 + 320 = 360 */
    const float gy1 = DISP_OFF_Y + GAME_H * DISP_SCALE;   /* 0 + 240 = 240 */
    if (fdw <= 0.0f || fdh <= 0.0f) return false;
    /* corte ESQUERDA */
    if (fdx < gx0) {
        float cut = gx0 - fdx;
        if (cut >= fdw) return false;            /* todo fora */
        float frac = cut / fdw;
        u0 += (u1 - u0) * frac;                   /* avanca a UV-x na mesma proporcao */
        fdx += cut; fdw -= cut;
    }
    /* corte DIREITA */
    if (fdx + fdw > gx1) {
        float cut = (fdx + fdw) - gx1;
        if (cut >= fdw) return false;
        float frac = cut / fdw;
        u1 -= (u1 - u0) * frac;
        fdw -= cut;
    }
    /* corte TOPO (nota: v_top corresponde a sy menor; em UV invertido v_top>v_bottom) */
    if (fdy < gy0) {
        float cut = gy0 - fdy;
        if (cut >= fdh) return false;
        float frac = cut / fdh;
        v_top += (v_bottom - v_top) * frac;
        fdy += cut; fdh -= cut;
    }
    /* corte FUNDO */
    if (fdy + fdh > gy1) {
        float cut = (fdy + fdh) - gy1;
        if (cut >= fdh) return false;
        float frac = cut / fdh;
        v_bottom -= (v_bottom - v_top) * frac;
        fdh -= cut;
    }
    return true;
}

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
    t->last_blit_frame = s_frame_count;   /* SUPER: necessidade (foi desenhada) */

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

    /* CLIP a area de jogo: impede imagens maiores que o ecra (ex: introPlayerbg
     * 1024x160) de vazar para as barras pretas laterais. Se nada sobra visivel,
     * nao desenha. Ajusta UVs proporcionalmente (ver clip_to_game_area). */
    if (!clip_to_game_area(fdx, fdy, fdw, fdh, u0, u1, v_top, v_bottom)) {
        s_blit_skipped++;
        return;
    }

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
     * SubTexture estavam trocados -> passar u1 como left e u0 como right.
     * A largura/altura da SubTexture (em texels) deriva dos UVs JA' clipados
     * (u1-u0)*tw, para a regiao mostrada bater com a parte visivel apos o clip. */
    Tex3DS_SubTexture sub = {
        (u16)(int)((u1 - u0) * tw + 0.5f),
        (u16)(int)((v_top - v_bottom) * th + 0.5f),
        u0, v_top, u1, v_bottom
    };
    /* [SPRFX 2026-07-10] angle/mirror/color/tone POR SPRITE — os campos existiam
     * no struct (setters RGSS ok) mas NUNCA eram aplicados no desenho. Globais
     * postos por sprites_draw_all antes do blit e repostos depois (neutro=~0 custo). */
    if (g_blit_mirror) { float utmp = sub.left; sub.left = sub.right; sub.right = utmp; }
    C2D_Image img = { &t->tex, &sub };
    C2D_DrawParams p = { {fdx, fdy, fdw, fdh}, {0.0f, 0.0f}, 0.5f, 0.0f };
    if (g_blit_angle != 0.0f) {
        /* citro2d roda em torno de pos+center; compensa o pos p/ girar no CENTRO
         * do quad. So' mexe quando ha' angulo (center!=0 com angle=0 desloca). */
        p.center.x = fdw * 0.5f; p.center.y = fdh * 0.5f;
        p.pos.x += p.center.x;   p.pos.y += p.center.y;
        p.angle = -g_blit_angle * 0.017453293f;   /* graus RGSS (anti-horario) -> rad */
    }

    /* FIX BRANCO (historico): blend=0 mantem o RGB; alpha do tint = opacidade.
     * [SPRFX] color (mistura p/ a cor, peso=alpha — flash de captura) e tone
     * (aprox: positivo mistura p/ a cor do tone, negativo escurece) -> blend. */
    float tblend = 0.0f, tr = 1.0f, tg = 1.0f, tb = 1.0f;
    if (g_blit_color[3] > 0) {
        tblend = g_blit_color[3] / 255.0f;
        tr = g_blit_color[0] / 255.0f; tg = g_blit_color[1] / 255.0f; tb = g_blit_color[2] / 255.0f;
    } else if (g_blit_tone[0] != 0.0f || g_blit_tone[1] != 0.0f || g_blit_tone[2] != 0.0f) {
        float mx = g_blit_tone[0]; if (g_blit_tone[1] > mx) mx = g_blit_tone[1]; if (g_blit_tone[2] > mx) mx = g_blit_tone[2];
        float mn = g_blit_tone[0]; if (g_blit_tone[1] < mn) mn = g_blit_tone[1]; if (g_blit_tone[2] < mn) mn = g_blit_tone[2];
        if (mx > 0.0f) {
            tblend = (mx / 255.0f) * 0.85f;
            tr = (g_blit_tone[0] > 0 ? g_blit_tone[0] : 0) / mx;
            tg = (g_blit_tone[1] > 0 ? g_blit_tone[1] : 0) / mx;
            tb = (g_blit_tone[2] > 0 ? g_blit_tone[2] : 0) / mx;
        } else if (mn < 0.0f) {
            tblend = (-mn / 255.0f); tr = tg = tb = 0.0f;
        }
    }
    C2D_ImageTint tint;
    u32 tcol = C2D_Color32f(tr, tg, tb, alpha);
    for (int ci = 0; ci < 4; ci++) {
        tint.corners[ci].color = tcol;
        tint.corners[ci].blend = tblend;
    }
    mkxp_apply_blend();   /* [BLEND-MODE] */
    /* [EVICT-USO 2026-07-11] carimbar o USO no DRAW: o last_blit_frame so' era
     * escrito no UPLOAD -> texturas estaticas (charsets desenhados todos os
     * frames sem re-upload) pareciam "idle" aos 120 frames e o TEX-PRESSURE
     * evictava-as EM USO (log 22:07: os 6 handles de personagens trocados apos
     * cada evict preventivo = re-uploads em loop, trabalho por frame perdido). */
    t->last_blit_frame = s_frame_count;
    C2D_DrawImage(img, &p, &tint);
}

void display_3ds_fill_rect(float x, float y, float w, float h,
                           unsigned char r, unsigned char g,
                           unsigned char b, unsigned char a) {
    /* [BLEND-MODE] overlays (SCREENFX/TRANS-FADE) sao sempre src-over; se o
     * ultimo blit deixou o GPU em aditivo, repor (g_blit_blend ja esta a 0). */
    mkxp_apply_blend();
    C2D_DrawRectSolid(x, y, 0.5f, w, h, C2D_Color32(r, g, b, a));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * display_3ds_blit_ex — versão com destino dw/dh separado (zoom != 1.0)
 * ═══════════════════════════════════════════════════════════════════════════ */
/* [BLITDIAG 2026-07-09] switch runtime $MKXP_BLITDIAG=true (probe linha 1; espelhado
 * em binding). Regista CADA blit de sprites tamanho-battler (192x192) E databox
 * (160x136, o controlo que APARECE): ordem-no-frame, tex ptr/valid, quad destino,
 * src, alpha. Compara battler vs databox no MESMO frame -> revela se o battler e'
 * desenhado antes do fundo (overdraw intra-frame), tex errada, ou quad fora.
 * Desligar: $MKXP_BLITDIAG=false (ou remover a linha do probe). Cap 200 linhas. */
bool g_blitdiag = false;
void display_3ds_blit_ex(DS3Texture* t,
                          float dx, float dy, float dw, float dh,
                          float sx, float sy, float sw, float sh,
                          float alpha) {
    /* filtro v2: TUDO >=136px em ambas as dims (battler 192/346/401, databox 160x136,
     * backdrop 512x384, tilemap 832x704) -> ve-se a ORDEM intra-frame completa. */
    if (g_blitdiag && t && t->width >= 136 && t->height >= 136) {
        extern FILE *g_dbglog; static int bd_n = 0; static int bd_seq = 0, bd_fr = -1;
        /* v4: o flag vem do sprite_binding (assinatura REAL do battler: bitmap
         * 192x192 + viewport batalha + z 5..50). O titulo (arte 346/401, paineis
         * 328) ja' NAO consome o cap — so' a batalha a serio e' registada. */
        extern int g_bd_battle_frame_flag;
        bool bd_battle_frame = (g_bd_battle_frame_flag != 0);
        if (bd_fr != s_frame_count) { bd_fr = s_frame_count; bd_seq = 0; }
        bd_seq++;
        if (bd_battle_frame && bd_n < 600 && g_dbglog) { bd_n++;
            fprintf(g_dbglog, "[BLITDIAG] f=%d seq=%d tex=%p %dx%d valid=%d dst=(%.0f,%.0f %.0fx%.0f) src=(%.0f,%.0f %.0fx%.0f) a=%.2f\n",
                    s_frame_count, bd_seq, (void*)t, t->width, t->height, t->valid?1:0,
                    dx, dy, dw, dh, sx, sy, sw, sh, alpha);
            fflush(g_dbglog); }
    }
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
    t->last_blit_frame = s_frame_count;   /* SUPER: necessidade (foi desenhada) */

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

    /* CLIP a area de jogo (ver display_3ds_blit): contem imagens maiores que o
     * ecra dentro da area logica, sem vazar para as barras pretas. */
    if (!clip_to_game_area(fdx, fdy, fdw, fdh, u0, u1, v_top, v_bottom)) {
        s_blit_skipped++;
        return;
    }

    /* DIAG TEXFLOW: blit de um bitmap COM TEXTO para o ecra. Mapeamento EXACTO
     * src->dst + fator de stretch. Agora atras de DBG_SUPER (off por defeito): a
     * caixa de dialogo esta' resolvida ("Perfeito") e o fluxo de texto ja' foi
     * diagnosticado, por isso nao precisa de escrever ~1x/12f durante cada
     * mensagem no perfil de jogo. Reativa com DBG_SUPER. */
    if (DBGV(DBG_SUPER) && t->has_text && (s_frame_count % 12 == 0)) {
        printf("[TEXFLOW|SCR] f%d texbmp=%dx%d src=(%.0f,%.0f,%.0fx%.0f) -> dst=%.1fx%.1f @(%.1f,%.1f) stretch=x%.2f,y%.2f\n",
               s_frame_count, t->width, t->height, sx, sy, sw, sh,
               fdw, fdh, fdx, fdy,
               (sw>0?fdw/sw:0.0f), (sh>0?fdh/sh:0.0f));
    }

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
     * SubTexture estavam trocados -> passar u1 como left e u0 como right.
     * Largura/altura (texels) derivada dos UVs JA' clipados, ver display_3ds_blit. */
    Tex3DS_SubTexture sub = {
        (u16)(int)((u1 - u0) * tw + 0.5f),
        (u16)(int)((v_top - v_bottom) * th + 0.5f),
        u0, v_top, u1, v_bottom
    };
    /* [SPRFX 2026-07-10] angle/mirror/color/tone POR SPRITE — os campos existiam
     * no struct (setters RGSS ok) mas NUNCA eram aplicados no desenho. Globais
     * postos por sprites_draw_all antes do blit e repostos depois (neutro=~0 custo). */
    if (g_blit_mirror) { float utmp = sub.left; sub.left = sub.right; sub.right = utmp; }
    C2D_Image img = { &t->tex, &sub };
    C2D_DrawParams p = { {fdx, fdy, fdw, fdh}, {0.0f, 0.0f}, 0.5f, 0.0f };
    if (g_blit_angle != 0.0f) {
        /* citro2d roda em torno de pos+center; compensa o pos p/ girar no CENTRO
         * do quad. So' mexe quando ha' angulo (center!=0 com angle=0 desloca). */
        p.center.x = fdw * 0.5f; p.center.y = fdh * 0.5f;
        p.pos.x += p.center.x;   p.pos.y += p.center.y;
        p.angle = -g_blit_angle * 0.017453293f;   /* graus RGSS (anti-horario) -> rad */
    }

    /* FIX BRANCO (historico): blend=0 mantem o RGB; alpha do tint = opacidade.
     * [SPRFX] color (mistura p/ a cor, peso=alpha — flash de captura) e tone
     * (aprox: positivo mistura p/ a cor do tone, negativo escurece) -> blend. */
    float tblend = 0.0f, tr = 1.0f, tg = 1.0f, tb = 1.0f;
    if (g_blit_color[3] > 0) {
        tblend = g_blit_color[3] / 255.0f;
        tr = g_blit_color[0] / 255.0f; tg = g_blit_color[1] / 255.0f; tb = g_blit_color[2] / 255.0f;
    } else if (g_blit_tone[0] != 0.0f || g_blit_tone[1] != 0.0f || g_blit_tone[2] != 0.0f) {
        float mx = g_blit_tone[0]; if (g_blit_tone[1] > mx) mx = g_blit_tone[1]; if (g_blit_tone[2] > mx) mx = g_blit_tone[2];
        float mn = g_blit_tone[0]; if (g_blit_tone[1] < mn) mn = g_blit_tone[1]; if (g_blit_tone[2] < mn) mn = g_blit_tone[2];
        if (mx > 0.0f) {
            tblend = (mx / 255.0f) * 0.85f;
            tr = (g_blit_tone[0] > 0 ? g_blit_tone[0] : 0) / mx;
            tg = (g_blit_tone[1] > 0 ? g_blit_tone[1] : 0) / mx;
            tb = (g_blit_tone[2] > 0 ? g_blit_tone[2] : 0) / mx;
        } else if (mn < 0.0f) {
            tblend = (-mn / 255.0f); tr = tg = tb = 0.0f;
        }
    }
    C2D_ImageTint tint;
    u32 tcol = C2D_Color32f(tr, tg, tb, alpha);
    for (int ci = 0; ci < 4; ci++) {
        tint.corners[ci].color = tcol;
        tint.corners[ci].blend = tblend;
    }
    mkxp_apply_blend();   /* [BLEND-MODE] */
    /* [EVICT-USO 2026-07-11] carimbar o USO no DRAW: o last_blit_frame so' era
     * escrito no UPLOAD -> texturas estaticas (charsets desenhados todos os
     * frames sem re-upload) pareciam "idle" aos 120 frames e o TEX-PRESSURE
     * evictava-as EM USO (log 22:07: os 6 handles de personagens trocados apos
     * cada evict preventivo = re-uploads em loop, trabalho por frame perdido). */
    t->last_blit_frame = s_frame_count;
    C2D_DrawImage(img, &p, &tint);
}