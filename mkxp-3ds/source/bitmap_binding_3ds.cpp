#include "bitmap_binding_3ds.h"
#include "display_3ds.h"
#include "debug_3ds.h"
#include <3ds.h>
#include <citro2d.h>
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/value.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/gc.h>   /* mrb_full_gc: reciclar bitmaps orfaos quando um asset grande nao cabe */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <dirent.h>   /* opendir: testar se uma PASTA existe (FileTest.directory?) */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"
#include <png.h>   /* libpng: decoder robusto p/ PNGs que o stb falha no ARM 32-bit */

/* ===== DECODER LIBPNG (a obra: resolve o "outofmem" do stb no ARM) ============
 * O [DECODE|PROBE] PROVOU que a intro picture NAO falha por memoria (os 2 blocos
 * de ~4.6MB cabiam) -- e' um bug do stb_image compilado para ARM 32-bit (o malloc
 * interno do stb devolve NULL com memoria disponivel). O mkxp-z (o emulador de
 * RPGXP para iOS/Android) usa libpng via SDL_image e descodifica estas mesmas
 * imagens sem problema. Portamos a mesma solucao: libpng (API png_image 1.6+,
 * simples e robusta). Validado no host com as 5 introPictures reais (todas OK).
 * Devolve buffer RGBA (4 bytes/px) via malloc (adotado por bmp_alloc_adopt), ou
 * NULL se nao for PNG valido (cai no stb, como antes). */
/* Toggle do decode 4444 p/ tilesets gigantes. true (default) = tilesets >16MB
 * (em 8888) sao decodificados direto p/ RGBA4444 (metade da RAM) e usados sem
 * embrulho. Por p/ false reverte ao comportamento antigo (magenta nos gigantes).
 * Mantido como flag p/ poder desligar sem recompilar a logica toda. */
static bool g_tileset_4444 = true;

/* mrb_state global (definido em binding_3ds.cpp). Usado APENAS no caminho de
 * falha de alocacao de assets grandes: forca um mrb_full_gc para libertar
 * bitmaps ORFAOS (tileset do mapa anterior desreferenciado mas nao disposto) e
 * abrir espaco em malloc. Helper devolve true se, apos o GC, houver de novo
 * espaco -- o chamador deve RETENTAR a alocacao. Seguro: nunca corre no fluxo
 * normal (so' quando malloc+linear ja' falharam), e a cutscene usa um tileset
 * pequeno que nunca falha, por isso o GC nunca dispara la'. */
extern mrb_state *g_mkxp_mrb;
static int g_gc_oom_calls = 0;
static bool gc_reclaim_on_oom(const char *who) {
    if (!g_mkxp_mrb) return false;
    int live_before = (int)g_mkxp_mrb->gc.live;
    mrb_full_gc(g_mkxp_mrb);
    int live_after = (int)g_mkxp_mrb->gc.live;
    g_gc_oom_calls++;
    if (g_dbglog) {
        fprintf(g_dbglog, "[BMP|GC-OOM] #%d (%s): full_gc live %d->%d (libertou %d objs orfaos) -> retry\n",
                g_gc_oom_calls, who ? who : "?", live_before, live_after, live_before - live_after);
        fflush(g_dbglog);
    }
    return (live_after < live_before);   /* houve algo a libertar -> vale a pena retentar */
}

/* Leitor de PNG a partir de memoria, p/ a API de baixo nivel do libpng (row a
 * row). png_set_read_fn chama isto; o io_ptr aponta p/ este struct. */
struct PngMemRdr { const unsigned char* buf; size_t len; size_t pos; };
static void png_mem_read_cb(png_structp png, png_bytep out, png_size_t n) {
    PngMemRdr* m = (PngMemRdr*)png_get_io_ptr(png);
    if (!m || m->pos + n > m->len) { png_error(png, "read past end"); return; }
    memcpy(out, m->buf + m->pos, (size_t)n);
    m->pos += (size_t)n;
}

static unsigned char* decode_png_libpng(const unsigned char* buf, size_t len,
                                        int* out_w, int* out_h, bool* out_islinear) {
    /* ── FALLBACK PARA LINEAR (descoberto pelo log) ─────────────────────────
     * O [BMP|LIBPNG-ERR] provou que NEM o malloc grande PRE-libpng cabe: um
     * malloc de 5.7MB falha com 17MB de LINEAR livre. Ou seja o heap de malloc
     * esta' esgotado pela pool do mruby (~28MB que nunca devolve), mas o heap
     * LINEAR tem espaco. A memoria de linearAlloc serve igual como b->pixels
     * (e' CPU-legivel; o blt e o upload de textura funcionam na mesma), por isso
     * fazemos malloc PRIMEIRO e, se falhar, linearAlloc. out_islinear diz ao
     * caller para o bitmap_free usar linearFree. As dimensoes vem do IHDR
     * (offsets 16=largura, 20=altura, big-endian); PNG_FORMAT_RGBA => w*h*4. */
    if (out_islinear) *out_islinear = false;
    if (len < 29) return nullptr;   /* sem IHDR completo nao ha' dimensoes */
    unsigned int iw = ((unsigned)buf[16] << 24) | ((unsigned)buf[17] << 16) |
                      ((unsigned)buf[18] << 8)  |  (unsigned)buf[19];
    unsigned int ih = ((unsigned)buf[20] << 24) | ((unsigned)buf[21] << 16) |
                      ((unsigned)buf[22] << 8)  |  (unsigned)buf[23];
    unsigned char bitdepth = buf[24], colortype = buf[25], interlace = buf[28];
    if (iw == 0 || ih == 0 || iw > 65535 || ih > 65535) return nullptr;
    size_t need = (size_t)iw * (size_t)ih * 4u;

    /* malloc primeiro; se o heap de malloc nao tem bloco, cai p/ linearAlloc */
    bool used_linear = false;
    unsigned char* out = (unsigned char*)malloc(need);
    if (!out) {
        out = (unsigned char*)linearAlloc(need);
        used_linear = (out != nullptr);
        if (g_dbglog) {
            fprintf(g_dbglog, "[BMP|LIBPNG-%s] need=%zuKB (%ux%u bd=%u ct=%u il=%u) "
                    "malloc esgotado -> linearAlloc %s (linear_livre=%.0fKB)\n",
                    used_linear ? "LINEAR" : "ERR", need / 1024, iw, ih, bitdepth,
                    colortype, interlace, used_linear ? "OK" : "TAMBEM FALHOU",
                    linearSpaceFree() / 1024.0);
            fflush(g_dbglog);
        }
        if (!out) {
            /* malloc+linear cheios (ex: introPictures 5.6MB com o tileset da vila
             * a ocupar malloc). Reciclar orfaos via GC e RETENTAR antes do magenta. */
            if (gc_reclaim_on_oom("8888")) {
                out = (unsigned char*)malloc(need);
                used_linear = false;
                if (!out) { out = (unsigned char*)linearAlloc(need); used_linear = (out != nullptr); }
            }
            if (!out) return nullptr;
        }
    }
    #define DPL_FREE() do { if (used_linear) linearFree(out); else free(out); } while (0)

    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, buf, len)) {
        /* image.message tem o texto real; woe: bit1=warning bit2=error. */
        if (g_dbglog) {
            fprintf(g_dbglog, "[BMP|LIBPNG-ERR] begin_read FALHOU woe=%u msg='%s' "
                    "(%ux%u bd=%u ct=%u il=%u) len=%zu\n",
                    (unsigned)image.warning_or_error, image.message,
                    iw, ih, bitdepth, colortype, interlace, len);
            fflush(g_dbglog);
        }
        DPL_FREE();
        return nullptr;
    }
    image.format = PNG_FORMAT_RGBA;
    if (PNG_IMAGE_SIZE(image) != need) {
        /* IHDR e libpng discordam no tamanho RGBA -> nao arrisco; cai no antigo. */
        if (g_dbglog) {
            fprintf(g_dbglog, "[BMP|LIBPNG-ERR] tamanho IHDR(%zuKB) != libpng(%zuKB) "
                    "-- abortado sem risco\n", need / 1024,
                    (size_t)PNG_IMAGE_SIZE(image) / 1024);
            fflush(g_dbglog);
        }
        png_image_free(&image);
        DPL_FREE();
        return nullptr;
    }
    if (!png_image_finish_read(&image, nullptr, out, 0, nullptr)) {
        /* Buffer JA' alocado com sucesso -> falha aqui e' do decode em si
         * (estado/zlib interno do libpng), nao do buffer principal. */
        if (g_dbglog) {
            fprintf(g_dbglog, "[BMP|LIBPNG-ERR] finish_read FALHOU woe=%u msg='%s' "
                    "(%ux%u bd=%u ct=%u il=%u) need=%zuKB linear_livre=%.0fKB\n",
                    (unsigned)image.warning_or_error, image.message,
                    iw, ih, bitdepth, colortype, interlace, need / 1024,
                    linearSpaceFree() / 1024.0);
            fflush(g_dbglog);
        }
        DPL_FREE();
        return nullptr;   /* png_image liberta-se sozinho no finish_read */
    }
    #undef DPL_FREE
    if (g_dbglog) {
        fprintf(g_dbglog, "[BMP|LIBPNG-IHDR] '%ux%u' bd=%u ct=%u il=%u OK heap=%s\n",
                iw, ih, bitdepth, colortype, interlace, used_linear ? "LINEAR" : "malloc");
        fflush(g_dbglog);
    }
    if (out_islinear) *out_islinear = used_linear;
    *out_w = (int)image.width;
    *out_h = (int)image.height;
    return out;
}

/* ── DECODE DIRETO PARA RGBA4444 (tilesets gigantes) ────────────────────────
 * O 8888 de um tileset 256x31040 = 30MB nao cabe (heap de malloc esgotado pela
 * pool; linear so' tem ~16MB). Em 4444 = 15MB, cabe. Aqui usamos a API de BAIXO
 * NIVEL do libpng (png_read_row) p/ decodificar LINHA A LINHA, empacotando cada
 * linha 8888->4444 num buffer de 15MB. O scratch e' UMA linha 8888 (~1KB), nunca
 * os 30MB inteiros. Devolve buffer 4444 (u16/px) via malloc, ou linearAlloc se o
 * malloc esgotou (out_islinear marca-o p/ bitmap_free usar linearFree).
 * REQUER il=0 (sem entrelacamento): o entrelacado precisaria da imagem toda em
 * RAM, o que anula o ganho; estes tilesets sao il=0 (confirmado no log). Em
 * QUALQUER falha devolve NULL -> o chamador cai no decode 8888 antigo (magenta),
 * zero regressao. As variaveis libertadas no handler de erro sao 'volatile'
 * (exigencia do setjmp/longjmp p/ sobreviverem ao salto). */
