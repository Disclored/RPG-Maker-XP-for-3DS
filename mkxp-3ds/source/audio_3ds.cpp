/* ============================================================================
 * audio_3ds.cpp -- Subsistema de audio para 3DS via NDSP (FASE 1: SE/cries)
 * ----------------------------------------------------------------------------
 * Ver audio_3ds.h para a visao geral. Esta versao implementa SE one-shot com
 * decode total (OGG via stb_vorbis, WAV via parser proprio), buffers em
 * linearAlloc e canais NDSP 3..14 em round-robin. Logging detalhado [AUDIO].
 * ========================================================================== */
#include "audio_3ds.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <stdarg.h>    /* va_list (logging) */
#include <stdint.h>

/* stb_vorbis e' compilado como ficheiro .c separado no projeto. Aqui so'
 * declaramos as funcoes que usamos (evita incluir o .c duas vezes). */
extern "C" {
    typedef struct stb_vorbis stb_vorbis;
    extern int  stb_vorbis_decode_filename(const char *filename, int *channels,
                                           int *sample_rate, short **output);
}

/* O log do projeto: usamos o mesmo ficheiro/també stdout que o resto do
 * emulador. Reaproveitamos o ponteiro global g_dbglog definido no binding e a
 * mecanica de timestamp. Para nao criar dependencias de cabecalho, declaramos
 * o minimo e escrevemos com um helper local. */
/* O log do projeto: usamos o mesmo ficheiro/stdout que o resto do emulador.
 * g_dbglog e dbglog_stamp sao definidos em binding_3ds.cpp como simbolos C++
 * (NAO extern "C"), por isso declaramo-los aqui com a MESMA linkagem C++ para
 * o linker os resolver. Incluimos tambem o sistema de debug partilhado
 * (debug_3ds.h) para respeitar a mascara de categoria DBG_AUDIO -- assim o
 * audio liga/desliga como qualquer outra categoria, e o flush e' o partilhado. */
#include "debug_3ds.h"
extern FILE *g_dbglog;          /* definido em binding_3ds.cpp (C++ linkage) */
extern void dbglog_stamp(double *out_total_s, double *out_delta_ms);

/* Helper de log com timestamp, escrita no ficheiro debug_binding.log E no
 * stdout (consola), igual em espirito ao DBGLOG do binding. Prefixo [AUDIO] em
 * todas as linhas. So' escreve se a categoria DBG_AUDIO estiver ativa na
 * mascara global (g_dbg_mask) -- consistente com o resto do port. O contador
 * de flush e' o GLOBAL partilhado (g_dbglog_flushc), nao um local. */
