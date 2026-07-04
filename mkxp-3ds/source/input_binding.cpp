#include <mruby.h>
#include <mruby/class.h>
#include <mruby/array.h>
#include "input_3ds.h"
#include "debug_3ds.h"

static bool s_pressed[RMXP_KEY_COUNT];
static bool s_triggered[RMXP_KEY_COUNT];
static bool s_released[RMXP_KEY_COUNT];

/* LATCH de input:
 * O grph_update (C++) chama inputBindingUpdate() MUITAS vezes por cada
 * scene.update logico do jogo (ex: na tela de titulo, ~146 grph_update por
 * cada EventScene#update). Se s_triggered fosse so sobrescrito a cada chamada,
 * um toque no botao A (Input::USE) registado num frame intermedio seria
 * apagado antes de o jogo chamar Input.trigger?, perdendo-se.
 *
 * Solucao: acumular ("latch") o trigger/release num buffer que SO e limpo
 * quando o Ruby o le (em inp_trigger / inp_release). Assim qualquer toque
 * entre duas leituras do jogo fica guardado ate ser consumido. */
static bool s_trigger_latch[RMXP_KEY_COUNT];
static bool s_release_latch[RMXP_KEY_COUNT];

/* FIX menu/Outfit nao fecham/avancam com B: o latch de trigger era
 * SINGLE-CONSUME (inp_trigger lia E limpava). Com o mapa POR BAIXO do menu a
 * ler trigger?(B) primeiro, o flanco era DRENADO antes de o menu/picker o ver
 * (por isso o cartao fechava -- sem mapa por baixo -- mas o menu de pausa e o
 * picker de Outfit nao). Como inputBindingUpdate corre UMA vez por frame
 * (sincronizado com grph_update; Input.update Ruby e' no-op), nao precisamos de
 * acumular: repomos a semantica RGSS NORMAL -- o latch reflecte o flanco DESTE
 * frame (limpo a cada frame) e a leitura NAO consome, logo VARIOS leitores no
 * mesmo frame veem o flanco (standard RGSS, que e' como Essentials foi escrito).
 *   true  = novo comportamento (multi-leitor, corrige menus);
 *   false = antigo (single-consume).
 * REVERTER: por false aqui. */
static bool g_trigger_multiread = true;

/* Contador de frames que cada tecla esta' em baixo, para o Input.repeat? real
 * do RGSS. Sem isto, repeat? = press? (true em TODOS os frames) -> 1 toque
 * fisico (que dura ~4 frames) movia o cursor 4 vezes. */
static int  s_press_frames[RMXP_KEY_COUNT];
/* Lógica RGSS: repeat? e' true no 1o frame, depois espera REPEAT_DELAY, depois
 * repete a cada REPEAT_INTERVAL frames. (Valores classicos do RGSS @60fps.) */
#define RMXP_REPEAT_DELAY    15
#define RMXP_REPEAT_INTERVAL  4

