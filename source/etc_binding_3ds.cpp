#include "etc_binding_3ds.h"
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/value.h>
#include <mruby/string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Color { float r,g,b,a; };
struct Tone  { float r,g,b,gray; };
struct Rect  { int x,y,w,h; };

static void dfree(mrb_state*, void *p){ free(p); }

static const mrb_data_type ColorType = { "Color", dfree };
static const mrb_data_type ToneType  = { "Tone",  dfree };
static const mrb_data_type RectType  = { "Rect",  dfree };

#define GET_COLOR(s) ((Color*)DATA_PTR(s))
#define GET_TONE(s)  ((Tone*) DATA_PTR(s))
#define GET_RECT(s)  ((Rect*) DATA_PTR(s))

static inline float clampf(float v,float lo,float hi){return v<lo?lo:v>hi?hi:v;}

/* Color */
static mrb_value color_init(mrb_state *mrb, mrb_value self){
    mrb_float r=255,g=255,b=255,a=255;
    mrb_int argc = mrb_get_argc(mrb);
    if (argc >= 3) mrb_get_args(mrb,"fff|f",&r,&g,&b,&a);
    else if (argc == 1) mrb_get_args(mrb,"f",&r);
    Color *c=(Color*)malloc(sizeof(Color)); c->r=(float)r;c->g=(float)g;c->b=(float)b;c->a=(float)a;
    DATA_PTR(self)=c; DATA_TYPE(self)=&ColorType; return self;
}
static mrb_value color_set(mrb_state *mrb, mrb_value self){
    mrb_float r,g,b,a=255; mrb_get_args(mrb,"fff|f",&r,&g,&b,&a);
    Color *c=GET_COLOR(self); c->r=(float)r;c->g=(float)g;c->b=(float)b;c->a=(float)a; return self;
}
static mrb_value color_copy(mrb_state *mrb, mrb_value self){
    mrb_value o; mrb_get_args(mrb,"o",&o);
    Color *c=(Color*)malloc(sizeof(Color)); *c=*GET_COLOR(o);
    DATA_PTR(self)=c; DATA_TYPE(self)=&ColorType; return self;
}
#define CGETSET(nm,f,lo,hi) \
static mrb_value color_get_##nm(mrb_state *mrb,mrb_value s){return mrb_float_value(mrb,GET_COLOR(s)->f);} \
static mrb_value color_set_##nm(mrb_state *mrb,mrb_value s){mrb_float v;mrb_get_args(mrb,"f",&v);GET_COLOR(s)->f=clampf((float)v,lo,hi);return mrb_float_value(mrb,v);}
CGETSET(red,r,0,255) CGETSET(green,g,0,255) CGETSET(blue,b,0,255) CGETSET(alpha,a,0,255)
static mrb_value color_to_s(mrb_state *mrb,mrb_value s){
    Color *c=GET_COLOR(s);char buf[64];sprintf(buf,"(%.1f, %.1f, %.1f, %.1f)",c->r,c->g,c->b,c->a);
    return mrb_str_new_cstr(mrb,buf);
}

