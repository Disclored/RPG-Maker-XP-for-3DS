#include "tilemap_binding_3ds.h"
#include "display_3ds.h"
#include <mruby/class.h>
#include <mruby/value.h>
#include <mruby/data.h>
#include <mruby/array.h>
#include <mruby/variable.h>
#include <cstdlib>
#include <vector>

static std::vector<Tilemap3DS*> g_tilemaps;

static void tilemap_free(mrb_state*, void *p) {
    Tilemap3DS *t = (Tilemap3DS*)p;
    if (t) {
        // remove da lista global
        for (size_t i = 0; i < g_tilemaps.size(); i++)
            if (g_tilemaps[i] == t) { g_tilemaps.erase(g_tilemaps.begin()+i); break; }
        free(t);
    }
}

const mrb_data_type Tilemap3DSType = { "Tilemap", tilemap_free };
#define GET_TM(s) ((Tilemap3DS*)DATA_PTR(s))

void tilemaps_draw_all() {
    for (Tilemap3DS *t : g_tilemaps) {
        if (!t || t->disposed || !t->visible || !t->tileset) continue;
        // Para já: blit do tileset inteiro em ox,oy como placeholder
        // Depois implementas o render tile-a-tile com map_data
        bmp_flush(t->tileset);
        if (!t->tileset->tex || !t->tileset->tex->valid) continue;
        display_3ds_blit(t->tileset->tex,
                         (float)-t->ox, (float)-t->oy,
                         0.0f, 0.0f,
                         (float)t->tileset->width,
                         (float)t->tileset->height,
                         1.0f);
    }
}

// initialize
static mrb_value tm_init(mrb_state *mrb, mrb_value self) {
    Tilemap3DS *t = (Tilemap3DS*)calloc(1, sizeof(Tilemap3DS));
    t->visible = true;
    DATA_PTR(self)  = t;
    DATA_TYPE(self) = &Tilemap3DSType;
    g_tilemaps.push_back(t);
    return self;
}

static mrb_value tm_dispose(mrb_state *mrb, mrb_value self) {
    Tilemap3DS *t = GET_TM(self);
    if (t) t->disposed = true;
    return mrb_nil_value();
}

static mrb_value tm_disposed(mrb_state *mrb, mrb_value self) {
    Tilemap3DS *t = GET_TM(self);
    return mrb_bool_value(!t || t->disposed);
}

static mrb_value tm_update(mrb_state *mrb, mrb_value self) {
    return mrb_nil_value(); // animações de autotile — implementar depois
}

// tileset= setter
static mrb_value tm_set_tileset(mrb_state *mrb, mrb_value self) {
    mrb_value bobj;
    mrb_get_args(mrb, "o", &bobj);
    Tilemap3DS *t = GET_TM(self);
    if (t) t->tileset = mrb_nil_p(bobj) ? nullptr : (Bitmap3DS*)DATA_PTR(bobj);
    return bobj;
}

static mrb_value tm_get_tileset(mrb_state *mrb, mrb_value self) {
    return mrb_nil_value(); // simplificado
}

// ox, oy
static mrb_value tm_set_ox(mrb_state *mrb, mrb_value self) {
    mrb_int v; mrb_get_args(mrb, "i", &v);
    Tilemap3DS *t = GET_TM(self); if (t) t->ox = (int)v;
    return mrb_fixnum_value(v);
}
static mrb_value tm_set_oy(mrb_state *mrb, mrb_value self) {
    mrb_int v; mrb_get_args(mrb, "i", &v);
    Tilemap3DS *t = GET_TM(self); if (t) t->oy = (int)v;
    return mrb_fixnum_value(v);
}
static mrb_value tm_get_ox(mrb_state *mrb, mrb_value self) {
    Tilemap3DS *t = GET_TM(self); return mrb_fixnum_value(t ? t->ox : 0);
}
static mrb_value tm_get_oy(mrb_state *mrb, mrb_value self) {
    Tilemap3DS *t = GET_TM(self); return mrb_fixnum_value(t ? t->oy : 0);
}

