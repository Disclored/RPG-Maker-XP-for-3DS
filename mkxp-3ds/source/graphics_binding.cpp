#include <mruby.h>
#include "debug_3ds.h"
#include <mruby/class.h>
#include <mruby/variable.h>
#include <mruby/string.h>
#include <mruby/gc.h>
#include <mruby/compile.h>   /* mrb_load_string (SAVETIME-FIX retry) */
#include <3ds.h>
#include "display_3ds.h"
#include "input_3ds.h"
#include "sprite_binding_3ds.h"
#include "tilemap_binding_3ds.h"
extern FILE *g_dbglog;
/* OTIMIZACAO 3DS: fflush() a cada printf forcava escrita SINCRONA no cartao SD
 * (lentissimo). Como o grph_update faz printf por frame (60x/s), isto era uma
 * causa direta de lentidao e desgaste do SD. Agora so faz flush a cada 64
 * escritas (contador global partilhado); o resto fica em buffer. Em caso de
 * crash perdem-se no maximo ~64 linhas de log -- troca aceitavel pelo ganho. */
extern int g_dbglog_flushc;
#include <cstring>

/* [SPRFX/SCREENFX] helpers type-checked de etc_binding */
extern "C" void *mkxp_etc_tone_ptr(mrb_state*, mrb_value);
extern "C" void *mkxp_etc_color_ptr(mrb_state*, mrb_value);
extern "C" void *mkxp_etc_rect_ptr(mrb_state*, mrb_value);   /* [VP-REGISTRY] */
#include <mruby/array.h>                                      /* [VP-REGISTRY] */
extern bool g_log_verbose;
extern bool g_boot_done;
/* [BOOT-LEAN] durante o boot, os printf deste ficheiro (GFX, WD, etc.) so' passam
 * se forem plugins/GameData/erros. */
static inline bool __raw_suppress(const char *fmt){
    if (g_log_verbose || !fmt) return false;
    if (strstr(fmt,"FALH")||strstr(fmt,"ERRO")||strstr(fmt,"WARN")||strstr(fmt,"CRASH")) return false;
    if (!g_boot_done && (strstr(fmt,"[PLUGINS]")||strstr(fmt,"[Game.init]"))) return false;
    return true;
}
#define printf(fmt, ...) do { if (!__raw_suppress(fmt) && g_dbglog) { fprintf(g_dbglog, fmt, ##__VA_ARGS__); \
    if (++g_dbglog_flushc >= 64) { g_dbglog_flushc = 0; dbg_flush(); } } } while(0)

/* OTIMIZACAO 3DS: controla os logs por-frame do grph_update. 0 = silencioso
 * (recomendado, jogo mais leve e log limpo). 1 = verboso (debug do loop). */
#ifndef GFX_VERBOSE
#define GFX_VERBOSE 0
#endif

extern void inputBindingUpdate();

/* CLAMP do frame_rate. O jogo faz Graphics.frame_rate = 60, mas o New 3DS so'
 * atinge ~20-30 FPS em mapas pesados. Como o movimento do Essentials e'
 * frame-rate-INDEPENDENTE (move_speed_real = val*40/frame_rate; scroll idem),
 * a velocidade real fica correta SE o FPS real == frame_rate declarado. Ao
 * reportar 30 (em vez de 60), a logica do jogo calibra para 30 -> a 30 FPS
 * reais o movimento fica a' velocidade de design (em vez de 1/3, que e' o
 * descompasso 60-declarado-vs-20-reais que faz o jogo parecer camara lenta).
 * Muda aqui o alvo se quiseres (20/40 sao "redondos" p/ a matematica /20). */
#ifndef MKXP_FPS_CAP
#define MKXP_FPS_CAP 30
#endif

static int  s_frame_rate  = MKXP_FPS_CAP;
static int  s_frame_count = 0;
static bool s_frozen      = false;
/* [TRANS-FADE 2026-07-02] Graphics.transition ANIMADO. RAIZ (provada no log 08:39:
 * transition #1/#2 no MESMO frame, skipped=0 -> corte instantaneo): grph_transition
 * era um STUB (so' s_frozen=false) e snap_to_bitmap=nil, logo NENHUMA transicao do
 * jogo (097_Transitions.rb foi rebindado por cima) alguma vez animou. Sem captura de
 * framebuffer (arriscado no PICA200), a aproximacao SEGURA e' um fade-in a partir de
 * preto: transition(N) desenha a cena NOVA com um retangulo preto por cima cuja alpha
 * desce 255->0 em N frames (display_3ds_fill_rect, por cima de todos os sprites).
 * Pior caso = o comportamento antigo (corte). transition(0) continua instantaneo.
 * Semantica RGSS: sem args -> 8 frames. Gate: $MKXP_TRANS_FADE=false desliga. */
static int  s_fade_left  = 0;
static int  s_fade_total = 0;
static long long g_last_present_us = 0;   /* limitador de FPS por relogio (us) */
extern "C" void display_3ds_evict_pressure(int);   /* [TEX-PRESSURE] valvula preventiva */

/* FIX vsync judder (precipicio 30->20fps): ver bloco de pacing em grph_update.
 * true  = pacing suave (frames lentos seguem ja', sem o salto 35ms->50ms);
 * false = comportamento antigo (gspWaitForVBlank incondicional).
 * volatile + nao-static: pode ser lido/escrito de fora (ex: toggle de debug). */
volatile bool g_vsync_smooth = true;

/* [FIX-GCIDLE] Dreno de GC no tempo IDLE (espera do VBlank do GPU).
 * O dreno major (mrb_full_gc) corria no INICIO do frame (tempo ocupado) e causava
 * o hitch periodico (coleccao dos ~18k objetos a meio do trabalho). Agora corre na
 * janela de present/espera-VBlank, onde o CPU esta' PARADO a' espera do GPU: a
 * coleccao sobrepoe-se a esse tempo morto e o hitch encolhe. Mesma coleccao, momento
 * melhor. true = dreno no idle; false = comportamento antigo (dreno a 600k no inicio).
 * Override em runtime: $__gc_idle_drain (nil=usa este default; true/false manda). */
volatile bool g_gc_idle_drain = true;
/* Cooldown PARTILHADO entre a valvula de emergencia (inicio) e o dreno idle (fim):
 * garante no maximo 1 full_gc por 30 frames -> nunca ha 2 coleccoes seguidas. */
static int s_gc_cooldown = 0;

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
	/* OTIMIZACAO 3DS: estes 2 logs corriam a CADA frame (60x/s), enchendo o
	 * log e gastando I/O. Agora so aparecem se GFX_VERBOSE=1. Para debug
	 * detalhado do loop, mudar para 1. */
#if GFX_VERBOSE
	printf("[GFX|ENTER] grph_update entrou\n");
	printf("[GFX] grph_update CALLED frame=%d frozen=%d\n", s_frame_count, s_frozen);