/* ============================================================================
 * [FIX-INPUT-RGSS]  Modelo de input com semantica RGSS correcta (held-state).
 * ----------------------------------------------------------------------------
 * PROBLEMA (auto-Bag + B nao fecha o menu): o modelo de flanco anterior tinha
 * de escolher entre (a) consumir o flanco ao ler -> o mapa por baixo do menu
 * drenava o B antes do menu (B nao fechava), ou (b) multi-read -> o C que ABRIU
 * o menu persistia para dentro dele e confirmava a 1a opcao (Bag). Alem disso,
 * Input.update (Ruby) era no-op, por isso o ciclo do menu nao conseguia "limpar"
 * o flanco como no RGSS.
 *
 * SOLUCAO (standard RGSS, robusta e generica para QUALQUER jogo):
 *  - grph_update faz poll do ESTADO MANTIDO (nivel, hidKeysHeld) -> s_held[].
 *    Imune as ~146 chamadas/frame (o nivel nao se gasta como o flanco).
 *  - Input.update (logica do frame) CALCULA os flancos:
 *        trigger = (foi premido neste frame logico) && (nao estava mantido no
 *                   frame logico anterior)
 *        release = (nao esta mantido agora) && (estava mantido antes)
 *    => trigger?(C) e' true SO' no 1o Input.update apos a pressao (Scene_Map
 *       abre o menu); no ciclo do menu o C ja' esta' "mantido" -> flanco falso ->
 *       NAO confirma sozinho. O B continua multi-read -> fecha. Resolve os dois.
 *  - A leitura (trigger?/release?) NAO consome -> varios leitores no mesmo frame
 *    veem o flanco (como o Essentials espera).
 *
 * Isolamento: g_input_rgss_model liga/desliga. Se este fix causar regressao no
 * input, poe FALSE aqui -> volta exactamente ao modelo de flanco/latch anterior.
 *   true  = modelo RGSS held-state calculado em GRPH_UPDATE (NOVO v2, default);
 *   false = modelo de flanco/latch (antigo). REVERTER se algo no input regredir.
 *
 * v1 (FALHOU): calculava os flancos em Input.update -> [DIAG-INPUT]=0 PROVOU que
 * o Essentials/EBDX intercetam Input.update e nunca chamam o nativo -> input morto.
 * v2 (este): calcula os flancos no grph_update (que corre SEMPRE, por isso o input
 * funciona), e usa a LEITURA de input do jogo (inp_trigger/press/dir4 nativos, que
 * SAO chamados -- e' por isso que o input antigo funciona) como sinal de fronteira
 * de frame logico: quando o jogo leu input, o proximo grph_update avanca a linha de
 * base do flanco. Assim trigger?(C) e' true so' no 1o frame logico apos a pressao
 * (abre o menu) e no frame seguinte do menu o C ja' esta' "mantido" -> flanco falso
 * -> NAO auto-confirma a Bag; o B continua multi-read -> fecha. O acumulador
 * (s_held_any) apanha toques curtos durante cutscenes (146 grph_update/frame). */
static bool g_input_rgss_model = true;
static bool s_input_was_read = false;   /* o jogo leu input desde o ultimo grph_update? */

static bool s_held[RMXP_KEY_COUNT];          /* mantido AGORA (ultimo grph_update) */
static bool s_held_any[RMXP_KEY_COUNT];      /* mantido em ALGUM grph_update desde o ultimo Input.update (apanha toques curtos) */
static bool s_was_held[RMXP_KEY_COUNT];      /* mantido no Input.update ANTERIOR (linha de base do flanco) */
static bool s_triggered_rgss[RMXP_KEY_COUNT];/* flanco de pressao calculado em Input.update */
static bool s_released_rgss[RMXP_KEY_COUNT]; /* flanco de soltura calculado em Input.update */
static bool s_rgss_logged = false;

/* [FIX-INPUT-TRANSITION gap] Deteta transicoes SEM freeze nem Graphics.transition --
 * ex.: o slide-in PROPRIO do menu Voltseon (~6 frames de Graphics.update sem ler
 * input) e qualquer fade manual. Uma tecla que ja' estava em baixo quando a leitura
 * de input PAROU (inicio do gap) = o press que iniciou a transicao (o A=USE que abriu
 * o menu) -> NAO deve gerar flanco na cena nova -> mata a auto-Bag. Teclas premidas
 * DURANTE o gap (toque para avancar uma cutscene) mantem o flanco -> nao quebra isso.
 * Generico p/ qualquer jogo RPG Maker XP. Toggle: $MKXP_INPUT_FLUSH_ON_TRANSITION. */
bool        g_input_flush_on = true;          /* posto por grph_update a partir do global Ruby */
static int  s_noread_run = 0;                 /* grph_updates consecutivos SEM leitura de input */
static bool s_held_at_gap[RMXP_KEY_COUNT];    /* snapshot do held no inicio do gap */
static bool s_edge_consumed[RMXP_KEY_COUNT];  /* flanco de pressao ja' LIDO pelo jogo (consumido) */
static bool s_consumed_at_gap[RMXP_KEY_COUNT];/* snapshot de s_edge_consumed no inicio do gap */
static bool s_gapflush_logged = false;

