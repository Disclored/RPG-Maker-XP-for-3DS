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
    /* ---- API de STREAMING (pull) -- usada pelo BGM/BGS/ME ------------------- */
    typedef struct stb_vorbis_alloc stb_vorbis_alloc;   /* opaco; passamos NULL */
    typedef struct {
        unsigned int sample_rate;
        int          channels;
        unsigned int setup_memory_required;
        unsigned int setup_temp_memory_required;
        unsigned int temp_memory_required;
        int          max_frame_size;
    } stb_vorbis_info;
    extern stb_vorbis *stb_vorbis_open_filename(const char *filename, int *error,
                                                const stb_vorbis_alloc *alloc_buffer);
    extern stb_vorbis_info stb_vorbis_get_info(stb_vorbis *f);
    extern int  stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels,
                                                         short *buffer, int num_shorts);
    extern int  stb_vorbis_seek_start(stb_vorbis *f);
    extern void stb_vorbis_close(stb_vorbis *f);
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
/* ============================================================================
 * STREAMING (BGM/BGS/ME) -- decode incremental de OGG, alimentado por frame.
 * ----------------------------------------------------------------------------
 * Canais NDSP dedicados: 0=BGM, 1=BGS, 2=ME (os SE usam 3..14). Cada stream usa
 * 2 buffers LINEAR (~0.35s cada) em ping-pong: quando o DSP termina um, o
 * audio_3ds_update() (1x/frame) descodifica o bloco seguinte e re-enfileira.
 * Loop (BGM/BGS): ao chegar ao fim faz stb_vorbis_seek_start e continua a encher
 * o MESMO bloco (sem buraco audivel). ME nao faz loop. Buffers pequenos =>
 * ~150KB por stream activo => NAO mexe na memoria dos mapas grandes.
 * ========================================================================= */
#define STREAM_SECS_PER_BUF 0.35f
#define BGM_CHANNEL 0
#define BGS_CHANNEL 1
#define ME_CHANNEL  2

struct Stream {
    stb_vorbis *v;
    int16_t    *buf[2];
    ndspWaveBuf wbuf[2];
    int         channel;
    int         chans;        /* 1 ou 2 */
    int         rate;
    int         bufSamples;   /* samples por canal por buffer */
    bool        looping;
    bool        active;
    int         volume;       /* 0..100 */
    char        name[256];
};

static Stream s_bgm, s_bgs, s_me;

/* Descodifica bufSamples (por canal) para dst, fazendo loop se 'looping'.
 * Devolve nº de samples por canal escritos (0 = fim e sem loop). */
static int stream_decode_block(Stream *s, int16_t *dst) {
    int total = 0, guard = 0;
    while (total < s->bufSamples) {
        int want = s->bufSamples - total;
        int got  = stb_vorbis_get_samples_short_interleaved(
                       s->v, s->chans, dst + total * s->chans, want * s->chans);
        if (got <= 0) {
            if (s->looping) {
                stb_vorbis_seek_start(s->v);
                if (++guard > 2) break;     /* ficheiro vazio: evita loop infinito */
                continue;
            }
            break;
        }
        total += got;
    }
    return total;
}

/* Enche e enfileira o buffer 'which'. false => fim (sem loop). */
static bool stream_queue(Stream *s, int which) {
    int n = stream_decode_block(s, s->buf[which]);
    if (n <= 0) return false;
    memset(&s->wbuf[which], 0, sizeof(ndspWaveBuf));
    s->wbuf[which].data_vaddr = s->buf[which];
    s->wbuf[which].nsamples   = n;
    s->wbuf[which].looping    = false;
    DSP_FlushDataCache(s->buf[which], (u32)n * s->chans * sizeof(int16_t));
    ndspChnWaveBufAdd(s->channel, &s->wbuf[which]);
    return true;
}