#endif
    (void)self;

    if (!aptMainLoop()) {
        DBG("aptMainLoop() devolveu false → quit");
        mrb_raise(mrb, E_RUNTIME_ERROR, "aptMainLoop: quit requested");
    }

    /* [EPOCH-CLOCK] avanca o relogio de recencia do GIANT-LRU/OOM-STRIP mesmo
     * quando ninguem compoe (batalha/menus) — sem isto "parado" era indetectavel. */
    { extern void mkxp_epoch_tick(void); mkxp_epoch_tick(); }

    /* [CHURN-WATCH 2026-07-11] vigia catch-all do churn de 1.5M objetos/batalha:
     * loga quando gc.live CRUZA 800k (subida), 1x por cruzamento, com o frame.
     * Correlaciona com os marcos [MFD] da batalha para localizar a fase que
     * explode, mesmo que nao seja o setPokemonBitmap ([CHURN-TRACE]). Barato:
     * 1 leitura de campo + comparacao por frame. */
    extern mrb_state *g_mkxp_mrb;
    if (g_mkxp_mrb && g_dbglog) {
        static bool s_cw_high = false;
        int _cw_live = (int)g_mkxp_mrb->gc.live;
        if (!s_cw_high && _cw_live > 800000) {
            s_cw_high = true;
            fprintf(g_dbglog, "[CHURN-WATCH] gc.live CRUZOU 800k (=%d) no frame %d -- fase que explode\n",
                    _cw_live, s_frame_count);
            fflush(g_dbglog);
        } else if (s_cw_high && _cw_live < 400000) {
            s_cw_high = false;   /* re-arma apos o GC baixar */
        }
    }

    /* [GC-CEILING 2026-07-11] TETO RIGIDO no TOPO GARANTIDO do grph_update (a
     * valvula normal de 720k vive dentro do bloco de render, SALTADO no titulo/
     * transicoes — por isso o [CHURN-WATCH] apanhou 1.39M sem GC). Aqui corre
     * SEMPRE: acima de 1M, full_gc JA', ignorando o cooldown. So' dispara em
     * churn patologico (titulo/batalha/EBDX) -> zero custo no jogo normal (<720k),
     * mata o pico de 1.4M que esgota o malloc (magenta/OOM) e o stall do sweep
     * gigante. Gate $__gc_ceiling=false reverte. */
    if (g_mkxp_mrb) {
        bool _gc_ceiling = true;
        { mrb_value _v = mrb_gv_get(g_mkxp_mrb, mrb_intern_cstr(g_mkxp_mrb, "$__gc_ceiling"));
          if (!mrb_nil_p(_v)) _gc_ceiling = mrb_test(_v); }
        static int s_ceil_cool = 0;
        if (s_ceil_cool > 0) s_ceil_cool--;
        if (_gc_ceiling && s_ceil_cool == 0 && (size_t)g_mkxp_mrb->gc.live > 1000000) {
            size_t _cb = (size_t)g_mkxp_mrb->gc.live;
            mrb_full_gc(g_mkxp_mrb);
            s_ceil_cool = 8;   /* min 8 frames entre tetos (evita thrash mas cobre bursts) */
            if (g_dbglog) {
                fprintf(g_dbglog, "[GC|TETO] vivos=%u > 1M -> full_gc -> %u (recolheu %d) [frame %d]\n",
                        (unsigned)_cb, (unsigned)g_mkxp_mrb->gc.live,
                        (int)((long)_cb - (long)g_mkxp_mrb->gc.live), s_frame_count);
                fflush(g_dbglog);
            }
        }
    }

    /* [GFXSPLIT 2026-07-05] fatias EXATAS do grph_update (o FRAMEBUDGET provou
     * gfx=26-28ms/frame com render+swap ~9 -> 14-18ms escondidos AQUI dentro).
     * A=fixes/checks do topo; B=render(begin+draw+end); C=GC-idle+pacing+resto.
     * Report 1x/600 frames no g_dbglog. Tick->ms: /268123.48 */
    u64 _gs_t0 = svcGetSystemTick();
    u64 _dperf_t0 = 0, _dperf_t1 = 0, _dperf_t2 = 0;   /* hoisted: o if do render foi partido */
    static double s_gs_a = 0, s_gs_b = 0, s_gs_c = 0;
    static double s_gs_gc = 0, s_gs_sleep = 0;   /* sub-fatias: GC (idle+slack) e vblank+sleep do cap */
    static int s_gs_n = 0;

    /* [SAVETIME-FIX retry 2026-07-02] Anti-crash "comparison of Integer with Time"
     * no TITULO (Scene_Intro->closeTitle->get_newest_save_slot). Um save legado tem
     * Player#last_time_saved como Integer (epoch); a comparacao usa Integer#> que nao
     * conhece a classe Time do motor -> ArgumentError -> Scene_Intro morre -> fecha.
     * PORQUE AQUI: em Game.init (t~6.7s) o plugin Auto Multi Save ainda NAO definiu
     * Player#last_time_saved (so' o faz depois). O grph_update corre ~cada frame DESDE
     * o titulo (t~6.9s), por isso tentamos aplicar o patch a cada frame ate' o Player
     * estar pronto -- garantidamente ANTES do closeTitle. Auto-para ($__savetime_patched
     * ou apos ~900 frames). Custo: 1 gv_get/frame ate' aplicar, ZERO depois. Normaliza
     * o getter: numerico -> Time.at(v) -> a comparacao passa a Time#> (robusto). */
    {
        static int s_stf = 0;   /* 0=a tentar; !=0 feito/desistido */
        if (s_stf == 0) {
            if (mrb_test(mrb_gv_get(mrb, mrb_intern_cstr(mrb, "$__savetime_patched")))) {
                s_stf = 1;
            } else {
                static int s_stf_tries = 0;
                if (++s_stf_tries > 900) {
                    s_stf = 1;   /* nunca ficou pronto -> desiste (nao ha' save/Player) */
                } else {
                    /* Introspecao ROBUSTA: neste mruby, defined?/method_defined?/
                     * respond_to? sao pouco fiaveis (ja' partiram fixes antes). Usamos
                     * Object.const_defined? + instance_methods.include? (fiaveis). O
                     * [SAVETIME-DIAG] loga UMA vez o estado real (Player visto? tem
                     * last_time_saved?) p/ diagnostico definitivo caso ainda nao pegue. */
                    /* (1) coage Player#last_time_saved numerico->Time (fix correto).
                     * (2) rede de seguranca: get_newest_save_slot nunca rebenta (nil se
                     *     falhar) -> se (1) nao pegar, o jogo ARRANCA em vez de fechar. */
                    mrb_load_string(mrb,
                        "begin;"
                        " if $__savetime_patched != true && Object.const_defined?(:Player);"
                        "  _k=Object.const_get(:Player); _im=_k.instance_methods;"
                        "  if $__stf_diag != true; $__stf_diag=true;"
                        "   (MKXPDebug.log('[SAVETIME-DIAG] Player visto; has_lts=' + _im.include?(:last_time_saved).to_s) rescue nil);"
                        "  end;"
                        "  if _im.include?(:last_time_saved) && !_im.include?(:__mkxp_orig_lts);"
                        "   _k.class_eval('alias_method :__mkxp_orig_lts, :last_time_saved; def last_time_saved; v=__mkxp_orig_lts; (v.is_a?(Integer)||v.is_a?(Float)) ? (Time.at(v) rescue v) : v; end');"
                        "   $__savetime_patched=true;"
                        "   (MKXPDebug.log('[SAVETIME-FIX] aplicado (class_eval)') rescue nil);"
                        "  end;"
                        " end;"
                        " if $__gnss_wrapped != true && Object.const_defined?(:SaveData);"
                        "  _sd=Object.const_get(:SaveData); _sm=_sd.singleton_class.instance_methods;"
                        "  if _sm.include?(:get_newest_save_slot) && !_sm.include?(:__mkxp_orig_gnss);"
                        "   _sd.singleton_class.send(:alias_method,:__mkxp_orig_gnss,:get_newest_save_slot);"
                        "   _sd.define_singleton_method(:get_newest_save_slot){ begin; __mkxp_orig_gnss; rescue => e; (MKXPDebug.log('[SAVETIME-FIX] gnss rescue: '+e.class.to_s+': '+e.message.to_s[0,80]) rescue nil); nil; end };"
                        "   $__gnss_wrapped=true;"
                        "   (MKXPDebug.log('[SAVETIME-FIX] get_newest_save_slot protegido (rescue->nil)') rescue nil);"
                        "  end;"
                        " end;"
                        "rescue; end");
                    mrb->exc = 0;   /* nunca deixar excecao vazar p/ o frame */
                }
            }
        }
    }

    /* [INPUT-MOVIDO 2026-07-05] O poll de input (inputBindingUpdate: hidScanInput
     * + latches) custava ~2-4ms REAIS na fatia critica do frame (GFXSPLIT:
     * fixes_topo=4.4ms — no Azahar o HID emulado e' caro). O poll so' precisa de
     * acontecer ANTES do proximo scene.update — movido para DEPOIS do render
     * (ver [INPUT-POLL-TARDIO] abaixo), onde se sobrepoe ao tempo que ia ser
     * cap_sleep: nos frames rapidos os ms saem de graca. Semantica intacta:
     * o main loop e' Graphics.update -> Input.update -> scene.update, por isso
     * o poll no FIM do grph_update continua a preceder a leitura do jogo. */

    /* ── [FCRAM] VÁLVULA DE SEGURANÇA anti-OOM de OBJETOS (crash do roupeiro) ───
     * O full_gc no freeze (grph_freeze) limpa o lixo ENTRE cenas. Mas um menu
     * DENTRO do mapa — como o roupeiro/wardrobe deste jogo, que corre o seu
     * proprio loop com Graphics.update sem nunca fazer freeze — acumula objetos
     * orfaos no old-gen que o GC GERACIONAL so' recolhe num full GC. Forense do
     * crash (log: troca de outfit): o working-set normal e' ~230K vivos, picos
     * normais ate' ~590K; o menu do roupeiro escalou ate' 1.2M (970K orfaos) ->
     * o heap REGULAR do mruby (malloc, onde vivem os RObject) esgotou -> a
     * proxima alocacao de QUALQUER objeto falhou -> abort duro (o log termina
     * a seco logo a seguir a CommonEvent #112). bmp_alloc nao crasha (cai p/
     * linearAlloc); o que crasha e' o mruby sem RAM p/ objetos.
     *
     * Esta valvula forca um full GC quando os vivos passam um limiar seguro
     * (720K: acima dos picos normais ~590K, bem abaixo do ponto de crash 1.2M),
     * capando o pico. Cooldown de 30 frames p/ nunca fazer thrash (no max. 1
     * full GC / 30 frames sob pressao). No jogo normal (sempre < limiar) NUNCA
     * dispara -> custo zero. Generico p/ qualquer menu/churn in-map; mantem
     * diagnostico no log sempre que atua. */
    {
        /* [GC-FPS v2] 600000. Com o GC de gameplay agora GERACIONAL + major automatico
         * TRAVADO (majorgc_old_threshold=3000000, binding_3ds.cpp ~5568), os minors
         * por-frame sao baratos mas a old-gen CRESCE sem o major: esta valvula passa a
         * ser o DRENO PRINCIPAL (alem do mrb_full_gc do grph_freeze nas transicoes).
         * 600k da' folga suficiente entre drenos mantendo o full_gc periodico abaixo do
         * pico antigo de 740k. Se o hitch periodico incomodar, o passo seguinte e' o GC
         * orcamentado por-frame (mrb_incremental_gc) para o espalhar. */
        /* [FIX-GCIDLE] Com o dreno idle LIGADO, este ponto (inicio do frame, tempo
         * ocupado) fica SO' como valvula de EMERGENCIA: threshold mais alto (720k,
         * acima dos picos normais ~590k e do dreno idle a 600k) p/ a seguranca
         * anti-OOM/anti-crash do churn extremo (menu do roupeiro). No jogo normal
         * NUNCA dispara aqui -> a coleccao normal acontece toda no idle (fim do frame).
         * Com o dreno idle DESLIGADO, volta ao comportamento antigo (dreno a 600k aqui).
         * Cooldown PARTILHADO (s_gc_cooldown) com o dreno idle. */
        bool _gci_on = g_gc_idle_drain;
        { mrb_value _v = mrb_gv_get(mrb, mrb_intern_cstr(mrb, "$__gc_idle_drain"));
          if (!mrb_nil_p(_v)) _gci_on = mrb_test(_v); }
        const size_t GC_VALVE_LIVE = _gci_on ? 720000 : 600000;
        if (s_gc_cooldown > 0) {
            s_gc_cooldown--;
        } else if ((size_t)mrb->gc.live > GC_VALVE_LIVE) {
            size_t before = (size_t)mrb->gc.live;
            mrb_full_gc(mrb);
            s_gc_cooldown = 30;
            printf("[GC|%s] vivos=%u > %u -> full_gc -> %u (recolheu %d) "
                   "[frame %d] -- %s\n",
                   _gci_on ? "EMERGENCIA" : "VALVULA",
                   (unsigned)before, (unsigned)GC_VALVE_LIVE, (unsigned)mrb->gc.live,
                   (int)((long)before - (long)mrb->gc.live), s_frame_count,
                   _gci_on ? "churn extremo (idle-drain ON)" : "anti-OOM menu/churn in-map");
        }
    }

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

    extern bool g_log_verbose;   /* [LIMPEZA] gate dos diagnosticos runtime C-puros */

    /* [FRAMESKIP 2026-07-11, padrao Better Fast-Forward provado em Essentials]
     * Quando a LOGICA Ruby estourou o orcamento (frame anterior >40ms), salta a
     * COMPOSICAO deste ciclo (a logica continua; o LCD mantem o frame anterior)
     * — em vez de slow-motion, o jogo larga 1 frame visual e mantem o ritmo.
     * Nunca salta: durante freeze/transicao (o present e' o efeito), 2 frames
     * seguidos (half-rate minimo), ou os primeiros 300 frames (boot/intro).
     * Kill-switch: $MKXP_FRAMESKIP=false. */
    bool skip_render = false;
    {
        static int fs_on = -1;
        if (fs_on < 0) {
            mrb_value fv = mrb_gv_get(mrb, mrb_intern_lit(mrb, "$MKXP_FRAMESKIP"));
            fs_on = (!mrb_nil_p(fv) && !mrb_test(fv)) ? 0 : 1;   /* default ON */
        }
        static u64 fs_last_tick = 0;
        static bool fs_skipped_last = false;
        u64 now = svcGetSystemTick();
        double frame_ms = fs_last_tick ? (double)(now - fs_last_tick) / 268123.48 : 0.0;
        fs_last_tick = now;
        if (fs_on == 1 && !s_frozen && s_frame_count > 300 &&
            frame_ms > 40.0 && !fs_skipped_last) {
            skip_render = true;
            fs_skipped_last = true;
        } else {
            fs_skipped_last = false;
        }
    }

    if (!s_frozen && !skip_render) {
        /* [DBG-B] Antes de draw: confirmar que o render target existe */
        if (g_log_verbose && (s_frame_count < 3 || s_frame_count % 300 == 0)) {
            printf("[GFX|RENDER] frame %d → begin_frame + sprites_draw_all + end_frame\n",
                   s_frame_count);
        }

        u64 _gs_t1 = svcGetSystemTick();
        s_gs_a += (double)(_gs_t1 - _gs_t0) / 268123.48;
        _gs_t0 = _gs_t1;   /* reutiliza p/ medir o render */
        display_3ds_begin_frame();

        /* [DBG-B] sprites_draw_all é uma caixa negra — meter probe antes/depois */
        int pre_blit = display_3ds_blit_count();   /* ← adicionar accessor em display_3ds */
        _dperf_t0 = osGetTime();   /* [DIAG-PERF] ms */
        tilemaps_draw_all();
        _dperf_t1 = osGetTime();   /* [DIAG-PERF] */
        sprites_draw_all();
        _dperf_t2 = osGetTime();   /* [DIAG-PERF] */
        /* DUMP-DE-FRAME (botao Y): despeja todos os sprites DESTE frame, ja'
         * ordenados por z. So' corre quando o utilizador pediu (flag do input).
         * Limita a alguns despejos por sessao para nao encher o SD se ficar
         * carregado. */
        {
            extern volatile bool g_dump_frame_requested;
            if (g_dump_frame_requested) {
                g_dump_frame_requested = false;
                static int s_dump_n = 0;
                if (s_dump_n < 12) {
                    s_dump_n++;
                    sprites_dump_frame(mrb, "botao-Y");
                }
            }
        }
        int post_blit = display_3ds_blit_count();

        /* [TRANS-FADE] overlay do fade-in: retangulo PRETO por cima de toda a cena,
         * alpha 255->0 ao longo de s_fade_total frames. Desenhado depois de todos os
         * sprites e antes do end_frame -> cobre o ecra inteiro no alvo atual. Custo:
         * 1 rect/frame durante o fade; zero fora dele. */
        if (s_fade_left > 0 && s_fade_total > 0) {
            int a = (255 * s_fade_left) / s_fade_total;
            if (a > 255) a = 255;
            display_3ds_fill_rect(0.0f, 0.0f,
                                  (float)display_3ds_screen_width(),
                                  (float)display_3ds_screen_height(),
                                  0, 0, 0, (unsigned char)a);
            s_fade_left--;
        }

        /* [SCREENFX 2026-07-10] tone + flash do $game_screen — 2139 usos no jogo
         * (Tint/Flash Screen de eventos) eram NO-OP no 3DS: cutscenes sem anoitecer,
         * sem flashes de relampago, sem escurecer de caves. Overlay pos-sprites:
         *  - tone negativo -> retangulo preto (escurece);
         *  - tone positivo -> retangulo da cor do tone (aprox de clarear; sem blend
         *    aditivo no fill_rect, o alpha e' escalado a 55% p/ nao "lavar");
         *  - flash_color (alpha>0) -> retangulo da cor (o proprio jogo decai o alpha).
         * Custo: 1 gv_get + 2 iv_get por frame; rects so' quando ativos.
         * (shake fica p/ ciclo proprio: exige transladar o alvo, nao um overlay.) */
        /* [SCREENFX v2 2026-07-11] o bloco direto do $game_screen foi DESLIGADO:
         * o Essentials ja propaga tudo para viewports (065_Spriteset_Map:120/144/
         * 145 -> viewport1.ox=shake, viewport1.tone=tone, viewport3.color=flash)
         * e agora o [VP-REGISTRY] + [TONE-DUAL] + [VPOFS] aplicam-no pelo caminho
         * RGSS-correto -- manter o overlay direto duplicaria o tint/flash (2x
         * mais escuro que o Windows). Reativavel p/ diagnostico: $MKXP_GSFX=true. */
        {
            static int gsfx_on = -1;
            if (gsfx_on < 0) {
                mrb_value gf = mrb_gv_get(mrb, mrb_intern_lit(mrb, "$MKXP_GSFX"));
                gsfx_on = (!mrb_nil_p(gf) && mrb_test(gf)) ? 1 : 0;   /* default OFF */
            }
            static mrb_sym sy_gs = 0, sy_tn = 0, sy_fc = 0;
            if (!sy_gs) { sy_gs = mrb_intern_lit(mrb, "$game_screen");
                          sy_tn = mrb_intern_lit(mrb, "@tone");
                          sy_fc = mrb_intern_lit(mrb, "@flash_color"); }
            mrb_value gs = (gsfx_on == 1) ? mrb_gv_get(mrb, sy_gs) : mrb_nil_value();
            if (!mrb_nil_p(gs) && mrb_type(gs) == MRB_TT_OBJECT) {
                float SW = (float)display_3ds_screen_width();
                float SH = (float)display_3ds_screen_height();
                struct TF { float r,g,b,gray; };
                struct CF { float r,g,b,a; };
                mrb_value tv = mrb_iv_get(mrb, gs, sy_tn);
                TF *tn = (TF*)(mrb_nil_p(tv) ? NULL : mkxp_etc_tone_ptr(mrb, tv));
                if (tn) {
                    float mn = tn->r; if (tn->g < mn) mn = tn->g; if (tn->b < mn) mn = tn->b;
                    float mx = tn->r; if (tn->g > mx) mx = tn->g; if (tn->b > mx) mx = tn->b;
                    if (mn < -1.0f) {   /* escurecer (ex: noite/cave: tone -60ss) */
                        unsigned char a = (unsigned char)(-mn > 255 ? 255 : -mn);
                        display_3ds_fill_rect(0, 0, SW, SH, 0, 0, 0, a);
                    }
                    if (mx > 1.0f) {    /* clarear/tingir (ex: por-do-sol +40,-20,-40) */
                        unsigned char r = (unsigned char)(tn->r > 0 ? (tn->r > 255 ? 255 : tn->r) : 0);
                        unsigned char g = (unsigned char)(tn->g > 0 ? (tn->g > 255 ? 255 : tn->g) : 0);
                        unsigned char b = (unsigned char)(tn->b > 0 ? (tn->b > 255 ? 255 : tn->b) : 0);
                        unsigned char a = (unsigned char)((mx > 255 ? 255 : mx) * 0.55f);
                        display_3ds_fill_rect(0, 0, SW, SH, r, g, b, a);
                    }
                }
                mrb_value fv = mrb_iv_get(mrb, gs, sy_fc);
                CF *fc = (CF*)(mrb_nil_p(fv) ? NULL : mkxp_etc_color_ptr(mrb, fv));
                if (fc && fc->a > 1.0f) {
                    display_3ds_fill_rect(0, 0, SW, SH,
                        (unsigned char)(fc->r > 255 ? 255 : fc->r),
                        (unsigned char)(fc->g > 255 ? 255 : fc->g),
                        (unsigned char)(fc->b > 255 ? 255 : fc->b),
                        (unsigned char)(fc->a > 255 ? 255 : fc->a));
                }
            }
            /* [VP-REGISTRY 2026-07-11, substitui o VPFX] color/tone de TODOS os
             * viewports vivos. O VPFX so' lia $scene.@viewport — que NUNCA existe
             * (Scene_Map/Intro/Credits nao teem @viewport) -> 176 call-sites de
             * pbFadeOutIn (fade de entrada/saida de TODAS as cenas/lojas/menus,
             * 080:549-576) + fades do EBDX (0090/0092/0095) ficavam sem efeito.
             * O compat regista cada Viewport em $__mkxp_vps ao nascer; aqui
             * percorremos a lista (poucos por cena), lemos @color/@tone via os
             * ptr dual-path ([TONE-DUAL] cobre Ruby-puro) e desenhamos overlay
             * no RECT do viewport (le @rect por ivars; fallback fullscreen). */
            {
                static mrb_sym sy_vps = 0, sy_col = 0, sy_tn2 = 0, sy_rc = 0,
                               sy_rx = 0, sy_ry = 0, sy_rw = 0, sy_rh = 0, sy_disp = 0;
                if (!sy_vps) { sy_vps = mrb_intern_lit(mrb, "$__mkxp_vps");
                               sy_col = mrb_intern_lit(mrb, "@color");
                               sy_tn2 = mrb_intern_lit(mrb, "@tone");
                               sy_rc  = mrb_intern_lit(mrb, "@rect");
                               sy_rx  = mrb_intern_lit(mrb, "@x");
                               sy_ry  = mrb_intern_lit(mrb, "@y");
                               sy_rw  = mrb_intern_lit(mrb, "@width");
                               sy_rh  = mrb_intern_lit(mrb, "@height");
                               sy_disp= mrb_intern_lit(mrb, "@disposed"); }
                mrb_value vlist = mrb_gv_get(mrb, sy_vps);
                if (mrb_array_p(vlist)) {
                    float SW = (float)display_3ds_screen_width();
                    float SH = (float)display_3ds_screen_height();
                    struct CF2 { float r,g,b,a; };
                    struct TF2 { float r,g,b,gray; };
                    mrb_int vn = RARRAY_LEN(vlist);
                    for (mrb_int vi = 0; vi < vn; vi++) {
                        mrb_value vp = mrb_ary_ref(mrb, vlist, vi);
                        if (mrb_nil_p(vp) || mrb_type(vp) != MRB_TT_OBJECT) continue;
                        if (mrb_test(mrb_iv_get(mrb, vp, sy_disp))) continue;
                        /* rect do viewport (Ruby-puro ou nativo via ivars do compat) */
                        float rx = 0, ry = 0, rw = SW, rh = SH;
                        mrb_value ro = mrb_iv_get(mrb, vp, sy_rc);
                        if (!mrb_nil_p(ro)) {
                            void *rp = mkxp_etc_rect_ptr(mrb, ro);
                            if (rp) { struct RI { int x,y,w,h; }; RI *r = (RI*)rp;
                                      rx=(float)r->x; ry=(float)r->y; rw=(float)r->w; rh=(float)r->h; }
                            else if (mrb_type(ro) == MRB_TT_OBJECT) {
                                mrb_value xx = mrb_iv_get(mrb, ro, sy_rx);
                                mrb_value yy = mrb_iv_get(mrb, ro, sy_ry);
                                mrb_value ww = mrb_iv_get(mrb, ro, sy_rw);
                                mrb_value hh = mrb_iv_get(mrb, ro, sy_rh);
                                if (mrb_fixnum_p(xx)) rx = (float)mrb_fixnum(xx);
                                if (mrb_fixnum_p(yy)) ry = (float)mrb_fixnum(yy);
                                if (mrb_fixnum_p(ww)) rw = (float)mrb_fixnum(ww);
                                if (mrb_fixnum_p(hh)) rh = (float)mrb_fixnum(hh);
                            }
                        }
                        if (rw <= 0 || rh <= 0) continue;
                        mrb_value cv = mrb_iv_get(mrb, vp, sy_col);
                        CF2 *vc = (CF2*)(mrb_nil_p(cv) ? NULL : mkxp_etc_color_ptr(mrb, cv));
                        if (vc && vc->a > 1.0f) {
                            display_3ds_fill_rect(rx, ry, rw, rh,
                                (unsigned char)(vc->r > 255 ? 255 : vc->r),
                                (unsigned char)(vc->g > 255 ? 255 : vc->g),
                                (unsigned char)(vc->b > 255 ? 255 : vc->b),
                                (unsigned char)(vc->a > 255 ? 255 : vc->a));
                        }
                        mrb_value tv2 = mrb_iv_get(mrb, vp, sy_tn2);
                        TF2 *vt = (TF2*)(mrb_nil_p(tv2) ? NULL : mkxp_etc_tone_ptr(mrb, tv2));
                        if (vt) {
                            float mn2 = vt->r; if (vt->g < mn2) mn2 = vt->g; if (vt->b < mn2) mn2 = vt->b;
                            float mx2 = vt->r; if (vt->g > mx2) mx2 = vt->g; if (vt->b > mx2) mx2 = vt->b;
                            if (mn2 < -1.0f)
                                display_3ds_fill_rect(rx, ry, rw, rh, 0, 0, 0,
                                    (unsigned char)(-mn2 > 255 ? 255 : -mn2));
                            if (mx2 > 1.0f)
                                display_3ds_fill_rect(rx, ry, rw, rh,
                                    (unsigned char)(vt->r > 0 ? (vt->r > 255 ? 255 : vt->r) : 0),
                                    (unsigned char)(vt->g > 0 ? (vt->g > 255 ? 255 : vt->g) : 0),
                                    (unsigned char)(vt->b > 0 ? (vt->b > 255 ? 255 : vt->b) : 0),
                                    (unsigned char)((mx2 > 255 ? 255 : mx2) * 0.55f));
                        }
                    }
                }
            }
        }

        if (g_log_verbose && (s_frame_count < 3 || s_frame_count % 300 == 0)) {
            printf("[GFX|RENDER] frame %d → sprites_draw_all fez %d blits novos\n",
                   s_frame_count, post_blit - pre_blit);
            if (post_blit == pre_blit) {
                printf("[GFX|WARN]   → ZERO blits! Sprites não registados ou todos invisíveis.\n");
                printf("[GFX|WARN]     Verificar: sprite.visible=true, sprite.bitmap!=nil, z correcto\n");
                dbg_dump_scene(mrb, "zero-blits");
            }
        }

        display_3ds_end_frame();
        {
            u64 _gs_t2 = svcGetSystemTick();
            s_gs_b += (double)(_gs_t2 - _gs_t0) / 268123.48;
            _gs_t0 = _gs_t2;
        }
    }

    /* [INPUT-POLL-TARDIO 2026-07-05] poll de input APOS o render (frame ja' no
     * GPU): o custo (~2-4ms no emulador) sobrepoe-se ao periodo que ia ser
     * cap_sleep. Corre TODOS os frames (frozen incluido — fora do if acima). */
    {
        extern bool g_input_flush_on;
        mrb_value _fv = mrb_gv_get(mrb, mrb_intern_cstr(mrb, "$MKXP_INPUT_FLUSH_ON_TRANSITION"));
        g_input_flush_on = (mrb_nil_p(_fv) || mrb_test(_fv));
        inputBindingUpdate();
    }
    if (!s_frozen) {
        /* (continua o caminho normal: flash/pos-render) */

        /* ── [GC|ORCAMENTADO] drena a old-gen em fatias previsiveis ──────────────
         * Com majorgc_old_threshold=3.000.000 (binding_3ds ~5568) o mruby NUNCA corre
         * o major automatico, por isso sem isto a old-gen so' era limpa pela valvula
         * (full_gc a 600k = hitch ~150ms, 33x/sessao = a gaguez mais visivel nos mapas
         * densos). Aqui avancamos o GC incremental ate' ~3ms/frame: o lixo de vida-media
         * e' recolhido suavemente, a 'live' nao chega a 600k -> a valvula deixa de
         * disparar. mrb_incremental_gc avanca UM passo; paramos quando o ciclo fecha
         * (estado volta a ROOT) ou ao esgotar o orcamento. Teto deterministico 3ms. */
        /* [GC|ORCAMENTADO v2 2026-07-04] Os 3ms fixos corriam TAMBEM nos frames
         * LENTOS (roubavam 3ms a um frame ja' atrasado) e contavam no bucket
         * 'swap+sync' do DIAG-PERF. Agora: 1 passo aqui (mantem o ciclo vivo em
         * qualquer frame) e o GROSSO do dreno mudou para a FOLGA do pacing (fim
         * de grph_update) — nos frames rapidos o CPU ia DORMIR; gastamos essa
         * folga em GC com custo liquido ZERO no frame-time. */
        mrb_incremental_gc(mrb);

        /* ── [CHURN] proxy de alocacao/frame (sem recompilar mruby) ──────────────
         * Mede o crescimento liquido de 'live' por frame. O firehose real (~6500
         * objs/frame estimado pela taxa da valvula) vem de mrb_obj_alloc dentro da
         * VM (na lib pre-compilada), inacessivel sem rebuild; este proxy expoe a
         * tendencia para vermos se o GC orcamentado esta' a segurar o heap. */
        {
            static size_t s_prev_live = 0; static double s_acc = 0; static int s_n = 0;
            size_t live_now = (size_t)mrb->gc.live;
            long net = (long)live_now - (long)s_prev_live; s_prev_live = live_now;
            s_acc += (net > 0 ? net : 0); s_n++;
            if (s_n >= 120) {
                extern FILE *g_dbglog;
                /* [SUPERDBG] snapshot consolidado por ~120 frames: GC + memoria (linear
                 * livre, MAIOR bloco contiguo = fragmentacao, VRAM livre) + texturas vivas
                 * + sprites + objs Ruby. Tudo num so' sitio p/ correlacionar FPS<->memoria. */
                /* linearSpaceFree()/vramSpaceFree() ja' vem do 3ds.h (u32); NAO redeclarar. */
                extern unsigned linear_biggest_block(void);
                extern void     display_3ds_tex_stats(size_t*, size_t*, size_t*);
                size_t tcnt=0, tbytes=0, tvram=0;
                display_3ds_tex_stats(&tcnt, &tbytes, &tvram);
                char sb[400];
                snprintf(sb, sizeof(sb),
                    "[SUPERDBG] churn=%.0f obj/f live=%u | LINEAR livre=%.0fKB maior_bloco=%uKB"
                    " | VRAM livre=%.0fKB | TEX vivas=%u (%.0fKB, vram %uKB) | frame=%d\n",
                    s_acc / s_n, (unsigned)live_now,
                    linearSpaceFree()/1024.0, linear_biggest_block()/1024,
                    vramSpaceFree()/1024.0, (unsigned)tcnt, tbytes/1024.0, (unsigned)(tvram/1024),
                    s_frame_count);
                if (g_log_verbose && g_dbglog) { fprintf(g_dbglog, "%s", sb); dbg_flush(); }   /* [LOG-LEAN] */
                s_acc = 0; s_n = 0;
            }
        }

        u64 _dperf_t3 = osGetTime();   /* [DIAG-PERF] apos swap+sync */
        /* [DIAG-PERF] Repartir o "resto" (Graphics.update) que dava ~120ms na rua:
         * saber se o tempo esta' no tilemap (compose/upload), nos sprites (render)
         * ou no swap+sync da GPU. Media a cada 120 frames p/ nao floodar o SD.
         * Escreve direto no g_dbglog (sempre ligado). */
        if (g_dbglog) {
            static double s_acc_tm = 0, s_acc_spr = 0, s_acc_end = 0; static int s_acc_n = 0;
            s_acc_tm  += (double)(_dperf_t1 - _dperf_t0);
            s_acc_spr += (double)(_dperf_t2 - _dperf_t1);
            s_acc_end += (double)(_dperf_t3 - _dperf_t2);
            if (++s_acc_n >= 120) {
                if (g_log_verbose)
                    fprintf(g_dbglog, "[DIAG-PERF] media/120f: tilemaps=%.1fms sprites=%.1fms swap+sync=%.1fms\n",
                            s_acc_tm / s_acc_n, s_acc_spr / s_acc_n, s_acc_end / s_acc_n);
                s_acc_tm = s_acc_spr = s_acc_end = 0; s_acc_n = 0;
            }
        }

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
        if (g_log_verbose && s_watchdog_count % 600 == 0) {   /* [LOG-LEAN] era 60 */
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

    /* vsync + limitador por relogio (CORRIGIDO). O codigo antigo somava um
     * "residual" FIXO depois do vblank: quando a logica ja' passava de 1 vblank
     * (mapas pesados, >16.7ms), isso davam OVERSHOOT -- um frame de 25ms ficava
     * 50ms. Aqui so' dormimos se o frame foi MAIS RAPIDO que o alvo; frames
     * lentos passam direto (so' o vblank de vsync, como dantes). Isto mantem o
     * cap no alvo (ex: 30) para os mapas leves nao correrem a 60 (rapido demais
     * para a calibracao do jogo), sem penalizar os pesados. */
    /* ── PACING SEM PRECIPICIO (FIX do salto 30->20fps) ───────────────────────
     * O codigo anterior fazia gspWaitForVBlank() INCONDICIONAL. Como o ecra' do
     * 3DS atualiza a 60Hz (vblank cada 16.7ms) e o alvo sao 30fps (2 vblanks =
     * 33.3ms), um frame que demore so' um pouco MAIS que 33.3ms (ex: 35ms) perde
     * a janela e e' empurrado para o PROXIMO vblank (~50ms) -> 20fps. E' o
     * classico "vsync judder": basta passar 1ms do alvo para cair de 30 para 20.
     * Como o C3D_FrameEnd (display_3ds_end_frame) ja' enfileira o swap e o GSP
     * apresenta sincronizado com vblank, o gspWaitForVBlank extra so' servia para
     * ESPACAR frames RAPIDOS. Agora:
     *   - frame RAPIDO (abaixo do alvo): espera vblank + dorme ate' ao alvo
     *     (cap suave a 30, sem tearing, como antes);
     *   - frame LENTO (ja' passou do alvo): segue JA' para o proximo (o GSP
     *     alinha o swap no vblank na mesma) -> 28fps em vez de 20fps.
     * Reverter: substituir todo este bloco por um simples "gspWaitForVBlank();".
     * Flag para desligar em runtime: g_vsync_smooth (default true). */
    {
        /* [FIX-GCIDLE] ── DRENO de GC sobreposto a' espera do VBlank ──────────────
         * Estamos no fim de grph_update: o frame JA' foi entregue ao GPU (C3D_FrameEnd
         * mais acima) e o GSP vai apresenta-lo sincronizado com o proximo VBlank. Ate'
         * la', o CPU ia ficar PARADO (os ~8-10ms de 'swap+sync' do DIAG-PERF). Em vez
         * disso, fazemos AQUI a coleccao major (mrb_full_gc) -- corre enquanto o GPU
         * trabalha, por isso sobrepoe-se ao tempo morto em vez de roubar tempo ao frame
         * (era o hitch periodico). Como isto corre ANTES de medir 'now_us'/'elapsed'
         * (linhas abaixo), o pacing ja' conta o tempo do GC e nao acrescenta sleep.
         * Cooldown partilhado (s_gc_cooldown): no max 1 full_gc / 30 frames -> sem thrash.
         * Reverter: g_gc_idle_drain=false ou $__gc_idle_drain=false. */
        {
            bool _gci_on = g_gc_idle_drain;
            { mrb_value _v = mrb_gv_get(mrb, mrb_intern_cstr(mrb, "$__gc_idle_drain"));
              if (!mrb_nil_p(_v)) _gci_on = mrb_test(_v); }
            if (_gci_on && s_gc_cooldown == 0 && (size_t)mrb->gc.live > 600000) {
                size_t before = (size_t)mrb->gc.live;
                long long _g0 = (long long)osGetTime();
                u64 _gs_g0 = svcGetSystemTick();
                mrb_full_gc(mrb);
                s_gs_gc += (double)(svcGetSystemTick() - _gs_g0) / 268123.48;
                long long _g1 = (long long)osGetTime();
                s_gc_cooldown = 30;
                printf("[FIX-GCIDLE] full_gc no idle: vivos %u -> %u (recolheu %d, %lldms) "
                       "[frame %d] -- sobreposto a' espera do VBlank do GPU\n",
                       (unsigned)before, (unsigned)mrb->gc.live,
                       (int)((long)before - (long)mrb->gc.live),
                       (long long)(_g1 - _g0), s_frame_count);
            }
        }

        long long frame_us = (s_frame_rate > 0) ? (1000000LL / s_frame_rate) : 0;
        long long now_us   = (long long)osGetTime() * 1000LL;   /* osGetTime() = ms  */
        long long elapsed  = (g_last_present_us != 0) ? (now_us - g_last_present_us)
                                                      : frame_us;
        if (!g_vsync_smooth || frame_us == 0) {
            /* comportamento antigo (vsync incondicional) */
            gspWaitForVBlank();
            if (frame_us > 0) {
                now_us  = (long long)osGetTime() * 1000LL;
                elapsed = now_us - g_last_present_us;
                if (g_last_present_us != 0 && elapsed > 0 && elapsed < frame_us)
                    svcSleepThread((frame_us - elapsed) * 1000LL);
            }
        } else if (elapsed < frame_us) {
            /* [GC-SLACK 2026-07-04] frame rapido: gastar a folga em GC incremental
             * ANTES de dormir (custo liquido zero — este tempo ia ser sleep). O
             * codigo abaixo recalcula elapsed apos o vblank, por isso o sleep
             * encolhe automaticamente pelo tempo gasto aqui. Guarda de 1ms para
             * nunca passar do alvo por causa da resolucao de osGetTime(). */
            {
                long long _slack_ms = (frame_us - elapsed) / 1000;
                if (_slack_ms > 1) {
                    u64 _gc_dl = osGetTime() + (u64)(_slack_ms - 1);
                    u64 _gs_s0 = svcGetSystemTick();
                    do {
                        mrb_incremental_gc(mrb);
                    } while (mrb->gc.state != MRB_GC_STATE_ROOT && osGetTime() < _gc_dl);
                    s_gs_gc += (double)(svcGetSystemTick() - _gs_s0) / 268123.48;
                }
            }
            /* frame rapido: vsync + dorme o resto ate' ao alvo (cap a 30) */
            {
                u64 _gs_p0 = svcGetSystemTick();
                gspWaitForVBlank();
                now_us  = (long long)osGetTime() * 1000LL;
                elapsed = now_us - g_last_present_us;
                if (elapsed > 0 && elapsed < frame_us)
                    svcSleepThread((frame_us - elapsed) * 1000LL);
                s_gs_sleep += (double)(svcGetSystemTick() - _gs_p0) / 268123.48;
            }
        }
        /* frame lento: sem espera extra -> evita o salto 35ms->50ms */
    }
    g_last_present_us = (long long)osGetTime() * 1000LL;
    /* [TEX-PRESSURE] valvula preventiva (1x/120f; so' atua com linear < 6MB) */
    if ((s_frame_count % 120) == 0) {
        display_3ds_evict_pressure(s_frame_count);
    }
    {
        u64 _gs_t3 = svcGetSystemTick();
        s_gs_c += (double)(_gs_t3 - _gs_t0) / 268123.48;
        if (++s_gs_n >= 600) {
            extern FILE *g_dbglog;
            if (g_dbglog) {
                fprintf(g_dbglog, "[GFXSPLIT] media/600f: fixes_topo=%.2fms render=%.2fms cauda=%.2fms (da cauda: gc=%.2fms cap_sleep=%.2fms resto=%.2fms)\n",
                        s_gs_a / 600.0, s_gs_b / 600.0, s_gs_c / 600.0,
                        s_gs_gc / 600.0, s_gs_sleep / 600.0,
                        (s_gs_c - s_gs_gc - s_gs_sleep) / 600.0);
                fflush(g_dbglog);
            }
            s_gs_a = s_gs_b = s_gs_c = 0;
            s_gs_gc = s_gs_sleep = 0;
            s_gs_n = 0;
        }
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

    /* [FIX-INPUT-TRANSITION] Limpar os flancos de input PENDENTES nesta transicao de
     * cena: senao o press que abriu o menu (Y) "sangra" para a cena nova e ativa a
     * entrada Bag sozinha (e o menu amarelo nunca fecha com B). Generico p/ qualquer
     * jogo RMXP. Toggle de runtime (ON por omissao): $MKXP_INPUT_FLUSH_ON_TRANSITION. */
    {
        mrb_value _v = mrb_gv_get(mrb, mrb_intern_cstr(mrb, "$MKXP_INPUT_FLUSH_ON_TRANSITION"));
        if (mrb_nil_p(_v) || mrb_test(_v)) {
            extern void inputBindingFlushEdges();
            inputBindingFlushEdges();
        }
    }

    printf("[GFX|FREEZE] freeze #%d chamado no frame %d\n",
           s_freeze_count, s_frame_count);
    dbg_dump_scene(mrb, "freeze");
    printf("[GFX|FREEZE]   → ecrã ficará preto até Graphics.transition() ser chamado!\n");

    /* ── [FCRAM] full_gc determinístico na transição de cena ──────────────────
     * O freeze marca SEMPRE uma troca de cena em RMXP (o ecrã fica congelado a
     * seguir). Aproveitamos esse instante — em que o jogo não está a desenhar —
     * para correr um full GC: o "hitch" fica invisível (ecrã congelado) e a
     * cena seguinte, que vai alocar muitos bitmaps/objetos, encontra o heap
     * malloc limpo. Isto ataca diretamente o OOM observado ([POOL|GROW] malloc
     * FALHOU) que acontecia DURANTE diálogos longos logo após uma transição:
     * o lixo Ruby acumulado da cena anterior é recolhido antes do pico, em vez
     * de forçar o pool a crescer até esgotar a FCRAM. Genérico para qualquer
     * jogo RMXP. mrb_full_gc é sempre seguro. */
    {
        size_t vivos_antes = mrb->gc.live;
        mrb_full_gc(mrb);
        size_t vivos_depois = mrb->gc.live;
        printf("[GC|FREEZE] full_gc na transicao: vivos %u -> %u (recolheu %d)\n",
               (unsigned)vivos_antes, (unsigned)vivos_depois,
               (int)((long)vivos_antes - (long)vivos_depois));
    }
    return mrb_nil_value();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * grph_transition
 * [DBG] Confirma desbloqueio. Se nunca aparecer no log → bug de freeze eterno.
 * ═══════════════════════════════════════════════════════════════════════════ */
static mrb_value grph_transition(mrb_state *mrb, mrb_value self) {
    (void)self;
    s_transition_count++;
    /* [TRANS-FADE] aceitar os args RGSS: transition(duration=8, filename="", vague=40).
     * "*" apanha todos sem rebentar com aridades variadas dos scripts/plugins. */
    mrb_value *argv = nullptr; mrb_int argc = 0;
    mrb_get_args(mrb, "*", &argv, &argc);
    int dur = 8;   /* default RGSS quando chamado sem args */
    if (argc >= 1) {
        if (mrb_fixnum_p(argv[0]))     dur = (int)mrb_fixnum(argv[0]);
        else if (mrb_float_p(argv[0])) dur = (int)mrb_float(argv[0]);
    }
    printf("[GFX|TRANSITION] transition #%d chamado no frame %d "
           "(estava frozen=%d, skipped=%d frames, dur=%d)\n",
           s_transition_count, s_frame_count, s_frozen, s_skipped_frames, dur);
    dbg_dump_scene(mrb, "transition");
    if (!s_frozen) {
        printf("[GFX|TRANSITION]   AVISO: transition() chamado mas frozen já era false!\n");
    }
    s_frozen = false;
    s_skipped_frames = 0;
    /* [TRANS-FADE] armar o fade-in (preto -> cena nova) por dur frames. Clamp p/ nao
     * prender o ecra com valores absurdos; gate de runtime $MKXP_TRANS_FADE=false. */
    {
        mrb_value gv = mrb_gv_get(mrb, mrb_intern_lit(mrb, "$MKXP_TRANS_FADE"));
        bool fade_on = !(!mrb_nil_p(gv) && !mrb_test(gv));   /* so' false explicito desliga */
        if (fade_on && dur > 0) {
            if (dur > 45) dur = 45;
            s_fade_total = s_fade_left = dur;
            extern FILE *g_dbglog;
            if (g_dbglog) { fprintf(g_dbglog, "[TRANS-FADE] fade-in armado: %d frames (transition #%d)\n", dur, s_transition_count); dbg_flush(); }
        } else {
            s_fade_total = s_fade_left = 0;
        }
    }
    return mrb_nil_value();
}

static mrb_value grph_frame_rate_get(mrb_state *mrb, mrb_value self) {
    (void)self; return mrb_int_value(mrb, s_frame_rate);
}
static mrb_value grph_frame_rate_set(mrb_state *mrb, mrb_value self) {
    (void)self; mrb_int v; mrb_get_args(mrb, "i", &v);
    int old = s_frame_rate;
    int req = (int)v;
    if (req < 1) req = 1;
    if (req > MKXP_FPS_CAP) req = MKXP_FPS_CAP;   /* CLAMP: 60 pedido -> 30 efetivo */
    s_frame_rate = req;
    printf("[GFX] frame_rate pedido=%d -> efetivo=%d (cap=%d) [old=%d]\n",
           (int)v, s_frame_rate, MKXP_FPS_CAP, old);
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
        /* FIX INPUT: idem grph_update -- inputBindingUpdate() ja faz o scan. */
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