/* Chamado UMA vez por frame, exclusivamente por grph_update (C++).
 * Input.update (Ruby) e um no-op para evitar double-poll. */
void inputBindingUpdate() {
    if (g_input_rgss_model) {
        /* [FIX-INPUT-RGSS v2] Flancos calculados AQUI (grph_update corre sempre,
         * ao contrario de Input.update que esta' intercetado -> [DIAG-INPUT]=0).
         * Fronteira de frame logico = LEITURA de input do jogo (s_input_was_read,
         * posto em inp_trigger/press/repeat/release/dir). Quando o jogo leu, o
         * proximo grph_update avanca a linha de base (s_was_held) ANTES de computar
         * os novos flancos -> trigger?(C) e' true so' no 1o frame apos a pressao. */
        if (s_input_was_read) {
            bool was_gap = (g_input_flush_on && s_noread_run >= 2);
            for (int i = 0; i < RMXP_KEY_COUNT; i++) {
                s_was_held[i] = s_held[i];   /* base = estado do frame logico anterior */
                s_held_any[i] = s_held[i];   /* repor acumulador */
                /* [FIX-INPUT-TRANSITION gap] Se houve transicao sem leitura, suprime o
                 * flanco de uma tecla que (a) ja' estava em baixo no inicio do gap = o
                 * press que abriu a cena (segurado), OU (b) ja' tinha o flanco CONSUMIDO
                 * antes do gap = o press que abriu a cena e foi largado durante o
                 * slide-in. Em qualquer caso e' o mesmo toque a sangrar -> nao gera
                 * novo flanco. Toques NASCIDOS dentro do gap (avancar cutscene) nao
                 * caem em (a) nem (b) -> mantem o flanco. */
                if (was_gap && (s_held_at_gap[i] || s_consumed_at_gap[i])) {
                    s_was_held[i] = true;        /* !was_held = false -> sem flanco de pressao */
                    s_held_any[i] = s_held[i];   /* descarta acumulacao do press de abertura */
                }
            }
            if (was_gap && !s_gapflush_logged && g_dbglog) {
                fprintf(g_dbglog, "[FIX-INPUT-TRANSITION] gap-flush ATIVO: press de abertura (segurado OU consumido+largado) nao sangra p/ cena nova\n");
                dbg_flush(); s_gapflush_logged = true;
            }
            s_noread_run = 0;
            s_input_was_read = false;
        } else {
            /* frame SEM leitura (fade/slide-in/animacao): conta o gap. No 1o frame do
             * gap, fotografa held atual E os flancos ja' consumidos = estado do toque
             * que iniciou a transicao. */
            if (s_noread_run == 0) {
                for (int i = 0; i < RMXP_KEY_COUNT; i++) {
                    s_held_at_gap[i]   = s_held[i];
                    s_consumed_at_gap[i] = s_edge_consumed[i];
                }
            }
            s_noread_run++;
        }
        bool held[RMXP_KEY_COUNT] = {};
        input_3ds_poll_held(held, RMXP_KEY_COUNT);
        for (int i = 0; i < RMXP_KEY_COUNT; i++) {
            bool prev = s_held[i];                       /* held no grph_update ANTERIOR */
            s_held[i] = held[i];
            bool rising = held[i] && !prev;              /* flanco de SUBIDA fisico neste frame */
            /* [FIX-INPUT-RGSS v2] Acumula o flanco SO' na subida (nao durante o hold).
             * Isto preserva os toques curtos em cutscenes (o flanco fica pendente em
             * s_held_any ate' ser lido) SEM o defeito antigo de a acumulacao continua
             * manter o flanco vivo e o re-disparar na cena seguinte. */
            if (rising) s_held_any[i] = true;
            /* [FIX-INPUT-TRANSITION] Quando um flanco JA' foi lido (consumido) por uma
             * cena, limpa a acumulacao no grph_update seguinte -> o MESMO toque nao pode
             * ser re-lido por uma cena nova (mata a auto-Bag / @bg=1 do roupeiro / commit
             * a 0). A leitura multipla no MESMO frame continua a funcionar porque
             * s_triggered_rgss so' e' recalculado aqui, nao entre leituras. */
            if (s_edge_consumed[i]) { s_held_any[i] = false; s_edge_consumed[i] = false; }
            s_triggered_rgss[i] = s_held_any[i] && !s_was_held[i];
            s_released_rgss[i]  = !held[i] && s_was_held[i];
            s_pressed[i] = held[i];
            if (held[i]) s_press_frames[i]++; else s_press_frames[i] = 0;
        }
        if (!s_rgss_logged && g_dbglog) {
            fprintf(g_dbglog, "[FIX-INPUT-RGSS] v2.1 ATIVO (rising-edge + consume-clear): flanco acumula so' na subida e limpa quando consumido -> nao sangra entre cenas\n");
            dbg_flush(); s_rgss_logged = true;
        }
        return;
    }
    /* ---- CAMINHO ANTIGO (flanco/latch); preservado para revert ---- */
    bool new_triggered[RMXP_KEY_COUNT] = {};
    bool new_released[RMXP_KEY_COUNT]  = {};

    input_3ds_poll(new_triggered, new_released, RMXP_KEY_COUNT);

    for (int i = 0; i < RMXP_KEY_COUNT; i++) {
        s_triggered[i] = new_triggered[i];
        s_released[i]  = new_released[i];
        if (g_trigger_multiread) {
            /* latch = flanco DESTE frame (limpo automaticamente a cada frame ->
             * leitura nao-consumidora; varios leitores veem-no). */
            s_trigger_latch[i] = new_triggered[i];
            s_release_latch[i] = new_released[i];
        } else {
            /* antigo: acumula no latch -- uma vez true, fica true ate o Ruby ler */
            if (new_triggered[i]) s_trigger_latch[i] = true;
            if (new_released[i])  s_release_latch[i]  = true;
        }
        if (new_triggered[i]) s_pressed[i] = true;
        if (new_released[i])  s_pressed[i] = false;
        /* contar frames em baixo (para o repeat? real): incrementa enquanto
         * pressionado, zera quando solto. */
        if (s_pressed[i]) s_press_frames[i]++;
        else              s_press_frames[i] = 0;
        /* Super Debug: registar QUALQUER tecla fisica detectada. Mostra no log
         * se o input do 3DS/Azahar esta a chegar ao binding. Se carregas em A
         * e NAO aparece "tecla detectada k=13", o problema e' a montante
         * (input_3ds_poll / mapeamento / foco da janela). */
        if (new_triggered[i] && DBGV(DBG_INPUT)) {
            DBG(DBG_INPUT, "tecla detectada k=%d (latch ON)", i);
        }
    }
}