static unsigned char* decode_png_to_4444(const unsigned char* buf, size_t len,
                                         int* out_w, int* out_h, bool* out_islinear) {
    if (out_islinear) *out_islinear = false;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) return nullptr;
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, nullptr, nullptr); return nullptr; }

    unsigned char* volatile out = nullptr;
    bool volatile used_linear = false;
    png_bytep volatile rowbuf = nullptr;
    png_bytep volatile full = nullptr;       /* buffer RGBA completo p/ PNG entrelaçado */
    png_bytep* volatile rows = nullptr;      /* ponteiros de linha p/ entrelaçado */
    bool volatile full_linear = false;
    if (setjmp(png_jmpbuf(png))) {                 /* erro do libpng -> limpa+falha */
        if (rowbuf) free((void*)rowbuf);
        if (rows) free((void*)rows);
        if (full) { if (full_linear) linearFree((void*)full); else free((void*)full); }
        if (out) { if (used_linear) linearFree((void*)out); else free((void*)out); }
        png_destroy_read_struct(&png, &info, nullptr);
        if (g_dbglog) { fprintf(g_dbglog, "[BMP|PNG4444-ERR] libpng longjmp (decode)\n"); fflush(g_dbglog); }
        return nullptr;
    }

    PngMemRdr rdr = { buf, len, 0 };
    png_set_read_fn(png, &rdr, png_mem_read_cb);
    png_read_info(png, info);

    png_uint_32 w = 0, h = 0; int bitd = 0, colort = 0, interl = 0;
    png_get_IHDR(png, info, &w, &h, &bitd, &colort, &interl, nullptr, nullptr);
    if (w == 0 || h == 0) {
        png_destroy_read_struct(&png, &info, nullptr);
        if (g_dbglog) { fprintf(g_dbglog, "[BMP|PNG4444-ERR] dims invalidas %ux%u (il=%d)\n", (unsigned)w, (unsigned)h, interl); fflush(g_dbglog); }
        return nullptr;
    }
    /* normalizar tudo p/ RGBA 8-bit por linha (estes sao ct=6 bd=8 -> sem-op) */
    if (colort == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (colort == PNG_COLOR_TYPE_GRAY && bitd < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (bitd == 16) png_set_strip_16(png);
    if (colort == PNG_COLOR_TYPE_GRAY || colort == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    if (colort == PNG_COLOR_TYPE_RGB || colort == PNG_COLOR_TYPE_GRAY ||
        colort == PNG_COLOR_TYPE_PALETTE)
        png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    /* PNG entrelaçado (Adam7) tem de ser lido com TODOS os passes; isto activa o
       tratamento e devolve o nr de passes (1 = nao-entrelaçado, 7 = entrelaçado). */
    int n_passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);

    size_t need4444 = (size_t)w * (size_t)h * 2u;     /* u16 por pixel */
    /* CENSO antes do decode: precisa vs livre vs maior_bloco contiguo, malloc,
     * pool de texturas e quanto seria evictavel -> diz logo se vai caber, se e'
     * fragmentacao, ou se falta mesmo. (so' p/ tilesets grandes 4444, nao spam) */
    { extern void mkxp_mem_census(const char*, unsigned);
      mkxp_mem_census("tileset-pre", (unsigned)need4444); }
    out = (unsigned char*)malloc(need4444);
    if (!out) { out = (unsigned char*)linearAlloc(need4444); used_linear = (out != nullptr); }
    if (!out) {
        /* malloc+linear cheios. Antes de desistir (magenta), reciclar bitmaps
         * ORFAOS via GC e RETENTAR. E' isto que liberta o tileset do mapa
         * anterior (ex: vila 15.5MB) que ficou desreferenciado mas nao disposto
         * -> o tileset do mapa novo (ex: quarto 8.5MB) passa a caber. */
        if (gc_reclaim_on_oom("4444")) {
            out = (unsigned char*)malloc(need4444);
            if (!out) { out = (unsigned char*)linearAlloc(need4444); used_linear = (out != nullptr); }
        }
    }
    if (!out) {
        /* O GC nao chegou: a memoria nao esta' em bitmaps Ruby orfaos, esta' em
         * TEXTURAS C3D vivas da cena ANTERIOR (ex: a intro acabada de sair, cujas
         * imagens ainda ocupam o heap linear). Evicta-as p/ libertar linear e
         * RETENTA. So' corre quando o tileset ja' ia ser magenta -> nunca piora
         * um caso bom; a textura evictada e' recriada do b->pixels no proximo
         * blit. O log [TEX|EVICT-LIN] mostra quanto libertou vs o que precisa. */
        extern unsigned display_3ds_evict_linear(unsigned want_bytes);
        if (display_3ds_evict_linear((unsigned)need4444) > 0) {
            out = (unsigned char*)malloc(need4444);
            if (!out) { out = (unsigned char*)linearAlloc(need4444); used_linear = (out != nullptr); }
        }
    }
    if (!out) {
        png_destroy_read_struct(&png, &info, nullptr);
        { extern void mkxp_mem_census(const char*, unsigned);
          mkxp_mem_census("tileset-FALHOU", (unsigned)need4444); }
        if (g_dbglog) { fprintf(g_dbglog, "[BMP|PNG4444-ERR] alloc 4444 FALHOU (mesmo apos GC+EVICT) need=%zuKB linear_livre=%.0fKB\n", need4444 / 1024, linearSpaceFree() / 1024.0); fflush(g_dbglog); }
        return nullptr;
    }
    rowbuf = (png_bytep)malloc((size_t)w * 4u);
    if (!rowbuf) {
        if (used_linear) linearFree((void*)out); else free((void*)out);
        png_destroy_read_struct(&png, &info, nullptr);
        return nullptr;
    }
    unsigned short* o16 = (unsigned short*)out;
    png_bytep rb = (png_bytep)rowbuf;
    if (n_passes <= 1) {
        /* nao-entrelaçado: linha-a-linha (eficiente em memoria) */
        for (png_uint_32 y = 0; y < h; y++) {
            png_read_row(png, rb, nullptr);
            size_t base = (size_t)y * (size_t)w;
            for (png_uint_32 x = 0; x < w; x++) {
                unsigned char r = rb[x * 4 + 0], g = rb[x * 4 + 1],
                              b = rb[x * 4 + 2], a = rb[x * 4 + 3];
                o16[base + x] = (unsigned short)(((r >> 4) << 12) | ((g >> 4) << 8) |
                                                 ((b >> 4) << 4) | (a >> 4));
            }
        }
    } else {
        /* ENTRELAÇADO (Adam7): os passes interleavam-se, nao da' p/ ler linha-a-linha.
           Aloca um RGBA completo temporario, le a imagem toda, depois converte p/ 4444.
           (Ex: introPictures3.png 1696x864 -> ~5.7MB temporario.) */
        size_t fullsz = (size_t)w * (size_t)h * 4u;
        full = (png_bytep)malloc(fullsz);
        if (!full) { full = (png_bytep)linearAlloc(fullsz); full_linear = (full != nullptr); }
        if (!full && gc_reclaim_on_oom("il4444")) {
            full = (png_bytep)malloc(fullsz);
            if (!full) { full = (png_bytep)linearAlloc(fullsz); full_linear = (full != nullptr); }
        }
        if (!full) {
            free((void*)rowbuf); rowbuf = nullptr;
            if (used_linear) linearFree((void*)out); else free((void*)out);
            png_destroy_read_struct(&png, &info, nullptr);
            if (g_dbglog) { fprintf(g_dbglog, "[BMP|PNG4444-ERR] alloc RGBA p/ entrelacado FALHOU %ux%u (%zuKB)\n", (unsigned)w, (unsigned)h, fullsz / 1024); fflush(g_dbglog); }
            return nullptr;
        }
        rows = (png_bytep*)malloc((size_t)h * sizeof(png_bytep));
        if (!rows) {
            if (full_linear) linearFree((void*)full); else free((void*)full); full = nullptr;
            free((void*)rowbuf); rowbuf = nullptr;
            if (used_linear) linearFree((void*)out); else free((void*)out);
            png_destroy_read_struct(&png, &info, nullptr);
            return nullptr;
        }
        for (png_uint_32 y = 0; y < h; y++) ((png_bytep*)rows)[y] = (png_bytep)full + (size_t)y * (size_t)w * 4u;
        png_read_image(png, (png_bytep*)rows);   /* le todos os passes de uma vez */
        for (png_uint_32 y = 0; y < h; y++) {
            png_bytep src = ((png_bytep*)rows)[y];
            size_t base = (size_t)y * (size_t)w;
            for (png_uint_32 x = 0; x < w; x++) {
                unsigned char r = src[x * 4 + 0], g = src[x * 4 + 1],
                              b = src[x * 4 + 2], a = src[x * 4 + 3];
                o16[base + x] = (unsigned short)(((r >> 4) << 12) | ((g >> 4) << 8) |
                                                 ((b >> 4) << 4) | (a >> 4));
            }
        }
        free((void*)rows); rows = nullptr;
        if (full_linear) linearFree((void*)full); else free((void*)full); full = nullptr;
        if (g_dbglog) { fprintf(g_dbglog, "[BMP|PNG4444] entrelacado %ux%u OK (%d passes)\n", (unsigned)w, (unsigned)h, n_passes); fflush(g_dbglog); }
    }
    png_read_end(png, nullptr);
    free((void*)rowbuf); rowbuf = nullptr;
    png_destroy_read_struct(&png, &info, nullptr);
    if (g_dbglog) {
        fprintf(g_dbglog, "[BMP|PNG4444] '%ux%u' bd=%d ct=%d -> RGBA4444 %zuKB heap=%s OK\n",
                (unsigned)w, (unsigned)h, bitd, colort, need4444 / 1024,
                used_linear ? "LINEAR" : "malloc");
        fflush(g_dbglog);
    }
    if (out_islinear) *out_islinear = (bool)used_linear;
    *out_w = (int)w;
    *out_h = (int)h;
    return (unsigned char*)out;
}

/* ── FIX CRÍTICO: usar mrb_get_argc em vez de mrb_get_args("*") ──────────────
 * Em mruby 3.2, chamar mrb_get_args(mrb, "*", &argv, &argc) pode "consumir"
 * os argumentos do frame actual, fazendo com que chamadas subsequentes a
 * mrb_get_args(mrb, "o"/"ii", ...) vejam argc=0 e lancem
 * ArgumentError: wrong number of arguments (given N, expected 0).
 *
 * mrb_get_argc(mrb) lê o contador sem modificar o estado do frame, sendo
 * seguro para usar antes de qualquer outra chamada a mrb_get_args.
 * ─────────────────────────────────────────────────────────────────────────── */

struct Color3 { float r, g, b, a; };
struct Rect3  { int   x, y, w, h; };

static bool extract_rect(mrb_state *mrb, mrb_value obj, int *x, int *y, int *w, int *h) {
    void *ptr = DATA_PTR(obj);
    if (ptr) {
        Rect3 *r = (Rect3*)ptr;
        *x = r->x; *y = r->y; *w = r->w; *h = r->h;
        return true;
    }
    mrb_value vx = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "@x"));
    mrb_value vy = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "@y"));
    mrb_value vw = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "@width"));
    mrb_value vh = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "@height"));
    if (mrb_nil_p(vx)) return false;
    *x = (int)mrb_integer(mrb_to_int(mrb, vx));
    *y = (int)mrb_integer(mrb_to_int(mrb, vy));
    *w = (int)mrb_integer(mrb_to_int(mrb, vw));
    *h = (int)mrb_integer(mrb_to_int(mrb, vh));
    return true;
}

static bool extract_color(mrb_state *mrb, mrb_value obj, float *r, float *g, float *b, float *a) {
    void *ptr = DATA_PTR(obj);
    if (ptr) {
        Color3 *c = (Color3*)ptr;
        *r = c->r; *g = c->g; *b = c->b; *a = c->a;
        return true;
    }
    mrb_value vr = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "@red"));
    mrb_value vg = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "@green"));
    mrb_value vb = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "@blue"));
    mrb_value va = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "@alpha"));
    if (mrb_nil_p(vr)) return false;
    *r = (float)mrb_float(mrb_to_float(mrb, vr));
    *g = (float)mrb_float(mrb_to_float(mrb, vg));
    *b = (float)mrb_float(mrb_to_float(mrb, vb));
    *a = (float)mrb_float(mrb_to_float(mrb, va));
    return true;
}

/* SUPERDBG: contador de Bitmaps vivos (lido por MKXPDebug.mem_info no SNAP). */
int g_live_bitmaps = 0;

static void bitmap_free(mrb_state*, void *p) {
    Bitmap3DS *b = (Bitmap3DS*)p;
    if (!b) return;
    g_live_bitmaps--;   /* SUPERDBG */
    if (b->pixels) {
        if (b->pixels_linear) linearFree(b->pixels);  /* veio de linearAlloc */
        else                  free(b->pixels);
    }
    if (b->tex)    display_3ds_free_texture(b->tex);
    free(b);
}
const mrb_data_type Bitmap3DSType = { "Bitmap", bitmap_free };

#define GET_BMP(s) ((Bitmap3DS*)DATA_PTR(s))

static Bitmap3DS *bmp_alloc(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    Bitmap3DS *b  = (Bitmap3DS*)calloc(1, sizeof(Bitmap3DS));
    if (!b) return nullptr;                  /* extremo: nem a struct coube */
    g_live_bitmaps++;   /* SUPERDBG */
    b->width      = w;
    b->height     = h;
    size_t nbytes = (size_t)w * (size_t)h * 4;

    /* caminho rapido: heap normal (calloc). */
    b->pixels     = (unsigned char*)calloc(nbytes, 1);

    /* FIX (cartao vazio + qualquer bitmap in-RAM sob pressao de memoria):
     * Sob pressao -- ex.: abrir o Trainer Card LOGO a seguir as intro Pictures
     * gigantes (1696x864 RGBA) que enchem o heap -- este calloc FALHAVA e
     * b->pixels ficava NULL, MAS b->width/height continuavam 512x384. Como
     * bmp_clear e bmp_draw_text guardam ambos `!b->pixels` e RETORNAM, o
     * overlay do cartao desenhava-se VAZIO: sem texto, e ate' sem a linha de
     * diagnostico [CARTAO-TXT] (o draw_text saia no 1o if, antes do log).
     * Recuperacao em cascata (so' corre no caminho de FALHA -> custo zero
     * no fluxo normal), reaproveitando as ferramentas que ja' tens:
     *   (1) full_gc liberta bitmaps Ruby ORFAOS e retenta no heap normal;
     *   (2) heap LINEAR (separado do pool do mruby; no log o linear_livre
     *       era ~24MB > 768KB que o overlay precisa) e marca pixels_linear;
     *   (3) se ate' o linear estiver cheio de texturas, display_3ds_evict_linear
     *       liberta-as (recriadas do b->pixels no proximo blit) e retenta. */
    if (!b->pixels && nbytes > 0) {
        for (int tries = 0; tries < 3 && !b->pixels; tries++) {
            if (!gc_reclaim_on_oom("bmp_alloc")) break;   /* nada a libertar */
            b->pixels = (unsigned char*)calloc(nbytes, 1);
        }
        if (!b->pixels) {
            b->pixels = (unsigned char*)linearAlloc(nbytes);
            if (!b->pixels) {
                /* linear tambem esgotado -> evictar texturas e retentar */
                extern unsigned display_3ds_evict_linear(unsigned want_bytes);
                if (display_3ds_evict_linear((unsigned)nbytes) > 0)
                    b->pixels = (unsigned char*)linearAlloc(nbytes);
            }
            if (b->pixels) {
                memset(b->pixels, 0, nbytes);
                b->pixels_linear = true;
                if (g_dbglog) {
                    fprintf(g_dbglog,
                        "[BMP|ALLOC-OOM] bmp_alloc(%dx%d=%uKB): heap normal esgotado -> linearAlloc OK (cartao/overlay salvo)\n",
                        w, h, (unsigned)(nbytes / 1024));
                    fflush(g_dbglog);
                }
            } else if (g_dbglog) {
                fprintf(g_dbglog,
                    "[BMP|ALLOC-FATAL] bmp_alloc(%dx%d=%uKB): calloc+GC+linearAlloc+EVICT TODOS falharam -> bitmap sem pixels\n",
                    w, h, (unsigned)(nbytes / 1024));
                fflush(g_dbglog);
            }
        }
    }

    b->tex_dirty  = true;
    b->tex        = nullptr;
    return b;
}

/* FIX MEMORIA (tilesets gigantes 256x31040 = 30MB que NAO carregavam): o caminho
 * normal era stbi_load (aloca 30MB) + bmp_alloc (aloca OUTROS 30MB) + memcpy ->
 * PICO de 60MB. No Old 3DS (RAM ~64MB) isso esgota a memoria e o load FALHA (->
 * MISS -> tile magenta). Esta variante faz o Bitmap ADOTAR o buffer que o stbi
 * ja' alocou (mesmo allocator: stb usa malloc/free, e bitmap_free usa free), em
 * vez de alocar um segundo buffer e copiar. Resultado: pico cai de 60MB p/ 30MB
 * (so' o buffer do stbi), sem memcpy. Generico: qualquer imagem grande beneficia. */
static Bitmap3DS *bmp_alloc_adopt(int w, int h, unsigned char *owned_pixels,
                                  bool is_linear = false) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    Bitmap3DS *b  = (Bitmap3DS*)calloc(1, sizeof(Bitmap3DS));
    b->width      = w;
    b->height     = h;
    b->pixels     = owned_pixels;   /* adota -- libertado por bitmap_free */
    b->pixels_linear = is_linear;   /* true -> bitmap_free usa linearFree */
    b->tex_dirty  = true;
    b->tex        = nullptr;
    g_live_bitmaps++;   /* SUPERDBG */
    return b;
}

void __attribute__((used)) bmp_flush(Bitmap3DS *b) {
    if (!b || !b->tex_dirty || b->disposed) return;
    /* Um bitmap RGBA4444 e' SO' fonte de blt (tileset gigante) -> nunca textura.
     * (Os 4444 sao sempre >1024 pela regra dos 16MB, logo cairiam no tex_failed
     * abaixo de qualquer forma; este guard explicita e evita ler 4444 como 8888
     * em display_3ds_create_texture caso a regra mude.) */
    if (b->pixels_4444) { b->tex_failed = true; b->tex_dirty = false; return; }
    /* FIX RAIZ (tilesets gigantes a falhar + magenta + 2.2 fps): o PICA200 so'
     * suporta texturas ate' 1024x1024. Tilesets do Essentials sao 256x31040 (!),
     * impossiveis de meter numa textura. MAS esses bitmaps gigantes NUNCA sao
     * desenhados como sprite -- servem so' de FONTE de blt (cópia RAM->RAM) para
     * o layer do tilemap (832x704, que cabe). O blt opera em b->pixels (RAM), nao
     * precisa de textura GPU. Por isso: se o bitmap excede 1024 em qualquer eixo,
     * NAO tentamos criar a textura (que falharia, deixava magenta e re-tentava
     * todos os frames). Deixamos os pixels em RAM (intactos p/ o blt) e marcamos
     * tex_failed (o draw ignora; se algum dia for desenhado direto, nao re-tenta).
     * Isto resolve os 32 tilesets gigantes do jogo sem tocar nos scripts. */
    if (b->width > 1024 || b->height > 1024) {
        b->tex_failed = true;
        b->tex_dirty  = false;   /* pixels continuam validos em RAM p/ blt */
        return;
    }
    if (b->tex) {
        display_3ds_free_texture(b->tex);
        b->tex = nullptr;
    }
    if (b->pixels && b->width > 0 && b->height > 0) {
        b->tex = display_3ds_create_texture(b->width, b->height, b->pixels, b->prefer_vram);
        if (!b->tex) {
            /* FIX FPS: marcar que falhou p/ nao re-tentar todos os frames (o
             * draw verifica tex_failed). So' se re-tenta quando tex_dirty voltar
             * a true (pixels mudaram). Caso tipico: tileset >1024 (POT 1024 nao
             * chega) ou VRAM esgotada. Sem isto, loop de C3D_TexInit -> 2.2 fps. */
            b->tex_failed = true;
            printf("[BMP] AVISO: create_texture falhou %dx%d\n", b->width, b->height);
        }
        else {
            /* [EVICT] registar o slot do dono para eviccao segura: se esta
             * textura for libertada por falta de heap, b->tex fica a nullptr e
             * e' recriada a partir de b->pixels (intactos em RAM) quando precisar. */
            display_3ds_set_texture_owner(b->tex, &b->tex);
            if (b->has_text) {
                /* texto: LINEAR no downscale (GPU, custo zero) -> legivel em vez de papa */
                display_3ds_set_texture_filter(b->tex, true);
                b->tex->has_text = true;   /* DIAG: marca a textura p/ log TEXFLOW no ecra */
            }
        }
    }
    b->tex_dirty = false;
}

