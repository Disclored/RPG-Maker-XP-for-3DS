#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdio.h>
#include "platform_3ds.h"
#include "audio_3ds.h"

extern int  mkxp_3ds_run(const char* game_path);
extern void mkxp_3ds_cleanup();

/* Flag global: true se estamos num New 3DS / New 2DS XL. Lida pelo bitmap_binding
 * para decidir se decodifica os tilesets gigantes (Gen4, ~30MB) usando a RAM
 * estendida do New 3DS. No Old 3DS/2DS fica false e nada muda. */
bool g_mkxp_is_new3ds = false;

/* Resultado do ndspInit() guardado para o binding o registar no log de debug
 * (que so' abre depois do init_services). Se for R_FAILED, NAO ha som nenhum:
 * o DSP nao processa os wavebufs (no log isto aparece como "0 buffers de SE
 * reciclados" e os 12 canais de SE a encherem e nunca libertarem). Causa quase
 * sempre: falta o firmware do DSP (dspfirm.cdc). */
s32 g_ndsp_init_result = 0;

/* Deteta o modelo e, se for New 3DS, liga o clock de 804 MHz + cache L2 via
 * osSetSpeedupEnable. No Old 3DS/2DS isto e' inofensivo (no-op) e a flag fica
 * false. O ganho grande vem daqui: a logica Ruby e' single-thread e domina o
 * tempo de frame, por isso acelera quase na proporcao do clock (~3x). */
static void configure_new3ds() {
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    g_mkxp_is_new3ds = isNew;
    if (isNew) {
        osSetSpeedupEnable(true);   /* 804 MHz + L2 (so' tem efeito no New 3DS) */
        printf("[NEW3DS] New 3DS detetado: speedup LIGADO (804 MHz + L2)\n");
    } else {
        printf("[NEW3DS] Old 3DS/2DS: clock fixo 268 MHz (sem speedup)\n");
    }
}

static void init_services() {
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    consoleInit(GFX_BOTTOM, NULL);
    romfsInit();
    /* ndspInit(): em 3DS real / emulador o DSP precisa do firmware (dspfirm.cdc).
     * Se falhar, NAO ha som (BGM nem SE) -- o resto do jogo corre na mesma. Antes
     * o resultado era ignorado; agora verificamos e avisamos no ecra de baixo. */
    g_ndsp_init_result = ndspInit();
    if (R_FAILED(g_ndsp_init_result)) {
        printf("\n[AUDIO][FALHA] ndspInit rc=0x%08lX -> SEM SOM.\n", (unsigned long)g_ndsp_init_result);
        printf("  Falta o firmware do DSP. Solucao:\n");
        printf("   * 3DS REAL: corre uma vez o homebrew 'DSP1' (extrai dspfirm.cdc).\n");
        printf("   * Azahar/Citra: Emulation>Configure>Audio = HLE (Output: cubeb/auto).\n");
        printf("  Confirma tambem que o volume nao esta a 0 nem em silencio.\n\n");
    } else {
        printf("[AUDIO] ndspInit OK rc=0x%08lX (DSP disponivel)\n", (unsigned long)g_ndsp_init_result);
    }
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
    configure_new3ds();   /* New 3DS: liga 804 MHz + L2 e marca g_mkxp_is_new3ds */
    platform3ds_init();
    printf("mkxp-3DS v0.1\n");
    printf("Game: sdmc:/mkxp/game\n");
    printf("START = quit\n");
    int result = mkxp_3ds_run("sdmc:/mkxp/game");
    exit_services();
    return result;
}