/* [FIX-INPUT-TRANSITION] Limpa os flancos de input no inicio de uma transicao de
 * cena (chamado por grph_freeze). RAIZ do bug "menu amarelo entra sozinho na Bag e
 * nao fecha com B": a tecla que INICIA a transicao (o Y que abre o menu de pausa) ou
 * premida durante o fade fica no acumulador (s_held_any) e, quando a cena NOVA le
 * input pela 1a vez, conta como um FLANCO NOVO -> a entrada por omissao (Bag) ativa-se
 * sozinha. Ao avancar a linha-de-base p/ o estado ATUAL e limpar o acumulador, a cena
 * nova comeca SEM flancos pendentes: e' preciso soltar e voltar a premir p/ interagir
 * (semantica RGSS correcta). Generico p/ qualquer jogo RMXP. Sem custo (corre 1x por
 * transicao). Reversivel: $MKXP_INPUT_FLUSH_ON_TRANSITION=false (gate em grph_freeze). */
static bool s_flush_logged = false;
void inputBindingFlushEdges() {
    for (int i = 0; i < RMXP_KEY_COUNT; i++) {
        s_was_held[i]       = s_held[i];   /* base = held atual -> held != flanco novo */
        s_held_any[i]       = s_held[i];   /* descarta toques acumulados na transicao */
        s_triggered_rgss[i] = false;       /* nenhum flanco de pressao pendente (v2)   */
        s_released_rgss[i]  = false;
        s_trigger_latch[i]  = false;       /* idem no caminho antigo (latch)           */
        s_release_latch[i]  = false;
    }
    s_input_was_read = false;
    if (!s_flush_logged && g_dbglog) {
        fprintf(g_dbglog, "[FIX-INPUT-TRANSITION] flush de flancos na transicao de cena ATIVO "
                          "(anti auto-Bag / menu nao fecha; $MKXP_INPUT_FLUSH_ON_TRANSITION=false reverte)\n");
        dbg_flush(); s_flush_logged = true;
    }
}