void bmp_ensure_texture(Bitmap3DS *b) {
    bmp_flush(b);
}

static const char *s_game_roots[] = {
    "sdmc:/mkxp/game/",
    "sdmc:/mkxp/",
    "romfs:/",
    "",
    nullptr
};

/* FIX RAIZ (recursos em subpastas que nao carregam, ex: background do teclado
 * "Naming/bg_2"): o FileTest.exist? do Ruby (e logo safeExists? e pbResolveBitmap)
 * devolvia SEMPRE false. Por isso pbResolveBitmap("Graphics/Pictures/Naming/bg_2")
 * dava nil, e addBackgroundPlane fazia bitmap=nil -> Plane invisivel -> via-se a
 * faixa amber (introPlayerbg) por tras. Esta funcao testa MESMO o ficheiro no
 * filesystem do 3DS, percorrendo as raizes do jogo (s_game_roots) e as extensoes
 * comuns -- a MESMA logica do loader bmp_load_file, por isso o que esta funcao diz
 * existir, o loader consegue carregar. Exposta ao Ruby como Bitmap._native_exist?
 * e usada pelo FileTest.exist?. Generico p/ QUALQUER recurso em qualquer subpasta. */
/* fwd decl (definicao mais abaixo): tem de ser a' escala de namespace porque
 * 'extern "C"' NAO e' permitido dentro do corpo de uma funcao (erro C++:
 * "expected unqualified-id before string constant"). */
extern "C" int mkxp_native_dir_exists(const char *path);
extern "C" int mkxp_native_file_exists(const char *path) {
    if (!path || path[0] == '\0') return 0;
    static const char *exts[] = { "", ".png", ".gif", ".jpg", ".jpeg", ".bmp",
                                  ".PNG", ".GIF", ".JPG", ".BMP", ".ogg", ".wav",
                                  ".dat", ".rxdata", ".txt", nullptr };
    for (int ri = 0; s_game_roots[ri] != nullptr; ri++) {
        const char *root = s_game_roots[ri];
        for (int ei = 0; exts[ei] != nullptr; ei++) {
            char full[512];
            if (root[0] != '\0' && path[0] == '/') {
                snprintf(full, sizeof(full), "%s%s%s", root, path + 1, exts[ei]);
            } else {
                snprintf(full, sizeof(full), "%s%s%s", root, path, exts[ei]);
            }
            /* fopen em vez de stat(): mais portavel no devkitARM/3DS (nao precisa
             * de <sys/stat.h>) e testa exactamente o que importa -- se o ficheiro
             * abre para leitura. E' a mesma verificacao que o loader bmp_load_file
             * faz logo a seguir, por isso o que abre aqui, o loader carrega. */
            FILE *fp = fopen(full, "rb");
            if (fp) { fclose(fp); return 1; }
        }
    }
    /* O FileTest.exist? do Ruby tambem e' true para PASTAS, nao so' ficheiros. O
     * loop acima so' testa fopen (ficheiros) -> falha numa pasta (ainda mais com
     * '/' final). Por isso o menu Voltseon, que faz safeExists?(MENU_FILE_PATH)
     * sobre "Graphics/Pictures/Voltseon's Pause Menu/", dava sempre "directory
     * does not exist" apesar de a pasta estar no SD. Tenta como diretorio. */
    if (mkxp_native_dir_exists(path)) return 1;
    return 0;
}

/* Como mkxp_native_file_exists, mas para PASTAS: tenta abrir o caminho como
 * diretorio (opendir) em cada raiz do jogo. Devolve 1 se ABRE numa raiz. Isto
 * corrige o FileTest.directory? do porte, que estava fixo em false -> qualquer
 * jogo/plugin que valida a existencia de uma pasta (ex: o menu de pausa Voltseon,
 * que verifica MENU_FILE_PATH e mostrava "This directory does not exist") recebia
 * sempre erro mesmo com a pasta presente no SD. SEM extensoes (e' pasta). Se
 * opendir falhar em todas -> 0 (= comportamento antigo, zero regressao). */
extern "C" int mkxp_native_dir_exists(const char *path) {
    if (!path || path[0] == '\0') return 0;
    for (int ri = 0; s_game_roots[ri] != nullptr; ri++) {
        const char *root = s_game_roots[ri];
        char full[512];
        if (root[0] != '\0' && path[0] == '/') {
            snprintf(full, sizeof(full), "%s%s", root, path + 1);
        } else {
            snprintf(full, sizeof(full), "%s%s", root, path);
        }
        /* tira barra final (opendir do newlib/3ds e' esquisito com 'dir/') */
        size_t fl = strlen(full);
        while (fl > 1 && (full[fl-1] == '/' || full[fl-1] == '\\')) full[--fl] = '\0';
        DIR *d = opendir(full);
        if (d) { closedir(d); return 1; }
    }
    return 0;
}

/* Wrapper Ruby: Bitmap._native_exist?(path) -> true/false. Metodo de classe de
 * Bitmap (Bitmap ja' existe como binding, e' o sitio natural sem criar classes). */
static mrb_value bmp_native_exist(mrb_state *mrb, mrb_value) {
    const char *p = nullptr; mrb_int plen = 0;
    mrb_get_args(mrb, "s", &p, &plen);
    char buf[512];
    int n = (plen < (mrb_int)sizeof(buf)-1) ? (int)plen : (int)sizeof(buf)-1;
    memcpy(buf, p, n); buf[n] = '\0';
    return mkxp_native_file_exists(buf) ? mrb_true_value() : mrb_false_value();
}

/* Wrapper Ruby: Bitmap._native_dir_exist?(path) -> true/false. Liga o
 * FileTest.directory? a um opendir real nas raizes do jogo (corrige o stub
 * fixo em false que partia validacoes de pasta, ex: menu Voltseon). */
static mrb_value bmp_native_dir_exist(mrb_state *mrb, mrb_value) {
    const char *p = nullptr; mrb_int plen = 0;
    mrb_get_args(mrb, "s", &p, &plen);
    char buf[512];
    int n = (plen < (mrb_int)sizeof(buf)-1) ? (int)plen : (int)sizeof(buf)-1;
    memcpy(buf, p, n); buf[n] = '\0';
    return mkxp_native_dir_exists(buf) ? mrb_true_value() : mrb_false_value();
}

/* true se este Bitmap esta' guardado em RGBA4444 (tileset gigante). O Ruby
 * (CustomTilemap) usa isto p/ NAO embrulhar (o 4444 e' usado direto como fonte). */
static mrb_value bmp_native_4444(mrb_state *mrb, mrb_value self) {
    Bitmap3DS *b = (Bitmap3DS*)DATA_PTR(self);
    (void)mrb;
    return (b && b->pixels_4444) ? mrb_true_value() : mrb_false_value();
}

static void str_tolower(char *s) {
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32;
}

/* ── Cache NEGATIVA de loads falhados ───────────────────────────────────────
 * O log mostrou o MESMO tileset (ex: 'Gen4 Interior' 16.6MB) pedido e a FALHAR
 * 24x: cada tentativa lia 232KB do SD + tentava alocar (4444 + linear) + stb,
 * tudo a falhar, TODOS os frames -> desperdicio enorme + spam no log. Isto
 * regista os caminhos ja' falhados e devolve o fallback IMEDIATAMENTE (sem IO
 * nem decode) nos pedidos seguintes. Limpa-se em mudanca de mapa (via
 * Bitmap._clear_failed_loads, chamado no CustomTilemap#tileset=), p/ um mapa
 * novo voltar a tentar os seus tilesets de raiz. */
static unsigned int s_failed_hash[32];
static int          s_failed_n = 0;
static unsigned int dy_djb2(const char* s) {
    unsigned int h = 5381; for (; *s; ++s) h = ((h << 5) + h) ^ (unsigned char)*s; return h;
}
static bool path_failed_before(const char* path) {
    unsigned int h = dy_djb2(path);
    for (int i = 0; i < s_failed_n; i++) if (s_failed_hash[i] == h) return true;
    return false;
}
static void mark_path_failed(const char* path) {
    unsigned int h = dy_djb2(path);
    for (int i = 0; i < s_failed_n; i++) if (s_failed_hash[i] == h) return;  /* ja' marcado */
    if (s_failed_n < (int)(sizeof(s_failed_hash)/sizeof(s_failed_hash[0])))
        s_failed_hash[s_failed_n++] = h;   /* cheio (>32 falhas/mapa, improvavel): para de somar */
    /* Marca de CONFIRMACAO: dispara UMA vez por caminho falhado. Se isto aparece
     * no log, a cache negativa esta' ativa neste build (e os pedidos seguintes
     * deste caminho ja' nao re-tentam). Uma linha por tileset, nao e' spam. */
    extern FILE *g_dbglog;
    if (g_dbglog) {
        fprintf(g_dbglog, "[BMP|NCACHE] '%s' marcado falhado -> proximos pedidos = fallback imediato (cache ON)\n", path);
        fflush(g_dbglog);
    }
}
/* Nao-static p/ ser chamavel pelo wrapper Ruby (bmp_clear_failed_loads). */
void mkxp_bmp_clear_failed_loads() { s_failed_n = 0; }

/* Constroi o bitmap 32x32 de fallback: TRANSPARENTE p/ Pictures (decorativo,
 * invisivel como no PC), MAGENTA p/ o resto (asset em falta = erro visivel). */
static Bitmap3DS *make_fallback_bitmap(const char *path) {
    /* TRANSPARENTE (invisivel, como no PC) para assets DECORATIVOS em falta:
     * Pictures/ e Windowskins/. A windowskin 'speech black' nao existe neste
     * jogo -> antes ficava MAGENTA (a mancha rosa na caixa do nome "Auntie").
     * Agora fica transparente: a moldura desaparece mas o texto continua legivel.
     * O resto (charsets, tilesets...) continua magenta = erro bem visivel. */
    bool transparent = false;
    if (path) {
        for (const char *p = path; *p; p++) {
            if ((p[0]=='P'||p[0]=='p') &&
                (strncmp(p, "Pictures/", 9) == 0 || strncmp(p, "pictures/", 9) == 0)) {
                transparent = true; break;
            }
            if ((p[0]=='W'||p[0]=='w') &&
                (strncmp(p, "Windowskins/", 12) == 0 || strncmp(p, "windowskins/", 12) == 0)) {
                transparent = true; break;
            }
        }
    }
    Bitmap3DS *b = bmp_alloc(32, 32);
    unsigned char rc = transparent ? 0 : 255;   /* R e B; G=0 sempre */
    unsigned char ac = transparent ? 0 : 255;   /* alpha */
    for (int i = 0; i < 32 * 32; i++) {
        b->pixels[i*4+0] = rc; b->pixels[i*4+1] = 0;
        b->pixels[i*4+2] = rc; b->pixels[i*4+3] = ac;
    }
    b->tex_dirty = true;
    bmp_flush(b);
    return b;
}