/* Tone */
static mrb_value tone_init(mrb_state *mrb, mrb_value self){
    mrb_float r=0,g=0,b=0,gray=0;
    mrb_int argc = mrb_get_argc(mrb);
    if (argc >= 3) mrb_get_args(mrb,"fff|f",&r,&g,&b,&gray);
    else if (argc == 1) mrb_get_args(mrb,"f",&r);
    /* 0 args -> default Tone(0,0,0,0) */
    Tone *t=(Tone*)malloc(sizeof(Tone)); t->r=(float)r;t->g=(float)g;t->b=(float)b;t->gray=(float)gray;
    DATA_PTR(self)=t; DATA_TYPE(self)=&ToneType; return self;
}
static mrb_value tone_set(mrb_state *mrb, mrb_value self){
    mrb_float r,g,b,gray=0; mrb_get_args(mrb,"fff|f",&r,&g,&b,&gray);
    Tone *t=GET_TONE(self); t->r=(float)r;t->g=(float)g;t->b=(float)b;t->gray=(float)gray; return self;
}
static mrb_value tone_copy(mrb_state *mrb, mrb_value self){
    mrb_value o; mrb_get_args(mrb,"o",&o);
    Tone *t=(Tone*)malloc(sizeof(Tone)); *t=*GET_TONE(o);
    DATA_PTR(self)=t; DATA_TYPE(self)=&ToneType; return self;
}
#define TGETSET(nm,f,lo,hi) \
static mrb_value tone_get_##nm(mrb_state *mrb,mrb_value s){return mrb_float_value(mrb,GET_TONE(s)->f);} \
static mrb_value tone_set_##nm(mrb_state *mrb,mrb_value s){mrb_float v;mrb_get_args(mrb,"f",&v);GET_TONE(s)->f=clampf((float)v,lo,hi);return mrb_float_value(mrb,v);}
TGETSET(red,r,-255,255) TGETSET(green,g,-255,255) TGETSET(blue,b,-255,255) TGETSET(gray,gray,0,255)
static mrb_value tone_to_s(mrb_state *mrb,mrb_value s){
    Tone *t=GET_TONE(s);char buf[64];sprintf(buf,"(%.1f, %.1f, %.1f, %.1f)",t->r,t->g,t->b,t->gray);
    return mrb_str_new_cstr(mrb,buf);
}

/* Rect */
static mrb_value rect_init(mrb_state *mrb, mrb_value self){
    mrb_int x=0,y=0,w=0,h=0;
    mrb_int argc = mrb_get_argc(mrb);
    if (argc >= 3) mrb_get_args(mrb,"iii|i",&x,&y,&w,&h);
    Rect *r=(Rect*)malloc(sizeof(Rect)); r->x=(int)x;r->y=(int)y;r->w=(int)w;r->h=(int)h;
    DATA_PTR(self)=r; DATA_TYPE(self)=&RectType; return self;
}
static mrb_value rect_set(mrb_state *mrb, mrb_value self){
    mrb_int x,y,w,h=0; mrb_get_args(mrb,"iii|i",&x,&y,&w,&h);
    Rect *r=GET_RECT(self); r->x=(int)x;r->y=(int)y;r->w=(int)w;r->h=(int)h; return self;
}
static mrb_value rect_copy(mrb_state *mrb, mrb_value self){
    mrb_value o; mrb_get_args(mrb,"o",&o);
    Rect *r=(Rect*)malloc(sizeof(Rect)); *r=*GET_RECT(o);
    DATA_PTR(self)=r; DATA_TYPE(self)=&RectType; return self;
}
static mrb_value rect_empty(mrb_state *mrb,mrb_value s){Rect *r=GET_RECT(s);r->x=r->y=r->w=r->h=0;return mrb_nil_value();}
#define RGETSET(nm,f) \
static mrb_value rect_get_##nm(mrb_state *mrb,mrb_value s){return mrb_fixnum_value(GET_RECT(s)->f);} \
static mrb_value rect_set_##nm(mrb_state *mrb,mrb_value s){mrb_int v;mrb_get_args(mrb,"i",&v);GET_RECT(s)->f=(int)v;return mrb_fixnum_value(v);}
RGETSET(x,x) RGETSET(y,y) RGETSET(width,w) RGETSET(height,h)
static mrb_value rect_to_s(mrb_state *mrb,mrb_value s){
    Rect *r=GET_RECT(s);char buf[64];sprintf(buf,"(%d, %d, %d, %d)",r->x,r->y,r->w,r->h);
    return mrb_str_new_cstr(mrb,buf);
}