/* Input.update chamado pelo Ruby PE -- NO-OP intencional.
 * O poll real ja foi feito em grph_update antes de scene.update. */
static mrb_value inp_update(mrb_state *mrb, mrb_value self) {
    (void)mrb; (void)self;
    /* [DIAG-INPUT] Confirmar se o Input.update do JOGO alcanca esta funcao
     * nativa. Se um plugin (Essentials/EBDX) redefinir Input.update em Ruby sem
     * chamar a nativa, esta linha NUNCA aparece -> e' a razao de o modelo
     * held-state (que calcula os flancos AQUI) ter deixado o input morto.
     * Corre INDEPENDENTE da flag (tambem com o modelo antigo), logo nao parte
     * nada -- so' diz se podemos contar com o Input.update Ruby. */
    static unsigned long s_upd_calls = 0;
    s_upd_calls++;
    /* Escrita DIRECTA no ficheiro de log (g_dbglog), que esta' SEMPRE ligado --
     * ao contrario de DBG(DBG_INPUT) que depende do canal estar ativo (e nao
     * estava, por isso o teste anterior foi inconclusivo). Assim a presenca/
     * ausencia desta linha e' DEFINITIVA. */
    if (g_dbglog && (s_upd_calls == 1 || (s_upd_calls % 600) == 0)) {
        fprintf(g_dbglog, "[DIAG-INPUT] Input.update NATIVO alcancado (chamada #%lu) -- da' para contar com ele\n", s_upd_calls);
        dbg_flush();
    }

    /* v2: os flancos sao calculados em grph_update (inputBindingUpdate), NAO aqui
     * -- porque Input.update esta' intercetado e esta funcao quase nunca corre
     * ([DIAG-INPUT]=0). Aqui fica no-op (so' o [DIAG-INPUT] acima, p/ registo). */
    return mrb_nil_value();
}
static mrb_value inp_press(mrb_state *mrb, mrb_value self) {
    (void)self; mrb_int k; mrb_get_args(mrb, "i", &k);
    if (g_input_rgss_model) s_input_was_read = true;   /* fronteira de frame logico (v2) */
    return (k>=0&&k<RMXP_KEY_COUNT&&s_pressed[k]) ? mrb_true_value() : mrb_false_value();
}
/* [MENUDIAG] Bug 4 (fantasma auto-Bag, NAO CONFIRMADO): frame ate' ao qual o
 * inp_trigger loga TODA a leitura de trigger?(ACTION/BACK/USE). Armado por
 * Input.begin_menu_diag(n), chamado a' entrada de call_menu (wrapper em
 * binding_3ds.cpp). -1 = desarmado. */
static int g_menu_diag_until = -1;

