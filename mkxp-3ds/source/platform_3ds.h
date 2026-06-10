#pragma once
#ifdef __3DS__
#include <3ds.h>
#endif
void        platform3ds_init();
const char* platform3ds_get_config_file();
void        platform3ds_print(const char* msg);
bool        platform3ds_should_quit();