static Bitmap3DS *bmp_load_file(const char *path) {
    if (!path || path[0] == '\0') {
        printf("[BMP] AVISO: bmp_load_file chamado com path VAZIO\n");
        fflush(stdout);
        Bitmap3DS *b = bmp_alloc(32, 32);
        for (int i = 0; i < 32 * 32; i++) {
            b->pixels[i*4+0] = 255;
            b->pixels[i*4+1] = 0;
            b->pixels[i*4+2] = 255;
            b->pixels[i*4+3] = 255;
        }
        b->tex_dirty = true;
        bmp_flush(b);
        return b;
    }

    /* CACHE NEGATIVA: se este caminho ja' falhou neste mapa, devolve o fallback
     * sem reler o ficheiro nem re-tentar o decode (mata o storm de 24x/frame). */
    if (path_failed_before(path)) return make_fallback_bitmap(path);
    /* FIX/PREVENCAO imagens que nao carregam: a lista de extensoes tinha de
     * cobrir TODAS as que o Essentials usa. Faltava .gif (usado em varias
     * imagens; o pbResolveBitmap tenta .png E .gif). Sem .gif aqui, uma imagem
     * .gif resolvia o nome mas o stbi_load falhava -> quadrado magenta. Tambem
     * incluimos variantes em MAIUSCULAS (.PNG/.GIF) porque alguns ficheiros do
     * jogo usam extensao maiuscula (ex: uparrow.PNG) e o filesystem do 3DS e'
     * sensivel a maiusculas/minusculas. */
    static const char *exts[] = { "", ".png", ".gif", ".jpg", ".jpeg", ".bmp",
                                  ".PNG", ".GIF", ".JPG", ".BMP", nullptr };

    for (int ri = 0; s_game_roots[ri] != nullptr; ri++) {
        for (int ei = 0; exts[ei] != nullptr; ei++) {
            char full[512];
            const char *root = s_game_roots[ri];
            if (root[0] != '\0' && path[0] == '/') {
                snprintf(full, sizeof(full), "%s%s%s", root, path + 1, exts[ei]);
            } else {
                snprintf(full, sizeof(full), "%s%s%s", root, path, exts[ei]);
            }

            int w, h, ch;

            /* ── [M1] CURTO-CIRCUITO de tilesets gigantes ───────────────────────
             * Tilesets Gen4 do Essentials sao 256x31040 (30.3MB) / 256x29632
             * (28.9MB). Em Old 3DS NUNCA cabem em RAM (malloc livre cai a ~1.4MB
             * tarde no mapa), logo o stbi_load gasta ~768ms a ler+tentar alocar
             * e FALHA na mesma -> magenta. Aqui lemos so' o CABECALHO (stbi_info,
             * barato) ANTES do decode caro: se a imagem excede um limite que nem
             * com VWrap caberia, saltamos o decode e vamos direto ao fallback.
             * SEGURANCA: so' salta quando stbi_info TEM SUCESSO e reporta tamanho
             * acima do limite. Se stbi_info falhar (ex: GIF que o info nao le mas
             * o load leria), NAO salta -> comportamento inalterado, zero regressao.
             * Limite 24MB: apanha os Gen4 (28-30MB) e nada mais (um tileset de
             * 8-16MB ainda e' tentado, pode carregar via adopt+blt-de-RAM). */
            {
                extern bool g_mkxp_is_new3ds;   /* definido em main.cpp */
                /* No New 3DS ha' RAM estendida: permitimos tilesets ate' 48MB
                 * (apanha os Gen4 de 28-30MB). No Old 3DS mantemos 24MB -- acima
                 * disso o malloc nunca tem espaco e o decode so' gasta ~768ms p/
                 * falhar na mesma. */
                const long M1_MAX_DECODE_BYTES =
                    (g_mkxp_is_new3ds ? 48L : 24L) * 1024 * 1024;
                int iw = 0, ih = 0, ic = 0;
                if (stbi_info(full, &iw, &ih, &ic)) {
                    long need = (long)iw * ih * 4;
                    if (need > M1_MAX_DECODE_BYTES) {
                        printf("[BMP|SKIP-BIG] '%s' info=%dx%d (%.1fMB) > limite %ldMB "
                               "-- decode SALTADO [M1]%s; fallback magenta\n",
                               full, iw, ih, need / 1048576.0,
                               M1_MAX_DECODE_BYTES / (1024 * 1024),
                               g_mkxp_is_new3ds ? " (New 3DS)" : "");
                        if (g_dbglog)
                            fprintf(g_dbglog, "[BMP|SKIP-BIG] '%s' info=%dx%d (%.1fMB) "
                                    "decode saltado [M1] limite=%ldMB new3ds=%d\n",
                                    full, iw, ih, need / 1048576.0,
                                    M1_MAX_DECODE_BYTES / (1024 * 1024),
                                    (int)g_mkxp_is_new3ds);
                        fflush(stdout);
                        continue;   /* nao decodifica; tenta proxima ext/raiz -> fallback */
                    }
                    /* New 3DS + tileset grande (acima do antigo limite de 24MB):
                     * ANTES do decode caro, confirmamos que a RAM estendida esta'
                     * mesmo disponivel com um probe-malloc. Se nao couber (ex:
                     * launcher sem memoria estendida), saltamos para magenta SEM
                     * gastar os ~768ms do stbi_load -- nunca crasha. */
                    if (g_mkxp_is_new3ds && need > 24L * 1024 * 1024) {
                        void *probe = malloc((size_t)need);
                        if (!probe) {
                            printf("[BMP|N3DS-NOFIT] '%s' %dx%d (%.1fMB) nao cabe no "
                                   "malloc (sem RAM estendida?) -- magenta sem decode\n",
                                   full, iw, ih, need / 1048576.0);
                            if (g_dbglog)
                                fprintf(g_dbglog, "[BMP|N3DS-NOFIT] '%s' %.1fMB nao "
                                        "coube -- magenta\n", full, need / 1048576.0);
                            fflush(stdout);
                            continue;
                        }
                        free(probe);
                        printf("[BMP|N3DS-FIT] '%s' %dx%d (%.1fMB) cabe -> a decodificar "
                               "(RAM estendida do New 3DS)\n", full, iw, ih, need / 1048576.0);
                        if (g_dbglog)
                            fprintf(g_dbglog, "[BMP|N3DS-FIT] '%s' %.1fMB a decodificar\n",
                                    full, need / 1048576.0);
                        fflush(stdout);
                    }
                }
            }

            unsigned char *px = stbi_load(full, &w, &h, &ch, 4);
            if (px) {
                /* diagnostico: para imagens do titulo (MODTS/Titles), mostra
                 * que caminho EXATO resolveu e o tamanho. Ajuda a perceber porque
                 * start.png / pokemon.png apareciam a 1x1. */
                if (strstr(path, "MODTS") || strstr(path, "Titles") ||
                    strstr(path, "start") || strstr(path, "pokemon")) {
                    printf("[BMP|OK] '%s' -> '%s' (%dx%d)\n", path, full, w, h);
                    fflush(stdout);
                }
                /* FIX MEMORIA: ADOTA o buffer do stbi (sem bmp_alloc+memcpy) ->
                 * pico de RAM cai p/ metade. Critico p/ tilesets 256x31040 (30MB)
                 * que de outra forma precisavam de 60MB e falhavam no Old 3DS. */
                Bitmap3DS *b = bmp_alloc_adopt(w, h, px);
                b->tex_dirty = true;
                bmp_flush(b);
                return b;
            }

            /* ── FALLBACK [M2]: leitura MANUAL + decode-de-MEMORIA ───────────────
             * O stbi_load(ficheiro) acima devolveu NULL. Antes de desistir, lemos o
             * ficheiro INTEIRO para um buffer (fopen+fread) e decodificamos com
             * stbi_load_from_memory -- evitando o IO interno do stb (fseek/fread em
             * stream), suspeito de falhar em ficheiros grandes no SD do 3DS: as
             * introPictures* (~3-5MB) davam 'no SOI' APESAR de stbi_info ler o
             * tamanho, o que so' faz sentido se a deteccao de formato visse dados
             * truncados/lixo. Este caminho ISOLA a causa no log (le' o ficheiro
             * todo? assinatura PNG? decode?) e PODE corrigir. So' corre DEPOIS de o
             * stbi_load normal ter falhado -> zero regressao para o que ja' carrega.
             * Em FAT32 (SD do 3DS) o fopen e' case-insensitive, logo 'full' resolve
             * tanto .PNG como .png. */
            if (!px) {
                FILE *mf = fopen(full, "rb");
                if (mf) {
                    fseek(mf, 0, SEEK_END);
                    long fsz = ftell(mf);
                    fseek(mf, 0, SEEK_SET);
                    unsigned char *fbuf = (fsz > 0 && fsz < 64L * 1024 * 1024)
                                          ? (unsigned char *)malloc((size_t)fsz) : nullptr;
                    size_t rd = fbuf ? fread(fbuf, 1, (size_t)fsz, mf) : 0;
                    fclose(mf);
                    int sig_png = (rd >= 8 && fbuf && fbuf[0] == 0x89 &&
                                   fbuf[1] == 'P' && fbuf[2] == 'N' && fbuf[3] == 'G');
                    if (g_dbglog)
                        fprintf(g_dbglog, "[BMP|M2READ] '%s' fsz=%ld lido=%zu sig_png=%d "
                                "linear_livre=%.0fKB buf=%s\n", full, fsz, rd, sig_png,
                                linearSpaceFree() / 1024.0, fbuf ? "ok" : "NULL");
                    if (fbuf && fsz > 0 && rd == (size_t)fsz) {
                        int mw = 0, mh = 0, mch = 0;
                        /* ===== LIBPNG PRIMEIRO (a correcao) =====================
                         * Para PNGs, tenta libpng antes do stb. O stb falha
                         * "outofmem" em PNGs grandes no ARM (bug do stb, NAO
                         * memoria -- ver [DECODE|PROBE]). libpng descodifica-os
                         * bem. Se libpng der, adota e devolve; senao, cai no
                         * probe + stb por baixo (que ficam como diagnostico). */
                        if (sig_png) {
                            /* TILESET GIGANTE: se o 8888 passaria de 16MB (so' os
                             * tilesets 256xN chegam la'), decodifica direto p/ RGBA4444
                             * (metade da RAM) -> o decode de 15MB cabe onde o de 30MB
                             * falhava. Usado SEM embrulho (o Ruby deteta native_4444?).
                             * Se falhar, cai no decode 8888 normal (magenta, como antes). */
                            if (g_tileset_4444 && fsz >= 29) {
                                unsigned int gw = ((unsigned)fbuf[16] << 24) | ((unsigned)fbuf[17] << 16) |
                                                  ((unsigned)fbuf[18] << 8)  |  (unsigned)fbuf[19];
                                unsigned int gh = ((unsigned)fbuf[20] << 24) | ((unsigned)fbuf[21] << 16) |
                                                  ((unsigned)fbuf[22] << 8)  |  (unsigned)fbuf[23];
                                if ((size_t)gw * (size_t)gh * 4u > (size_t)16 * 1024 * 1024) {
                                    int qw = 0, qh = 0; bool q_linear = false;
                                    unsigned char* qpx = decode_png_to_4444(fbuf, (size_t)fsz, &qw, &qh, &q_linear);
                                    if (qpx) {
                                        free(fbuf);
                                        Bitmap3DS *b = bmp_alloc_adopt(qw, qh, qpx, q_linear);
                                        b->pixels_4444 = true;
                                        b->tex_dirty   = true;
                                        bmp_flush(b);  /* >1024 -> tex_failed; 4444 nunca vira textura */
                                        if (g_dbglog) {
                                            fprintf(g_dbglog, "[BMP|PNG4444] '%s' adotado RGBA4444 %dx%d "
                                                    "heap=%s (era 8888=%.1fMB -> 4444=%.1fMB)\n", full, qw, qh,
                                                    q_linear ? "LINEAR" : "malloc",
                                                    (double)gw * gh * 4 / 1048576.0,
                                                    (double)gw * gh * 2 / 1048576.0);
                                            fflush(g_dbglog);
                                        }
                                        return b;
                                    }
                                    /* 4444 falhou -> segue p/ 8888 normal (magenta) */
                                }
                            }
                            int lw = 0, lh = 0; bool l_linear = false;
                            unsigned char* lpx = decode_png_libpng(fbuf, (size_t)fsz, &lw, &lh, &l_linear);
                            if (lpx) {
                                free(fbuf);
                                if (g_dbglog) {
                                    fprintf(g_dbglog, "[BMP|LIBPNG] '%s' %dx%d OK via "
                                            "libpng heap=%s\n", full, lw, lh,
                                            l_linear ? "LINEAR" : "malloc");
                                    fflush(g_dbglog);
                                }
                                Bitmap3DS *b = bmp_alloc_adopt(lw, lh, lpx, l_linear);
                                b->tex_dirty = true;
                                bmp_flush(b);
                                return b;
                            }
                            if (g_dbglog) {
                                fprintf(g_dbglog, "[BMP|LIBPNG] '%s' libpng FALHOU -> "
                                        "cai no stb (diagnostico abaixo)\n", full);
                                fflush(g_dbglog);
                            }
                        }
                        /* DEBUG CERTEIRO (cracker da contradicao 24MB-livres-mas-falha):
                         * o stb segura DOIS blocos grandes ao mesmo tempo no decode
                         * (out + expanded ~= 2 x w*h*4). O probe de 1 bloco dava 24MB,
                         * mas o decode falha "outofmem". Aqui testo o PADRAO REAL: 1
                         * bloco de w*h*4, depois 2 em simultaneo. 3 resultados possiveis:
                         *  - 2 blocos CABEM            -> nao e' memoria; e' bug interno
                         *    do stb no ARM ou heap corrompida (proximo passo: outro angulo)
                         *  - 1 cabe mas 2 nao          -> fragmentacao: ha' espaco total
                         *    mas nao 2 blocos contiguos (fix: arena de decode pre-alocada)
                         *  - nem 1 cabe                -> falta memoria mesmo (o probe de
                         *    1x24MB engana; fix: libertar heap antes do decode) */
                        /* THROTTLE: o DECODE|PROBE aloca 2x w*h*4. Em cutscenes com
                         * retry (a mesma imagem falha varias vezes) isto corria a
                         * cada falha e inflava o tempo. Ja' provou o que tinha a
                         * provar (CABE-2). Corre so' as primeiras vezes; o resultado
                         * one-liner [BMP|M2OK/M2FAIL] continua sempre. */
                        static int s_probe_budget = 4;
                        if (g_dbglog && s_probe_budget > 0) {
                            s_probe_budget--;
                            int iw = 0, ih = 0, ic = 0;
                            if (stbi_info_from_memory(fbuf, (int)fsz, &iw, &ih, &ic)) {
                                size_t need = (size_t)iw * (size_t)ih * 4u;
                                void *p1 = malloc(need);
                                void *p2 = malloc(need);   /* 2o em simultaneo com o 1o */
                                fprintf(g_dbglog,
                                    "[DECODE|PROBE] '%s' %dx%d need=%zuKB/bloco | 1bloco=%s "
                                    "2blocos=%s | linear_livre=%.0fKB => %s\n",
                                    full, iw, ih, need/1024,
                                    p1 ? "OK" : "FALHOU",
                                    (p1 && p2) ? "OK" : "FALHOU",
                                    linearSpaceFree() / 1024.0,
                                    (p1 && p2) ? "CABE-2 (nao e' memoria: stb/heap)"
                                               : p1 ? "SO-1 (fragmentacao)"
                                                    : "NEM-1 (falta memoria mesmo)");
                                if (p1) free(p1);
                                if (p2) free(p2);
                                fflush(g_dbglog);
                            }
                        }
                        unsigned char *mpx = stbi_load_from_memory(fbuf, (int)fsz,
                                                                   &mw, &mh, &mch, 4);
                        free(fbuf);
                        if (mpx) {
                            if (g_dbglog) {
                                fprintf(g_dbglog, "[BMP|M2OK] '%s' %dx%d decodificado de "
                                        "MEMORIA (o IO em stream do stb e' que falhava)\n",
                                        full, mw, mh);
                                fflush(g_dbglog);
                            }
                            Bitmap3DS *b = bmp_alloc_adopt(mw, mh, mpx);
                            b->tex_dirty = true;
                            bmp_flush(b);
                            return b;
                        } else if (g_dbglog) {
                            fprintf(g_dbglog, "[BMP|M2FAIL] '%s' decode-de-memoria TAMBEM "
                                    "falhou (stbi:%s) sig_png=%d -> e' DECODE/MEMORIA, nao IO\n",
                                    full, stbi_failure_reason(), sig_png);
                            /* DEBUG LOG CERTEIRO: este e' o INSTANTE do OOM. Despejamos
                             * (a) o estado completo do pool do mruby -- quanto esta'
                             * preso em slots LIVRES (= o que um release-de-chunks daria
                             * de volta) -- e (b) a sonda do maior bloco de malloc
                             * disponivel. Juntos decidem o fix CEGO C: se ha' muitos MB
                             * presos no pool e o maior bloco e' <12MB, o release vale o
                             * risco; senao, a unica saida e' pre-escalar a imagem (B). */
                            extern void mkxp_pool_diag_full(const char*);
                            extern void mkxp_malloc_probe(const char*);
                            /* THROTTLE: estes dois sao CAROS (pool_diag percorre os
                             * ~28MB do pool inteiro; malloc_probe tenta 24MB). Numa
                             * cutscene com retry corriam a cada falha -> 180 varreduras
                             * no log. Ja' deram o retrato da memoria. So' as 1as vezes;
                             * o [BMP|M2FAIL] one-liner acima continua sempre. */
                            static int s_oom_diag_budget = 3;
                            if (s_oom_diag_budget > 0) {
                                s_oom_diag_budget--;
                                mkxp_pool_diag_full("@intro-OOM");
                                mkxp_malloc_probe("@intro-OOM");
                            }
                        }
                    } else {
                        if (fbuf) free(fbuf);
                        if (g_dbglog)
                            fprintf(g_dbglog, "[BMP|M2FAIL] '%s' leitura INCOMPLETA %zu/%ld "
                                    "(buf=%s) -> e' IO/ficheiro, nao decode\n",
                                    full, rd, fsz, fbuf ? "ok" : "NULL");
                    }
                }
            }

            char full_lc[512];
            snprintf(full_lc, sizeof(full_lc), "%s", full);
            char *dot = nullptr;
            for (char *p = full_lc; *p; p++) if (*p == '.') dot = p;
            if (dot) str_tolower(dot);
            if (dot && strcmp(full_lc, full) != 0) {
                px = stbi_load(full_lc, &w, &h, &ch, 4);
                if (px) {
                    /* FIX MEMORIA: adota o buffer (sem copia) -- ver acima. */
                    Bitmap3DS *b = bmp_alloc_adopt(w, h, px);
                    b->tex_dirty = true;
                    bmp_flush(b);
                    return b;
                }
                /* DIAGNOSTICO (tilesets gigantes que falham a CARREGAR, nao a
                 * texturizar): se o stbi falhou e a dimensao seria grande, o mais
                 * provavel e' falta de RAM (256x31040 = 30MB). Logar a razao do
                 * stbi ajuda a confirmar (vs ficheiro corrompido/ausente). */
                int sw = 0, sh = 0, sc = 0;
                if (stbi_info(full, &sw, &sh, &sc)) {
                    long bytes = (long)sw * sh * 4;
                    printf("[BMP|BIGFAIL] '%s' info=%dx%d (%.1fMB) mas stbi_load=NULL "
                           "-- causa provavel: RAM insuficiente. stbi: %s\n",
                           full, sw, sh, bytes/1048576.0, stbi_failure_reason());
                    if (g_dbglog)
                        fprintf(g_dbglog, "[BMP|BIGFAIL] '%s' info=%dx%d (%.1fMB) "
                                "stbi_load=NULL RAM? stbi:%s\n",
                                full, sw, sh, bytes/1048576.0, stbi_failure_reason());
                    fflush(stdout);
                }
            }
        }
    }

    /* LOGGING DETALHADO de falha (prevencao/diagnostico futuro): regista o nome
     * pedido E todas as raizes/extensoes tentadas, para que qualquer imagem que
     * venha a falhar fique identificada no log com precisao (caminho exato que
     * faltou). Sem isto, so' se via "magenta" no ecra sem saber o ficheiro.
     * Casos previstos a falhar (do Graphics.zip analisado): Graphics/System/(varios)
     * (pasta inexistente), Graphics/Battlers/eggCracks (em falta).
     * IMPORTANTE: escreve no stdout E no ficheiro debug_binding.log (g_dbglog),
     * para o diagnostico ficar no log persistente como o resto. */
    {
        extern FILE *g_dbglog;
        printf("[BMP|MISS] FALHOU carregar: '%s' -- fallback 32x32 magenta\n", path);
        if (g_dbglog) fprintf(g_dbglog, "[BMP|MISS] FALHOU carregar: '%s' -- fallback 32x32 magenta\n", path);
        int logged = 0;
        for (int ri = 0; s_game_roots[ri] != nullptr && logged < 3; ri++) {
            char shown[512];
            const char *root = s_game_roots[ri];
            if (root[0] != '\0' && path[0] == '/')
                snprintf(shown, sizeof(shown), "%s%s", root, path + 1);
            else
                snprintf(shown, sizeof(shown), "%s%s", root, path);
            printf("[BMP|MISS]   tentou raiz[%d]: '%s' (+ .png/.gif/.PNG/...)\n", ri, shown);
            if (g_dbglog) fprintf(g_dbglog, "[BMP|MISS]   tentou raiz[%d]: '%s' (+ .png/.gif/.PNG/...)\n", ri, shown);
            logged++;
        }
        if (g_dbglog) fflush(g_dbglog);
    }
    fflush(stdout);
    /* Marca o caminho como falhado (curto-circuita os proximos pedidos neste
     * mapa) e devolve o fallback. A cor (transparente p/ Pictures, magenta p/ o
     * resto) e o log [BMP|MISS] acima mantem-se -- so' deixa de RE-TENTAR. */
    mark_path_failed(path);
    return make_fallback_bitmap(path);
}

