#pragma once
#include <mruby.h>
#include <stdio.h>

void      marshalBindingInit(mrb_state *mrb);
void      marshalDumpInt(mrb_state *mrb, FILE *fp, mrb_value val);
mrb_value marshalLoadInt(mrb_state *mrb, FILE *fp);