static void stream_stop(Stream *s) {
    if (s->active) {
        ndspChnWaveBufClear(s->channel);
        ndspChnReset(s->channel);
    }
    if (s->v) { stb_vorbis_close(s->v); s->v = NULL; }
    for (int i = 0; i < 2; i++)
        if (s->buf[i]) { linearFree(s->buf[i]); s->buf[i] = NULL; }
    s->active = false;
}

static void stream_start(Stream *s, const char *name, int volume, int pitch,
                         bool looping, const char *tag) {
    stream_stop(s);
    if (!name || name[0] == '\0') { aud_log("[AUDIO] %s_play nome VAZIO -- ignorado\n", tag); return; }

    char full[512];
    int type = resolve_audio(name, full, sizeof(full));
    if (type == 0) { aud_log("[AUDIO|MISS] %s_play: '%s' nao encontrado\n", tag, name); return; }
    if (type != 1) { aud_log("[AUDIO] %s_play: '%s' nao e' OGG (streaming so' OGG por agora) -- ignorado\n", tag, name); return; }

    int err = 0;
    s->v = stb_vorbis_open_filename(full, &err, NULL);
    if (!s->v) { aud_log("[AUDIO|ERR] %s_play: stb_vorbis_open '%s' falhou (err=%d)\n", tag, full, err); return; }

    stb_vorbis_info info = stb_vorbis_get_info(s->v);
    s->chans      = (info.channels >= 2) ? 2 : 1;
    s->rate       = (info.sample_rate > 0) ? (int)info.sample_rate : 44100;
    s->bufSamples = (int)(s->rate * STREAM_SECS_PER_BUF);
    if (s->bufSamples < 1024) s->bufSamples = 1024;
    s->looping    = looping;
    s->volume     = (volume < 0 ? 0 : (volume > 100 ? 100 : volume));
    strncpy(s->name, name, sizeof(s->name) - 1); s->name[sizeof(s->name) - 1] = '\0';

    size_t bytes = (size_t)s->bufSamples * s->chans * sizeof(int16_t);
    for (int i = 0; i < 2; i++) {
        s->buf[i] = (int16_t *)linearAlloc(bytes);
        if (!s->buf[i]) { aud_log("[AUDIO|ERR] %s_play: linearAlloc %u falhou\n", tag, (unsigned)bytes); stream_stop(s); return; }
    }

    ndspChnReset(s->channel);
    ndspChnSetInterp(s->channel, NDSP_INTERP_LINEAR);
    float rate = (float)s->rate * ((pitch > 0 && pitch != 100) ? (pitch / 100.0f) : 1.0f);
    ndspChnSetRate(s->channel, rate);
    ndspChnSetFormat(s->channel, (s->chans == 2) ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    float vol = s->volume / 100.0f;
    float mix[12]; memset(mix, 0, sizeof(mix));
    mix[0] = vol; mix[1] = vol;
    ndspChnSetMix(s->channel, mix);

    s->active = true;
    bool a = stream_queue(s, 0);
    bool b = stream_queue(s, 1);
    if (!a && !b) { aud_log("[AUDIO|ERR] %s_play: '%s' sem dados decodificaveis\n", tag, name); stream_stop(s); return; }

    aud_log("[AUDIO] %s_play '%s' -> OGG %dch %dHz buf=%dsmp x2 canal=%d vol=%d pitch=%d loop=%d\n",
            tag, name, s->chans, s->rate, s->bufSamples, s->channel, s->volume, pitch, looping ? 1 : 0);
}

/* 1x/frame: re-enche os buffers que o DSP ja' consumiu. */
static void stream_pump(Stream *s) {
    if (!s->active || !s->v) return;
    bool anyAlive = false;
    for (int i = 0; i < 2; i++) {
        if (s->wbuf[i].status == NDSP_WBUF_DONE) {
            if (stream_queue(s, i)) anyAlive = true;
        } else if (s->wbuf[i].status == NDSP_WBUF_QUEUED ||
                   s->wbuf[i].status == NDSP_WBUF_PLAYING) {
            anyAlive = true;
        }
    }
    if (!anyAlive && !s->looping) {
        aud_log("[AUDIO] stream canal=%d terminou (sem loop) -- a parar\n", s->channel);
        stream_stop(s);
    }
}

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
    /* streams BGM/BGS/ME: zerar estado + reset dos canais 0/1/2 (SE usam 3..14) */
    memset(&s_bgm, 0, sizeof(Stream)); s_bgm.channel = BGM_CHANNEL;
    memset(&s_bgs, 0, sizeof(Stream)); s_bgs.channel = BGS_CHANNEL;
    memset(&s_me,  0, sizeof(Stream)); s_me.channel  = ME_CHANNEL;
    ndspChnReset(BGM_CHANNEL); ndspChnReset(BGS_CHANNEL); ndspChnReset(ME_CHANNEL);
    s_se_rr = 0;
    s_play_count = 0;
    s_inited = true;
    aud_log("[AUDIO] init OK: NDSP STEREO | SE %d canais (%d..%d) | BGM/BGS/ME streaming canais 0/1/2 | OGG+WAV\n",
            SE_SLOTS, SE_CH_FIRST, SE_CH_LAST);
}

