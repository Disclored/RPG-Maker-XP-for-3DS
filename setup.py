import os

base = os.path.expanduser("~/mkxp-3ds")
src  = base + "/source"
for d in [src, base+"/libs/include", base+"/libs/lib",
          base+"/ruby/include", base+"/ruby/lib", base+"/romfs"]:
    os.makedirs(d, exist_ok=True)

files = {}

files[base+"/build_ruby.sh"] = (
    "#!/bin/bash\n"
    "set -e\n"
    "DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}\n"
    "DEVKITARM=${DEVKITARM:-$DEVKITPRO/devkitARM}\n"
    "PREFIX=$DEVKITARM/bin/arm-none-eabi\n"
    "RUBY_SRC=${RUBY_SRC:-$HOME/joiplay-ruby}\n"
    "OUT=$HOME/mkxp-3ds/ruby\n"
    "export PATH=\"$DEVKITARM/bin:$PATH\"\n"
    "CC=\"$PREFIX-gcc\" ; CXX=\"$PREFIX-g++\" ; AR=\"$PREFIX-ar\" ; RANLIB=\"$PREFIX-ranlib\"\n"
    "CFLAGS=\"-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft \\\n"
    "        -O1 -ffunction-sections -fdata-sections \\\n"
    "        -D__3DS__ -DARM_32BIT -DNO_FORK -DNO_EXEC \\\n"
    "        -I$DEVKITPRO/libctru/include\"\n"
    "cd \"$RUBY_SRC\"\n"
    "echo \"[1/3] Configuring Ruby 1.8.7 for 3DS...\"\n"
    "./configure \\\n"
    "    --host=arm-none-eabi --target=arm-none-eabi --prefix=\"$OUT\" \\\n"
    "    --disable-shared --enable-static --disable-install-doc \\\n"
    "    --without-tcl --without-tk --without-dbm --without-gdbm \\\n"
    "    --without-readline --without-curses --without-dl \\\n"
    "    CC=\"$CC\" CXX=\"$CXX\" AR=\"$AR\" RANLIB=\"$RANLIB\" \\\n"
    "    CFLAGS=\"$CFLAGS\" LDFLAGS=\"-L$DEVKITPRO/libctru/lib\" \\\n"
    "    ac_cv_func_fork=no ac_cv_func_vfork=no \\\n"
    "    ac_cv_func_popen=no ac_cv_func_pclose=no \\\n"
    "    ac_cv_have_decl_sys_nerr=no ac_cv_sizeof_off_t=4\n"
    "echo \"[2/3] Building libruby.a...\"\n"
    "make -j$(nproc) MAKEFLAGS= libruby-static.a\n"
    "echo \"[3/3] Installing...\"\n"
    "mkdir -p \"$OUT/include/ruby\" \"$OUT/lib\"\n"
    "cp libruby-static.a \"$OUT/lib/libruby.a\"\n"
    "cp ruby.h \"$OUT/include/\"\n"
    "cp -r .ext/include/arm-none-eabi/ruby \"$OUT/include/\" 2>/dev/null || true\n"
    "cp config.h \"$OUT/include/ruby/\" 2>/dev/null || true\n"
    "echo \"=== Ruby built at: $OUT ===\"\n"
)

files[base+"/install_deps.sh"] = (
    "#!/bin/bash\n"
    "set -e\n"
    "echo \"Installing devkitPro 3DS dependencies...\"\n"
    "dkp-pacman -S --noconfirm \\\n"
    "    3ds-dev \\\n"
    "    3ds-zlib \\\n"
    "    3ds-libpng \\\n"
    "    3ds-libogg \\\n"
    "    3ds-libvorbisidec \\\n"
    "    3ds-freetype\n"
    "echo \"Done.\"\n"
)