static void aud_log(const char *fmt, ...) {
    if (!(g_dbg_mask & DBG_AUDIO)) return;   /* categoria desligada -> nada */
    double t = 0, d = 0;
    dbglog_stamp(&t, &d);
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    /* consola (visivel no ecra de baixo) */
    printf("[t=%8.3fs d=%7.1fms] %s", t, d, line);
    /* ficheiro de log partilhado (debug_binding.log) com flush periodico global */
    if (g_dbglog) {
        fprintf(g_dbglog, "[t=%8.3fs d=%7.1fms] %s", t, d, line);
        if (++g_dbglog_flushc >= 512) { g_dbglog_flushc = 0; fflush(g_dbglog); }
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Estado
 * ─────────────────────────────────────────────────────────────────────────── */
#define SE_CH_FIRST 3      /* primeiro canal NDSP para SE */
#define SE_CH_LAST  14     /* ultimo canal NDSP para SE (12 canais 3..14) */
#define SE_SLOTS    (SE_CH_LAST - SE_CH_FIRST + 1)

struct SeSlot {
    ndspWaveBuf wbuf;      /* descritor do buffer para o DSP */
    int16_t    *pcm;       /* PCM16 em linearAlloc (NULL = livre) */
    int         channel;   /* canal NDSP deste slot */
    bool        active;    /* tem som a tocar/agendado */
};

static bool    s_inited = false;
static SeSlot  s_se[SE_SLOTS];
static int     s_se_rr = 0;          /* indice round-robin */
static long    s_play_count = 0;     /* diagnostico */

/* Raizes do jogo onde procurar o audio (igual ao loader de bitmaps). */
static const char *s_roots[] = {
    "sdmc:/mkxp/game/",
    "",                  /* path tal-qual (ja' absoluto / com extensao) */
    NULL
};

/* ─────────────────────────────────────────────────────────────────────────────
 * Decode WAV (RIFF/PCM16). Devolve PCM alocado com malloc (libertar com free).
 * Suporta PCM linear 8/16-bit, mono/estereo. Devolve nº de samples por canal.
 * ─────────────────────────────────────────────────────────────────────────── */
static int16_t *wav_decode(const char *path, int *out_ch, int *out_rate, int *out_samples) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char hdr[44];
    if (fread(hdr, 1, 44, f) != 44) { fclose(f); return NULL; }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) { fclose(f); return NULL; }
    uint16_t fmt   = hdr[20] | (hdr[21] << 8);
    uint16_t ch    = hdr[22] | (hdr[23] << 8);
    uint32_t rate  = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    uint16_t bits  = hdr[34] | (hdr[35] << 8);
    if (fmt != 1 || (bits != 16 && bits != 8) || ch < 1 || ch > 2) {
        /* formato nao-PCM (ex: comprimido) -> deixar o OGG/MP3 tratar */
        fclose(f);
        return NULL;
    }
    /* Procurar o chunk 'data' (pode nao estar logo a seguir ao fmt). */
    fseek(f, 12, SEEK_SET);
    uint32_t data_size = 0; long data_off = -1;
    unsigned char ck[8];
    while (fread(ck, 1, 8, f) == 8) {
        uint32_t sz = ck[4] | (ck[5] << 8) | (ck[6] << 16) | (ck[7] << 24);
        if (memcmp(ck, "data", 4) == 0) { data_size = sz; data_off = ftell(f); break; }
        fseek(f, (sz + 1) & ~1u, SEEK_CUR);   /* chunks sao alinhados a 2 bytes */
    }
    if (data_off < 0 || data_size == 0) { fclose(f); return NULL; }

    int samples = (int)(data_size / (ch * (bits / 8)));
    int16_t *pcm = (int16_t *)malloc((size_t)samples * ch * sizeof(int16_t));
    if (!pcm) { fclose(f); return NULL; }
    fseek(f, data_off, SEEK_SET);
    if (bits == 16) {
        if (fread(pcm, sizeof(int16_t), (size_t)samples * ch, f) != (size_t)samples * ch) {
            free(pcm); fclose(f); return NULL;
        }
    } else { /* 8-bit unsigned -> 16-bit signed */
        unsigned char *tmp = (unsigned char *)malloc((size_t)samples * ch);
        if (!tmp) { free(pcm); fclose(f); return NULL; }
        if (fread(tmp, 1, (size_t)samples * ch, f) != (size_t)samples * ch) {
            free(tmp); free(pcm); fclose(f); return NULL;
        }
        for (int i = 0; i < samples * ch; i++) pcm[i] = (int16_t)((tmp[i] - 128) << 8);
        free(tmp);
    }
    fclose(f);
    *out_ch = ch; *out_rate = (int)rate; *out_samples = samples;
    return pcm;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Resolucao de ficheiro: tenta raizes x extensoes ate' encontrar.
 * Preenche 'full' com o caminho que existe. Devolve o tipo: 1=ogg 2=wav 0=nao.
 * ─────────────────────────────────────────────────────────────────────────── */
static int resolve_audio(const char *name, char *full, size_t fullsz) {
    /* extensoes por ordem de preferencia (o jogo e' maioritariamente OGG) */
    static const char *exts[] = { "", ".ogg", ".wav", ".OGG", ".WAV", NULL };
    for (int ri = 0; s_roots[ri] != NULL; ri++) {
        for (int ei = 0; exts[ei] != NULL; ei++) {
            const char *root = s_roots[ri];
            if (root[0] != '\0' && name[0] == '/')
                snprintf(full, fullsz, "%s%s%s", root, name + 1, exts[ei]);
            else
                snprintf(full, fullsz, "%s%s%s", root, name, exts[ei]);
            FILE *t = fopen(full, "rb");
            if (t) {
                fclose(t);
                /* determinar tipo pela extensao do caminho resolvido */
                size_t L = strlen(full);
                if (L >= 4) {
                    const char *e = full + L - 4;
                    if (strcasecmp(e, ".ogg") == 0) return 1;
                    if (strcasecmp(e, ".wav") == 0) return 2;
                }
                /* sem extensao reconhecida: tentar adivinhar pelos magic bytes */
                FILE *m = fopen(full, "rb");
                if (m) {
                    unsigned char mg[4] = {0,0,0,0};
                    size_t got = fread(mg, 1, 4, m); fclose(m);
                    if (got >= 4) {
                        if (memcmp(mg, "OggS", 4) == 0) return 1;
                        if (memcmp(mg, "RIFF", 4) == 0) return 2;
                    }
                }
                return 1; /* assumir ogg por omissao */
            }
        }
    }
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Libertar um slot (parar canal + free do PCM linear).
 * ─────────────────────────────────────────────────────────────────────────── */
static void se_slot_free(SeSlot *s) {
    if (s->active) {
        ndspChnWaveBufClear(s->channel);
    }
    if (s->pcm) {
        linearFree(s->pcm);
        s->pcm = NULL;
    }
    s->active = false;
}

/* Reciclar slots cujo som ja' terminou (chamado por audio_3ds_update). */
static int se_reap(void) {
    int freed = 0;
    for (int i = 0; i < SE_SLOTS; i++) {
        SeSlot *s = &s_se[i];
        if (s->active && s->wbuf.status == NDSP_WBUF_DONE) {
            se_slot_free(s);
            freed++;
        }
    }
    return freed;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * API publica
 * ─────────────────────────────────────────────────────────────────────────── */
extern "C" void audio_3ds_init(void) {
    if (s_inited) { aud_log("[AUDIO] init: ja' inicializado (ignorado)\n"); return; }

    /* ndspInit() ja' foi chamado em main.cpp. Configuramos o mixer master. */
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    for (int i = 0; i < SE_SLOTS; i++) {
        memset(&s_se[i], 0, sizeof(SeSlot));
        s_se[i].channel = SE_CH_FIRST + i;
        s_se[i].pcm     = NULL;
        s_se[i].active  = false;
        ndspChnReset(s_se[i].channel);
    }
    s_se_rr = 0;
    s_play_count = 0;
    s_inited = true;
    aud_log("[AUDIO] init OK: NDSP STEREO, %d canais SE (%d..%d), decoders OGG+WAV\n",
            SE_SLOTS, SE_CH_FIRST, SE_CH_LAST);
}

extern "C" void audio_3ds_exit(void) {
    if (!s_inited) return;
    for (int i = 0; i < SE_SLOTS; i++) se_slot_free(&s_se[i]);
    s_inited = false;
    aud_log("[AUDIO] exit: %ld SE tocados nesta sessao, buffers libertados\n", s_play_count);
}

extern "C" void audio_3ds_se_play(const char *name, int volume, int pitch) {
    if (!s_inited) { aud_log("[AUDIO] se_play ANTES de init -- ignorado ('%s')\n", name ? name : "?"); return; }
    if (!name || name[0] == '\0') { aud_log("[AUDIO] se_play com nome VAZIO -- ignorado\n"); return; }

    /* 1) resolver ficheiro */
    char full[600];
    int type = resolve_audio(name, full, sizeof(full));
    if (type == 0) {
        aud_log("[AUDIO|MISS] se_play: '%s' NAO encontrado (.ogg/.wav em sdmc:/mkxp/game/)\n", name);
        return;
    }

    /* 2) decode -> PCM16 (malloc normal, depois copiamos p/ linear) */
    int ch = 0, rate = 0, samples = 0;
    int16_t *pcm_tmp = NULL;
    const char *tname = (type == 1) ? "OGG" : "WAV";
    if (type == 1) {
        short *out = NULL;
        int s = stb_vorbis_decode_filename(full, &ch, &rate, &out);
        if (s > 0 && out) { pcm_tmp = out; samples = s; }
    } else {
        pcm_tmp = wav_decode(full, &ch, &rate, &samples);
    }
    if (!pcm_tmp || samples <= 0 || ch < 1 || ch > 2) {
        aud_log("[AUDIO|ERR] se_play: decode %s falhou '%s'\n", tname, full);
        if (pcm_tmp) free(pcm_tmp);
        return;
    }

    /* 3) copiar PCM para memoria LINEAR (o DSP acede por DMA) */
    size_t bytes = (size_t)samples * ch * sizeof(int16_t);
    int16_t *pcm_lin = (int16_t *)linearAlloc(bytes);
    if (!pcm_lin) {
        aud_log("[AUDIO|ERR] se_play: linearAlloc %u bytes falhou (sem VRAM linear?)\n", (unsigned)bytes);
        free(pcm_tmp);
        return;
    }
    memcpy(pcm_lin, pcm_tmp, bytes);
    free(pcm_tmp);

    /* 4) escolher slot (round-robin); se ocupado, recuperar/forcar */
    se_reap();
    SeSlot *slot = NULL;
    for (int k = 0; k < SE_SLOTS; k++) {
        int idx = (s_se_rr + k) % SE_SLOTS;
        if (!s_se[idx].active) { slot = &s_se[idx]; s_se_rr = (idx + 1) % SE_SLOTS; break; }
    }
    if (!slot) {
        /* todos ocupados -> reutilizar o do round-robin (corta o mais antigo) */
        slot = &s_se[s_se_rr];
        aud_log("[AUDIO] se_play: todos os %d canais ocupados, a cortar canal %d\n",
                SE_SLOTS, slot->channel);
        se_slot_free(slot);
        s_se_rr = (s_se_rr + 1) % SE_SLOTS;
    }

    /* 5) configurar canal NDSP */
    int chan = slot->channel;
    ndspChnReset(chan);
    ndspChnSetInterp(chan, NDSP_INTERP_LINEAR);
    ndspChnSetRate(chan, (float)rate);
    ndspChnSetFormat(chan, (ch == 2) ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);

    /* volume 0..100 -> 0.0..1.0 (mix nos dois lados) */
    float vol = (volume < 0 ? 0 : (volume > 100 ? 100 : volume)) / 100.0f;
    float mix[12]; memset(mix, 0, sizeof(mix));
    mix[0] = vol; mix[1] = vol;     /* front-left / front-right */
    ndspChnSetMix(chan, mix);

    /* pitch: o RMXP da' pitch em % (100=normal). Aplicamos como factor de rate. */
    if (pitch > 0 && pitch != 100) {
        ndspChnSetRate(chan, (float)rate * (pitch / 100.0f));
    }

    /* 6) preparar wavebuf e enfileirar */
    slot->pcm = pcm_lin;
    memset(&slot->wbuf, 0, sizeof(ndspWaveBuf));
    slot->wbuf.data_vaddr = pcm_lin;
    slot->wbuf.nsamples   = samples;      /* nº de samples por canal */
    slot->wbuf.looping    = false;
    DSP_FlushDataCache(pcm_lin, bytes);
    ndspChnWaveBufAdd(chan, &slot->wbuf);
    slot->active = true;
    s_play_count++;

    aud_log("[AUDIO] SE #%ld '%s' -> %s %dch %dHz %d samples (%.1fKB) canal=%d vol=%d pitch=%d\n",
            s_play_count, name, tname, ch, rate, samples, bytes / 1024.0, chan, volume, pitch);
}

extern "C" void audio_3ds_se_stop(void) {
    if (!s_inited) return;
    int n = 0;
    for (int i = 0; i < SE_SLOTS; i++) if (s_se[i].active) { se_slot_free(&s_se[i]); n++; }
    aud_log("[AUDIO] se_stop: %d canais parados\n", n);
}

extern "C" void audio_3ds_update(void) {
    if (!s_inited) return;
    int freed = se_reap();
    if (freed > 0) aud_log("[AUDIO] update: %d buffer(s) de SE reciclado(s)\n", freed);
}

/* ---- BGM/BGS/ME: stubs da Fase 1 (logam, nao tocam ainda) ----------------- */
extern "C" void audio_3ds_bgm_play(const char *name, int volume, int pitch) {
    aud_log("[AUDIO|TODO] bgm_play '%s' vol=%d pitch=%d (streaming na Fase 2)\n",
            name ? name : "?", volume, pitch);
}
extern "C" void audio_3ds_bgm_stop(void) { aud_log("[AUDIO|TODO] bgm_stop (Fase 2)\n"); }
extern "C" void audio_3ds_bgs_play(const char *name, int volume, int pitch) {
    aud_log("[AUDIO|TODO] bgs_play '%s' vol=%d pitch=%d (Fase 2)\n", name ? name : "?", volume, pitch);
}
extern "C" void audio_3ds_bgs_stop(void) { aud_log("[AUDIO|TODO] bgs_stop (Fase 2)\n"); }
extern "C" void audio_3ds_me_play(const char *name, int volume, int pitch) {
    aud_log("[AUDIO|TODO] me_play '%s' vol=%d pitch=%d (Fase 3)\n", name ? name : "?", volume, pitch);
}
extern "C" void audio_3ds_me_stop(void) { aud_log("[AUDIO|TODO] me_stop (Fase 3)\n"); }