/* ── bmp_init ─────────────────────────────────────────────────────────────────
 * CORRECÇÃO CRÍTICA: usar mrb_get_argc() para contar argumentos SEM modificar
 * o estado do frame de chamada.
 *
 * A versão anterior usava mrb_argc_compat() que chamava mrb_get_args(mrb,"*",...)
 * internamente. Em mruby 3.2, mrb_get_args("*") consome os argumentos do frame
 * actual, deixando argc=0 para todas as chamadas subsequentes a mrb_get_args.
 * Isso causava o erro "wrong number of arguments (given 2, expected 1)" quando
 * o código tentava mrb_get_args(mrb,"o",&arg) depois de mrb_get_args(mrb,"*",...).
 *
 * mrb_get_argc(mrb) é seguro: lê apenas o contador sem tocar nos argumentos.
 * ─────────────────────────────────────────────────────────────────────────── */
mrb_value bmp_init(mrb_state *mrb, mrb_value self) {
    mrb_data_init(self, nullptr, &Bitmap3DSType);

    /* FIX: usar mrb_get_argc em vez de mrb_argc_compat para evitar
     * o problema de double-consume de argumentos em mruby 3.2 */
    mrb_int   argc = mrb_get_argc(mrb);
    Bitmap3DS *b   = nullptr;

    if (argc >= 2) {
        /* Forma (width, height) -- mais frequente no PE */
        mrb_int w, h;
        if (mrb_get_args(mrb, "ii", &w, &h) == 2) {
            b = bmp_alloc((int)w, (int)h);
        } else {
            /* Tipo não era inteiro -- tentar como objecto */
            mrb_value a1, a2;
            mrb_get_args(mrb, "oo", &a1, &a2);
            if (mrb_integer_p(a1) && mrb_integer_p(a2)) {
                b = bmp_alloc((int)mrb_integer(a1), (int)mrb_integer(a2));
            } else {
                b = bmp_alloc(1, 1);
            }
        }
    } else if (argc == 1) {
        mrb_value arg;
        mrb_get_args(mrb, "o", &arg);
        if (mrb_string_p(arg)) {
            const char *req_path = mrb_str_to_cstr(mrb, arg);
            b = bmp_load_file(req_path);
            /* ── [LOADTRACE] (SUPERDBG): cada load de ficheiro -- pedido, dims,
             * formato, fallback. UNICO ponto por onde TODOS os loads passam.
             * Cap 4000 linhas (loads sao cacheados -> nao por-frame). ──────── */
            {
                extern FILE *g_dbglog;
                static int g_loadtrace_n = 0;
                if (g_dbglog && g_loadtrace_n < 4000) {
                    g_loadtrace_n++;
                    if (b && b->pixels) {
                        bool maybe_magenta = (b->width == 32 && b->height == 32);
                        fprintf(g_dbglog, "[LOADTRACE] '%s' -> %dx%d 4444=%d%s\n",
                                req_path ? req_path : "(null)", b->width, b->height,
                                b->pixels_4444 ? 1 : 0,
                                maybe_magenta ? " (32x32 -> possivel fallback magenta)" : "");
                    } else {
                        fprintf(g_dbglog, "[LOADTRACE] '%s' -> NULL/sem-pixels\n",
                                req_path ? req_path : "(null)");
                    }
                    fflush(g_dbglog);
                }
            }
        } else if (mrb_integer_p(arg)) {
            /* Só um inteiro -- usar como dimensão quadrada */
            mrb_int sz = mrb_integer(arg);
            b = bmp_alloc((int)sz, (int)sz);
        } else if (!mrb_nil_p(arg)) {
            /* Objecto Bitmap passado directamente (estilo BitmapWrapper original PE).
             * Copiar pixel data se disponível, ou criar 1x1. */
            Bitmap3DS *src = (Bitmap3DS*)DATA_PTR(arg);
            if (src && src->width > 0 && src->height > 0 && src->pixels) {
                b = bmp_alloc(src->width, src->height);
                memcpy(b->pixels, src->pixels, (size_t)(src->width * src->height * 4));
                b->tex_dirty = true;
                bmp_flush(b);
            } else {
                /* Tentar obter via ivar @bitmap (BitmapWrapper aninhado) */
                mrb_value inner = mrb_iv_get(mrb, arg, mrb_intern_lit(mrb, "@bitmap"));
                if (!mrb_nil_p(inner)) {
                    src = (Bitmap3DS*)DATA_PTR(inner);
                }
                if (src && src->width > 0 && src->height > 0 && src->pixels) {
                    b = bmp_alloc(src->width, src->height);
                    memcpy(b->pixels, src->pixels, (size_t)(src->width * src->height * 4));
                    b->tex_dirty = true;
                    bmp_flush(b);
                } else {
                    b = bmp_alloc(1, 1);
                }
            }
        } else {
            b = bmp_alloc(1, 1);
        }
    } else {
        b = bmp_alloc(1, 1);
    }

    DATA_PTR(self)  = b;
    DATA_TYPE(self) = &Bitmap3DSType;
    return self;
}

static mrb_value bmp_dispose(mrb_state*, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    if (b && !b->disposed) {
        /* RGSS semantics: dispose() must release GPU + CPU memory IMMEDIATELY,
         * not defer to GC. This is critical on 3DS hardware: the linear heap
         * (texture memory) is small (~32MB) and RMXP games dispose bitmaps
         * constantly (scene transitions, temporary sprites, message boxes,
         * animations). Deferring the free to GC lets disposed textures pile up
         * in the linear heap under allocation churn -> heap exhaustion and
         * C3D_TexInit failures. Freeing here is generic and correct for any
         * RPG Maker XP game. bitmap_free() (GC path) is null-safe, so the
         * eventual collection of this object will not double-free. */
        if (b->tex)    { display_3ds_free_texture(b->tex); b->tex = nullptr; }
        /* FIX: respeitar a origem do buffer. b->pixels pode vir de linearAlloc
         * (fallback de OOM em bmp_alloc, ou PNG carregado p/ heap linear): nesse
         * caso free() corrompe o heap linear. Espelha o bitmap_free (GC). */
        if (b->pixels) {
            if (b->pixels_linear) linearFree(b->pixels);
            else                  free(b->pixels);
            b->pixels = nullptr;
        }
        b->tex_dirty = false;
        b->disposed  = true;
    }
    return mrb_nil_value();
}
static mrb_value bmp_disposed(mrb_state*, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    return mrb_bool_value(!b || b->disposed);
}
static mrb_value bmp_width(mrb_state*, mrb_value s) {
    Bitmap3DS *b = GET_BMP(s); return mrb_fixnum_value(b ? b->width  : 0); }
static mrb_value bmp_height(mrb_state*, mrb_value s) {
    Bitmap3DS *b = GET_BMP(s); return mrb_fixnum_value(b ? b->height : 0); }

/* [fix #5] prefer_vram=: o probe marca os tilesets compactados (estaticos) p/ a
 * textura GPU ir para VRAM em vez do heap linear. Se a flag mudar, forca re-flush
 * (tex_dirty) p/ a textura ser recriada com a nova preferencia. */
static mrb_value bmp_set_prefer_vram(mrb_state *mrb, mrb_value self) {
    mrb_value v; mrb_get_args(mrb, "o", &v);
    Bitmap3DS *b = GET_BMP(self);
    if (b) {
        bool nv = mrb_test(v);
        if (nv != b->prefer_vram) { b->prefer_vram = nv; b->tex_dirty = true; }
    }
    return v;
}
static mrb_value bmp_get_prefer_vram(mrb_state*, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    return mrb_bool_value(b && b->prefer_vram);
}

static mrb_value bmp_rect(mrb_state *mrb, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    RClass *rk = mrb_class_get(mrb, "Rect");
    mrb_value argv[4] = {
        mrb_fixnum_value(0), mrb_fixnum_value(0),
        mrb_fixnum_value(b ? b->width : 0), mrb_fixnum_value(b ? b->height : 0)
    };
    return mrb_obj_new(mrb, rk, 4, argv);
}

static mrb_value bmp_clear(mrb_state*, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    if (b && b->pixels) {
        memset(b->pixels, 0, (size_t)(b->width * b->height * 4));
        b->tex_dirty = true;
    }
    return mrb_nil_value();
}

/* /255 rapido para x em [0, 65025]: evita a divisao por software (o ARM11 do 3DS
 * nao tem instrucao de divisao inteira). Devolve ~round(x/255), exato nos
 * extremos. Usado no blending em ponto-fixo de bmp_blt/bmp_stretch_blt -- subst.
 * o blending em virgula flutuante (4 mult + 4 conversoes float->u8 por pixel),
 * caro no ARM. So' afeta pixels semi-transparentes; os opacos usam copia direta. */
#define DIV255(x) (((x) + 1 + ((x) >> 8)) >> 8)

/* ── Leitura de 1 pixel como RGBA8888, tratando origem 8888 OU 4444 ──────────
 * idx = y*width + x (indice de pixel, nao de byte). Os tilesets gigantes guardam
 * b->pixels em RGBA4444 (u16/px); todo o resto e' 8888 (4 bytes/px). Expandimos
 * cada nibble com *17 (0..15 -> 0..255, exato nos extremos: 0->0, 15->255). */
static inline void px_read(const Bitmap3DS *b, int idx,
                           unsigned char &r, unsigned char &g,
                           unsigned char &bl, unsigned char &a) {
    if (b->pixels_4444) {
        unsigned short v = ((const unsigned short*)b->pixels)[idx];
        r  = (unsigned char)(((v >> 12) & 0xF) * 17);
        g  = (unsigned char)(((v >> 8)  & 0xF) * 17);
        bl = (unsigned char)(((v >> 4)  & 0xF) * 17);
        a  = (unsigned char)(( v        & 0xF) * 17);
    } else {
        const unsigned char *p = b->pixels + (size_t)idx * 4;
        r = p[0]; g = p[1]; bl = p[2]; a = p[3];
    }
}

static mrb_value bmp_fill_rect(mrb_state *mrb, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    if (!b || !b->pixels) return mrb_nil_value();
    if (b->pixels_4444) return mrb_nil_value();  /* 4444 e' so' fonte; nao escrever */

    int x = 0, y = 0, w = 0, h = 0;
    float cr = 255, cg = 255, cb = 255, ca = 255;

    mrb_int argc = mrb_get_argc(mrb);
    if (argc == 2) {
        mrb_value ro, co; mrb_get_args(mrb, "oo", &ro, &co);
        if (!extract_rect(mrb, ro, &x, &y, &w, &h))      return mrb_nil_value();
        if (!extract_color(mrb, co, &cr, &cg, &cb, &ca)) return mrb_nil_value();
    } else {
        mrb_int xi, yi, wi, hi; mrb_value co;
        mrb_get_args(mrb, "iiiio", &xi, &yi, &wi, &hi, &co);
        x = (int)xi; y = (int)yi; w = (int)wi; h = (int)hi;
        if (!extract_color(mrb, co, &cr, &cg, &cb, &ca)) return mrb_nil_value();
    }

    unsigned char R = (unsigned char)cr, G = (unsigned char)cg,
                  B = (unsigned char)cb, A = (unsigned char)ca;
    for (int py = y; py < y + h && py < b->height; py++) {
        if (py < 0) continue;
        for (int px2 = x; px2 < x + w && px2 < b->width; px2++) {
            if (px2 < 0) continue;
            int idx = (py * b->width + px2) * 4;
            b->pixels[idx]   = R;
            b->pixels[idx+1] = G;
            b->pixels[idx+2] = B;
            b->pixels[idx+3] = A;
        }
    }
    b->tex_dirty = true;
    return mrb_nil_value();
}

