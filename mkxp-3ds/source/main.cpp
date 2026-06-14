#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdio.h>
#include "platform_3ds.h"
#include "audio_3ds.h"

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
    audio_3ds_init();   /* subsistema de audio (SE/cries) -- depois de ndspInit */
}

static void exit_services() {
    mkxp_3ds_cleanup();
    audio_3ds_exit();   /* libertar buffers de audio antes de ndspExit */
    ndspExit();
    romfsExit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

int main(int argc, char* argv[]) {
    init_services();
    platform3ds_init();
    printf("mkxp-3DS v0.1\n");
    printf("Game: sdmc:/mkxp/game\n");
    printf("START = quit\n");
    int result = mkxp_3ds_run("sdmc:/mkxp/game");
    exit_services();
    return result;
}
