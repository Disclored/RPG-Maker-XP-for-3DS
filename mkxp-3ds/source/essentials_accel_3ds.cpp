#include "essentials_accel_3ds.h"
#include <mruby.h>
#include <mruby/variable.h>   /* mrb_iv_get, mrb_gv_get */
#include <math.h>

/* Constantes RGSS/Essentials (fixas no motor RGSS1 / Game_Map). */
#define ACC_SUBPIXELS  4    /* Game_Map::X_SUBPIXELS == Y_SUBPIXELS */
#define ACC_TILE       32   /* Game_Map::TILE_WIDTH  == TILE_HEIGHT */

/* Simbolos interned UMA vez no init (rapido; evita re-intern por chamada). */
static struct {
    int     ready;
    mrb_sym real_x, real_y, width, tile_id, always_on_top, map_iv;
    mrb_sym display_x, display_y, jump_count, jump_distance_left;
    mrb_sym gv_game_map;
} S = { 0 };

/* nil/outro -> 0.0; fixnum/float -> double */
static inline double acc_d(mrb_value v) {
    if (mrb_fixnum_p(v)) return (double)mrb_fixnum(v);
    if (mrb_float_p(v))  return (double)mrb_float(v);
    return 0.0;
}

/* Replica ((real - disp) / X_SUBPIXELS).round do Ruby EXATAMENTE:
 *   - ambos inteiros -> divisao inteira do Ruby (floor p/ -inf); .round no-op
 *   - algum float    -> divisao float + .round (half-away-from-zero)
 * (validado no host contra ruby: 14/14 casos identicos) */
static long acc_div_sub(mrb_value real, mrb_value disp) {
    if (mrb_fixnum_p(real) && mrb_fixnum_p(disp)) {
        long diff = (long)mrb_fixnum(real) - (long)mrb_fixnum(disp);
        return (long)floor((double)diff / (double)ACC_SUBPIXELS);
    }
    double diff = acc_d(real) - acc_d(disp);
    return (long)round(diff / (double)ACC_SUBPIXELS);
}

/* map do character = @map se nao-nil, senao $game_map (replica Game_Character#map) */
static mrb_value acc_char_map(mrb_state *mrb, mrb_value chr) {
    mrb_value m = mrb_iv_get(mrb, chr, S.map_iv);
    if (mrb_nil_p(m)) m = mrb_gv_get(mrb, S.gv_game_map);
    return m;
}

/* screen_y_ground = ((real_y - map.display_y)/4).round + TILE_HEIGHT */
static long acc_screen_y_ground(mrb_state *mrb, mrb_value chr) {
    mrb_value map = acc_char_map(mrb, chr);
    mrb_value real_y = mrb_iv_get(mrb, chr, S.real_y);
    mrb_value disp_y = mrb_iv_get(mrb, map, S.display_y);
    return acc_div_sub(real_y, disp_y) + ACC_TILE;
}

/* __gc_native_screen_x(character) -> Integer
 * = ((real_x - map.display_x)/4).round + @width*TILE_WIDTH/2 */
static mrb_value gc_native_screen_x(mrb_state *mrb, mrb_value kself) {
    (void)kself;
    mrb_value chr;
    mrb_get_args(mrb, "o", &chr);
    mrb_value map = acc_char_map(mrb, chr);
    if (mrb_nil_p(map)) return mrb_nil_value();   /* sem mapa: deixa Ruby */
    mrb_value real_x = mrb_iv_get(mrb, chr, S.real_x);
    mrb_value disp_x = mrb_iv_get(mrb, map, S.display_x);
    long r = acc_div_sub(real_x, disp_x);
    mrb_value wv = mrb_iv_get(mrb, chr, S.width);
    long w = mrb_fixnum_p(wv) ? (long)mrb_fixnum(wv) : 1;
    r += w * ACC_TILE / 2;
    return mrb_fixnum_value((mrb_int)r);
}

/* __gc_native_screen_y(character) -> Integer | nil
 * jumping? -> nil (o arco do salto fica em Ruby). Senao screen_y_ground. */
static mrb_value gc_native_screen_y(mrb_state *mrb, mrb_value kself) {
    (void)kself;
    mrb_value chr;
    mrb_get_args(mrb, "o", &chr);
    mrb_value map = acc_char_map(mrb, chr);
    if (mrb_nil_p(map)) return mrb_nil_value();
    /* jumping? = (@jump_distance_left||0) > 0 || @jump_count > 0 */
    mrb_value jc  = mrb_iv_get(mrb, chr, S.jump_count);
    mrb_value jdl = mrb_iv_get(mrb, chr, S.jump_distance_left);
    long   jcount = mrb_fixnum_p(jc) ? (long)mrb_fixnum(jc) : 0;
    double jleft  = acc_d(jdl);
    if (jleft > 0.0 || jcount > 0) return mrb_nil_value();
    return mrb_fixnum_value((mrb_int)acc_screen_y_ground(mrb, chr));
}

/* __gc_native_screen_z(character, height) -> Integer | nil
 * @tile_id>0 ou @always_on_top -> nil (prioridade do tile / 999: fica em Ruby).
 * Senao screen_y_ground + ((height > TILE_HEIGHT) ? TILE_HEIGHT-1 : 0). */
static mrb_value gc_native_screen_z(mrb_state *mrb, mrb_value kself) {
    (void)kself;
    mrb_value chr, height_v;
    mrb_get_args(mrb, "oo", &chr, &height_v);
    mrb_value map = acc_char_map(mrb, chr);
    if (mrb_nil_p(map)) return mrb_nil_value();
    long tile = 0;
    mrb_value tid = mrb_iv_get(mrb, chr, S.tile_id);
    if (mrb_fixnum_p(tid)) tile = (long)mrb_fixnum(tid);
    mrb_value aot = mrb_iv_get(mrb, chr, S.always_on_top);
    if (tile > 0 || mrb_test(aot)) return mrb_nil_value();
    double height = acc_d(height_v);
    long z = acc_screen_y_ground(mrb, chr);
    z += (height > (double)ACC_TILE) ? (ACC_TILE - 1) : 0;
    return mrb_fixnum_value((mrb_int)z);
}

void essentialsAccelInit(mrb_state *mrb) {
    if (S.ready) return;
    S.real_x             = mrb_intern_lit(mrb, "@real_x");
    S.real_y             = mrb_intern_lit(mrb, "@real_y");
    S.width              = mrb_intern_lit(mrb, "@width");
    S.tile_id            = mrb_intern_lit(mrb, "@tile_id");
    S.always_on_top      = mrb_intern_lit(mrb, "@always_on_top");
    S.map_iv             = mrb_intern_lit(mrb, "@map");
    S.display_x          = mrb_intern_lit(mrb, "@display_x");
    S.display_y          = mrb_intern_lit(mrb, "@display_y");
    S.jump_count         = mrb_intern_lit(mrb, "@jump_count");
    S.jump_distance_left = mrb_intern_lit(mrb, "@jump_distance_left");
    S.gv_game_map        = mrb_intern_lit(mrb, "$game_map");
    S.ready = 1;
    mrb_define_method(mrb, mrb->kernel_module, "__gc_native_screen_x",
                      gc_native_screen_x, MRB_ARGS_REQ(1));
    mrb_define_method(mrb, mrb->kernel_module, "__gc_native_screen_y",
                      gc_native_screen_y, MRB_ARGS_REQ(1));
    mrb_define_method(mrb, mrb->kernel_module, "__gc_native_screen_z",
                      gc_native_screen_z, MRB_ARGS_REQ(2));
}