static mrb_value bmp_blt(mrb_state *mrb, mrb_value self) {
    Bitmap3DS *dst = GET_BMP(self);
    if (!dst || !dst->pixels) return mrb_nil_value();
    mrb_int dx, dy, opacity = 255;
    mrb_value srco, sro;
    mrb_get_args(mrb, "iioo|i", &dx, &dy, &srco, &sro, &opacity);
    Bitmap3DS *src = (Bitmap3DS*)DATA_PTR(srco);
    if (!src || !src->pixels) return mrb_nil_value();
    /* DESTINO 4444 nunca acontece (4444 e' so' fonte/tileset gigante). Falha-seguro:
     * escrever 4444 como 8888 corromperia -> registamos e saimos sem escrever. */
    if (dst->pixels_4444) {
        if (g_dbglog) { fprintf(g_dbglog, "[BMP|4444-GUARD] blt p/ destino 4444 ignorado (%dx%d)\n", dst->width, dst->height); fflush(g_dbglog); }
        return mrb_nil_value();
    }
    int sx0 = 0, sy0 = 0, sw = 0, sh = 0;
    if (!extract_rect(mrb, sro, &sx0, &sy0, &sw, &sh)) return mrb_nil_value();
    if (opacity < 0) opacity = 0; else if (opacity > 255) opacity = 255;

    /* ── P2: clipping por INTERVALO (calculado UMA vez) em vez de um ramo por
     * pixel. Mapeamento fonte->destino: (sx0+i, sy0+j) -> (dx+i, dy+j). Os
     * limites validos de i e j garantem fonte E destino dentro dos limites. */
    const int sW = src->width,  sH = src->height;
    const int dW = dst->width,  dH = dst->height;
    const int DX = (int)dx, DY = (int)dy;
    int i0 = 0;  if (-sx0 > i0) i0 = -sx0;  if (-DX > i0) i0 = -DX;
    int i1 = sw; if (sW - sx0 < i1) i1 = sW - sx0;  if (dW - DX < i1) i1 = dW - DX;
    int j0 = 0;  if (-sy0 > j0) j0 = -sy0;  if (-DY > j0) j0 = -DY;
    int j1 = sh; if (sH - sy0 < j1) j1 = sH - sy0;  if (dH - DY < j1) j1 = dH - DY;

    unsigned char *dpx = dst->pixels;
    const int op = (int)opacity;
    for (int j = j0; j < j1; j++) {
        const int srow = (sy0 + j) * sW;
        const int drow = (DY  + j) * dW;
        for (int i = i0; i < i1; i++) {
            unsigned char sr, sg, sb, sal;
            px_read(src, srow + (sx0 + i), sr, sg, sb, sal);
            if (sal == 0) continue;                    /* fonte 100% transparente */
            const int di = (drow + (DX + i)) * 4;
            if (op >= 255 && sal == 255) {             /* CAMINHO RAPIDO: copia opaca */
                dpx[di] = sr; dpx[di+1] = sg; dpx[di+2] = sb; dpx[di+3] = 255;
                continue;
            }
            /* blending em ponto-fixo (sem FPU): a = alpha-fonte efetivo 0..255 */
            const int a  = (op >= 255) ? (int)sal : DIV255((int)sal * op);
            if (a == 0) continue;
            const int ia = 255 - a;
            dpx[di]   = (unsigned char)DIV255(sr * a + dpx[di]   * ia);
            dpx[di+1] = (unsigned char)DIV255(sg * a + dpx[di+1] * ia);
            dpx[di+2] = (unsigned char)DIV255(sb * a + dpx[di+2] * ia);
            dpx[di+3] = (unsigned char)DIV255(a * 255 + dpx[di+3] * ia);
        }
    }
    dst->tex_dirty = true;
    /* ── [SHEET?|BLT] Diagnóstico das "4 setas" ───────────────────────────────
     * O caminho de sprite ([SHEET?]) não disparou para a downarrow 224x40 -> a
     * tira de setas deve ser desenhada por BLT (copiada para os "contents" da
     * janela), não como sprite. Aqui detetamos uma TIRA horizontal (folha de
     * frames: altura<=64 e largura>=3x altura) copiada com a LARGURA TODA
     * (sw>=largura da origem) -> ou seja, todas as frames de uma vez, em vez de
     * recortar uma só (o que produz o efeito visual de várias setas lado a lado).
     * Dedup por dimensão (máx. 16), escreve no g_dbglog (aparece no log .txt).
     * Só diagnóstico: identifica a origem para o conserto definitivo. */
    if (src->height > 0 && src->height <= 64 && !src->has_text &&
        src->width >= 3 * src->height && sw >= src->width) {
        static int seen_w[16], seen_h[16], seen_n = 0;
        bool dup = false;
        for (int i = 0; i < seen_n; i++)
            if (seen_w[i] == src->width && seen_h[i] == src->height) { dup = true; break; }
        if (!dup && seen_n < 16) {
            seen_w[seen_n] = src->width; seen_h[seen_n] = src->height; seen_n++;
            extern FILE *g_dbglog;
            if (g_dbglog) {
                fprintf(g_dbglog,
                    "[SHEET?|BLT] tira %dx%d copiada INTEIRA por blt (sw=%d, sx0=%d) "
                    "-> SUSPEITA das setas (todas as frames de uma vez). dst=%dx%d @(%d,%d)\n",
                    src->width, src->height, sw, sx0,
                    dst->width, dst->height, (int)dx, (int)dy);
                fflush(g_dbglog);
            }
        }
    }
    /* DIAG TEXFLOW: copia que ENVOLVE texto (origem ou destino). Mostra a cadeia:
     * bitmap-de-texto -> contents/janela. Incondicional (so' p/ bitmaps de texto,
     * nao inunda com o tilemap). Se a origem tem texto, o destino passa a ter
     * (segue a cadeia + faz o destino usar LINEAR). */
    if (src->has_text || dst->has_text) {
        printf("[TEXFLOW|BLT] src=%dx%d(txt=%d) rect=(%d,%d,%d,%d) -> dst=%dx%d(txt=%d) @(%d,%d) op=%d\n",
            src->width, src->height, src->has_text?1:0, sx0, sy0, sw, sh,
            dst->width, dst->height, dst->has_text?1:0, (int)dx, (int)dy, (int)opacity);
        if (src->has_text) dst->has_text = true;
    }
    /* LOG (DBG_DIALOG): imagem copiada (retrato, moldura) -- origem WxH + rect,
     * destino WxH + posicao. Revela tamanhos das imagens das caixas. */
    DBG(DBG_DIALOG, "[BLT] src=%dx%d rect=(%d,%d,%d,%d) -> dst=%dx%d @(%d,%d) op=%d",
        src->width, src->height, sx0, sy0, sw, sh,
        dst->width, dst->height, (int)dx, (int)dy, (int)opacity);
    return mrb_nil_value();
}

static mrb_value bmp_stretch_blt(mrb_state *mrb, mrb_value self) {
    Bitmap3DS *dst = GET_BMP(self);
    if (!dst || !dst->pixels) return mrb_nil_value();

    mrb_value dro, srco, sro;
    mrb_int opacity = 255;
    mrb_int argc = mrb_get_argc(mrb);
    if (argc >= 3) {
        if (argc >= 4)
            mrb_get_args(mrb, "ooo|i", &dro, &srco, &sro, &opacity);
        else
            mrb_get_args(mrb, "ooo", &dro, &srco, &sro);
    } else {
        return mrb_nil_value();
    }

    Bitmap3DS *src = (Bitmap3DS*)DATA_PTR(srco);
    if (!src || !src->pixels) return mrb_nil_value();
    if (dst->pixels_4444) {
        if (g_dbglog) { fprintf(g_dbglog, "[BMP|4444-GUARD] stretch_blt p/ destino 4444 ignorado (%dx%d)\n", dst->width, dst->height); fflush(g_dbglog); }
        return mrb_nil_value();
    }

    int dx, dy, dw, dh, sx0, sy0, sw, sh;
    if (!extract_rect(mrb, dro, &dx, &dy, &dw, &dh)) return mrb_nil_value();
    if (!extract_rect(mrb, sro, &sx0, &sy0, &sw, &sh)) return mrb_nil_value();
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return mrb_nil_value();

    if (opacity < 0) opacity = 0; else if (opacity > 255) opacity = 255;
    const int op  = (int)opacity;
    float sx_step = (float)sw / (float)dw;
    float sy_step = (float)sh / (float)dh;

    for (int py = 0; py < dh; py++) {
        int dpy = dy + py;
        if (dpy < 0 || dpy >= dst->height) continue;
        int spy = sy0 + (int)(py * sy_step);
        if (spy < 0 || spy >= src->height) continue;
        for (int px2 = 0; px2 < dw; px2++) {
            int dpx = dx + px2;
            if (dpx < 0 || dpx >= dst->width) continue;
            int spx = sx0 + (int)(px2 * sx_step);
            if (spx < 0 || spx >= src->width) continue;
            unsigned char sr, sg, sb, sal;
            px_read(src, spy * src->width + spx, sr, sg, sb, sal);  /* origem 8888 ou 4444 */
            if (sal == 0) continue;                    /* fonte 100% transparente */
            int di = (dpy * dst->width + dpx) * 4;
            if (op >= 255 && sal == 255) {             /* CAMINHO RAPIDO: copia opaca */
                dst->pixels[di] = sr; dst->pixels[di+1] = sg;
                dst->pixels[di+2] = sb; dst->pixels[di+3] = 255;
                continue;
            }
            const int a  = (op >= 255) ? (int)sal : DIV255((int)sal * op);
            if (a == 0) continue;
            const int ia = 255 - a;
            dst->pixels[di]   = (unsigned char)DIV255(sr * a + dst->pixels[di]   * ia);
            dst->pixels[di+1] = (unsigned char)DIV255(sg * a + dst->pixels[di+1] * ia);
            dst->pixels[di+2] = (unsigned char)DIV255(sb * a + dst->pixels[di+2] * ia);
            dst->pixels[di+3] = (unsigned char)DIV255(a * 255 + dst->pixels[di+3] * ia);
        }
    }
    dst->tex_dirty = true;
    /* [SHEET?|STRETCH] mesma heurística do blt, para o caminho de stretch_blt:
     * tira horizontal (h<=64, w>=3h, não-texto) esticada com a LARGURA TODA
     * (sw>=largura da origem) = todas as frames de uma vez -> suspeita das setas. */
    if (src->height > 0 && src->height <= 64 && !src->has_text &&
        src->width >= 3 * src->height && sw >= src->width) {
        static int seen_w[16], seen_h[16], seen_n = 0;
        bool dup = false;
        for (int i = 0; i < seen_n; i++)
            if (seen_w[i] == src->width && seen_h[i] == src->height) { dup = true; break; }
        if (!dup && seen_n < 16) {
            seen_w[seen_n] = src->width; seen_h[seen_n] = src->height; seen_n++;
            extern FILE *g_dbglog;
            if (g_dbglog) {
                fprintf(g_dbglog,
                    "[SHEET?|STRETCH] tira %dx%d esticada INTEIRA (sw=%d, sx0=%d) -> "
                    "SUSPEITA das setas. dst d=(%d,%d,%d,%d)\n",
                    src->width, src->height, sw, sx0, dx, dy, dw, dh);
                fflush(g_dbglog);
            }
        }
    }
    /* DIAG TEXFLOW: ESTICAMENTO que envolve texto. ESTE e' o suspeito principal:
     * se o bitmap de texto (ex: 216x32) for esticado para a janela (ex: 512x96),
     * o fator de stretch >1 explica o borrao. Incondicional p/ bitmaps de texto. */
    if (src->has_text || dst->has_text) {
        printf("[TEXFLOW|STRETCH] src=%dx%d(txt=%d) s=(%d,%d,%d,%d) -> dst=%dx%d(txt=%d) d=(%d,%d,%d,%d) stretch=x%.2f,y%.2f\n",
            src->width, src->height, src->has_text?1:0, sx0, sy0, sw, sh,
            dst->width, dst->height, dst->has_text?1:0, dx, dy, dw, dh,
            (sw>0?(float)dw/sw:0.0f), (sh>0?(float)dh/sh:0.0f));
        if (src->has_text) dst->has_text = true;
    }
    /* LOG (DBG_DIALOG): esticamento -- e' assim que a windowskin (ex: 96x48) e'
     * redimensionada para a moldura da caixa. O retangulo DESTINO (dx,dy,dw,dh)
     * = dimensoes reais da janela desenhada. Compara com o tamanho esperado. */
    DBG(DBG_DIALOG, "[STRETCH] src=%dx%d s=(%d,%d,%d,%d) -> dst=%dx%d d=(%d,%d,%d,%d)",
        src->width, src->height, sx0, sy0, sw, sh,
        dst->width, dst->height, dx, dy, dw, dh);
    return mrb_nil_value();
}

/* ============================================================================
 * RENDERIZACAO DE TEXTO — fonte do sistema 3DS (BCFNT) rasterizada para o
 * buffer de pixeis do Bitmap.
 *
 * Porque CPU/rasterizacao e nao C2D_DrawText: o draw_text do RGSS desenha para
 * a TEXTURA de um Bitmap (ex.: o `contents` de uma janela), que o jogo depois
 * compoe via Sprite. C2D_DrawText desenha no render target do ecra durante um
 * frame -- nao serve para escrever num bitmap arbitrario. Por isso lemos os
 * glifos da sheet BCFNT (formato tiled/morton, alpha) e fazemos alpha-blend no
 * buffer RGBA do bitmap, na cor do texto. Da' texto com acentos (latino
 * completo da fonte do sistema) e encaixa no pipeline de sprites existente.
 * ========================================================================== */

/* Des-tiling Morton (igual ao do display_3ds.cpp): dado (x,y) numa textura de
 * largura tw (em pixeis, multipla de 8), devolve o indice linear do pixel no
 * buffer tiled em blocos de 8x8. */
/* Des-tiling Morton: usa exatamente a mesma formula comprovada do
 * display_3ds.cpp (tabela de swizzle), para garantir consistencia com o resto
 * do pipeline de texturas. (x,y) em pixeis, tw = largura da sheet (mult. de 8). */
static inline int fnt_morton_offset(int x, int y, int tw) {
    static const int t[8] = {0,1,4,5,16,17,20,21};
    return ((y>>3)*(tw>>3)+(x>>3))*64 + t[x&7] + (t[y&7]<<1);
}

/* Le o valor alpha (0..255) de um glifo na sheet BCFNT, na coordenada (gx,gy)
 * em pixeis dentro da sheet. As sheets do sistema sao tipicamente A4 (4 bits
 * por pixel, so alpha) mas tratamos tambem A8. tw/th = dimensoes da sheet. */
static inline int fnt_sheet_alpha(const u8 *sheet, int gx, int gy,
                                  int tw, int th, GPU_TEXCOLOR fmt) {
    if (gx < 0 || gy < 0 || gx >= tw || gy >= th) return 0;
    int off = fnt_morton_offset(gx, gy, tw);
    if (fmt == GPU_A4) {
        /* 4 bits por pixel: 2 pixeis por byte */
        int byteOff = off >> 1;
        u8 byte = sheet[byteOff];
        int nib = (off & 1) ? (byte >> 4) : (byte & 0x0F);
        return nib * 17;            /* 0..15 -> 0..255 */
    } else { /* GPU_A8 */
        return sheet[off];
    }
}

/* Decodifica um codepoint UTF-8 de `s` (avanca `*i`). Devolve o codepoint. */
static u32 fnt_utf8_next(const char *s, int len, int *i) {
    unsigned char c = (unsigned char)s[*i];
    if (c < 0x80) { (*i)++; return c; }
    if ((c >> 5) == 0x6 && *i + 1 < len) {
        u32 cp = ((c & 0x1F) << 6) | ((unsigned char)s[*i+1] & 0x3F);
        *i += 2; return cp;
    }
    if ((c >> 4) == 0xE && *i + 2 < len) {
        u32 cp = ((c & 0x0F) << 12) | (((unsigned char)s[*i+1] & 0x3F) << 6) |
                 ((unsigned char)s[*i+2] & 0x3F);
        *i += 3; return cp;
    }
    if ((c >> 3) == 0x1E && *i + 3 < len) {
        u32 cp = ((c & 0x07) << 18) | (((unsigned char)s[*i+1] & 0x3F) << 12) |
                 (((unsigned char)s[*i+2] & 0x3F) << 6) |
                 ((unsigned char)s[*i+3] & 0x3F);
        *i += 4; return cp;
    }
    (*i)++; return c;               /* byte invalido: avanca 1 */
}

/* Escala do texto. A fonte do sistema e' grande (~30px de linha); o RGSS usa
 * tipicamente ~24-32px de altura de linha. Usamos uma escala que da' ~22px de
 * altura, adequada para caber nas caixas do Essentials. */