files[base+"/Makefile"] = """.SUFFIXES:
ifeq ($(strip $(DEVKITARM)),)
$(error "DEVKITARM not set.")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET   := mkxp-3ds
BUILD    := build
SOURCES  := source
DATA     := data
INCLUDES := source \\
            $(CURDIR)/ruby/include \\
            $(CURDIR)/libs/include
ROMFS    := romfs

APP_TITLE  := mkxp-3DS
APP_AUTHOR := mkxp-3ds-port
APP_DESC   := RPG Maker XP for Nintendo 3DS

ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS   := -g -Wall -O2 -mword-relocations \\
            -fomit-frame-pointer -ffunction-sections \\
            $(ARCH) $(INCLUDE) -D__3DS__
CXXFLAGS := $(CFLAGS) -fno-rtti -std=c++11 -fexceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS  := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS     := -lcitro2d -lcitro3d -lctru -lm
LIBDIRS  := $(CTRULIB)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \\
                  $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))

ifeq ($(strip $(CPPFILES)),)
  export LD := $(CC)
else
  export LD := $(CXX)
endif

export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export OFILES         := $(OFILES_SOURCES)
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \\
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \\
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all
all: $(BUILD)
$(BUILD):
\t@[ -d $@ ] || mkdir -p $@
\t@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
\t@echo Cleaning...
\t@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf

else
DEPENDS := $(OFILES:.o=.d)
all: $(OUTPUT).3dsx
$(OUTPUT).3dsx: $(OUTPUT).elf
$(OUTPUT).elf:  $(OFILES)
-include $(DEPENDS)
endif
"""

files[src+"/platform_3ds.h"] = """#pragma once
#ifdef __3DS__
#include <3ds.h>
#endif
void        platform3ds_init();
const char* platform3ds_get_config_file();
void        platform3ds_print(const char* msg);
bool        platform3ds_should_quit();
"""

files[src+"/platform_3ds.cpp"] = """#include "platform_3ds.h"
#include <3ds.h>
#include <stdio.h>

void platform3ds_init() {}

const char* platform3ds_get_config_file() {
    return "sdmc:/mkxp/game/mkxp.json";
}

void platform3ds_print(const char* msg) {
    printf("%s\\n", msg);
}

bool platform3ds_should_quit() {
    return !aptMainLoop();
}
"""

files[src+"/main.cpp"] = """#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdio.h>
#include "platform_3ds.h"

extern int  mkxp_3ds_run(const char* game_path);
extern void mkxp_3ds_cleanup();

static void init_services() {
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    consoleInit(GFX_BOTTOM, NULL);
    romfsInit();
    ndspInit();
}

static void exit_services() {
    mkxp_3ds_cleanup();
    ndspExit();
    romfsExit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

int main(int argc, char* argv[]) {
    init_services();
    platform3ds_init();
    printf("mkxp-3DS v0.1\\n");
    printf("Game: sdmc:/mkxp/game\\n");
    printf("START = quit\\n");
    int result = mkxp_3ds_run("sdmc:/mkxp/game");
    exit_services();
    return result;
}
"""

files[src+"/display_3ds.h"] = """#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

struct DS3Texture {
    C3D_Tex tex;
    int     width;
    int     height;
    bool    valid;
};

void        display_3ds_init();
void        display_3ds_begin_frame();
void        display_3ds_end_frame();
DS3Texture* display_3ds_create_texture(int w, int h, const unsigned char* rgba);
void        display_3ds_free_texture(DS3Texture* t);
void        display_3ds_blit(DS3Texture* t,
                             float dst_x, float dst_y,
                             float src_x, float src_y,
                             float src_w, float src_h,
                             float alpha);
void        display_3ds_fill_rect(float x, float y, float w, float h,
                                  unsigned char r, unsigned char g,
                                  unsigned char b, unsigned char a);
int         display_3ds_screen_width();
int         display_3ds_screen_height();
"""