// visible
static mrb_value tm_set_visible(mrb_state *mrb, mrb_value self) {
    mrb_bool v; mrb_get_args(mrb, "b", &v);
    Tilemap3DS *t = GET_TM(self); if (t) t->visible = v;
    return mrb_bool_value(v);
}
static mrb_value tm_get_visible(mrb_state *mrb, mrb_value self) {
    Tilemap3DS *t = GET_TM(self); return mrb_bool_value(t && t->visible);
}

static mrb_value tm_stub_setter(mrb_state *mrb, mrb_value self) {
    mrb_value v;
    mrb_get_args(mrb, "o", &v);
    return v;
}

static mrb_value tm_autotiles(mrb_state *mrb, mrb_value self) {
    // Devolver o array de autotiles guardado como ivar Ruby
    mrb_value cached = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@autotiles"));
    if (mrb_nil_p(cached) || !mrb_array_p(cached)) {
        // Inicializar com 7 strings vazias
        mrb_value arr = mrb_ary_new_capa(mrb, 7);
        for (int i = 0; i < 7; i++)
            mrb_ary_push(mrb, arr, mrb_str_new_cstr(mrb, ""));
        mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@autotiles"), arr);
        return arr;
    }
    return cached;
}

static mrb_value tm_set_autotiles(mrb_state *mrb, mrb_value self) {
    mrb_value arr;
    mrb_get_args(mrb, "o", &arr);
    // Normalizar: garantir que é um array de 7 strings
    mrb_value normalized = mrb_ary_new_capa(mrb, 7);
    for (int i = 0; i < 7; i++) {
        mrb_value elem = mrb_nil_value();
        if (mrb_array_p(arr) && i < (int)RARRAY_LEN(arr))
            elem = mrb_ary_ref(mrb, arr, i);
        if (mrb_nil_p(elem) || !mrb_string_p(elem))
            elem = mrb_str_new_cstr(mrb, "");
        mrb_ary_push(mrb, normalized, elem);
    }
    mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@autotiles"), normalized);
    return normalized;
}

void tilemapBindingInit(mrb_state *mrb) {
    RClass *k = mrb_define_class(mrb, "Tilemap", mrb->object_class);
    MRB_SET_INSTANCE_TT(k, MRB_TT_DATA);

    mrb_define_method(mrb, k, "initialize", tm_init,         MRB_ARGS_OPT(1));
    mrb_define_method(mrb, k, "dispose",    tm_dispose,      MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "disposed?",  tm_disposed,     MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "update",     tm_update,       MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "tileset=",   tm_set_tileset,  MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "tileset",    tm_get_tileset,  MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "ox=",        tm_set_ox,       MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "oy=",        tm_set_oy,       MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "ox",         tm_get_ox,       MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "oy",         tm_get_oy,       MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "visible=",   tm_set_visible,  MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "visible",    tm_get_visible,  MRB_ARGS_NONE());

    // map_data, flash_data, priorities — stubs por agora
    mrb_define_method(mrb, k, "map_data=",    tm_stub_setter, MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "flash_data=",  tm_stub_setter, MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "priorities=",  tm_stub_setter, MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "autotiles",    tm_autotiles,   MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "autotiles=",   tm_set_autotiles, MRB_ARGS_REQ(1));
    // viewport -- guardado como ivar Ruby, scripts acedem a ele mas o render ignora
    mrb_define_method(mrb, k, "viewport",
        [](mrb_state *mrb, mrb_value self) -> mrb_value {
            return mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@viewport"));
        }, MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "viewport=",
        [](mrb_state *mrb, mrb_value self) -> mrb_value {
            mrb_value v; mrb_get_args(mrb, "o", &v);
            mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@viewport"), v);
            return v;
        }, MRB_ARGS_REQ(1));
}