static const float FNT_SCALE = 0.65f;

/* Garante que a fonte do sistema esta' carregada e mapeada. fontGetSystemFont()
 * pode devolver a fonte mas as sheets de glifos so' ficam acessiveis apos
 * fontEnsureMapped(). Chamado (idempotente) antes do 1o desenho. */
static CFNT_s *fnt_system(void) {
    static bool tried = false;
    static CFNT_s *cached = 0;
    if (!tried) {
        tried = true;
        cached = fontGetSystemFont();
        if (cached) {
            /* mapeia a fonte partilhada para acesso aos glifos (idempotente) */
            fontEnsureMapped();
        }
        if (cached) printf("[FONT] system font OK\n");
        else        printf("[FONT] system font INDISPONIVEL (texto nao desenha)\n");
    }
    return cached;
}

/* Mede a largura (em pixeis) que uma string ocuparia, a' escala FNT_SCALE. */
static int fnt_measure_width(const char *str, int len) {
    CFNT_s *font = fnt_system();
    if (!font) return len * 8;          /* fallback grosseiro */
    float w = 0.0f;
    int i = 0;
    while (i < len) {
        u32 cp = fnt_utf8_next(str, len, &i);
        if (cp == '\n') break;
        int idx = fontGlyphIndexFromCodePoint(font, cp);
        charWidthInfo_s *cwi = fontGetCharWidthInfo(font, idx);
        if (cwi) w += cwi->charWidth * FNT_SCALE;
    }
    return (int)(w + 0.5f);
}

/* Desenha `str` no bitmap `b`, comecando em (x,y) (canto sup-esq), na cor
 * (R,G,B,A). Rasteriza cada glifo da fonte do sistema com alpha-blend. */
static void fnt_draw_into_bitmap(Bitmap3DS *b, int x, int y,
                                 const char *str, int len,
                                 u8 R, u8 G, u8 B, u8 A) {
    if (!b || !b->pixels || len <= 0) return;
    CFNT_s *font = fnt_system();
    if (!font) return;
    TGLP_s *tglp = fontGetGlyphInfo(font);
    if (!tglp) return;

    int sheetW = tglp->sheetWidth;
    int sheetH = tglp->sheetHeight;
    GPU_TEXCOLOR fmt = (GPU_TEXCOLOR)tglp->sheetFmt;
    /* a baseline do texto fica mais abaixo; cellHeight inclui ascender. */
    float penX = (float)x;

    int i = 0;
    while (i < len) {
        u32 cp = fnt_utf8_next(str, len, &i);
        if (cp == '\n') break;          /* uma linha por chamada (RGSS) */

        int gidx = fontGlyphIndexFromCodePoint(font, cp);
        fontGlyphPos_s gp;
        fontCalcGlyphPos(&gp, font, gidx, GLYPH_POS_CALC_VTXCOORD,
                         FNT_SCALE, FNT_SCALE);

        const u8 *sheet = (const u8*)fontGetGlyphSheetTex(font, gp.sheetIndex);
        if (!sheet) { penX += gp.xAdvance; continue; }

        /* UVs do glifo na sheet (0..1) -> pixeis */
        int gx0 = (int)(gp.texcoord.left   * sheetW + 0.5f);
        int gx1 = (int)(gp.texcoord.right  * sheetW + 0.5f);
        /* nota: top/bottom invertidos no atlas (top = maior V) */
        int gy0 = (int)((1.0f - gp.texcoord.top)    * sheetH + 0.5f);
        int gy1 = (int)((1.0f - gp.texcoord.bottom) * sheetH + 0.5f);
        if (gy0 > gy1) { int t = gy0; gy0 = gy1; gy1 = t; }
        if (gx0 > gx1) { int t = gx0; gx0 = gx1; gx1 = t; }

        int gw = gx1 - gx0;
        int gh = gy1 - gy0;
        if (gw <= 0 || gh <= 0) { penX += gp.xAdvance; continue; }

        /* dimensoes de destino (vtx coords ja' escaladas) */
        float dstW = gp.vtxcoord.right  - gp.vtxcoord.left;
        float dstH = gp.vtxcoord.bottom - gp.vtxcoord.top;
        if (dstW <= 0) dstW = gw;
        if (dstH <= 0) dstH = gh;

        int baseX = (int)(penX + gp.vtxcoord.left + 0.5f);
        int baseY = (int)(y    + gp.vtxcoord.top  + 0.5f);

        /* amostragem BILINEAR da sheet para o destino.
         * Antes: nearest (1 pixel) -> bordas em escada + ilegivel ao reduzir.
         * Agora: media ponderada dos 4 vizinhos pela posicao fracionaria ->
         * texto suave e legivel mesmo com FNT_SCALE pequeno. Custa 4 leituras
         * por pixel (so' no desenho de texto, nao por frame inteiro). */
        for (int dy = 0; dy < (int)dstH; dy++) {
            int py = baseY + dy;
            if (py < 0 || py >= b->height) continue;
            float fsy = gy0 + (dy + 0.5f) * (float)gh / dstH - 0.5f;
            int   sy0 = (int)floorf(fsy);
            float wy  = fsy - sy0;
            for (int dx = 0; dx < (int)dstW; dx++) {
                int px = baseX + dx;
                if (px < 0 || px >= b->width) continue;
                float fsx = gx0 + (dx + 0.5f) * (float)gw / dstW - 0.5f;
                int   sx0 = (int)floorf(fsx);
                float wx  = fsx - sx0;

                /* 4 vizinhos (clamp implicito via fnt_sheet_alpha que devolve 0
                 * fora dos limites; aqui usamos os limites do glifo gx0..gx1). */
                int a00 = fnt_sheet_alpha(sheet, sx0,   sy0,   sheetW, sheetH, fmt);
                int a10 = fnt_sheet_alpha(sheet, sx0+1, sy0,   sheetW, sheetH, fmt);
                int a01 = fnt_sheet_alpha(sheet, sx0,   sy0+1, sheetW, sheetH, fmt);
                int a11 = fnt_sheet_alpha(sheet, sx0+1, sy0+1, sheetW, sheetH, fmt);
                float top = a00 * (1.0f - wx) + a10 * wx;
                float bot = a01 * (1.0f - wx) + a11 * wx;
                int a = (int)(top * (1.0f - wy) + bot * wy + 0.5f);
                if (a <= 0) continue;

                /* alpha do glifo * alpha do texto */
                int af = a * A / 255;
                if (af <= 0) continue;
                int idx = (py * b->width + px) * 4;
                int inv = 255 - af;
                b->pixels[idx+0] = (unsigned char)((R * af + b->pixels[idx+0] * inv) / 255);
                b->pixels[idx+1] = (unsigned char)((G * af + b->pixels[idx+1] * inv) / 255);
                b->pixels[idx+2] = (unsigned char)((B * af + b->pixels[idx+2] * inv) / 255);
                int da = b->pixels[idx+3];
                b->pixels[idx+3] = (unsigned char)(af + da * inv / 255);
            }
        }
        penX += gp.xAdvance;
    }
    b->tex_dirty = true;
}

/* draw_text(x, y, width, height, str, align=0)  OU  draw_text(rect, str, align=0)
 * Desenha `str` dentro da area dada, usando a cor da fonte do bitmap
 * (bitmap.font.color) se existir, senao branco. Suporta alinhamento
 * 0=esq, 1=centro, 2=dir. */