files[src+"/display_3ds.cpp"] = """#include "display_3ds.h"
#include <cstring>

#define TOP_W 400
#define TOP_H 240

static C3D_RenderTarget* s_top    = nullptr;
static C3D_RenderTarget* s_bottom = nullptr;

void display_3ds_init() {
    s_top    = C2D_CreateScreenTarget(GFX_TOP,    GFX_LEFT);
    s_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
}

void display_3ds_begin_frame() {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(s_top,    C2D_Color32(0,0,0,255));
    C2D_TargetClear(s_bottom, C2D_Color32(0,0,0,255));
    C2D_SceneBegin(s_top);
}

void display_3ds_end_frame() { C3D_FrameEnd(0); }

int display_3ds_screen_width()  { return TOP_W; }
int display_3ds_screen_height() { return TOP_H; }

static inline int morton_offset(int x, int y, int tw) {
    static const int t[8] = {0,1,4,5,16,17,20,21};
    return ((y>>3)*(tw>>3)+(x>>3))*64 + t[x&7] + (t[y&7]<<1);
}

DS3Texture* display_3ds_create_texture(int w, int h, const unsigned char* rgba) {
    DS3Texture* tex = new DS3Texture();
    tex->width = w; tex->height = h; tex->valid = false;

    int tw = 8, th = 8;
    while (tw < w) tw <<= 1;
    while (th < h) th <<= 1;
    if (tw > 1024) tw = 1024;
    if (th > 1024) th = 1024;

    if (!C3D_TexInit(&tex->tex, (u16)tw, (u16)th, GPU_RGBA8)) {
        delete tex; return nullptr;
    }

    u8* dst = static_cast<u8*>(tex->tex.data);
    memset(dst, 0, tw*th*4);

    for (int y = 0; y < h && y < th; y++) {
        for (int x = 0; x < w && x < tw; x++) {
            int off = morton_offset(x, th-1-y, tw);
            const u8* src = rgba + (y*w+x)*4;
            dst[off*4+0] = src[3];
            dst[off*4+1] = src[2];
            dst[off*4+2] = src[1];
            dst[off*4+3] = src[0];
        }
    }

    C3D_TexFlush(&tex->tex);
    C3D_TexSetFilter(&tex->tex, GPU_LINEAR, GPU_NEAREST);
    tex->valid = true;
    return tex;
}

void display_3ds_free_texture(DS3Texture* t) {
    if (!t) return;
    if (t->valid) C3D_TexDelete(&t->tex);
    delete t;
}

void display_3ds_blit(DS3Texture* t,
                      float dx, float dy,
                      float sx, float sy, float sw, float sh,
                      float alpha) {
    if (!t || !t->valid) return;
    float tw = (float)t->tex.width, th = (float)t->tex.height;
    Tex3DS_SubTexture sub = {
        (u16)sw,(u16)sh, sx/tw, sy/th, (sx+sw)/tw, (sy+sh)/th
    };
    C2D_Image img = { &t->tex, &sub };
    C2D_DrawParams p = { {dx,dy,sw,sh},{0,0},0.5f,0.0f };
    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, C2D_Color32f(1,1,1,alpha), 1.0f);
    C2D_DrawImage(img, &p, &tint);
}

void display_3ds_fill_rect(float x, float y, float w, float h,
                           unsigned char r, unsigned char g,
                           unsigned char b, unsigned char a) {
    C2D_DrawRectSolid(x, y, 0.5f, w, h, C2D_Color32(r,g,b,a));
}
"""

files[src+"/input_3ds.h"] = """#pragma once
#include <3ds.h>

enum RmxpKey {
    RMXP_DOWN=2, RMXP_LEFT=4, RMXP_RIGHT=6, RMXP_UP=8,
    RMXP_A=11, RMXP_B=12, RMXP_C=13, RMXP_X=14,
    RMXP_Y=15,  RMXP_Z=16, RMXP_L=17, RMXP_R=18,
    RMXP_SHIFT=21, RMXP_F5=25,
    RMXP_KEY_COUNT=32
};

void input_3ds_poll(bool* pressed, bool* released, int key_count);
bool input_3ds_should_quit();
"""

