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

/* Contador de frames que cada tecla esta' em baixo, para o Input.repeat? real
 * do RGSS. Sem isto, repeat? = press? (true em TODOS os frames) -> 1 toque
 * fisico (que dura ~4 frames) movia o cursor 4 vezes. */
static int  s_press_frames[RMXP_KEY_COUNT];
/* Lógica RGSS: repeat? e' true no 1o frame, depois espera REPEAT_DELAY, depois
 * repete a cada REPEAT_INTERVAL frames. (Valores classicos do RGSS @60fps.) */
#define RMXP_REPEAT_DELAY    15
#define RMXP_REPEAT_INTERVAL  4

/* Chamado UMA vez por frame, exclusivamente por grph_update (C++).
 * Input.update (Ruby) e um no-op para evitar double-poll. */
void inputBindingUpdate() {
    bool new_triggered[RMXP_KEY_COUNT] = {};
    bool new_released[RMXP_KEY_COUNT]  = {};

    input_3ds_poll(new_triggered, new_released, RMXP_KEY_COUNT);

    for (int i = 0; i < RMXP_KEY_COUNT; i++) {
        s_triggered[i] = new_triggered[i];
        s_released[i]  = new_released[i];
        /* acumula no latch: uma vez true, fica true ate o Ruby ler */
        if (new_triggered[i]) s_trigger_latch[i] = true;
        if (new_released[i])  s_release_latch[i]  = true;
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

/* Input.update chamado pelo Ruby PE -- NO-OP intencional.
 * O poll real ja foi feito em grph_update antes de scene.update. */
static mrb_value inp_update(mrb_state *mrb, mrb_value self) {
    (void)mrb; (void)self; return mrb_nil_value();
}
static mrb_value inp_press(mrb_state *mrb, mrb_value self) {
    (void)self; mrb_int k; mrb_get_args(mrb, "i", &k);
    return (k>=0&&k<RMXP_KEY_COUNT&&s_pressed[k]) ? mrb_true_value() : mrb_false_value();
}
static mrb_value inp_trigger(mrb_state *mrb, mrb_value self) {
    (void)self; mrb_int k; mrb_get_args(mrb, "i", &k);
    if (k>=0 && k<RMXP_KEY_COUNT && s_trigger_latch[k]) {
        s_trigger_latch[k] = false;   /* consome o latch ao ler */
        DBG(DBG_INPUT, "trigger? k=%d -> TRUE (latch consumido)", (int)k);
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
    if (k>=0 && k<RMXP_KEY_COUNT && s_release_latch[k]) {
        s_release_latch[k] = false;   /* consome o latch ao ler */
        return mrb_true_value();
    }
    return mrb_false_value();
}
static mrb_value inp_dir4(mrb_state *mrb, mrb_value self) {
    (void)self;
    if (s_pressed[RMXP_UP])    return mrb_int_value(mrb, 8);
    if (s_pressed[RMXP_DOWN])  return mrb_int_value(mrb, 2);
    if (s_pressed[RMXP_LEFT])  return mrb_int_value(mrb, 4);
    if (s_pressed[RMXP_RIGHT]) return mrb_int_value(mrb, 6);
    return mrb_int_value(mrb, 0);
}
static mrb_value inp_dir8(mrb_state *mrb, mrb_value self) {
    return inp_dir4(mrb, self);
}

void inputBindingInit(mrb_state *mrb) {
    RClass *mod = mrb_define_module(mrb, "Input");
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
