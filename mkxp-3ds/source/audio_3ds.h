/* ============================================================================
 * audio_3ds.h -- Subsistema de audio para 3DS via NDSP
 * ----------------------------------------------------------------------------
 * FASE 1 (esta versao): SE (efeitos sonoros) e cries de Pokemon.
 *   - Decode TOTAL de OGG (stb_vorbis) e WAV (parser RIFF proprio) para PCM16.
 *   - PCM colocado em memoria LINEAR (linearAlloc) -- o DSP acede por DMA.
 *   - Canais NDSP 3..14 em round-robin -> ate' 12 SE em simultaneo.
 *   - SEM threads (one-shot). BGM/BGS/ME (streaming + loop) vem nas Fases 2/3.
 *
 * Toda a actividade e' registada no log com prefixo [AUDIO] (detalhe pedido):
 * init, resolucao de ficheiro, formato (canais/rate/samples), canal usado,
 * libertacao de buffers, e qualquer falha.
 * ========================================================================== */
#ifndef AUDIO_3DS_H
#define AUDIO_3DS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Inicializa o subsistema. Chamar UMA vez, depois de ndspInit() (main.cpp).
 * Idempotente: chamadas repetidas nao fazem nada. */
void audio_3ds_init(void);

/* Liberta tudo (buffers PCM, canais). Chamar antes de ndspExit(). */
void audio_3ds_exit(void);

/* Toca um efeito sonoro. 'name' e' o nome/caminho como o jogo o pede
 * (ex: "Audio/SE/Cries/SOLGALEO" ou ja' resolvido com extensao). A resolucao
 * de extensao (.ogg/.wav/.mp3) e da raiz do jogo e' feita internamente.
 * volume 0..100, pitch em percentagem (100 = normal). */
void audio_3ds_se_play(const char *name, int volume, int pitch);

/* Para todos os SE a tocar e liberta os respectivos buffers. */
void audio_3ds_se_stop(void);

/* Chamar 1x por frame: recicla os buffers de SE que ja' terminaram
 * (liberta a memoria linear). Barato. */
void audio_3ds_update(void);

/* ---- BGM/BGS/ME: stubs da Fase 1 (apenas registam no log que foram
 * chamados, para nao perder eventos sonoros enquanto a Fase 2 nao chega). ---- */
void audio_3ds_bgm_play(const char *name, int volume, int pitch);
void audio_3ds_bgm_stop(void);
void audio_3ds_bgs_play(const char *name, int volume, int pitch);
void audio_3ds_bgs_stop(void);
void audio_3ds_me_play(const char *name, int volume, int pitch);
void audio_3ds_me_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_3DS_H */