files[src+"/input_3ds.cpp"] = """#include "input_3ds.h"
#include <string.h>

struct BMap { u32 btn; RmxpKey key; };
static const BMap MAP[] = {
    {KEY_DUP,    RMXP_UP},    {KEY_DDOWN,  RMXP_DOWN},
    {KEY_DLEFT,  RMXP_LEFT},  {KEY_DRIGHT, RMXP_RIGHT},
    {KEY_A,      RMXP_C},
    {KEY_B,      RMXP_X},
    {KEY_X,      RMXP_Z},
    {KEY_Y,      RMXP_A},
    {KEY_L,      RMXP_L},     {KEY_R,      RMXP_R},
    {KEY_START,  RMXP_F5},    {KEY_SELECT, RMXP_SHIFT},
};
static const int MSIZ = (int)(sizeof(MAP)/sizeof(MAP[0]));

void input_3ds_poll(bool* pressed, bool* released, int n) {
    hidScanInput();
    u32 dn = hidKeysDown(), up = hidKeysUp();
    for (int i = 0; i < MSIZ; i++) {
        int k = (int)MAP[i].key;
        if (k < 0 || k >= n) continue;
        if (dn & MAP[i].btn) pressed[k]  = true;
        if (up & MAP[i].btn) released[k] = true;
    }
    circlePosition c; hidCircleRead(&c);
    if (RMXP_UP    < n && c.dy >  40) pressed[RMXP_UP]    = true;
    if (RMXP_DOWN  < n && c.dy < -40) pressed[RMXP_DOWN]  = true;
    if (RMXP_LEFT  < n && c.dx < -40) pressed[RMXP_LEFT]  = true;
    if (RMXP_RIGHT < n && c.dx >  40) pressed[RMXP_RIGHT] = true;
}

bool input_3ds_should_quit() { return !aptMainLoop(); }
"""

files[src+"/engine_bridge.cpp"] = """#include "display_3ds.h"
#include "input_3ds.h"
#include "platform_3ds.h"
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>

static void draw_test_frame(int tick) {
    display_3ds_begin_frame();
    for (int i = 0; i < 8; i++) {
        int hue = (tick + i * 32) & 0xFF;
        display_3ds_fill_rect(i*50.0f, 0, 50, 240,
            (unsigned char)hue,
            (unsigned char)(255 - hue),
            180, 255);
    }
    display_3ds_fill_rect(150, 80, 100, 80, 255, 255, 255, 220);
    display_3ds_end_frame();
}

int mkxp_3ds_run(const char* game_path) {
    platform3ds_print("engine_bridge: display init");
    display_3ds_init();
    platform3ds_print("engine_bridge: entering game loop");
    printf("  game_path = %s\\n", game_path);

    bool pressed[RMXP_KEY_COUNT], released[RMXP_KEY_COUNT];
    int  tick = 0;

    while (!input_3ds_should_quit()) {
        memset(pressed,  0, sizeof(pressed));
        memset(released, 0, sizeof(released));
        input_3ds_poll(pressed, released, RMXP_KEY_COUNT);
        if (pressed[RMXP_F5]) break;
        draw_test_frame(tick++);
    }

    platform3ds_print("engine_bridge: loop done");
    return 0;
}

void mkxp_3ds_cleanup() {
    platform3ds_print("engine_bridge: cleanup");
}
"""

for path, content in files.items():
    with open(path, "w", newline="\n") as f:
        f.write(content)
    print("wrote", path.replace(os.path.expanduser("~"), "~"))

import stat as st
os.chmod(base+"/build_ruby.sh",   0o755)
os.chmod(base+"/install_deps.sh", 0o755)

print("\n── source/ ──────────────────────────────────")
for f in sorted(os.listdir(src)):
    sz = os.path.getsize(src+"/"+f)
    print(f"  {f:32s} {sz:5d} bytes")

print("\n── mkxp-3ds/ ────────────────────────────────")
for f in sorted(os.listdir(base)):
    if os.path.isfile(base+"/"+f):
        sz = os.path.getsize(base+"/"+f)
        print(f"  {f:32s} {sz:5d} bytes")