static mrb_value inp_trigger(mrb_state *mrb, mrb_value self) {
    (void)self; mrb_int k; mrb_get_args(mrb, "i", &k);
    if (g_input_rgss_model) {
        s_input_was_read = true;   /* sinal de fronteira de frame logico (v2) */
        /* [FIX-INPUT-RGSS] flanco calculado em grph_update; leitura NAO consome. */
        bool t = (k>=0 && k<RMXP_KEY_COUNT && s_triggered_rgss[k]);
        /* [MENUDIAG] Bug 4: durante a janela apos call_menu abrir, loga CADA leitura
         * de trigger?(ACTION=11/BACK=12/USE=13) na FONTE nativa, com resultado e flanco.
         * Prova 100% se um flanco fantasma (held_any stale) e' lido a' abertura do menu,
         * ou se a Bag abre por outro caminho (nenhum trigger?(USE)=1 aqui). Impossivel de
         * contornar por wrappers Ruby (e' o proprio inp_trigger). */
        if (g_menu_diag_until >= 0 && g_dbg_frame <= g_menu_diag_until &&
            (k == 11 || k == 12 || k == 13) && g_dbglog) {
            fprintf(g_dbglog, "[MENUDIAG] f=%d trigger?(%s)=%d press=%d held=%d was_held=%d held_any=%d\n",
                    g_dbg_frame, (k == 11 ? "ACTION" : k == 12 ? "BACK" : "USE"), t ? 1 : 0,
                    s_pressed[k] ? 1 : 0, s_held[k] ? 1 : 0, s_was_held[k] ? 1 : 0, s_held_any[k] ? 1 : 0);
            dbg_flush();
        }
        /* [USE-TRIG] loga trigger?(USE=13)=true com o estado de input.
         * press=0 -> FANTASMA (edge sem o botao em baixo = held_any stale -> auto-Bag do menu);
         * press=1 -> premido de facto. Revela a origem exata da auto-Bag.
         * ORCAMENTO POR JANELA (15 por 600 frames): o cap global de 40 esgotava-se no
         * inicio do jogo e NUNCA chegava a abertura do menu (t~340s = milhares de frames
         * depois). Com janela, ha' sempre orcamento fresco quando o menu abre. */
        if (t && k == 13 && g_dbglog) {
            static int __ut = 0; static int __ut_win = -1;
            int win = g_dbg_frame / 600;
            if (win != __ut_win) { __ut_win = win; __ut = 0; }
            if (__ut++ < 15) {
                fprintf(g_dbglog, "[USE-TRIG] f=%d trigger?(USE)=1 press=%d held=%d was_held=%d held_any=%d\n",
                        g_dbg_frame, s_pressed[13]?1:0, s_held[13]?1:0, s_was_held[13]?1:0, s_held_any[13]?1:0);
                dbg_flush();
            }
        }
        if (t) s_edge_consumed[k] = true;   /* flanco lido -> nao deve sobreviver a transicao (gap-flush) */
        return t ? mrb_true_value() : mrb_false_value();
    }
    if (k>=0 && k<RMXP_KEY_COUNT && s_trigger_latch[k]) {
        if (!g_trigger_multiread) s_trigger_latch[k] = false;   /* antigo: consome ao ler */
        DBG(DBG_INPUT, "trigger? k=%d -> TRUE", (int)k);
        return mrb_true_value();
    }
    /* DIAGNOSTICO: o log por-frame de "trigger? -> false" disparava 2x por frame
     * SEMPRE (mesmo sem input), gerando dezenas de milhares de escritas ao SD e
     * matando a performance (1 FPS). O input ja' esta' confirmado a funcionar,
     * por isso o caso "false" e' silenciado. Os eventos UTEIS continuam logados:
     * "tecla detectada" (latch ON, em inputBindingUpdate) e o ramo TRUE acima
     * ("latch consumido"). Para re-ativar o trace completo em depuracao futura,
     * liga DBG_POS junto de DBG_INPUT (gate abaixo). */
    if (DBGV(DBG_INPUT) && DBGV(DBG_POS) &&
        (k < 0 || k > RMXP_KEY_COUNT || k == 13 || k == 12 || k == 11)) {
        DBG(DBG_INPUT, "trigger? k=%d -> false (latch off ou fora de limites)", (int)k);
    }
    return mrb_false_value();
}
static mrb_value inp_repeat(mrb_state *mrb, mrb_value self) {
    (void)self; mrb_int k; mrb_get_args(mrb, "i", &k);
    if (g_input_rgss_model) s_input_was_read = true;   /* fronteira de frame logico (v2) */
    if (k<0 || k>=RMXP_KEY_COUNT || !s_pressed[k]) return mrb_false_value();
    /* Logica de repeticao do RGSS (em vez de devolver true em TODOS os frames):
     *  - true no 1o frame em que a tecla foi pressionada (toque simples = 1 vez)
     *  - depois fica em silencio durante RMXP_REPEAT_DELAY frames
     *  - so' entao comeca a repetir, a cada RMXP_REPEAT_INTERVAL frames
     * Assim 1 toque fisico (~4 frames) move o cursor SO' 1 vez, e segurar a
     * tecla faz scroll continuo (como num menu normal). */
    int f = s_press_frames[k];
    if (f <= 1) return mrb_true_value();                  /* primeiro frame */
    if (f > RMXP_REPEAT_DELAY &&
        ((f - RMXP_REPEAT_DELAY) % RMXP_REPEAT_INTERVAL) == 0)
        return mrb_true_value();                            /* repeticoes apos o atraso */
    return mrb_false_value();
}
static mrb_value inp_release(mrb_state *mrb, mrb_value self) {
    (void)self; mrb_int k; mrb_get_args(mrb, "i", &k);
    if (g_input_rgss_model) {
        s_input_was_read = true;   /* sinal de fronteira de frame logico (v2) */
        return (k>=0 && k<RMXP_KEY_COUNT && s_released_rgss[k]) ? mrb_true_value() : mrb_false_value();
    }
    if (k>=0 && k<RMXP_KEY_COUNT && s_release_latch[k]) {
        if (!g_trigger_multiread) s_release_latch[k] = false;   /* antigo: consome ao ler */
        return mrb_true_value();
    }
    return mrb_false_value();
}
static mrb_value inp_dir4(mrb_state *mrb, mrb_value self) {
    (void)self;
    if (g_input_rgss_model) s_input_was_read = true;   /* fronteira de frame logico (v2) */
    if (s_pressed[RMXP_UP])    return mrb_int_value(mrb, 8);
    if (s_pressed[RMXP_DOWN])  return mrb_int_value(mrb, 2);
    if (s_pressed[RMXP_LEFT])  return mrb_int_value(mrb, 4);
    if (s_pressed[RMXP_RIGHT]) return mrb_int_value(mrb, 6);
    return mrb_int_value(mrb, 0);
}
static mrb_value inp_dir8(mrb_state *mrb, mrb_value self) {
    return inp_dir4(mrb, self);
}