void etcBindingInit(mrb_state *mrb)
{
    RClass *k;

    k=mrb_define_class(mrb,"Color",mrb->object_class); MRB_SET_INSTANCE_TT(k,MRB_TT_DATA);
    mrb_define_method(mrb,k,"initialize",      color_init, MRB_ARGS_OPT(4));
    mrb_define_method(mrb,k,"initialize_copy", color_copy, MRB_ARGS_REQ(1));
    mrb_define_method(mrb,k,"set",  color_set,      MRB_ARGS_REQ(3)|MRB_ARGS_OPT(1));
    mrb_define_method(mrb,k,"to_s", color_to_s,     MRB_ARGS_NONE());
    mrb_define_method(mrb,k,"inspect",color_to_s,   MRB_ARGS_NONE());
    mrb_define_method(mrb,k,"red",   color_get_red,  MRB_ARGS_NONE()); mrb_define_method(mrb,k,"red=",   color_set_red,  MRB_ARGS_REQ(1));
    mrb_define_method(mrb,k,"green", color_get_green,MRB_ARGS_NONE()); mrb_define_method(mrb,k,"green=", color_set_green,MRB_ARGS_REQ(1));
    mrb_define_method(mrb,k,"blue",  color_get_blue, MRB_ARGS_NONE()); mrb_define_method(mrb,k,"blue=",  color_set_blue, MRB_ARGS_REQ(1));
    mrb_define_method(mrb,k,"alpha", color_get_alpha,MRB_ARGS_NONE()); mrb_define_method(mrb,k,"alpha=", color_set_alpha,MRB_ARGS_REQ(1));

    k=mrb_define_class(mrb,"Tone",mrb->object_class); MRB_SET_INSTANCE_TT(k,MRB_TT_DATA);
    mrb_define_method(mrb,k,"initialize",      tone_init, MRB_ARGS_OPT(4));
    mrb_define_method(mrb,k,"initialize_copy", tone_copy, MRB_ARGS_REQ(1));
    mrb_define_method(mrb,k,"set",  tone_set,       MRB_ARGS_REQ(3)|MRB_ARGS_OPT(1));
    mrb_define_method(mrb,k,"to_s", tone_to_s,      MRB_ARGS_NONE());
    mrb_define_method(mrb,k,"inspect",tone_to_s,    MRB_ARGS_NONE());
    mrb_define_method(mrb,k,"red",   tone_get_red,   MRB_ARGS_NONE()); mrb_define_method(mrb,k,"red=",   tone_set_red,   MRB_ARGS_REQ(1));
    mrb_define_method(mrb,k,"green", tone_get_green, MRB_ARGS_NONE()); mrb_define_method(mrb,k,"green=", tone_set_green, MRB_ARGS_REQ(1));
    mrb_define_method(mrb,k,"blue",  tone_get_blue,  MRB_ARGS_NONE()); mrb_define_method(mrb,k,"blue=",  tone_set_blue,  MRB_ARGS_REQ(1));
    mrb_define_method(mrb,k,"gray",  tone_get_gray,  MRB_ARGS_NONE()); mrb_define_method(mrb,k,"gray=",  tone_set_gray,  MRB_ARGS_REQ(1));

    k=mrb_define_class(mrb,"Rect",mrb->object_class); MRB_SET_INSTANCE_TT(k,MRB_TT_DATA);
    mrb_define_method(mrb,k,"initialize",      rect_init, MRB_ARGS_OPT(4));
    mrb_define_method(mrb,k,"initialize_copy", rect_copy, MRB_ARGS_REQ(1));
    mrb_define_method(mrb,k,"set",   rect_set,        MRB_ARGS_REQ(3)|MRB_ARGS_OPT(1));
    mrb_define_method(mrb,k,"empty", rect_empty,      MRB_ARGS_NONE());
    mrb_define_method(mrb,k,"to_s",  rect_to_s,       MRB_ARGS_NONE());
    mrb_define_method(mrb,k,"inspect",rect_to_s,      MRB_ARGS_NONE());
    mrb_define_method(mrb,k,"x",      rect_get_x,     MRB_ARGS_NONE()); mrb_define_method(mrb,k,"x=",      rect_set_x,     MRB_ARGS_REQ(1));
    mrb_define_method(mrb,k,"y",      rect_get_y,     MRB_ARGS_NONE()); mrb_define_method(mrb,k,"y=",      rect_set_y,     MRB_ARGS_REQ(1));
    mrb_define_method(mrb,k,"width",  rect_get_width, MRB_ARGS_NONE()); mrb_define_method(mrb,k,"width=",  rect_set_width, MRB_ARGS_REQ(1));
    mrb_define_method(mrb,k,"height", rect_get_height,MRB_ARGS_NONE()); mrb_define_method(mrb,k,"height=", rect_set_height,MRB_ARGS_REQ(1));
}
