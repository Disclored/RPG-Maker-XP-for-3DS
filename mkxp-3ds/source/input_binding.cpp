#include <mruby.h>
#include <mruby/class.h>
#include <mruby/array.h>
#include "input_3ds.h"

static bool s_pressed[RMXP_KEY_COUNT];
static bool s_triggered[RMXP_KEY_COUNT];
static bool s_released[RMXP_KEY_COUNT];

/* Chamado UMA vez por frame, exclusivamente por grph_update (C++).
 * Input.update (Ruby) e um no-op para evitar double-poll. */
void inputBindingUpdate() {
    bool new_triggered[RMXP_KEY_COUNT] = {};
    bool new_released[RMXP_KEY_COUNT]  = {};

    input_3ds_poll(new_triggered, new_released, RMXP_KEY_COUNT);

    for (int i = 0; i < RMXP_KEY_COUNT; i++) {
        s_triggered[i] = new_triggered[i];
        s_released[i]  = new_released[i];
        if (new_triggered[i]) s_pressed[i] = true;
        if (new_released[i])  s_pressed[i] = false;
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
    return (k>=0&&k<RMXP_KEY_COUNT&&s_triggered[k]) ? mrb_true_value() : mrb_false_value();
}
static mrb_value inp_repeat(mrb_state *mrb, mrb_value self) {
    (void)self; mrb_int k; mrb_get_args(mrb, "i", &k);
    return (k>=0&&k<RMXP_KEY_COUNT&&s_pressed[k]) ? mrb_true_value() : mrb_false_value();
}
static mrb_value inp_release(mrb_state *mrb, mrb_value self) {
    (void)self; mrb_int k; mrb_get_args(mrb, "i", &k);
    return (k>=0&&k<RMXP_KEY_COUNT&&s_released[k]) ? mrb_true_value() : mrb_false_value();
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