/* [MENU-FLUSH] Exposto a Ruby: limpa os flancos de input pendentes (mesmo motor do
 * flush de grph_freeze). Chamado a' ENTRADA de Scene_Map#call_menu (wrapper injetado
 * pos-scripts em binding_3ds.cpp) para matar o FANTASMA held_any que abria a Bag
 * sozinha. Raiz PROVADA no log: o menu Voltseon abre in-place (sem grph_freeze -> o
 * flush automatico nao corria) e a 1a leitura de trigger?(USE) apanhava um held_any
 * stale de um toque anterior -> [USE-TRIG] f=5517 press=0 held=0 was_held=0 held_any=1
 * -> seleciona a Bag -> loop de reabertura -> "menu congela". Generico p/ qualquer
 * menu RMXP que abra sem transicao de graficos. */
static mrb_value inp_flush_edges(mrb_state *mrb, mrb_value self) {
    (void)mrb; (void)self;
    inputBindingFlushEdges();
    return mrb_nil_value();
}

/* [MENUDIAG] Bug 4: arma a janela de log do inp_trigger por N frames e imprime JA' o
 * estado de flanco a' ENTRADA do menu (ACTION/BACK/USE). Chamado pelo wrapper de
 * call_menu (binding_3ds.cpp) ANTES de qualquer flush -> captura o input cru na
 * abertura, para confirmar/refutar o flanco fantasma. */