static mrb_value bmp_draw_text(mrb_state *mrb, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    /* DIAG-DT (cartao vazio -- SEMPRE ligado): apanha o caso que o early-return
     * '!b->pixels' a seguir engolia SEM deixar rasto. So' bitmaps ALTOS (o
     * overlay do cartao e' 384; exclui o dialogo 96 e os tiles 32). Para CADA
     * draw_text num bitmap alto regista: se b existe, se b->pixels existe (a
     * causa suspeita), o tamanho real, e se esta' disposed. Cruzar com:
     *   - se [DIAG-DT] NAO aparece quando abres o cartao -> draw_text nem sequer
     *     e' chamado no overlay -> a causa esta' no fluxo Ruby (cor/sprite/z),
     *     nao no desenho de texto;
     *   - se aparece com pixels=NULL -> o overlay perdeu/nunca teve buffer;
     *   - se aparece com pixels=OK -> entao o [CARTAO-TXT] mais abaixo TAMBEM
     *     dispara e diz a cor/alpha do texto. */
    {
        static int s_dtn = 0;
        if (b && b->height >= 200 && s_dtn < 80) {
            s_dtn++;
            extern FILE *g_dbglog;
            if (g_dbglog) {
                fprintf(g_dbglog, "[DIAG-DT] draw_text: b=%p bmp=%dx%d pixels=%s disposed=%d tex=%s\n",
                    (void*)b, b->width, b->height,
                    (b->pixels ? "OK" : "NULL"), (b->disposed ? 1 : 0),
                    (b->tex ? "sim" : "nao"));
                fflush(g_dbglog);
            }
        }
    }
    if (!b || !b->pixels) return mrb_nil_value();

    int ax = 0, ay = 0, aw = 0, ah = 0, align = 0;
    const char *str = 0; mrb_int slen = 0;

    mrb_int argc = mrb_get_argc(mrb);
    if (argc <= 3) {
        /* forma (rect, str[, align]) */
        mrb_value ro, sv; mrb_int al = 0;
        if (argc == 3) mrb_get_args(mrb, "ooi", &ro, &sv, &al);
        else           mrb_get_args(mrb, "oo",  &ro, &sv);
        if (!extract_rect(mrb, ro, &ax, &ay, &aw, &ah)) return mrb_nil_value();
        align = (int)al;
        mrb_value s2 = (mrb_string_p(sv)) ? sv : mrb_obj_as_string(mrb, sv);
        str = RSTRING_PTR(s2); slen = RSTRING_LEN(s2);
    } else {
        /* forma (x, y, w, h, str[, align]) */
        mrb_int xi, yi, wi, hi, al = 0; mrb_value sv;
        if (argc >= 6) mrb_get_args(mrb, "iiiioi", &xi, &yi, &wi, &hi, &sv, &al);
        else           mrb_get_args(mrb, "iiiio",  &xi, &yi, &wi, &hi, &sv);
        ax = (int)xi; ay = (int)yi; aw = (int)wi; ah = (int)hi; align = (int)al;
        mrb_value s2 = (mrb_string_p(sv)) ? sv : mrb_obj_as_string(mrb, sv);
        str = RSTRING_PTR(s2); slen = RSTRING_LEN(s2);
    }
    if (!str || slen <= 0) return mrb_nil_value();

    /* [DIAG-DT-TXT] Complemento do DIAG-DT: agora que o texto esta' extraido,
     * regista o conteudo (so' bitmaps ALTOS >=200, p/ apanhar o overlay do
     * cartao 512x384 e excluir o dialogo 96). Se ao abrir o cartao virmos aqui
     * 'Player', '3000', etc. -> draw_text E' chamado e os pixels escrevem-se;
     * a causa do cartao vazio passa a ser composicao/z-order do overlay, nao o
     * texto. Se NUNCA aparecer com o conteudo do cartao -> o texto vai por outro
     * caminho. Cap p/ nao encher o log. */
    if (b && b->height >= 200) {
        static int s_dttx = 0;
        if (s_dttx < 120) {
            s_dttx++;
            char tb[40]; int tn = (slen < 39) ? (int)slen : 39;
            for (int i = 0; i < tn; i++) { char c = str[i]; tb[i] = (c >= 32 && c < 127) ? c : '.'; }
            tb[tn] = 0;
            extern FILE *g_dbglog;
            if (g_dbglog) {
                fprintf(g_dbglog, "[DIAG-DT-TXT] bmp=%dx%d rect=(%d,%d,%d,%d) txt='%s'\n",
                    b->width, b->height, ax, ay, aw, ah, tb);
                fflush(g_dbglog);
            }
        }
    }

    /* cor do texto: bitmap.font.color (se existir), senao branco */
    u8 R = 255, G = 255, B = 255, A = 255;
    mrb_value fobj = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@font"));
    if (!mrb_nil_p(fobj)) {
        mrb_value col = mrb_iv_get(mrb, fobj, mrb_intern_lit(mrb, "@color"));
        if (mrb_nil_p(col) && mrb_respond_to(mrb, fobj, mrb_intern_lit(mrb, "color")))
            col = mrb_funcall(mrb, fobj, "color", 0);
        if (!mrb_nil_p(col)) {
            float fr, fg, fb, fa;
            if (extract_color(mrb, col, &fr, &fg, &fb, &fa)) {
                R = (u8)fr; G = (u8)fg; B = (u8)fb; A = (u8)fa;
            }
        }
    }

    /* alinhamento horizontal + centragem vertical aproximada */
    int tw = fnt_measure_width(str, (int)slen);
    int drawX = ax;
    if (align == 1)      drawX = ax + (aw - tw) / 2;   /* centro */
    else if (align == 2) drawX = ax + (aw - tw);       /* direita */
    /* baseline vertical: centra a linha (~22px) na altura da celula */
    int lineH = 22;
    int drawY = ay + ((ah > lineH) ? (ah - lineH) / 2 : 0);

    fnt_draw_into_bitmap(b, drawX, drawY, str, (int)slen, R, G, B, A);
    b->has_text = true;   /* -> textura usa filtro LINEAR (texto legivel no downscale) */
    b->tex_dirty = true;  /* FIX (cartao vazio): draw_text MODIFICA os pixels, logo a
                           * textura GPU tem de ser re-enviada. Sem isto, uma bitmap
                           * EXIBIDA DIRETAMENTE (ex.: @sprites["overlay"] do Trainer Card)
                           * mostrava a textura ANTERIOR: o overlay.clear marcava dirty e
                           * enviava a bitmap VAZIA, e o draw_text seguinte (nome/ID/dinheiro)
                           * nao re-marcava -> a textura vazia ficava -> cartao sem texto.
                           * (O dialogo escapava porque o texto e' BLT para a janela, e o
                           * blt ja' marcava dirty.) Idempotente: varios draw_text no mesmo
                           * frame -> um unico upload no bmp_flush do render. */
    /* LOG DIAGNOSTICO (DBG_DIALOG): o TEXTO que esta a ser desenhado, em que
     * retangulo, em que bitmap (tamanho), com que cor e largura medida. Mostra
     * o conteudo real das caixas e se o letra-a-letra avanca (strings a crescer). */
    DBG(DBG_DIALOG, "[TXT] '%.50s' rect=(%d,%d,%d,%d) al=%d -> bmp=%dx%d rgba=%d,%d,%d,%d tw=%d",
        str, ax, ay, aw, ah, align, b->width, b->height, R, G, B, A, tw);
    /* DIAGNOSTICO CARTAO (sempre ligado): so' para bitmaps ALTOS (>=200px) ->
     * apanha o TextSprite 512x384 do Trainer Card (e overlays grandes), exclui
     * caixas de dialogo (96px) e tiles (32px). Responde DEFINITIVAMENTE a:
     *   - o draw_text e' MESMO chamado p/ o texto do cartao (nome/ID/dinheiro)?
     *   - com que cor/ALPHA? (A=0 ou cor==fundo -> texto invisivel)
     *   - tw>0? (glifos encontrados) | @font era nil? | bitmap real (512x384)?
     * Se NAO aparecerem linhas [CARTAO-TXT] com o nome -> o cartao desenha o
     * texto por OUTRA via (drawTextEx/blt) e nao por draw_text -> ver probe. */
    if (b->height >= 200) {
        static int s_card_txt_n = 0;
        if (s_card_txt_n < 120) {
            s_card_txt_n++;
            bool font_nil = mrb_nil_p(mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@font")));
            printf("[CARTAO-TXT] #%d '%.40s' rect=(%d,%d,%d,%d) al=%d bmp=%dx%d rgba=%d,%d,%d,%d tw=%d font_nil=%d has_text=%d\n",
                   s_card_txt_n, str, ax, ay, aw, ah, align,
                   b->width, b->height, (int)R, (int)G, (int)B, (int)A, tw,
                   font_nil ? 1 : 0, b->has_text ? 1 : 0);
            fflush(stdout);
        }
    }
    return mrb_nil_value();
}

/* text_size(str) -> Rect(0,0,largura,altura) com a largura REAL do texto. */
static mrb_value bmp_text_size(mrb_state *mrb, mrb_value self) {
    (void)self;
    mrb_value strv;
    int w = 0;
    mrb_int argc = mrb_get_argc(mrb);
    if (argc >= 1) {
        mrb_get_args(mrb, "o", &strv);
        mrb_value s2 = (mrb_string_p(strv)) ? strv : mrb_obj_as_string(mrb, strv);
        w = fnt_measure_width(RSTRING_PTR(s2), (int)RSTRING_LEN(s2));
    }
    RClass *rk = mrb_class_get(mrb, "Rect");
    mrb_value argv[4] = {
        mrb_fixnum_value(0), mrb_fixnum_value(0),
        mrb_fixnum_value(w), mrb_fixnum_value(22)
    };
    return mrb_obj_new(mrb, rk, 4, argv);
}

static mrb_value bmp_get_pixel(mrb_state *mrb, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    mrb_int x, y; mrb_get_args(mrb, "ii", &x, &y);
    float r = 0, g = 0, bl = 0, a = 0;
    if (b && b->pixels && x >= 0 && y >= 0 && x < b->width && y < b->height) {
        unsigned char rr, gg, bb, aa;
        px_read(b, (int)(y * b->width + x), rr, gg, bb, aa);  /* 8888 ou 4444 */
        r = rr; g = gg; bl = bb; a = aa;
    }
    RClass *ck = mrb_class_get(mrb, "Color");
    mrb_value argv[4] = {
        mrb_float_value(mrb, r), mrb_float_value(mrb, g),
        mrb_float_value(mrb, bl), mrb_float_value(mrb, a)
    };
    return mrb_obj_new(mrb, ck, 4, argv);
}

static mrb_value bmp_set_pixel(mrb_state *mrb, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    if (b && b->pixels_4444) return mrb_nil_value();  /* 4444 e' so' fonte; nao escrever */
    mrb_int x, y; mrb_value co;
    mrb_get_args(mrb, "iio", &x, &y, &co);
    float cr, cg, cb2, ca;
    if (b && b->pixels && extract_color(mrb, co, &cr, &cg, &cb2, &ca)
         && x >= 0 && y >= 0 && x < b->width && y < b->height) {
        int idx = (y * b->width + x) * 4;
        b->pixels[idx]   = (unsigned char)cr;
        b->pixels[idx+1] = (unsigned char)cg;
        b->pixels[idx+2] = (unsigned char)cb2;
        b->pixels[idx+3] = (unsigned char)ca;
        b->tex_dirty = true;
    }
    return mrb_nil_value();
}

static mrb_value bmp_clear_rect(mrb_state *mrb, mrb_value self) {
    Bitmap3DS *b = GET_BMP(self);
    if (!b) return mrb_nil_value();
    if (b->pixels_4444) return mrb_nil_value();  /* 4444 e' so' fonte; nao escrever */
    int x = 0, y = 0, w = 0, h = 0;
    mrb_int argc = mrb_get_argc(mrb);
    if (argc == 1) {
        mrb_value ro; mrb_get_args(mrb, "o", &ro);
        if (!extract_rect(mrb, ro, &x, &y, &w, &h)) return mrb_nil_value();
    } else {
        mrb_int xi, yi, wi, hi; mrb_get_args(mrb, "iiii", &xi, &yi, &wi, &hi);
        x = (int)xi; y = (int)yi; w = (int)wi; h = (int)hi;
    }
    for (int py = y; py < y + h && py < b->height; py++) {
        if (py < 0) continue;
        for (int px2 = x; px2 < x + w && px2 < b->width; px2++) {
            if (px2 < 0) continue;
            int idx = (py * b->width + px2) * 4;
            b->pixels[idx] = b->pixels[idx+1] = b->pixels[idx+2] = b->pixels[idx+3] = 0;
        }
    }
    b->tex_dirty = true;
    return mrb_nil_value();
}

#ifdef __GNUC__
#  define MRB_CB __attribute__((used)) static mrb_value
#else
#  define MRB_CB static mrb_value
#endif

MRB_CB bmp_hue_change        (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();   }
MRB_CB bmp_gradient_fill_rect(mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();   }
MRB_CB bmp_blur              (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();   }
MRB_CB bmp_radial_blur       (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();   }

static mrb_value bmp_font(mrb_state *mrb, mrb_value self) {
    mrb_value cached = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@font"));
    if (!mrb_nil_p(cached)) return cached;
    if (mrb_class_defined(mrb, "Font")) {
        RClass *fk = mrb_class_get(mrb, "Font");
        mrb_value fobj = mrb_obj_new(mrb, fk, 0, nullptr);
        mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@font"), fobj);
        return fobj;
    }
    return mrb_nil_value();
}
static mrb_value bmp_set_font(mrb_state *mrb, mrb_value self) {
    mrb_value v; mrb_get_args(mrb, "o", &v);
    mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@font"), v);
    return v;
}

static mrb_value bmp_max_size(mrb_state *mrb, mrb_value self) {
    (void)mrb; (void)self;
    return mrb_fixnum_value(1024);
}

MRB_CB bmp_animated      (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_false_value();   }
/* mega? = false SEMPRE (decisao p/ 3DS): no Essentials, mega? a true faz o
 * CustomTilemap chamar TileWrap.wrapTileset, que COPIA o tileset gigante
 * (256x31040 = 30MB) para um novo bitmap re-empacotado em colunas -- mais 30MB
 * de RAM + 31 blts lentos, e o resultado (7936x1024) na mesma nao cabe numa
 * textura. Em vez disso mantemos mega?=false: o jogo usa o tileset ORIGINAL
 * direto como fonte de blt. Como o bmp_flush ja' NAO cria textura p/ bitmaps
 * >1024 (ficam em RAM), o tileset gigante existe inteiro em b->pixels e o blt de
 * cada tile 32x32 recorta-o em qualquer altura (incl. y>1024) sem problema. Isto
 * evita a copia, poupa RAM e e' o caminho mais leve para o hardware 3DS. */
MRB_CB bmp_mega          (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_false_value();   }
MRB_CB bmp_frame_count   (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_fixnum_value(1); }
MRB_CB bmp_current_frame (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_fixnum_value(0); }
MRB_CB bmp_set_cur_frame (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_fixnum_value(0); }
MRB_CB bmp_playing_get   (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_false_value();   }
MRB_CB bmp_playing_set   (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();     }
MRB_CB bmp_play          (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();     }
MRB_CB bmp_stop          (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();     }
MRB_CB bmp_goto_stop     (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();     }
MRB_CB bmp_goto_play     (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();     }
MRB_CB bmp_next_frame    (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_fixnum_value(0); }
MRB_CB bmp_prev_frame    (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_fixnum_value(0); }
MRB_CB bmp_add_frame     (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_fixnum_value(0); }
MRB_CB bmp_remove_frame  (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();     }
MRB_CB bmp_frame_rate_get(mrb_state *mrb, mrb_value s) { (void)s;  return mrb_float_value(mrb, 0.0);     }
MRB_CB bmp_frame_rate_set(mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();     }
MRB_CB bmp_looping_get   (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_false_value();   }
MRB_CB bmp_looping_set   (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();     }
MRB_CB bmp_snap          (mrb_state *mrb, mrb_value s) { (void)mrb; (void)s; return mrb_nil_value();     }

/* Bitmap._clear_failed_loads : esvazia a cache negativa de loads falhados.
 * Chamado pelo porte em mudanca de mapa (CustomTilemap#tileset=), p/ um mapa
 * novo voltar a tentar carregar os seus tilesets em vez de herdar o "falhou"
 * do mapa anterior. clear_failed_loads() esta' definida junto ao bmp_load_file. */
static mrb_value bmp_clear_failed_loads(mrb_state *mrb, mrb_value self) {
    (void)mrb; (void)self;
    extern void mkxp_bmp_clear_failed_loads();
    mkxp_bmp_clear_failed_loads();
    return mrb_nil_value();
}

void bitmapBindingInit(mrb_state *mrb) {
    RClass *k = mrb_define_class(mrb, "Bitmap", mrb->object_class);
    MRB_SET_INSTANCE_TT(k, MRB_TT_DATA);

    /* [BUILD-CHK-J20] Marcador INCONDICIONAL de compilacao. Dispara 1x no
     * arranque, sem depender de qualquer estado do jogo. Se NAO aparecer no
     * log, este bitmap_binding_3ds.cpp NAO foi compilado/ligado no binario que
     * corre no 3DS (independentemente do que o `make` disse). Resolve a duvida
     * "recompilou ou nao" de forma definitiva. */
    {
        extern FILE *g_dbglog;
        const char *m = "[BUILD-CHK-J20] bitmap_binding_3ds ATIVO (draw_text/DIAG-DT presentes)\n";
        printf("%s", m);
        if (g_dbglog) { fprintf(g_dbglog, "%s", m); fflush(g_dbglog); }
    }

    /* Pré-criar BitmapWrapper como subclasse C com MRB_TT_DATA ANTES dos scripts.
     * Quando os scripts fizerem "class BitmapWrapper < Bitmap" reabrerem a classe
     * já existente (em vez de criar nova) e MRB_TT_DATA mantém-se correcto. */
    {
        RClass *bw = mrb_define_class(mrb, "BitmapWrapper", k);
        MRB_SET_INSTANCE_TT(bw, MRB_TT_DATA);
        mrb_define_method(mrb, bw, "initialize", bmp_init, MRB_ARGS_ANY());
    }

    mrb_define_method(mrb, k, "initialize",         bmp_init,              MRB_ARGS_ANY());
    mrb_define_method(mrb, k, "dispose",            bmp_dispose,           MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "disposed?",          bmp_disposed,          MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "width",              bmp_width,             MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "height",             bmp_height,            MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "prefer_vram=",       bmp_set_prefer_vram,   MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "prefer_vram",        bmp_get_prefer_vram,   MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "rect",               bmp_rect,              MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "clear",              bmp_clear,             MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "fill_rect",          bmp_fill_rect,         MRB_ARGS_REQ(2)|MRB_ARGS_OPT(3));
    mrb_define_method(mrb, k, "clear_rect",         bmp_clear_rect,        MRB_ARGS_REQ(1)|MRB_ARGS_OPT(3));
    mrb_define_method(mrb, k, "gradient_fill_rect", bmp_gradient_fill_rect,MRB_ARGS_REQ(3)|MRB_ARGS_OPT(2));
    mrb_define_method(mrb, k, "blt",                bmp_blt,               MRB_ARGS_REQ(4)|MRB_ARGS_OPT(1));
    mrb_define_method(mrb, k, "stretch_blt",        bmp_stretch_blt,       MRB_ARGS_REQ(3)|MRB_ARGS_OPT(1));
    mrb_define_method(mrb, k, "draw_text",          bmp_draw_text,         MRB_ARGS_REQ(2)|MRB_ARGS_OPT(4));
    mrb_define_method(mrb, k, "text_size",          bmp_text_size,         MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "get_pixel",          bmp_get_pixel,         MRB_ARGS_REQ(2));
    mrb_define_method(mrb, k, "set_pixel",          bmp_set_pixel,         MRB_ARGS_REQ(3));
    mrb_define_method(mrb, k, "hue_change",         bmp_hue_change,        MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "blur",               bmp_blur,              MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "radial_blur",        bmp_radial_blur,       MRB_ARGS_REQ(2));
    mrb_define_method(mrb, k, "font",               bmp_font,              MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "font=",              bmp_set_font,          MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, k, "max_size",     bmp_max_size,          MRB_ARGS_NONE());
    mrb_define_class_method(mrb, k, "_native_exist?", bmp_native_exist,    MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, k, "_native_dir_exist?", bmp_native_dir_exist, MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "native_4444?",       bmp_native_4444,       MRB_ARGS_NONE());
    mrb_define_class_method(mrb, k, "_clear_failed_loads", bmp_clear_failed_loads, MRB_ARGS_NONE());

    mrb_define_method(mrb, k, "animated?",          bmp_animated,          MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "mega?",              bmp_mega,              MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "frame_count",        bmp_frame_count,       MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "current_frame",      bmp_current_frame,     MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "current_frame=",     bmp_set_cur_frame,     MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "playing",            bmp_playing_get,       MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "playing=",           bmp_playing_set,       MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "play",               bmp_play,              MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "stop",               bmp_stop,              MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "goto_and_stop",      bmp_goto_stop,         MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "goto_and_play",      bmp_goto_play,         MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "next_frame",         bmp_next_frame,        MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "previous_frame",     bmp_prev_frame,        MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "add_frame",          bmp_add_frame,         MRB_ARGS_REQ(1)|MRB_ARGS_OPT(1));
    mrb_define_method(mrb, k, "remove_frame",       bmp_remove_frame,      MRB_ARGS_OPT(1));
    mrb_define_method(mrb, k, "frame_rate",         bmp_frame_rate_get,    MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "frame_rate=",        bmp_frame_rate_set,    MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "looping",            bmp_looping_get,       MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "looping=",           bmp_looping_set,       MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "snap_to_bitmap",     bmp_snap,              MRB_ARGS_OPT(1));
}

void bitmapBindingReinit(mrb_state *mrb) {
    extern FILE *g_dbglog;

    if (mrb_class_defined(mrb, "Bitmap")) {
        struct RClass *k = mrb_class_get(mrb, "Bitmap");
        MRB_SET_INSTANCE_TT(k, MRB_TT_DATA);
        mrb_define_method(mrb, k, "initialize", bmp_init, MRB_ARGS_ANY());
        if (g_dbglog) {
            fprintf(g_dbglog, "[BMP|REINIT] Bitmap MRB_TT_DATA + bmp_init rebound OK\n");
            fflush(g_dbglog);
        }
    }

    mrb_sym bw_sym = mrb_intern_lit(mrb, "BitmapWrapper");
    if (mrb_const_defined(mrb, mrb_obj_value(mrb->object_class), bw_sym)) {
        mrb_value bw_val = mrb_const_get(mrb, mrb_obj_value(mrb->object_class), bw_sym);
        if (mrb_class_p(bw_val)) {
            struct RClass *bw = mrb_class_ptr(bw_val);
            MRB_SET_INSTANCE_TT(bw, MRB_TT_DATA);
            mrb_define_method(mrb, bw, "initialize", bmp_init, MRB_ARGS_ANY());
            MRB_SET_INSTANCE_TT(bw, MRB_TT_DATA);
            if (g_dbglog) {
                fprintf(g_dbglog,
                    "[BMP|REINIT] BitmapWrapper MRB_TT_DATA + bmp_init rebound OK\n");
                fflush(g_dbglog);
            }
        }
    } else {
        if (g_dbglog) {
            fprintf(g_dbglog,
                "[BMP|REINIT] BitmapWrapper nao encontrado (jogo nao usa PE?)\n");
            fflush(g_dbglog);
        }
    }
}