extern "C" void audio_3ds_exit(void) {
    if (!s_inited) return;
    stream_stop(&s_bgm); stream_stop(&s_bgs); stream_stop(&s_me);
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
    /* alimentar os streams (re-enche os buffers ja' consumidos pelo DSP) */
    stream_pump(&s_bgm);
    stream_pump(&s_bgs);
    stream_pump(&s_me);
}

/* ---- BGM/BGS/ME: streaming OGG real (canais 0/1/2) ------------------------ */
extern "C" void audio_3ds_bgm_play(const char *name, int volume, int pitch) {
    if (!s_inited) { aud_log("[AUDIO] bgm_play antes de init -- ignorado\n"); return; }
    /* mesmo BGM ja' a tocar em loop -> NAO reiniciar (evita cortar a musica a
       cada mapa que use a mesma faixa); apenas ajusta o volume se mudou. */
    if (s_bgm.active && name && strcmp(s_bgm.name, name) == 0) {
        int v = (volume < 0 ? 0 : (volume > 100 ? 100 : volume));
        if (v != s_bgm.volume) {
            s_bgm.volume = v;
            float mix[12]; memset(mix, 0, sizeof(mix));
            mix[0] = mix[1] = v / 100.0f;
            ndspChnSetMix(s_bgm.channel, mix);
            aud_log("[AUDIO] bgm '%s' ja' a tocar -- volume -> %d\n", name, v);
        }
        return;
    }
    stream_start(&s_bgm, name, volume, pitch, true, "bgm");
}
extern "C" void audio_3ds_bgm_stop(void) { if (s_inited) { stream_stop(&s_bgm); aud_log("[AUDIO] bgm_stop\n"); } }

extern "C" void audio_3ds_bgs_play(const char *name, int volume, int pitch) {
    if (!s_inited) return;
    if (s_bgs.active && name && strcmp(s_bgs.name, name) == 0) return;
    stream_start(&s_bgs, name, volume, pitch, true, "bgs");
}
extern "C" void audio_3ds_bgs_stop(void) { if (s_inited) { stream_stop(&s_bgs); aud_log("[AUDIO] bgs_stop\n"); } }

extern "C" void audio_3ds_me_play(const char *name, int volume, int pitch) {
    if (!s_inited) return;
    /* ME (jingle, ex: subir de nivel / apanhar item): one-shot, sem loop. Em
       RMXP o ME pausa o BGM; aqui toca por cima e o jogo volta a chamar bgm_play
       quando o ME acaba (o motor RMXP trata do resume ao nivel do script). */
    stream_start(&s_me, name, volume, pitch, false, "me");
}
extern "C" void audio_3ds_me_stop(void) { if (s_inited) { stream_stop(&s_me); aud_log("[AUDIO] me_stop\n"); } }