static mrb_value inp_begin_menu_diag(mrb_state *mrb, mrb_value self) {
    (void)self; mrb_int n; mrb_get_args(mrb, "i", &n);
    g_menu_diag_until = g_dbg_frame + (int)n;
    if (g_dbglog) {
        fprintf(g_dbglog, "[MENUDIAG] === janela ARMADA f=%d ate=%d === entrada do menu: "
                "ACTION press=%d held=%d was_held=%d held_any=%d | BACK press=%d held_any=%d | USE press=%d held=%d was_held=%d held_any=%d\n",
                g_dbg_frame, g_menu_diag_until,
                s_pressed[11] ? 1 : 0, s_held[11] ? 1 : 0, s_was_held[11] ? 1 : 0, s_held_any[11] ? 1 : 0,
                s_pressed[12] ? 1 : 0, s_held_any[12] ? 1 : 0,
                s_pressed[13] ? 1 : 0, s_held[13] ? 1 : 0, s_was_held[13] ? 1 : 0, s_held_any[13] ? 1 : 0);
        dbg_flush();
    }
    return mrb_nil_value();
}

void inputBindingInit(mrb_state *mrb) {
    RClass *mod = mrb_define_module(mrb, "Input");
    mrb_define_module_function(mrb, mod, "flush_edges", inp_flush_edges, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, mod, "begin_menu_diag", inp_begin_menu_diag, MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, mod, "update",   inp_update,  MRB_ARGS_NONE());
    mrb_define_module_function(mrb, mod, "press?",   inp_press,   MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, mod, "trigger?", inp_trigger, MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, mod, "repeat?",  inp_repeat,  MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, mod, "release?", inp_release, MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, mod, "dir4",     inp_dir4,    MRB_ARGS_NONE());
    mrb_define_module_function(mrb, mod, "dir8",     inp_dir8,    MRB_ARGS_NONE());

    /* ALIASES NATIVOS: os mesmos metodos C++ sob nomes que NENHUM plugin toca.
     * O Essentials/EBDX reabrem `module Input` e fazem
     *   alias :_old_fl_trigger? :trigger?  +  def trigger?(button) ...wrapper...
     * Se nesse instante o `trigger?` ja' era uma versao Ruby (KGC, etc.), o alias
     * captura ESSA, nao o nosso C++ -> o nosso latch nunca e' lido. Com nomes
     * proprios (trigger_native? etc.), garantimos um caminho direto ao C++ que
     * podemos repor por cima de qualquer wrapper depois dos scripts carregarem. */
    mrb_define_module_function(mrb, mod, "trigger_native?", inp_trigger, MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, mod, "press_native?",   inp_press,   MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, mod, "repeat_native?",  inp_repeat,  MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, mod, "release_native?", inp_release, MRB_ARGS_REQ(1));

    mrb_define_const(mrb, mod, "DOWN",  mrb_int_value(mrb, RMXP_DOWN));
    mrb_define_const(mrb, mod, "LEFT",  mrb_int_value(mrb, RMXP_LEFT));
    mrb_define_const(mrb, mod, "RIGHT", mrb_int_value(mrb, RMXP_RIGHT));
    mrb_define_const(mrb, mod, "UP",    mrb_int_value(mrb, RMXP_UP));
    mrb_define_const(mrb, mod, "A",     mrb_int_value(mrb, RMXP_A));
    mrb_define_const(mrb, mod, "B",     mrb_int_value(mrb, RMXP_B));
    mrb_define_const(mrb, mod, "C",     mrb_int_value(mrb, RMXP_C));
    mrb_define_const(mrb, mod, "X",     mrb_int_value(mrb, RMXP_X));
    mrb_define_const(mrb, mod, "Y",     mrb_int_value(mrb, RMXP_Y));
    mrb_define_const(mrb, mod, "Z",     mrb_int_value(mrb, RMXP_Z));
    mrb_define_const(mrb, mod, "L",     mrb_int_value(mrb, RMXP_L));
    mrb_define_const(mrb, mod, "R",     mrb_int_value(mrb, RMXP_R));
    mrb_define_const(mrb, mod, "SHIFT", mrb_int_value(mrb, RMXP_SHIFT));
    mrb_define_const(mrb, mod, "F5",    mrb_int_value(mrb, RMXP_F5));
}
