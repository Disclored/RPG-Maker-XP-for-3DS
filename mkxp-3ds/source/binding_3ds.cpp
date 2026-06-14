#include <3ds.h>
#include <mruby.h>
#include <mruby/string.h>
#include <mruby/array.h>
#include <mruby/compile.h>
#include <mruby/class.h>
#include <mruby/variable.h>
#include <mruby/error.h>
#include <mruby/dump.h>
#include <mruby/irep.h>
#include <mruby/proc.h>
#include <zlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <vector>
#include "audio_3ds.h"
#include "profile_3ds.h"

/* Definida em marshal.cpp — limpa o cache de resolucao de classe do marshal.
 * Chamada quando o mruby e' (re)criado, para o cache nunca reter ponteiros de
 * uma sessao anterior. extern "C" tem de estar em scope de ficheiro (nao em
 * bloco de funcao). */
extern "C" void marshal_class_cache_reset(void);

/* ─────────────────────────────────────────────────────────────────────────
 * FALLBACK para mrb_dump_irep / constantes de dump.
 * A libmruby do port EXPORTA mrb_dump_irep (nm: "T mrb_dump_irep"), mas o
 * header <mruby/dump.h> esconde a DECLARACAO e as constantes (DUMP_ENDIAN_NAT,
 * MRB_DUMP_OK) atras de #ifndef MRB_NO_STDIO. Como a lib foi compilada com
 * MRB_NO_STDIO, o header nao as declara -> erro de compilacao, apesar de a
 * funcao existir no binario. Declaramo-las nos com a assinatura e valores
 * estaveis do mruby 3.2.0.
 * ───────────────────────────────────────────────────────────────────────── */
#ifndef MRB_DUMP_OK
#define MRB_DUMP_OK 0
#endif
#ifndef DUMP_ENDIAN_NAT
/* MRB_DUMP_ENDIAN_BIG(4) | MRB_DUMP_ENDIAN_LIT(8) = 12. "Native" no mruby 3.2. */
#define DUMP_ENDIAN_NAT 12
#endif
#ifndef MKXP_MRB_DUMP_IREP_DECLARED
#define MKXP_MRB_DUMP_IREP_DECLARED 1
extern "C" int mrb_dump_irep(mrb_state *mrb, const struct mrb_irep *irep,
                             uint8_t flags, uint8_t **bin, size_t *bin_size);
#endif

#include "display_3ds.h"
#include "input_3ds.h"
#include "platform_3ds.h"
#include "marshal.h"
extern void graphicsBindingInit(mrb_state *mrb);
extern void inputBindingInit(mrb_state *mrb);
#include "input_binding_3ds.h"
#include "graphics_binding_3ds.h"
#include "etc_binding_3ds.h"
#include "bitmap_binding_3ds.h"
#include "sprite_binding_3ds.h"
#include "compat_stubs.h"
#include "tilemap_binding_3ds.h"

/* Declaracao antecipada do log global (definido mais abaixo, ~linha 195) para
 * que o pool allocator (namespace mkxp_pool, logo a seguir) o possa usar via
 * ::g_dbglog no diagnostico. Sem esta linha, o 'extern' dentro do namespace
 * criava mkxp_pool::g_dbglog e ::g_dbglog ficava por declarar. */
extern FILE *g_dbglog;

/* ─────────────────────────────────────────────────────────────────────────────
 * INTERRUPTOR DO POOL ALLOCATOR
 * ----------------------------------------------------------------------------
 * 1 = usar o pool (mrb_open_allocf) -- rapido, mas em diagnostico no 3DS.
 * 0 = usar mrb_open() normal -- mais lento no boot (malloc O(n) do sistema),
 *     mas serve para ISOLAR: se com 0 o jogo arranca e com 1 trava, o problema
 *     esta' confirmadamente no pool. Mudar so' este valor e recompilar.
 * ─────────────────────────────────────────────────────────────────────────── */
#define USE_POOL_ALLOCATOR 1


/* ============================================================================
 * POOL ALLOCATOR PARA MRUBY (CORRECAO RAIZ DA LENTIDAO DO BOOT NO 3DS)
 * ----------------------------------------------------------------------------
 * CAUSA RAIZ (provada por medicao): ao desserializar dados grandes (ex:
 * CommonEvents.rxdata cria 113 mil objetos), o mruby chama o malloc do SISTEMA
 * ~100.000 vezes -- uma por cada objeto pequeno (string, array interno, ivar).
 * O malloc do devkitARM/newlib percorre uma free-list que CRESCE com o numero
 * de blocos ja' alocados -> cada malloc fica O(n) -> 100k mallocs com a heap
 * cheia (376 scripts + 54 plugins + dados ja' carregados) = O(n^2) = 220s SO'
 * no CommonEvents. (No PC isto nao aparece: o malloc do glibc e' O(1).) O
 * interval_ratio do GC NAO ajuda porque o problema nao e' o GC -- e' o malloc.
 *
 * SOLUCAO: dar ao mruby (via mrb_open_allocf) um allocator com pools de tamanho
 * fixo (size-class) e free-list por classe. alloc/free ficam O(1) e as 100k
 * chamadas ao malloc do sistema caem para ~200 (1 por chunk de 4096 objetos).
 * Medido no servidor: CommonEvents 100.611 -> 234 mallocs de sistema (-430x),
 * integridade 100% (len=257, todos os objetos validos), GC liberta na mesma.
 * Blocos grandes (> maior classe) caem no malloc normal (raros, sem impacto).
 * ========================================================================== */
namespace mkxp_pool {
    static const size_t kClassSizes[] = { 32, 64, 96, 128, 192, 256, 384, 512 };
    static const int    kNumClasses   = (int)(sizeof(kClassSizes)/sizeof(kClassSizes[0]));
    static const int    kChunkObjs    = 512;    /* objetos por chunk do sistema
        (ERA 4096 -> chunks ate' 2MB de uma vez, que no 3DS podem ser lentos a
        alocar ou falhar com heap fragmentada -> parecia travado no boot. Com
        512, o maior chunk e' 256KB: mallocs rapidos, cresce conforme precisa,
        e o overhead de mais chunks e' irrelevante porque ficam vivos.) */

    struct FreeNode { FreeNode *next; };
    struct Pool {
        FreeNode *free_list;
        char    **chunks;       /* chunks alocados (mantidos vivos toda a sessao) */
        int       chunk_count;
        int       chunk_capa;
        size_t    obj_size;
    };
    /* Cabecalho por bloco: guarda a classe (ou -1 se veio do malloc normal).
     * CRITICO (bug que travava o boot no 3DS): no ARM 32-bit, size_t = 4 bytes,
     * por isso sizeof(Header) seria 4. O ponteiro devolvido ao mruby e' (h+1),
     * ou seja chunk + sizeof(Header). Com Header de 4 bytes, esse ponteiro fica
     * alinhado a 4, NAO a 8. O mruby guarda nesses blocos valores que exigem
     * alinhamento de 8 no ARM (mrb_value/double/int64); aceder a eles num
     * endereco desalinhado provoca DATA ABORT -> crash silencioso (o 3DS
     * "congelava" no mrb_open_allocf). alignas(8) forca o Header a 8 bytes, por
     * isso (h+1) fica sempre alinhado a 8. (No servidor 64-bit size_t ja' tem 8,
     * por isso o bug nunca aparecia nos testes -- so' no hardware real.)
     * Isto explica o sintoma "com log detalhado funcionava": as escritas do log
     * mexiam na heap e calhavam alinhar os blocos por acaso. */
    struct alignas(8) Header { size_t cls; };

    static Pool s_pools[8];          /* = kNumClasses */
    static bool s_inited = false;
    static long s_sys_alloc = 0;     /* contadores p/ diagnostico no log */
    static long s_pool_alloc = 0;

    /* Forward declarations: init() chama alloc()/dealloc() (definidas abaixo)
     * para o auto-teste de alinhamento. */
    static void* alloc(size_t size);
    static void  dealloc(void *ptr);

    static void init() {
        for (int i = 0; i < kNumClasses; i++) {
            s_pools[i].free_list   = NULL;
            s_pools[i].chunks      = NULL;
            s_pools[i].chunk_count = 0;
            s_pools[i].chunk_capa  = 0;
            s_pools[i].obj_size    = kClassSizes[i];
        }
        s_inited = true;
        /* AUTO-TESTE DE ALINHAMENTO (corre no 3DS real, prova a correcao do bug).
         * Loga sizeof(Header) e sizeof(FreeNode) -- no ARM 32-bit ANTES da
         * correcao Header era 4; com alignas(8) deve ser 8. Depois aloca um
         * bloco de teste e confirma que o ponteiro devolvido esta' alinhado a 8.
         * Se aparecer "DESALINHADO" no log, o bug persiste e e' por aqui. */
        if (::g_dbglog) {
            fprintf(::g_dbglog,
                "[POOL|ALIGN] sizeof(Header)=%u sizeof(FreeNode)=%u sizeof(size_t)=%u sizeof(void*)=%u\n",
                (unsigned)sizeof(Header), (unsigned)sizeof(FreeNode),
                (unsigned)sizeof(size_t), (unsigned)sizeof(void*));
            /* teste de alinhamento de um bloco real */
            void *test = alloc(32);
            if (test) {
                unsigned long addr = (unsigned long)test;
                fprintf(::g_dbglog, "[POOL|ALIGN] bloco teste @ 0x%lx alinhado_a_8=%s\n",
                        addr, (addr % 8 == 0) ? "SIM (OK)" : "NAO (BUG!)");
                dealloc(test);
            }
            fflush(::g_dbglog);
        }
    }
    static int size_to_class(size_t size) {
        for (int i = 0; i < kNumClasses; i++)
            if (size <= kClassSizes[i]) return i;
        return -1;
    }
    static void grow(Pool *p) {
        size_t bytes = p->obj_size * (size_t)kChunkObjs;
        char *chunk = (char*)malloc(bytes);
        s_sys_alloc++;
        if (!chunk) {
            if (::g_dbglog) { fprintf(::g_dbglog, "[POOL|GROW] malloc(%u) FALHOU (OOM)\n", (unsigned)bytes); fflush(::g_dbglog); }
            return;   /* OOM: free_list fica NULL, alloc devolve NULL */
        }
        if (p->chunk_count >= p->chunk_capa) {
            p->chunk_capa = p->chunk_capa ? p->chunk_capa * 2 : 8;
            p->chunks = (char**)realloc(p->chunks, (size_t)p->chunk_capa * sizeof(char*));
        }
        p->chunks[p->chunk_count++] = chunk;
        for (size_t off = 0; off + p->obj_size <= bytes; off += p->obj_size) {
            FreeNode *n = (FreeNode*)(chunk + off);
            n->next = p->free_list;
            p->free_list = n;
        }
        /* DIAG so' em OOM (acima). O log por-grow foi removido depois de
         * confirmado que o pool funciona -- escrever no SD a cada chunk
         * abrandava o boot. */
    }
    static void* alloc(size_t size) {
        size_t total = size + sizeof(Header);
        int cls = size_to_class(total);
        if (cls < 0) {                     /* grande demais -> malloc normal */
            Header *h = (Header*)malloc(total);
            s_sys_alloc++;
            if (!h) return NULL;
            h->cls = (size_t)-1;
            return (void*)(h + 1);
        }
        Pool *p = &s_pools[cls];
        if (!p->free_list) grow(p);
        if (!p->free_list) return NULL;    /* OOM */
        FreeNode *n = p->free_list;
        p->free_list = n->next;
        Header *h = (Header*)n;
        h->cls = (size_t)cls;
        s_pool_alloc++;
        return (void*)(h + 1);
    }
    static void dealloc(void *ptr) {
        if (!ptr) return;
        Header *h = ((Header*)ptr) - 1;
        if (h->cls == (size_t)-1) { free(h); return; }
        Pool *p = &s_pools[h->cls];
        FreeNode *n = (FreeNode*)h;
        n->next = p->free_list;
        p->free_list = n;
    }
    static void* re_alloc(void *ptr, size_t size) {
        if (!ptr) return alloc(size);
        Header *h = ((Header*)ptr) - 1;
        size_t total = size + sizeof(Header);
        if (h->cls != (size_t)-1) {        /* estava no pool */
            if (size_to_class(total) == (int)h->cls) return ptr;  /* cabe na mesma classe */
            void *np = alloc(size);
            if (!np) return NULL;
            size_t oldsz = kClassSizes[h->cls] - sizeof(Header);
            memcpy(np, ptr, size < oldsz ? size : oldsz);
            dealloc(ptr);
            return np;
        }
        Header *nh = (Header*)realloc(h, total);   /* era grande -> realloc normal */
        if (!nh) return NULL;
        nh->cls = (size_t)-1;
        return (void*)(nh + 1);
    }
    /* Assinatura mrb_allocf: (mrb, ptr, size, ud).
     *   size==0            -> free
     *   ptr!=NULL,size>0   -> realloc
     *   ptr==NULL,size>0   -> malloc                                          */
    static long s_allocf_calls = 0;   /* diagnostico: quantas vezes foi chamado */
    static void* allocf(mrb_state *mrb, void *ptr, size_t size, void *ud) {
        (void)mrb; (void)ud;
        /* DIAGNOSTICO: durante a criacao do mruby (mrb_open_allocf), o allocator
         * e' chamado muitas vezes. Se travar AQUI, o log mostra ate' que numero
         * chegou. Logamos as primeiras chamadas e depois a cada 5000, direto no
         * ficheiro (g_dbglog ja' esta' aberto nesta fase). */
        s_allocf_calls++;
        /* DIAGNOSTICO ESCASSO: o pool ja' foi confirmado a funcionar (alinhamento
         * OK, contadores sobem). Reduzido para cada 50000 chamadas (era 2000) --
         * o suficiente para ver se trava, sem encher o log nem abrandar. */
        if (::g_dbglog) {
            bool show = (s_allocf_calls <= 3) || ((s_allocf_calls % 50000) == 0);
            if (show) {
                fprintf(::g_dbglog, "[POOL|DIAG] allocf #%ld (pool=%ld sys=%ld)\n",
                        s_allocf_calls, s_pool_alloc, s_sys_alloc);
                fflush(::g_dbglog);
            }
        }
        if (size == 0) {
            dealloc(ptr);
            g_prof_frees++;          /* PROFILING: frees = proxy da atividade do GC */
            return NULL;
        }
        if (ptr)        return re_alloc(ptr, size);
        g_prof_objs_created++;       /* PROFILING: nova alocacao (objeto/buffer) */
        return alloc(size);
    }
}

/* =========================================================
 * SUPER DEBUG HELPERS
 * ========================================================= */

/* Ficheiro de log global para dump detalhado */
FILE *g_dbglog = NULL;
/* Contador global de escritas para flush periodico do log (otimizacao 3DS:
 * evita fflush sincrono ao SD a cada escrita). Partilhado pelos macros de log
 * nos varios ficheiros via 'extern int g_dbglog_flushc'. */
int g_dbglog_flushc = 0;

/* ---- TIMESTAMPS no log -----------------------------------------------------
 * Para diagnosticar ONDE o boot demora (ex: CommonEvents->System a meio do
 * Game.initialize), cada linha de log leva um prefixo:
 *    [t=  12.345s d= 1234ms]
 * t = segundos totais desde o 1o log (relogio absoluto do boot)
 * d = delta em ms desde a linha ANTERIOR (mostra qual passo custou tempo)
 * Usa svcGetSystemTick() (relogio ARM11 a 268MHz) -> ms reais.
 * SYSCLOCK_ARM11 = 268111856 Hz. */
static u64 g_log_tick0    = 0;   /* tick do 1o log (base) */
static u64 g_log_tickprev = 0;   /* tick do log anterior (para delta) */
/* ticks->ms: o mesmo divisor que o contador de FPS em display_3ds.cpp
 * (268111.856 = SYSCLOCK_ARM11/1000), comprovadamente correto no 3DS. */
static double tick_to_ms(u64 ticks) {
    return (double)ticks / 268111.856;
}
/* Devolve, por referencia, ms-totais e ms-delta; atualiza o estado.
 * NAO static: partilhado com marshal.cpp (e outros) via 'extern' para que as
 * linhas [marshal] tambem levem timestamp. */
void dbglog_stamp(double *out_total_s, double *out_delta_ms) {
    u64 now = svcGetSystemTick();
    if (g_log_tick0 == 0) { g_log_tick0 = now; g_log_tickprev = now; }
    *out_total_s  = tick_to_ms(now - g_log_tick0) / 1000.0;
    *out_delta_ms = tick_to_ms(now - g_log_tickprev);
    g_log_tickprev = now;
}

static void dbglog_open() {
    g_dbglog = fopen("sdmc:/mkxp/debug_binding.log", "w");
    if (!g_dbglog) printf("[DBGLOG] AVISO: nao foi possivel criar sdmc:/mkxp/debug_binding.log\n");
    else {
        /* BUFFER GRANDE (64KB) em vez de line-buffering. Durante o diagnostico
         * do boot usavamos _IOLBF (flush por linha) para nao perder nada se
         * travasse -- mas isso fazia 1 escrita ao SD POR LINHA, e no jogo (com
         * muito log por frame) derrubava o FPS para ~4. Agora que o boot esta'
         * confirmado, um buffer de 64KB junta muitas linhas por escrita -> SD
         * toca raramente -> runtime rapido. O DBGLOG critico do boot faz flush
         * explicito nos pontos-chave ([BOOT] passo N/4), por isso nao se perde
         * o essencial. */
        static char s_logbuf[65536];
        setvbuf(g_dbglog, s_logbuf, _IOFBF, sizeof(s_logbuf));
        printf("[DBGLOG] log aberto em sdmc:/mkxp/debug_binding.log (buffer 64KB)\n");
    }
}
static void dbglog_close() { if (g_dbglog) { fclose(g_dbglog); g_dbglog = NULL; } }

/* Escreve para consola E para ficheiro, com timestamp [t=...s d=...ms] no inicio
 * de cada linha. O timestamp permite ver exatamente quanto tempo cada passo do
 * boot/jogo demorou e apanhar gargalos (ex: 2-3 min num passo especifico). */
#define DBGLOG(fmt, ...) \
    do { \
        double __ts_t, __ts_d; dbglog_stamp(&__ts_t, &__ts_d); \
        printf("[t=%8.3fs d=%7.1fms] " fmt, __ts_t, __ts_d, ##__VA_ARGS__); \
        if (g_dbglog) { fprintf(g_dbglog, "[t=%8.3fs d=%7.1fms] " fmt, __ts_t, __ts_d, ##__VA_ARGS__); \
            if (++g_dbglog_flushc >= 256) { g_dbglog_flushc = 0; fflush(g_dbglog); } } \
    } while(0)

/* Dump ate N bytes de um buffer como texto (printaveis) + hex para o resto */
static void dump_source_region(const char *label, const char *buf, size_t total,
                                size_t from, size_t to) {
    if (from >= total) return;
    if (to   >  total) to = total;
    DBGLOG("--- %s [bytes %zu..%zu] ---\n", label, from, to);
    for (size_t i = from; i < to; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\n') {
            if (g_dbglog) fputc('\n', g_dbglog);
            printf("\n");
        } else if (c == '\r') {
            /* skip \r, ja vira \n */
        } else if (c >= 0x20 && c < 0x7f) {
            if (g_dbglog) fputc(c, g_dbglog);
            printf("%c", c);
        } else {
            if (g_dbglog) fprintf(g_dbglog, "\\x%02X", c);
            printf("\\x%02X", c);
        }
    }
    DBGLOG("\n--- fim %s ---\n", label);
}

/* Dumpa o script inteiro para um ficheiro em sdmc */
static void dump_script_to_file(const char *name, const char *buf, size_t sz) {
    char path[256];
    snprintf(path, sizeof(path), "sdmc:/mkxp/script_dump_%s.rb", name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        DBGLOG("[DUMP] nao foi possivel criar %s\n", path);
        return;
    }
    fwrite(buf, 1, sz, f);
    fclose(f);
    DBGLOG("[DUMP] script '%s' guardado em %s (%zu bytes)\n", name, path, sz);
}

/* Procura uma string num buffer e imprime contexto (+/-80 chars) */
static void find_and_show(const char *haystack, size_t hlen,
                           const char *needle, const char *label) {
    size_t nlen = strlen(needle);
    const char *p = haystack;
    int found_count = 0;
    while ((size_t)(p - haystack) + nlen <= hlen) {
        if (memcmp(p, needle, nlen) == 0) {
            found_count++;
            size_t pos = (size_t)(p - haystack);
            size_t ctx_start = pos > 80 ? pos - 80 : 0;
            size_t ctx_end   = pos + nlen + 80 < hlen ? pos + nlen + 80 : hlen;
            DBGLOG("[FIND '%s' #%d] pos=%zu\n", label, found_count, pos);
            dump_source_region("contexto", haystack, hlen, ctx_start, ctx_end);
            p += nlen;
        } else {
            p++;
        }
    }
    if (found_count == 0)
        DBGLOG("[FIND '%s'] NAO ENCONTRADO no buffer\n", label);
    else
        DBGLOG("[FIND '%s'] total=%d ocorrencias\n", label, found_count);
}

/* =========================================================
 * WRAPPERS ORIGINAIS
 * ========================================================= */

static void show_error(mrb_state *mrb, const char *ctx_msg) {
    if (!mrb->exc) { DBGLOG("[ERROR] %s (no exception)\n", ctx_msg); return; }
    mrb_value exc = mrb_obj_value(mrb->exc);
    mrb->exc = 0;

    const char *cls = mrb_class_name(mrb, mrb_class(mrb, exc));
    if (!cls) cls = "?";

    mrb_value mesg = mrb_funcall(mrb, exc, "message", 0);
    if (mrb->exc) { mrb->exc = 0; mesg = mrb_nil_value(); }
    const char *msg = mrb_string_p(mesg) ? RSTRING_PTR(mesg) : "(no message)";

    DBGLOG("[ERROR] %s: %s: %s\n", ctx_msg, cls, msg);

    mrb_value bt = mrb_funcall(mrb, exc, "backtrace", 0);
    if (mrb->exc) { mrb->exc = 0; bt = mrb_nil_value(); }
    if (mrb_array_p(bt)) {
        int blen = (int)RARRAY_LEN(bt);
        if (blen > 8) blen = 8;
        for (int bi = 0; bi < blen; bi++) {
            mrb_value bl = mrb_ary_entry(bt, bi);
            if (mrb_string_p(bl)) DBGLOG("  bt[%d]: %s\n", bi, RSTRING_PTR(bl));
        }
    }
}

static void *local_memmem(const void *hay, size_t hlen,
                           const void *ndl, size_t nlen) {
    if (nlen == 0) return (void *)hay;
    if (hlen < nlen) return NULL;
    const char *h = (const char *)hay;
    const char *n = (const char *)ndl;
    for (size_t i = 0; i <= hlen - nlen; i++)
        if (memcmp(h + i, n, nlen) == 0) return (void *)(h + i);
    return NULL;
}

static const char * __attribute__((used)) normalize_script_name(const char *raw, size_t raw_len,
                                          char *out, size_t out_sz) {
    const char *p = raw;
    size_t remain = raw_len;
    while (remain > 0 && ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e)) {
        p++; remain--;
    }
    size_t copy_len = remain < out_sz - 1 ? remain : out_sz - 1;
    memcpy(out, p, copy_len);
    out[copy_len] = '\0';
    for (size_t i = 0; i < copy_len; i++) {
        if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e) {
            out[i] = '\0'; break;
        }
    }
    return out;
}

/* Converte nome de script num nome de ficheiro seguro (sem espacos/simbolos) */
static void __attribute__((used)) make_safe_name(const char *name, char *out, size_t out_sz) {
    size_t j = 0;
    for (size_t i = 0; name[i] && j < out_sz - 1; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            out[j++] = c;
        } else if (c == ' ' || c == '-') {
            out[j++] = '_';
        }
        /* outros caracteres ignorados */
    }
    if (j == 0) { out[j++] = 'x'; }
    out[j] = '\0';
}

static void remove_unless_block(char *buf, size_t len, const char *marker) {
    char *found = (char*)local_memmem(buf, len, marker, strlen(marker));
    if (!found) return;
    char *start = found;
    while (start > buf && start[-1] != '\n') start--;
    char *p = found;
    char *end_of_block = NULL;
    int depth = 0;
    while (p < buf + len - 3) {
        if (strncmp(p,"unless ",7)==0 || strncmp(p,"class ",6)==0 ||
            strncmp(p,"module ",7)==0 || strncmp(p,"if ",3)==0 ||
            strncmp(p,"def ",4)==0 || strncmp(p,"do ",3)==0 ||
            strncmp(p,"do\n",3)==0 || strncmp(p,"do\r",3)==0) depth++;
        if (strncmp(p,"end",3)==0) {
            char after = p[3];
            if (after=='\n'||after=='\r'||after==' '||after=='\0'||after=='#') {
                if (depth <= 1) {
                    end_of_block = p + 3;
                    if (*end_of_block == '\r') end_of_block++;
                    if (*end_of_block == '\n') end_of_block++;
                    break;
                }
                depth--;
            }
        }
        p++;
    }
    if (end_of_block && end_of_block > start)
        memset(start, ' ', end_of_block - start);
}

/* =========================================================
 * PATCH MAIN -- versao com debug exaustivo
 * ========================================================= */
static void patch_main_loop(char *buf, size_t buf_sz) {
    const char *markers[] = { "\r\nloop do\r\n", "\nloop do\r\n", "\nloop do\n", NULL };

    for (int m = 0; markers[m] != NULL; m++) {
        size_t marker_len = strlen(markers[m]);
        char *found = (char*)local_memmem(buf, buf_sz, markers[m], marker_len);
        if (!found) continue;

        /* erase_start: the '\n'/'\r\n' before 'loop', so the blank line is clean */
        char *erase_start = found;
        /* Start scanning AFTER the "loop do\n" (or "loop do\r\n") line */
        char *p = found + marker_len;

        /* We track depth only for blocks that START at column 0 (after a newline).
         * "loop do" at col 0 = depth 1. We need the "end" at col 0 that brings
         * depth back to 0. */
        int depth = 1;
        char *end_pos = NULL;

        while (p < buf + buf_sz) {
            /* Only look at the start of lines (after \n) */
            if (p == buf || p[-1] == '\n') {
                /* skip leading spaces/tabs */
                char *lp = p;
                while (lp < buf + buf_sz && (*lp == ' ' || *lp == '\t')) lp++;

                /* Check for block-opening keywords at this line start */
                if (strncmp(lp,"loop ",5)==0 || strncmp(lp,"loop\n",5)==0 ||
                    strncmp(lp,"loop\r",5)==0 ||
                    strncmp(lp,"while ",6)==0 || strncmp(lp,"until ",6)==0 ||
                    strncmp(lp,"for ",4)==0   ||
                    strncmp(lp,"if ",3)==0    || strncmp(lp,"if\n",3)==0   ||
                    strncmp(lp,"if\r",3)==0   ||
                    strncmp(lp,"unless ",7)==0||
                    strncmp(lp,"begin\n",6)==0|| strncmp(lp,"begin\r",6)==0||
                    strncmp(lp,"def ",4)==0   ||
                    strncmp(lp,"class ",6)==0 ||
                    strncmp(lp,"module ",7)==0) {
                    depth++;
                } else {
                    /* Also check for trailing " do" on this line (iterator blocks) */
                    char *eol = lp;
                    while (eol < buf + buf_sz && *eol != '\n' && *eol != '\r') eol++;
                    char *eoln = eol;
                    if (eoln > lp && eoln[-1] == '\r') eoln--;
                    if (eoln - lp >= 3 &&
                        eoln[-3]==' ' && eoln[-2]=='d' && eoln[-1]=='o') {
                        depth++;
                    }
                }

                /* Check for "end" closing a block */
                if (strncmp(lp,"end",3)==0) {
                    char after = lp[3];
                    if (after=='\n'||after=='\r'||after==' '||
                        after=='\0'||after=='#'||after==';') {
                        depth--;
                        if (depth == 0) {
                            end_pos = lp + 3;
                            if (end_pos < buf + buf_sz && *end_pos == '\r') end_pos++;
                            if (end_pos < buf + buf_sz && *end_pos == '\n') end_pos++;
                            break;
                        }
                    }
                }
            }
            p++;
        }

        if (end_pos && end_pos > erase_start) {
            size_t erase_bytes = (size_t)(end_pos - erase_start);
            DBGLOG("[PATCH Main] loop do..end removido (%zu bytes)\n", erase_bytes);
            memset(erase_start, ' ', erase_bytes);
        } else {
            DBGLOG("[PATCH Main] AVISO: 'end' correspondente nao encontrado\n");
            memset(erase_start, ' ', buf_sz - (size_t)(erase_start - buf));
        }
        return;
    }
    DBGLOG("[PATCH Main] AVISO: 'loop do' top-level nao encontrado\n");
}
/* =========================================================
 * REPLACE defined?(...) -- mruby nao suporta defined? como keyword em todos
 * os contextos. Substitui cada ocorrencia de defined?(EXPR) por nil para que
 * os blocos "unless defined?(X)" sejam sempre executados (comportamento seguro
 * para scripts de inicializacao como MessageConfig).
 * ========================================================= */
static void replace_defined_keyword(char *buf, size_t len) {
    const char needle[] = "defined?(";
    const size_t nlen   = sizeof(needle) - 1; /* 9 chars */

    char *p = buf;
    while ((size_t)(p - buf) + nlen <= len) {
        if (memcmp(p, needle, nlen) != 0) { p++; continue; }

        /* Verificar que nao esta dentro de um comentario de linha */
        char *line_start = p;
        while (line_start > buf && line_start[-1] != '\n') line_start--;
        bool in_comment = false;
        for (char *c = line_start; c < p; c++) {
            if (*c == '#') { in_comment = true; break; }
        }
        if (in_comment) { p += nlen; continue; }

        char *expr_start = p;           /* aponta para 'd' de defined?( */
        char *inner      = p + nlen;    /* logo apos o '(' inicial      */
        size_t remaining = len - (size_t)(inner - buf);
        (void)remaining;  /* usado implicitamente pelo loop abaixo via scan vs buf+len */
        int depth = 1;
        char *scan = inner;

        while ((size_t)(scan - buf) < len && depth > 0) {
            if      (*scan == '(') depth++;
            else if (*scan == ')') { depth--; if (depth == 0) break; }
            scan++;
        }

        if (depth != 0) { p += nlen; continue; } /* parentesis nao fechado */

        char *expr_end = scan + 1; /* logo apos o ')' de fecho */
        size_t total   = (size_t)(expr_end - expr_start);

        if (total < 3) { p += nlen; continue; }

        /* Substituir por "nil" + espacos */
        expr_start[0] = 'n';
        expr_start[1] = 'i';
        expr_start[2] = 'l';
        if (total > 3) memset(expr_start + 3, ' ', total - 3);

        p = expr_start + total;
    }
}

/* =========================================================
 * PATCH SCRIPT
 * ========================================================= */
static void patch_script(std::vector<Bytef> &decomp, uLong dest_sz, const char *name) {
    char *buf = (char*)decomp.data();

    if (strcmp(name, "PluginManager") == 0 ||
        strcmp(name, "Intl_Messages") == 0 ||
        strcmp(name, "Compiler") == 0 ||
        strcmp(name, "Compiler_CompilePBS") == 0 ||
        strcmp(name, "Compiler_WritePBS") == 0 ||
        strcmp(name, "Compiler_MapsAndEvents") == 0 ||
        // Fix 5: script 189 demasiado grande para codegen mruby (131 KB)
        // Fix 5: script 191 depende do 189 que falhou
        strcmp(name, "AI_Move_EffectScores") == 0 ||
        strcmp(name, "AI_Move_EffectScores_Gen8") == 0 ||
        // Fix 6: SyntaxError em Ruby nao suportado por mruby
        strcmp(name, "EBDX Wrapper Compatibility") == 0 ||
        // Fix 4: ArgumentError em validate/register -- bloco mruby vs CRuby
        // Estes scripts apenas registam campos de save; nao sao necessarios para boot
        strcmp(name, "Game_SaveValues") == 0 ||
        strcmp(name, "Game_SaveConversions") == 0 ||
        // Fix 6: deprecated_method_alias / deprecate_constant nao existem em mruby
        strcmp(name, "Player_Deprecated") == 0 ||
        strcmp(name, "Pokemon_Deprecated") == 0 ||
        // Fix 7: Sprite_DynamicShadows usa APIs graficas avancadas nao suportadas
        strcmp(name, "Sprite_DynamicShadows") == 0 ||
        // Fix 8: Game_DependentEvents falha com uninitialized constant no check_entry
        strcmp(name, "Game_DependentEvents") == 0 ||
        // Fix 9: FileTests redefine FileTest.exists? com validate interno que chama
        // o binding C nativo com assinatura errada -> ArgumentError em mruby.
        // Os nossos stubs em compat_stubs.h cobrem FileTest completamente.
        strcmp(name, "FileTests") == 0 ||
        // Fix 10: Validation usa validate privado que conflitua com mruby C binding
        strcmp(name, "Validation") == 0) {
        DBGLOG("[SKIP-PATCH] %s\n", name);
        memset(buf, ' ', dest_sz);
        return;
    }

    /* CRITICO: remove_unless_block tem de correr ANTES de replace_defined_keyword.
     * replace_defined_keyword substitui defined?(X) por nil (+ espacos), destruindo
     * os marcadores literais que remove_unless_block procura. Se a ordem for invertida,
     * remove_unless_block nao encontra nada e os blocos KGC correm sempre, sobrescrevendo
     * Graphics.transition e Graphics.update com versoes Ruby que ignoram o binding C++.
     * Resultado observado: grph_transition/grph_update nunca aparecem nos logs porque
     * os metodos C foram substituidos silenciosamente pelo script Transitions. */

    if (strcmp(name, "Input") == 0)
        remove_unless_block(buf, dest_sz, "unless defined?(update_KGC_ScreenCapture)");

    if (strcmp(name, "Transitions") == 0) {
        remove_unless_block(buf, dest_sz, "unless defined?(transition_KGC_SpecialTransition)");
        remove_unless_block(buf, dest_sz, "unless defined?(update_KGC_SpecialTransition)");
    }

    /* [DEFCHK] DIAGNOSTICO + FIX da forma "defined? expr" (SEM parentese colado).
     * O replace_defined_keyword (abaixo) so apanha "defined?(". A forma sem
     * parentese -- ex: "X = Y unless defined? Z" -- escapa, e o mruby tenta
     * chamar 'defined?' como metodo -> NoMethodError (rebenta o script, ex:
     * 003_Dialogue_Specific.rb:9). Aqui localizamos cada ocorrencia BARE, logamos
     * script:linha+contexto (para isolar), e substituimos os 8 bytes "defined?"
     * por "false &&" (exatamente 8 chars): curto-circuita (a expr NAO e avaliada)
     * e e falsy -- mesma semantica do replace que poe nil. Sem mudanca de tamanho. */
    {
        char *q = buf;
        size_t blen = (size_t)dest_sz;
        while ((size_t)(q - buf) + 8 <= blen) {
            if (memcmp(q, "defined?", 8) != 0) { q++; continue; }
            /* excluir sufixos de nome de metodo: method_defined?, const_defined?, obj.defined? */
            if (q > buf) {
                char pc = q[-1];
                bool ident = (pc >= 'a' && pc <= 'z') || (pc >= 'A' && pc <= 'Z') ||
                             (pc >= '0' && pc <= '9') || pc == '_' || pc == '.';
                if (ident) { q += 8; continue; }
            }
            /* excluir comentario de linha */
            {
                char *ls0 = q; bool cmt = false;
                while (ls0 > buf && ls0[-1] != '\n') ls0--;
                for (char *c = ls0; c < q; c++) { if (*c == '#') { cmt = true; break; } }
                if (cmt) { q += 8; continue; }
            }
            /* proximo char nao-espaco depois de "defined?" */
            char *r = q + 8;
            while ((size_t)(r - buf) < blen && (*r == ' ' || *r == '\t')) r++;
            char nx = ((size_t)(r - buf) < blen) ? *r : '\0';
            if (nx != '(') {
                /* forma BARE -> escapa ao replace_defined_keyword. Logar + corrigir. */
                int line = 1;
                for (char *c = buf; c < q; c++) if (*c == '\n') line++;
                char *ls = q; while (ls > buf && ls[-1] != '\n') ls--;
                char *le = q; while ((size_t)(le - buf) < blen && *le != '\n') le++;
                int n = (int)(le - ls); if (n > 90) n = 90;
                char ctx[96];
                memcpy(ctx, ls, n); ctx[n] = '\0';
                DBGLOG("[DEFCHK] %s:%d defined? SEM parentese -> corrigido (false &&): %s\n",
                       name, line, ctx);
                memcpy(q, "false &&", 8);   /* 8 bytes, mesmo tamanho */
            }
            q += 8;
        }
    }

    /* Substituir defined?(...) por nil em TODOS os scripts -- fix global para mruby.
     * Feito APOS os remove_unless_block acima para preservar os marcadores. */
    replace_defined_keyword(buf, (size_t)dest_sz);

    if (strcmp(name, "FileMixins") == 0) {
        const char *pat = "< IO";
        char *p = buf;
        while ((p = (char*)local_memmem(p, dest_sz - (size_t)(p - buf), pat, 4)) != NULL) {
            memset(p, ' ', 4); p += 4;
        }
    }

    if (strcmp(name, "Main") == 0) {
        patch_main_loop(buf, (size_t)dest_sz);
    }
}

/* =========================================================
 * VERIFICADOR DE METODOS EM TEMPO REAL
 * ========================================================= */
static void check_entry_methods(mrb_state *mrb, int script_idx, const char *script_name) {
    // -- FIX BUG 3: Forcar globals criticas APOS todos os 376 scripts carregarem --
    // Alguns scripts (ex: MKXP_Compatibility) contem:
    //   $DEBUG = false unless defined?($DEBUG)
    // O replace_defined_keyword() transforma defined?() em nil, produzindo:
    //   $DEBUG = false unless nil   <- nil e falsy, unless nil = sempre executa
    // Resultado: $DEBUG fica false mesmo que compat_stubs.h e ios_compat_3ds.rb
    // o tenham definido como true antes dos scripts carregarem.
    // Este bloco repoe os valores DEPOIS dos scripts e ANTES de mainFunctionDebug,
    // garantindo que pbCallTitle recebe $DEBUG=true -> Scene_DebugIntro (em vez de hang).
    {
        const char *force_globals =
            "$DEBUG   = true\n"
            "$MKXP    = true\n"
            "$joiplay = true\n"
            "$BTEST   = false\n"
            "# Garantir $data_system e sempre um RPG::System valido\n"
            "begin\n"
            "  unless $data_system.is_a?(RPG::System)\n"
            "    $data_system = RPG::System.new\n"
            "  end\n"
            "  sid = $data_system.start_map_id rescue nil\n"
            "  $data_system.start_map_id = 1 unless sid.is_a?(Integer) && sid > 0\n"
            "rescue; end\n"
            "# FIX: $data_tilesets -- Game_Map#setup acede a $data_tilesets[tileset_id]\n"
            "# Se for nil/Array simples, .tileset_name/.autotile_names crasham.\n"
            "begin\n"
            "  # DIAGNOSTICO: estado do $data_tilesets ANTES de decidir carregar\n"
            "  begin\n"
            "    _dtl = $data_tilesets.is_a?(Array) ? $data_tilesets.length : -1\n"
            "    _dt1 = ($data_tilesets.is_a?(Array) && $data_tilesets.length > 1) ? $data_tilesets[1] : nil\n"
            "    _dtn = (_dt1.respond_to?(:tileset_name) ? _dt1.tileset_name.to_s : '<sem metodo>')\n"
            "    dbg \"[TSET|DIAG] antes: class=#{$data_tilesets.class} len=#{_dtl} [1].tileset_name='#{_dtn}'\"\n"
            "  rescue => e\n"
            "    dbg \"[TSET|DIAG] antes ERRO: #{e.class}: #{e.message}\"\n"
            "  end\n"
            "  # FIX: carregar Tilesets.rxdata sempre que $data_tilesets[1] nao tenha nome real.\n"
            "  # (O fallback [nil, ts] tem length 2 e enganava a guarda antiga 'length <= 1',\n"
            "  #  deixando $data_tilesets[4] = nil -> Game_Map#setup le campos de nil -> mapa preto.)\n"
            "  t1 = ($data_tilesets.is_a?(Array) && $data_tilesets.length > 1) ? $data_tilesets[1] : nil\n"
            "  has_real = t1.respond_to?(:tileset_name) && (t1.tileset_name.to_s.length > 0)\n"
            "  if !has_real\n"
            "    begin\n"
            "      dbg '[TSET] sem dados reais -> a tentar load_data Tilesets.rxdata'\n"
            "      loaded = load_data('Data/Tilesets.rxdata')\n"
            "      dbg \"[TSET] carregado: #{loaded.class} len=#{loaded.length rescue '?'}\"\n"
            "      if loaded.is_a?(Array) && loaded.length > ($data_tilesets.is_a?(Array) ? $data_tilesets.length : 0)\n"
            "        $data_tilesets = loaded\n"
            "      end\n"
            "      dbg \"[TSET] depois: class=#{$data_tilesets.class} len=#{$data_tilesets.length rescue '?'} [1].name='#{($data_tilesets[1].tileset_name rescue '?')}'\"\n"
            "    rescue => e\n"
            "      dbg \"[TSET] ERRO load Tilesets.rxdata: #{e.class}: #{e.message}\"\n"
            "    end\n"
            "  else\n"
            "    dbg \"[TSET] ja tem dados reais (len=#{$data_tilesets.length rescue '?'}), skip load\"\n"
            "  end\n"
            "  unless $data_tilesets.is_a?(Array) && $data_tilesets.length > 1\n"
            "    ts = RPG::Tileset.new\n"
            "    ts.id = 1\n"
            "    $data_tilesets = [nil, ts]\n"
            "  end\n"
            "  if $data_tilesets.is_a?(Array)\n"
            "    $data_tilesets.each_with_index do |t, i|\n"
            "      next if i == 0\n"
            "      unless t.respond_to?(:tileset_name)\n"
            "        ts = RPG::Tileset.new\n"
            "        ts.id = i\n"
            "        $data_tilesets[i] = ts\n"
            "      end\n"
            "      if $data_tilesets[i].respond_to?(:autotile_names)\n"
            "        an = $data_tilesets[i].autotile_names rescue nil\n"
            "        if an.is_a?(Array)\n"
            "          $data_tilesets[i].autotile_names = an.map { |x| x.nil? ? '' : x.to_s }\n"
            "        else\n"
            "          $data_tilesets[i].autotile_names = Array.new(7, '')\n"
            "        end\n"
            "      end\n"
            "    end\n"
            "  end\n"
            "rescue; end\n"
            "# FIX: $data_map_infos -- visitedMaps e outros sistemas iterem sobre isto\n"
            "begin\n"
            "  unless $data_map_infos.is_a?(Hash)\n"
            "    mi = RPG::MapInfo.new\n"
            "    mi.name = 'Map001'\n"
            "    $data_map_infos = { 1 => mi }\n"
            "  end\n"
            "rescue; end\n"
            "# FIX: $data_animations -- Sprite_AnimationSprite usa isto\n"
            "begin\n"
            "  $data_animations = [] unless $data_animations.is_a?(Array)\n"
            "rescue; end\n"
            "# FIX: $data_common_events\n"
            "begin\n"
            "  $data_common_events = [nil] unless $data_common_events.is_a?(Array)\n"
            "rescue; end\n";
        mrb_load_string(mrb, force_globals);
        if (mrb->exc) {
            DBGLOG("[check_entry] WARN: force_globals falhou\n");
            mrb->exc = 0;
        } else {
            DBGLOG("[check_entry] globals forcados OK: $DEBUG=true $joiplay=true $MKXP=true\n");
        }
    }

    /* -----------------------------------------------------------------------
     * PATCH CRITICO: Spriteset_Map#initialize
     *
     * O Essentials PE redefine Spriteset_Map com um initialize que chama APIs
     * graficas avancadas (CustomTilemap, Sprite_Reflection, etc.) nao disponiveis
     * no 3DS. Quando falha, o rescue generico em Scene_Map#main engole o erro e
     * @spriteset fica nil -- causando NoMethodError a cada frame em Scene_Map#update.
     *
     * Este wrapper:
     *  (a) loga a excepcao real com classe, mensagem e backtrace completo
     *  (b) nao re-lanca -- deixa @spriteset como instancia valida (com ivars a nil/[])
     *      em vez de nil, evitando o crash em cascade.
     * ----------------------------------------------------------------------- */
    {
		static const char *ss_patch =
			/* Wrapper de Spriteset_Map#initialize */
			"begin\n"
			"  if Object.const_defined?(:Spriteset_Map) &&\n"
			"     Spriteset_Map.method_defined?(:initialize) &&\n"
			"     !Spriteset_Map.method_defined?(:__orig_init_3ds__)\n"
			"    class Spriteset_Map\n"
			"      alias __orig_init_3ds__ initialize\n"
			"      def initialize(*args)\n"
			"        MKXPDebug.log(\"[Spriteset_Map] initialize args=#{args.inspect}\") rescue nil\n"
			"        MKXPDebug.log('[Spriteset_Map] initialize: iniciando') rescue nil\n"
			"        begin\n"
			"          __orig_init_3ds__(*args)\n"
			"          MKXPDebug.log('[Spriteset_Map] initialize: OK') rescue nil\n"
			"          begin\n"
			"            _cs = @character_sprites rescue nil\n"
			"            _with_bmp = _cs.is_a?(Array) ? _cs.count { |s| s.respond_to?(:bitmap) && !s.bitmap.nil? rescue false } : -1\n"
			"            MKXPDebug.log(\"[Spriteset_Map] character_sprites=#{_cs.is_a?(Array) ? _cs.size : 'NIL'} com_bitmap=#{_with_bmp}\") rescue nil\n"
			"            _tm = @tilemap rescue nil\n"
			"            MKXPDebug.log(\"[Spriteset_Map] @tilemap=#{_tm.nil? ? 'NIL' : _tm.class}\") rescue nil\n"
			"            if _tm && _tm.respond_to?(:tileset)\n"
			"              _ts = _tm.tileset rescue nil\n"
			"              MKXPDebug.log(\"[Spriteset_Map] tileset=#{_ts.nil? ? 'NIL' : \"#{_ts.class} #{_ts.width rescue '?'}x#{_ts.height rescue '?'}\"}\") rescue nil\n"
			"            end\n"
			"            MKXPDebug.log(\"[Spriteset_Map] $game_map.events=#{$game_map.events.size rescue '?'}\") rescue nil\n"
			"          rescue; end\n"
			"        rescue => _ssinit_err\n"
			"          MKXPDebug.log(\"[Spriteset_Map] initialize CRASH: #{_ssinit_err.class}: #{_ssinit_err.message}\") rescue nil\n"
			"          begin\n"
			"            _bt = _ssinit_err.backtrace\n"
			"            if _bt && _bt.is_a?(Array)\n"
			"              _bt[0, 15].each_with_index { |l, i|\n"
			"                MKXPDebug.log(\"[Spriteset_Map]   bt[#{i}]: #{l}\") rescue nil\n"
			"              }\n"
			"            end\n"
			"          rescue; end\n"
			"          # Nao re-lancar -- deixar @spriteset como instancia valida\n"
			"          # com ivars criticos a valores seguros.\n"
			"          @tilemap           = nil rescue nil\n"
			"          @viewport1         = (Viewport.new(0,0,544,416) rescue nil)\n"
			"          @viewport2         = (Viewport.new(0,0,544,416) rescue nil)\n"
			"          @viewport3         = (Viewport.new(0,0,544,416) rescue nil)\n"
			"          @character_sprites = [] rescue nil\n"
			"          @picture_sprites   = [] rescue nil\n"
			"          @weather           = nil rescue nil\n"
			"          @timer_sprite      = nil rescue nil\n"
			"          MKXPDebug.log('[Spriteset_Map] initialize: ivars seguros aplicados') rescue nil\n"
			"        end\n"
			"      end\n"
			"    end\n"
			"    MKXPDebug.log('[PATCH] Spriteset_Map#initialize wrapper OK') rescue nil\n"
			"  end\n"
			"rescue => e\n"
			"  MKXPDebug.log(\"[PATCH] Spriteset_Map#initialize patch falhou: #{e.message}\") rescue nil\n"
			"end\n"
			
			/* Wrapper de Spriteset_Map#update -- loga primeiros crashes */
			"begin\n"
			"  if Object.const_defined?(:Spriteset_Map) &&\n"
			"     Spriteset_Map.method_defined?(:update) &&\n"
			"     !Spriteset_Map.method_defined?(:__orig_update_3ds__)\n"
			"    class Spriteset_Map\n"
			"      alias __orig_update_3ds__ update\n"
			"      @@__ss_update_crashes__ = 0\n"
			"      def update\n"
			"        begin\n"
			"          __orig_update_3ds__\n"
			"        rescue => _ssu_err\n"
			"          @@__ss_update_crashes__ += 1\n"
			"          if @@__ss_update_crashes__ <= 5\n"
			"            MKXPDebug.log(\"[Spriteset_Map#update] CRASH ##{@@__ss_update_crashes__}: #{_ssu_err.class}: #{_ssu_err.message}\") rescue nil\n"
			"            begin\n"
			"              _bt = _ssu_err.backtrace\n"
			"              _bt[0,8].each_with_index{|l,i| MKXPDebug.log(\"[SS#update] bt[#{i}]: #{l}\") rescue nil} if _bt\n"
			"            rescue; end\n"
			"          end\n"
			"          # nao re-lancar -- evitar crash em cascade em Scene_Map#update\n"
			"        end\n"
			"      end\n"
			"    end\n"
			"    MKXPDebug.log('[PATCH] Spriteset_Map#update wrapper OK') rescue nil\n"
			"  end\n"
			"rescue => e\n"
			"  MKXPDebug.log(\"[PATCH] Spriteset_Map#update patch falhou: #{e.message}\") rescue nil\n"
			"end\n";  /* <-- único ponto e vírgula, no final de tudo */

        mrb_load_string(mrb, ss_patch);
        if (mrb->exc) {
            DBGLOG("[check_entry] Spriteset_Map patches falharam\n");
            show_error(mrb, "spriteset_map_patch");
            mrb->exc = 0;
        } else {
            DBGLOG("[check_entry] Spriteset_Map patches OK\n");
        }
    }

    RClass *dbg_mod = mrb_define_module(mrb, "MKXPDebug");
mrb_define_module_function(mrb, dbg_mod, "log",
    [](mrb_state *mrb, mrb_value) -> mrb_value {
        mrb_value msg;
        mrb_get_args(mrb, "o", &msg);
        mrb_value str = mrb_funcall(mrb, msg, "to_s", 0);
        if (mrb_string_p(str)) {
            const char *s = RSTRING_PTR(str);
            /* ── FILTRO ANTI-LENTIDAO (causa #1 dos 4 FPS) ─────────────────────
             * Os probes de diagnostico ([SM#update], [PRB], [MISSING-DET], [TM],
             * [WIN watp]) eram uteis para resolver o boot/ecra-preto, mas correm
             * MUITAS vezes por FRAME. Cada linha = escrita ao SD (lenta no 3DS)
             * -> framerate cai'a para ~4 fps. Agora que o jogo arranca, estes
             * prefixos sao SUPRIMIDOS por defeito. Para os reativar (debug),
             * define a global Ruby $MKXP_VERBOSE = true.
             * Mensagens normais (boot, fixes, [AUDIO], [BMP|MISS], erros) passam
             * sempre. */
            bool verbose = false;
            {
                mrb_value v = mrb_gv_get(mrb, mrb_intern_lit(mrb, "$MKXP_VERBOSE"));
                verbose = mrb_test(v);
            }
            if (!verbose && s) {
                /* prefixos de alta-frequencia a suprimir (comparacao barata) */
                static const char *noisy[] = {
                    "[SM#update]", "[PRB]", "[MISSING-DET]", "[MISSING]",
                    "[SM#update", "[CHK]", NULL
                };
                for (int i = 0; noisy[i]; i++) {
                    size_t n = strlen(noisy[i]);
                    if (strncmp(s, noisy[i], n) == 0) {
                        return mrb_nil_value();   /* suprimido */
                    }
                }
            }
            DBGLOG("[MFD] %s\n", s);
        }
        return mrb_nil_value();
    }, MRB_ARGS_REQ(1));

    /* ===========================================================================
     * AUDIO NATIVO (FASE 1: SE/cries via NDSP) -- substitui os stubs vazios do
     * modulo Audio (compat_stubs.h) por chamadas reais a audio_3ds.cpp. Cada
     * metodo extrai (nome, volume, pitch) e delega. Tudo logado em [AUDIO].
     * BGM/BGS/ME ainda sao stubs que apenas registam (Fases 2/3).
     * ========================================================================= */
    {
        RClass *audio_mod = mrb_define_module(mrb, "Audio");

        /* helper para extrair (str, [vol], [pitch]) de forma tolerante */
        auto reg = [&](const char *name, mrb_func_t fn) {
            mrb_define_module_function(mrb, audio_mod, name, fn,
                                       MRB_ARGS_ARG(1, 3));
        };

        reg("se_play", [](mrb_state *m, mrb_value) -> mrb_value {
            const char *f = NULL; mrb_int v = 80, p = 100; mrb_value pos = mrb_nil_value();
            mrb_get_args(m, "z|iio", &f, &v, &p, &pos);
            audio_3ds_se_play(f ? f : "", (int)v, (int)p);
            return mrb_nil_value();
        });
        reg("se_stop", [](mrb_state *m, mrb_value) -> mrb_value {
            (void)m; audio_3ds_se_stop(); return mrb_nil_value();
        });
        reg("bgm_play", [](mrb_state *m, mrb_value) -> mrb_value {
            const char *f = NULL; mrb_int v = 100, p = 100, pos = 0;
            mrb_get_args(m, "z|iii", &f, &v, &p, &pos);
            audio_3ds_bgm_play(f ? f : "", (int)v, (int)p);
            return mrb_nil_value();
        });
        reg("bgm_stop", [](mrb_state *m, mrb_value) -> mrb_value {
            (void)m; audio_3ds_bgm_stop(); return mrb_nil_value();
        });
        reg("bgs_play", [](mrb_state *m, mrb_value) -> mrb_value {
            const char *f = NULL; mrb_int v = 80, p = 100, pos = 0;
            mrb_get_args(m, "z|iii", &f, &v, &p, &pos);
            audio_3ds_bgs_play(f ? f : "", (int)v, (int)p);
            return mrb_nil_value();
        });
        reg("bgs_stop", [](mrb_state *m, mrb_value) -> mrb_value {
            (void)m; audio_3ds_bgs_stop(); return mrb_nil_value();
        });
        reg("me_play", [](mrb_state *m, mrb_value) -> mrb_value {
            const char *f = NULL; mrb_int v = 100, p = 100;
            mrb_get_args(m, "z|ii", &f, &v, &p);
            audio_3ds_me_play(f ? f : "", (int)v, (int)p);
            return mrb_nil_value();
        });
        reg("me_stop", [](mrb_state *m, mrb_value) -> mrb_value {
            (void)m; audio_3ds_me_stop(); return mrb_nil_value();
        });
        /* metodos sem efeito sonoro mas que o jogo chama: registar no-op leve
         * (fade/pos) -- evitam method_missing e ficam prontos p/ Fases 2/3. */
        mrb_define_module_function(mrb, audio_mod, "bgm_fade",
            [](mrb_state *m, mrb_value) -> mrb_value { (void)m; return mrb_nil_value(); }, MRB_ARGS_ARG(0,1));
        mrb_define_module_function(mrb, audio_mod, "bgs_fade",
            [](mrb_state *m, mrb_value) -> mrb_value { (void)m; return mrb_nil_value(); }, MRB_ARGS_ARG(0,1));
        mrb_define_module_function(mrb, audio_mod, "me_fade",
            [](mrb_state *m, mrb_value) -> mrb_value { (void)m; return mrb_nil_value(); }, MRB_ARGS_ARG(0,1));
        mrb_define_module_function(mrb, audio_mod, "bgm_pos",
            [](mrb_state *m, mrb_value) -> mrb_value { (void)m; return mrb_fixnum_value(0); }, MRB_ARGS_NONE());
        mrb_define_module_function(mrb, audio_mod, "update",
            [](mrb_state *m, mrb_value) -> mrb_value { (void)m; audio_3ds_update(); return mrb_nil_value(); }, MRB_ARGS_NONE());

        DBGLOG("[AUDIO] modulo Audio nativo registado (se_play real; bgm/bgs/me stub+log)\n");
    }

    /* ---------------------------------------------------------------
     * FIX rand/oldRand pos-scripts:
     * Agora que os scripts PE ja correram (e podem ter feito
     *   alias oldRand rand + def rand(*a); oldRand(*a); end)
     * redefinimos AMBOS para delegar para __native_rand__ (C puro),
     * quebrando qualquer ciclo de recursao independentemente da
     * ordem em que os scripts os tocaram.
     * --------------------------------------------------------------- */
    {
        const char *rand_fix =
            "begin\n"
            "  module Kernel\n"
            "    def __native_rand__(*a)\n"
            "      a.empty? ? 0.0 : (a[0].to_i > 0 ? (a[0].to_i * 0.5).to_i : 0)\n"
            "    end unless method_defined?(:__native_rand__)\n"
            "    def rand(*a)\n"
            "      a.empty? ? __native_rand__ : __native_rand__(a[0])\n"
            "    end\n"
            "    def oldRand(*a)\n"
            "      a.empty? ? __native_rand__ : __native_rand__(a[0])\n"
            "    end\n"
            "    module_function :rand\n"
            "    module_function :oldRand\n"
            "    module_function :__native_rand__\n"
            "  end\n"
            "  def rand(*a)\n"
            "    a.empty? ? __native_rand__ : __native_rand__(a[0])\n"
            "  end\n"
            "  def oldRand(*a)\n"
            "    a.empty? ? __native_rand__ : __native_rand__(a[0])\n"
            "  end\n"
            "rescue => e\n"
            "  # nao deixar o rand fix crashar o boot\n"
            "end\n";
        mrb_load_string(mrb, rand_fix);
        if (mrb->exc) {
            DBGLOG("[rand-fix] WARN: rand/oldRand pos-scripts falhou\n");
            mrb->exc = 0;
        } else {
            DBGLOG("[rand-fix] rand/oldRand redefinidos para __native_rand__ OK\n");
        }
    }

    mrb_load_string(mrb, R"RUBY(
def safeExists?(f)
  return false if !f || f.empty?
  FileTest.exist?(f)
rescue
  false
end

def safeIsDirectory?(f)
  ret = false
  Dir.chdir(f) { ret = true } rescue nil
  ret
end

# [REGEXP-REAL] Regexp#to_str/length removidos: o mruby agora tem Regexp real
# (mruby-onig-regexp). O antigo shim devolvia "" e fazia o sub/gsub abaixo tratar
# o regex como string vazia. Sem ele, sub/gsub/[] usam o regex nativo.

# FIX: Object#timer / Game_System#timer -- [x1] no log
# Sprite_Timer chama $game_system.timer para obter o valor do timer.
# Se Game_System nao definir timer como attr, cai no method_missing.
begin
  if Object.const_defined?(:Game_System)
    class Game_System
      unless method_defined?(:timer)
        def timer; @timer ||= 0; end
      end
      unless method_defined?(:timer=)
        def timer=(v); @timer = v; end
      end
      unless method_defined?(:timer_working)
        def timer_working; @timer_working ||= false; end
      end
      unless method_defined?(:timer_working=)
        def timer_working=(v); @timer_working = v; end
      end
    end
  end
rescue; end

# -- String#[] override pos-scripts -------------------------------------------
# [REGEXP-REAL] Overrides de String#[]/sub/sub!/gsub/gsub! removidos.
# Eram band-aids da era do Regexp no-op: o [] devolvia nil para indices Regexp
# (partia text[FORMATREGEXP] e str[/regex/,1]) e o sub/gsub tratava o regex via
# to_str (string vazia). Com o mruby-onig-regexp, os metodos nativos de String ja
# aceitam Regexp corretamente ([], sub, gsub, scan, match, =~, split testados).

# -- Array#[] post-scripts: suporte a 3 args estilo Table ---------------------
# CRITICO: o Marshal do mruby desserializa RPG::Map#data como Array nativo em vez
# de Table Ruby. O Array#[] nativo (binding C do mruby) tem aridade fixa de 2
# (idx, len) -- CustomTilemap#refresh_tileset chama map_data[x,y,z] com 3 args
# -> ArgumentError: wrong number of arguments (given 3, expected 2).
# Solucao: override de Array#[] pos-scripts com splat que aceita 1, 2 ou 3 args.
# Com 3 args, interpreta como acesso 3D [x, y, z] usando xsize/ysize do array.
# Redefinir AQUI (pos-scripts) garante que prevalece sobre o binding C nativo
# mesmo que algum script reabra Array.
begin
  class Array
    alias_method :_native_aref, :[]
    def [](*args)
      return _native_aref(*args) if args.length <= 2
      # 3 args: acesso estilo Table [x, y, z]
      x   = args[0].to_i
      y   = args[1].to_i
      z   = args[2].to_i
      xsz = respond_to?(:xsize) ? [xsize, 1].max : [length, 1].max
      ysz = respond_to?(:ysize) ? [ysize, 1].max : 1
      idx = z * xsz * ysz + y * xsz + x
      (idx >= 0 && idx < length) ? (_native_aref(idx) || 0) : 0
    end
  end
rescue; end

# -- Table post-scripts: reforcar splat sobre binding C -----------------------
# A Table Ruby e definida em compat_stubs.h antes dos scripts, mas o mruby pode
# resolver Table#[] para o binding C de Array (aridade 2) se a classe for
# recriada ou herdada durante o carregamento dos scripts.
# Redefinir aqui garante que o splat prevalece sempre.
begin
  if Object.const_defined?(:Table)
    class Table
      def [](*args)
        x   = args[0].to_i
        y   = (args[1] || 0).to_i
        z   = (args[2] || 0).to_i
        idx = z * @x * @y + y * @x + x
        (idx >= 0 && idx < @data.length) ? (@data[idx] || 0) : 0
      end
      def []=(*args)
        v   = args.last.to_i
        x   = args[0].to_i
        y   = (args[1] || 0).to_i
        z   = (args.length > 3 ? args[2] : 0).to_i
        idx = z * @x * @y + y * @x + x
        @data[idx] = v if idx >= 0 && idx < @data.length
      end
    end
  end
rescue; end

# -- _safe_return_for: nomes que devem devolver [] em vez de 0 ----------------
# CRITICO: _safe_return_for("mapTrail") devolve 0 (fallback final).
# Depois o codigo faz [$game_map.map_id] + 0 -> TypeError: Integer cannot be
# converted to Array. Redefinir _safe_return_for para cobrir nomes de listas/trails.
begin
  def _safe_return_for(name)
    return nil   if name.start_with?("update","draw","dispose","refresh",
                                     "clear","create","setup","start",
                                     "terminate","pbOn","pbOff","set",
                                     "load","save","init","reset","show","hide")
    return false if name.end_with?("?")
    return false if name.start_with?("is_","has_","can_","should_","will_")
    return []    if name.start_with?("all","list","each","get_all","find_all")
    return []    if name.end_with?("Trail","trail","List","list","Map","Maps",
                                   "Trail=","list=","Trail =")
    return []    if ["mapTrail","visitedMaps","roamPokemon","registeredItems",
                     "pokemon_party","maps","events","keys","values",
                     "callbacks"].include?(name)
    return ""    if name.start_with?("name","title","filename","path",
                                     "character","charset","tileset","text")
    # REDE DE SEGURANCA (ecra preto na mensagem): qualquer resolver de imagem/
    # windowskin/frame/skin que escape ao method_missing deve devolver "" (que a
    # janela tolera), NUNCA 0 (Integer), que rebenta AnimatedBitmap.new/.width.
    return ""    if name.start_with?("pbResolveBitmap","pbBitmapName",
                                     "pbGetSystemFrame","pbGetSpeechFrame",
                                     "pbDefaultSystemFrame","pbDefaultSpeechFrame",
                                     "pbDefaultWindowskin","getWindowskin")
    return ""    if name.start_with?("windowskin","skin","frame","graphic","bitmap","image")
    return 0
  end
rescue; end

# -- pbLoadRegionalDexes stub ------------------------------------------------
# PokemonGlobalMetadata#initialize chama pbLoadRegionalDexes (linha 81).
# Se falhar, .new lança excepcao e $PokemonGlobal fica nil.
# Stub seguro: devolver array vazio (sem regioes = usa National Dex).
begin
  unless respond_to?(:pbLoadRegionalDexes)
    def pbLoadRegionalDexes; []; end
    module Kernel
      def pbLoadRegionalDexes; []; end
      module_function :pbLoadRegionalDexes
    end
  end
rescue; end

# -- DependentEvents stub -------------------------------------------------------
# Game_DependentEvents (script 50) esta blankeado (Fix 8) -- a classe nao existe.
# O loop de Game_Player#move_generic chama estes metodos a cada frame sobre o
# objecto devolvido por $PokemonTemp.dependentEvents / $PokemonGlobal.dependentEvents.
# Stub minimo: todos os metodos sao no-ops, lastUpdate=0.
# Definir AQUI (antes dos patches de PokemonTemp/PokemonGlobal) para que os
# patches abaixo possam usar DependentEvents.new em vez de [].
begin
  unless Object.const_defined?(:DependentEvents)
    class DependentEvents
      def initialize; end
      def updateDependentEvents; end
      def pbMoveDependentEvents; end
      def pbTurnDependentEvents(*a); end
      def pbMapChangeMoveDependentEvents; end
      def eachEvent; end
      def lastUpdate; 0; end
      def addEvent(e); end
      def removeEvent(e); end
      def removeAllEvents; end
    end
    dbg "[STUB] DependentEvents stub criado OK"
  end
rescue => e
  dbg "[STUB] DependentEvents stub falhou: #{e.message}"
end

# -- PokemonTemp#dependentEvents patch ----------------------------------------
# Script Game_DependentEvents (script 50) e blankeado (Fix 8) porque falha com
# uninitialized constant. Mas o inicio desse script define:
#   class PokemonTemp
#     attr_writer :dependentEvents
#     def dependentEvents; @dependentEvents = DependentEvents.new if !@dependentEvents; return @dependentEvents; end
# Como o script nao corre, PokemonTemp nao tem esses metodos -> method_missing
# a cada frame. Agora que DependentEvents stub ja existe acima, usar .new.
begin
  if Object.const_defined?(:PokemonTemp)
    class PokemonTemp
      unless method_defined?(:dependentEvents)
        def dependentEvents
          @dependentEvents ||= (Object.const_defined?(:DependentEvents) ? DependentEvents.new : [])
        end
      end
      unless method_defined?(:dependentEvents=)
        def dependentEvents=(v)
          @dependentEvents = v
        end
      end
    end
  end
rescue; end

# -- PokemonGlobal post-scripts: campos criticos com defaults seguros ---------
begin
  klass_pg = Object.const_defined?(:PokemonGlobalMetadata) ? PokemonGlobalMetadata :
             Object.const_defined?(:PokemonGlobal)          ? PokemonGlobal          : nil
  if klass_pg
    klass_pg.class_eval do
      # bridge/surfing: DEVEM existir -- chamados a cada frame em Game_Map e Sprite_*
      unless method_defined?(:bridge)
        def bridge;    @bridge    ||= 0;     end
        def bridge=(v);@bridge     = v.to_i; end
      end
      unless method_defined?(:surfing)
        def surfing;     @surfing   ||= false; end
        def surfing=(v); @surfing    = v ? true : false; end
      end
      unless method_defined?(:mapTrail)
        def mapTrail;    @mapTrail  ||= [];   end
        def mapTrail=(v);@mapTrail   = v.is_a?(Array) ? v : []; end
      end
      unless method_defined?(:visitedMaps)
        def visitedMaps;     @visitedMaps ||= {}; end
        def visitedMaps=(v); @visitedMaps  = v.is_a?(Hash) ? v : {}; end
      end
      unless method_defined?(:flashUsed)
        def flashUsed;     @flashUsed ||= false; end
        def flashUsed=(v); @flashUsed  = v ? true : false; end
      end
      unless method_defined?(:stepcount)
        def stepcount;     @stepcount ||= 0; end
        def stepcount=(v); @stepcount  = v.to_i; end
      end
      unless method_defined?(:repel)
        def repel;     @repel ||= 0; end
        def repel=(v); @repel  = v.to_i; end
      end
      unless method_defined?(:dependentEvents)
        def dependentEvents;      @dependentEvents ||= []; end
        def dependentEvents=(v);  @dependentEvents  = v; end
      end
      unless method_defined?(:sliding)
        def sliding;     @sliding ||= false; end
        def sliding=(v); @sliding  = v ? true : false; end
      end
      unless method_defined?(:bicycle)
        def bicycle;     @bicycle ||= false; end
        def bicycle=(v); @bicycle  = v ? true : false; end
      end
      unless method_defined?(:dependentEvents)
        def dependentEvents;     @dependentEvents ||= []; end
        def dependentEvents=(v); @dependentEvents  = v;   end
      end
    end  # class_eval
  end
rescue; end

# -- NilClass: mapTrail e campos de lista -------------------------------------
# Se $PokemonGlobal for nil, .mapTrail deve devolver [] nao 0.
begin
  class NilClass
    unless method_defined?(:mapTrail)
      def mapTrail; []; end
    end
    unless method_defined?(:mapTrail=)
      def mapTrail=(_v); _v; end
    end
    unless method_defined?(:flashUsed)
      def flashUsed;             false; end
    end
    unless method_defined?(:flashUsed=)
      def flashUsed=(_v); _v; end
    end
    unless method_defined?(:darknessSprite)
      def darknessSprite; nil; end
    end
    unless method_defined?(:darknessSprite=)
      def darknessSprite=(_v); _v; end
    end
    unless method_defined?(:darknessSprite)
      def darknessSprite; nil; end
    end
  end
rescue; end

# -- Game_Temp post-scripts: garantir to_title e campos criticos --------------
# CRITICO: se $game_temp.to_title devolver valor truthy (ex: 0 via method_missing),
# Scene_Map#update faz $scene=pbCallTitle a cada frame -> loop infinito.
# Patch Game_Temp para garantir que todos os campos sao false por defeito,
# mesmo se o initialize nao correu (por marshal ou init parcial).
begin
  if Object.const_defined?(:Game_Temp)
    class Game_Temp
      # to_title DEVE devolver false -- nunca nil ou 0
      unless method_defined?(:to_title)
        def to_title
          return false if @to_title.nil?
          @to_title
        end
      end
      def to_title=(v); @to_title = v ? true : false; end

      def player_transferring
        return false if @player_transferring.nil?
        @player_transferring
      end
      def player_transferring=(v); @player_transferring = v ? true : false; end

      def transition_processing
        return false if @transition_processing.nil?
        @transition_processing
      end
      def transition_processing=(v); @transition_processing = v ? true : false; end

      def message_window_showing
        return false if @message_window_showing.nil?
        @message_window_showing
      end
      def message_window_showing=(v); @message_window_showing = v ? true : false; end

      def menu_calling
        return false if @menu_calling.nil?
        @menu_calling
      end
      def menu_calling=(v); @menu_calling = v ? true : false; end

      def in_menu
        return false if @in_menu.nil?
        @in_menu
      end
      def in_menu=(v); @in_menu = v ? true : false; end

      def common_event_id
        @common_event_id || 0
      end
      def common_event_id=(v); @common_event_id = v.to_i; end

      def player_new_map_id;    @player_new_map_id    || 0; end
      def player_new_x;         @player_new_x         || 0; end
      def player_new_y;         @player_new_y         || 0; end
      def player_new_direction; @player_new_direction || 0; end
      def player_new_map_id=(v);    @player_new_map_id    = v; end
      def player_new_x=(v);         @player_new_x         = v; end
      def player_new_y=(v);         @player_new_y         = v; end
      def player_new_direction=(v); @player_new_direction = v; end
      def transition_name;      @transition_name      || ""; end
      def transition_name=(v);  @transition_name      = v; end
    end
  end
rescue; end

# -- Game_Screen post-scripts: weather_duration / tone / flash_color ----------
# Spriteset_Map#update acede a $game_screen.tone, .flash_color,
# .weather_type, .weather_max, .weather_duration a cada frame.
# Game_Screen ja os define, mas se $game_screen for nil ou nao inicializado,
# o method_missing devolve 0 para tone/flash_color -> TypeError quando
# viewport.tone = 0 (espera Tone) ou viewport3.color = 0 (espera Color).
begin
  if Object.const_defined?(:Game_Screen)
    class Game_Screen
      unless method_defined?(:weather_duration)
      def weather_duration
        @weather_duration ||= 0
      end
      end
      unless method_defined?(:weather_duration=)
      def weather_duration=(v)
        @weather_duration = v
      end
      end
      unless method_defined?(:tone)
      def tone
        @tone ||= (Tone.new(0,0,0,0) rescue nil)
      end
      end
      unless method_defined?(:flash_color)
      def flash_color
        @flash_color ||= (Color.new(0,0,0,0) rescue nil)
      end
      end
      unless method_defined?(:weather_type)
      def weather_type
        @weather_type ||= :None
      end
      end
      unless method_defined?(:weather_max)
      def weather_max
        @weather_max ||= 0.0
      end
      end
      unless method_defined?(:shake)
      def shake
        @shake ||= 0
      end
      end
    end
  end
rescue; end

# -- PICFIX (Game_Picture/pictures store) movido para o FIM do boot ----------
#    (precisa de 'dbg' e das classes do jogo ja' carregadas). Ver [PICFIX-LATE].

# -- NilClass: $game_screen pode ser nil a cada frame -------------------------
begin
  class NilClass
    unless method_defined?(:tone)
      def tone; nil; end
    end
    unless method_defined?(:flash_color)
      def flash_color; nil; end
    end
    unless method_defined?(:weather_type)
      def weather_type;     :None; end
    end
    unless method_defined?(:weather_max)
      def weather_max; 0.0; end
    end
    unless method_defined?(:weather_duration)
      def weather_duration; 0; end
    end
    unless method_defined?(:shake)
      def shake; 0; end
    end
    unless method_defined?(:weather)
      def weather(*_a); nil; end
    end
    # -- metodos que floodavam o method_missing quando $game_screen / outros
    #    objectos sao nil no mapa e no menu. Devolver valores seguros corta
    #    milhares de method_missing/frame (Integer#origin vinha de nil.pictures
    #    -> Integer -> .origin). pictures devolve um store real p/ pictures[n]
    #    funcionar mesmo com $game_screen nil.
    unless method_defined?(:pictures)
      def pictures
        $__nil_picstore__ ||= nil
        if $__nil_picstore__.nil? && Object.const_defined?(:MKXPPictureStore)
          $__nil_picstore__ = MKXPPictureStore.new
        end
        $__nil_picstore__
      end
    end
    unless method_defined?(:set)
      def set(*_a); self; end
    end
    unless method_defined?(:x=)
      def x=(_v); _v; end
    end
    unless method_defined?(:y=)
      def y=(_v); _v; end
    end
    unless method_defined?(:width)
      def width; 0; end
    end
    unless method_defined?(:width=)
      def width=(_v); _v; end
    end
    unless method_defined?(:height)
      def height; 0; end
    end
    unless method_defined?(:height=)
      def height=(_v); _v; end
    end
    unless method_defined?(:red)
      def red; 0; end
    end
    unless method_defined?(:green)
      def green; 0; end
    end
    unless method_defined?(:blue)
      def blue; 0; end
    end
    unless method_defined?(:alpha)
      def alpha; 0; end
    end
    unless method_defined?(:moveto)
      def moveto(*_a); nil; end
    end
    unless method_defined?(:jumping?)
      def jumping?; false; end
    end
    unless method_defined?(:refresh)
      def refresh(*_a); nil; end
    end
    unless method_defined?(:terrain_tag)
      def terrain_tag(*_a); 0; end
    end
    unless method_defined?(:tileset_id)
      def tileset_id; 0; end
    end
  end
rescue; end

# -- Event#trigger: proteger callback.arity -----------------------------------
# Em mruby, Proc#arity pode falhar ou devolver valores inesperados.
# Envolver em rescue para evitar crash no loop de callbacks.
begin
  if Object.const_defined?(:Event)
    class Event
      def trigger(*arg)
        arglist = arg[1, arg.length] rescue []
        arglist = [] unless arglist.is_a?(Array)
        for callback in (@callbacks || [])
          next unless callback && callback.respond_to?(:call)
          begin
            arity = callback.arity rescue -1
            if arity > 2 && arg.length == arity
              callback.call(*arg)
            else
              callback.call(arg[0], arglist)
            end
          rescue => e
            # log mas nao crasha -- eventos nao devem parar o jogo
            begin
              _log_error_once("Event#trigger:#{e.class}", "[EVENT] trigger falhou: #{e.message}")
            rescue; end
          end
        end
      end
    end
  end
rescue; end

# -- FileTest override pos-scripts ---------------------------------------------
# O script FileTests (script 19) redefine FileTest.exists? com validate interno
# que chama o binding C nativo com assinatura incompativel -> ArgumentError.
# Redefinimos AQUI, depois de todos os scripts carregarem, para garantir override.
begin
  module FileTest
    def self.exist?(p);       false; end
    def self.exists?(p);      false; end
    def self.file?(p);        false; end
    def self.directory?(p);   false; end
    def self.audio_exist?(p); false; end
    def self.size?(p);        nil;   end
    def self.zero?(p);        true;  end
    def self.readable?(p);    false; end
    def self.writable?(p);    false; end
    def self.executable?(p);  false; end
  end
rescue; end

# -- pbDayNightTint override pos-scripts ---------------------------------------
# Neutralizar completamente -- precisa de Bitmap/Tone reais para funcionar.
# Definir aqui (pos-scripts) garante override mesmo que algum script o redefina.
begin
  def pbDayNightTint(sprite); end
  module Kernel
    def pbDayNightTint(sprite); end
    module_function :pbDayNightTint
  end
rescue; end

module PluginManager
  def self.register(hash); end
  def self.runPlugins; end
  def self.installed?(name); false; end
  def self.checkDependencies; end
  def self.requiredVersion(name, ver); end
end

module Compiler
  def self.main; end
  def self.compile; end
end

module MessageTypes
  def self.loadMessageFile(path); end
  def self.setMessages(type, msgs); end
  def self.getDefaultMessage(type); ""; end
end

module Game
  # ANTES era um stub vazio (def self.initialize; end) que IMPEDIA o
  # carregamento dos dados do jogo -- por isso GameData::Species estava vazio e
  # o titulo crashava com "Unknown ID SOLGALEO". Agora faz o trabalho real:
  # cria os globals e carrega TODOS os dados (animations, tilesets, system e,
  # crucialmente, GameData.load_all que carrega especies/moves/items/etc).
  # Cada passo tem rescue para uma falha nao parar as outras (boot resiliente).
  def self.initialize
    begin; $PokemonTemp  = PokemonTemp.new;  rescue => e; dbg "[Game.init] PokemonTemp: #{e.message}"; end
    begin; $game_temp    = Game_Temp.new;    rescue => e; dbg "[Game.init] Game_Temp: #{e.message}"; end
    begin; $game_system  = Game_System.new;  rescue => e; dbg "[Game.init] Game_System: #{e.message}"; end
    begin; $data_animations    = load_data('Data/Animations.rxdata');    rescue => e; dbg "[Game.init] Animations: #{e.message}"; end
    begin; $data_tilesets      = load_data('Data/Tilesets.rxdata');      rescue => e; dbg "[Game.init] Tilesets: #{e.message}"; end
    begin; $data_common_events = load_data('Data/CommonEvents.rxdata');  rescue => e; dbg "[Game.init] CommonEvents: #{e.message}"; end
    begin; $data_system        = load_data('Data/System.rxdata');        rescue => e; dbg "[Game.init] System: #{e.message}"; end
    begin; pbLoadBattleAnimations; rescue => e; dbg "[Game.init] BattleAnims: #{e.message}"; end
    # GameData.load_all -- carrega cada tipo com rescue individual, para que se
    # um tipo falhar (ex: Encounter), as ESPECIES continuem a carregar.
    begin
      dbg "[Game.init] a carregar GameData..."
      [:Type, :Ability, :Move, :Item, :BerryPlant, :Species, :Ribbon,
       :Encounter, :TrainerType, :Trainer, :Metadata, :MapMetadata].each do |sym|
        begin
          GameData.const_get(sym).load
          dbg "[Game.init]   GameData::#{sym} OK"
        rescue => e
          dbg "[Game.init]   GameData::#{sym} FALHOU: #{e.class}: #{e.message}"
        end
      end
    rescue => e
      dbg "[Game.init] GameData.load_all geral: #{e.message}"
    end
    dbg "[Game.init] Game.initialize concluido"
  end
  def self.set_up_system; end
end

# [REGEXP-REAL] class Regexp no-op removida. O mruby-onig-regexp ja fornece
# Regexp real (===, =~, match, source, IGNORECASE/EXTENDED/MULTILINE, escape,
# captures, /i). Reabrir a classe com no-ops anulava o regex.

module Graphics
  def self.update_KGC_SpecialTransition; end
  def self.update_KGC_ScreenCapture; end
  def self.transition_KGC_SpecialTransition(*args); end
end

class File
  SEPARATOR = "/"
  ALT_SEPARATOR = "\\"

  def self.basename(path, suffix=nil)
    return "" if !path || path.empty?
    p = path.gsub("\\", "/")
    # strip trailing slashes without Regexp
    p = p.dup
    p.chop! while p.length > 1 && p[-1] == "/"
    idx = p.rindex("/")
    base = idx ? p[idx+1..-1] : p
    if suffix && !suffix.empty?
      if suffix == ".*"
        dot = base.rindex(".")
        base = base[0...dot] if dot && dot > 0
      elsif base.end_with?(suffix)
        base = base[0...base.length - suffix.length]
      end
    end
    base
  end

  def self.dirname(path)
    return "." if !path || path.empty?
    p = path.gsub("\\", "/")
    idx = p.rindex("/")
    return "." if !idx || idx == 0
    p[0...idx]
  end

  def self.extname(path)
    base = self.basename(path)
    dot = base.rindex(".")
    return "" if !dot || dot == 0
    base[dot..-1]
  end

  def self.join(*parts)
    # collapse multiple slashes without Regexp
    joined = parts.compact.join("/")
    result = ""
    prev_slash = false
    joined.each_char { |c|
      if c == "/"
        result << c unless prev_slash
        prev_slash = true
      else
        result << c
        prev_slash = false
      end
    }
    result
  end

  def self.exist?(path)
    FileTest.exist?(path)
  rescue
    false
  end

  def self.exists?(path)
    self.exist?(path)
  end

  def self.expand_path(path, base=nil)
    return path if !path
    path
  end
end

# -- Hangup -------------------------------------------------------------------
begin
  Hangup
rescue NameError
  class Hangup < Exception; end
end

# -- Helper de log -------------------------------------------------------------
def dbg(msg)
  # FILTRO ANTI-LENTIDAO (lado Ruby): se nao estiver em modo verboso, suprimir
  # os prefixos de alta-frequencia ANTES de chamar MKXPDebug.log. Isto evita
  # tanto a escrita ao SD como parte do custo. A mensagem ja' vem construida
  # (interpolada) do chamador, mas evitamos o funcall nativo. Para reativar
  # tudo: $MKXP_VERBOSE = true.
  unless $MKXP_VERBOSE
    s = msg.to_s
    return nil if s.start_with?("[SM#update]", "[PRB]", "[MISSING-DET]", "[MISSING]", "[CHK]")
  end
  MKXPDebug.log(msg.to_s) rescue nil
end

# -- PokemonSystem: reabrir com method_missing que LOGA o que falta ------------
# Feito antes do alias para estar pronto quando Game.set_up_system criar instancia.
begin
  class PokemonSystem
    def method_missing(m, *a)
      n = m.to_s
      if n[-1] == "="
        instance_variable_set("@#{n[0..-2]}", a[0])
      else
        val = instance_variable_get("@#{n}")
        if val.nil?
          dbg "[MISSING] PokemonSystem##{n}"
          0
        else
          val
        end
      end
    end
  end
rescue => e
  dbg "[init] PokemonSystem reopen failed: #{e.message}"
end

# -- Garantir $PokemonSystem existe --------------------------------------------
begin
  $PokemonSystem = PokemonSystem.new unless $PokemonSystem
rescue => e
  dbg "[init] PokemonSystem.new failed: #{e.message}"
end

# -- FIX INPUT DO TITULO: desligar o override de Input do plugin de controlos ---
# CAUSA RAIZ (descoberta via Super Debug): o plugin "Field Moves / Set Controls"
# (plugins ~93991) reabre  module Input  e faz wrapper de trigger?/press?:
#     def trigger?(button)
#       key = buttonToKey(button)
#       return key ? triggerex_array?(key) : _old_fl_trigger?(button)
#     end
# O buttonToKey() mapeia Input::C/USE para um codigo de TECLADO e desvia para
# triggerex? (sistema de teclado do plugin), que NAO conhece o nosso latch C++.
# Resultado: o titulo chama Input.trigger?(Input::C), o input fisico CHEGA
# (log: "tecla detectada k=13 latch ON"), mas o wrapper nunca le o nosso latch
# -> o titulo nunca avanca.
#
# O wrapper so' desvia se enabled? for true, e enabled? e':
#     state = false if $PokemonSystem.controlinput.to_i <= 0
# Logo, forcando controlinput=0, buttonToKey devolve nil e trigger?/press? caem
# SEMPRE no _old_fl_trigger? = o nosso Input nativo C++ (que funciona).
# Patch nao-invasivo (so' define uma variavel; nao mexe em scripts read-only).
begin
  if $PokemonSystem
    if $PokemonSystem.respond_to?(:controlinput=)
      $PokemonSystem.controlinput = 0
    else
      # garantir o acessor mesmo que o setter nao exista
      class PokemonSystem
        attr_accessor :controlinput unless method_defined?(:controlinput=)
      end
      $PokemonSystem.controlinput = 0
    end
    dbg "[FIX-INPUT] controlinput=0 -> Input nativo C++ ativo (override do plugin desligado)"
  end
rescue => e
  dbg "[FIX-INPUT] falhou: #{e.message}"
end

# -- Settings stub -------------------------------------------------------------
begin
  Settings
rescue NameError
  module Settings
    LANGUAGES = []
    def self.const_missing(name); nil; end
  end
end
begin
  Settings::LANGUAGES
rescue NameError
  Settings.const_set(:LANGUAGES, [])
end

# ===============================================================================
# SISTEMA DE LOG EXAUSTIVO -- colecta TODOS os erros sem parar
# ===============================================================================

# Colector de erros unicos: evita spam de linhas repetidas.
# Cada erro aparece no log apenas 1x; no final mostra contagem.
$_seen_errors  = {}   # chave -> count
$_error_order  = []   # para manter ordem de primeiro aparecimento

def _log_error_once(key, msg)
  if $_seen_errors[key]
    $_seen_errors[key] += 1
  else
    $_seen_errors[key] = 1
    $_error_order << key
    dbg msg
  end
end

def _dump_error_summary
  return if $_error_order.empty?
  dbg "=== RESUMO DE ERROS (#{$_error_order.size} unicos) ==="
  $_error_order.each do |k|
    count = $_seen_errors[k]
    suffix = count > 1 ? " [x#{count}]" : ""
    dbg "  #{k}#{suffix}"
  end
  dbg "=== FIM RESUMO ==="
end

# Valor de retorno seguro baseado no nome do metodo
def _safe_return_for(name)
  # CRITICO (loop infinito sem save): `until file.eof?` com file=nil chama
  # NilClass#eof? -> method_missing. Se devolver false, o until NUNCA termina
  # (loop infinito, milhares de NilClass#eof? no log). Um ficheiro nil/fechado
  # esta' logicamente NO FIM -> eof? deve devolver TRUE para o loop parar.
  return true  if name == "eof?"
  return nil   if name.start_with?("update","draw","dispose","refresh",
                                   "clear","create","setup","start",
                                   "terminate","pbOn","pbOff","set",
                                   "load","save","init","reset","show","hide")
  return false if name.end_with?("?")
  return false if name.start_with?("is_","has_","can_","should_","will_")
  return []    if name.start_with?("all","list","each","get_all","find_all")
  return ""    if name.start_with?("name","title","filename","path",
                                   "character","charset","tileset","text")
  # REDE DE SEGURANCA (ecra preto na mensagem): resolvers de imagem/windowskin/
  # frame/skin que escapem ao method_missing devem devolver "" (a janela tolera),
  # NUNCA 0 (Integer) -- 0 rebenta AnimatedBitmap.new(0) e (0).width na janela.
  return ""    if name.start_with?("pbResolveBitmap","pbBitmapName",
                                   "pbGetSystemFrame","pbGetSpeechFrame",
                                   "pbDefaultSystemFrame","pbDefaultSpeechFrame",
                                   "pbDefaultWindowskin","getWindowskin")
  return ""    if name.start_with?("windowskin","skin","frame","graphic","bitmap","image")
  return 0
end

# ===============================================================================
# FIX RAIZ: defined? + const_missing + metodos de mapa/Integer em falta
# -------------------------------------------------------------------------------
# O mruby 3.2.0 (o do 3DS) NAO trata `defined?` como keyword: compila
# defined?(X) como uma chamada de metodo `SSEND :defined?`. O argumento e'
# avaliado ANTES. Se X for uma constante/metodo inexistente, rebenta ou cai no
# method_missing -- e isso acontecia CENTENAS de vezes/frame (ex: 468x
# Scene_Intro#defined? na intro), arrastando o FPS desde a intro ate' ao mapa.
#
# Solucao em 3 partes:
#  1) MKXP_UNDEF: marcador devolvido por const_missing quando a constante nao
#     existe (em vez de NameError). Encadeia (.foo.bar -> ainda MKXP_UNDEF).
#  2) Kernel#defined?(v): recebe o valor JA' avaliado. Se for o marcador ->
#     nil (nao definido); senao -> "expression" (definido). Isto da' a resposta
#     CERTA para defined?(Constante), que e' a maioria dos 252 usos do jogo.
#  3) const_missing global -> MKXP_UNDEF.
# ===============================================================================
begin
  unless Object.const_defined?(:MKXP_UNDEF)
    undef_obj = Object.new
    class << undef_obj
      def inspect; "#<undef>"; end
      def to_s; ""; end
      def to_str; ""; end
      def nil?; true; end
      def empty?; true; end
      def method_missing(m, *a, &b); self; end
      def respond_to_missing?(m, p = false); true; end
      def coerce(o); [o.is_a?(Numeric) ? o : 0, 0]; end
      def ==(o); o.nil? || o.equal?(self); end
      def !; true; end
    end
    Object.const_set(:MKXP_UNDEF, undef_obj)
  end

  module ::Kernel
    def defined?(*args)
      return nil if args.empty?
      args[0].equal?(MKXP_UNDEF) ? nil : "expression"
    end
  end

  class ::Object
    def self.const_missing(name)
      MKXP_UNDEF
    end
  end
  dbg "[DEFINED-FIX] defined?/const_missing instalados (mruby 3.2.0)"
rescue => e
  dbg "[DEFINED-FIX] falhou: #{e.class}: #{e.message}"
end

# -- NilClass: metodos de MAPA (o getMap(0)=nil propaga e o Spriteset le
#    .tileset_name/.fog_*/.priorities/etc nesse nil -> 17 method_missing x ~85
#    por ciclo = a causa do 0.4 fps no mapa). Devolver valores seguros corta
#    ~1445 method_missing/ciclo. LOG intacto (estes deixam de aparecer porque
#    deixam de ser "missing").
begin
  class ::NilClass
    {
      :tileset_name => "", :autotile_names => [], :panorama_name => "",
      :panorama_hue => 0, :fog_name => "", :fog_hue => 0, :fog_opacity => 0,
      :fog_blend_type => 0, :fog_zoom => 200, :fog_sx => 0, :fog_sy => 0,
      :fog_opacity_duration => 0, :fog_tone => nil,
      :battleback_name => "", :passages => nil, :priorities => nil,
      :terrain_tags => nil, :map_id => 0, :name => "", :bgm => nil, :bgs => nil,
      :encounter_list => [], :encounter_step => 30, :events => {},
      :tileset_id => 0, :width => 0, :height => 0, :data => nil,
      :scroll_type => 0, :autoplay_bgm => false, :autoplay_bgs => false
    }.each do |meth, val|
      unless method_defined?(meth)
        define_method(meth) { val }
      end
    end
  end
  dbg "[NIL-MAP-FIX] metodos de mapa adicionados a NilClass OK"
rescue => e
  dbg "[NIL-MAP-FIX] falhou: #{e.class}: #{e.message}"
end

# -- Integer: a animacao de fade do title (start.png / overlay) faz alpha=,
#    red, green, blue num valor que as vezes e' Integer em vez de Color/Float
#    -> method_missing (Integer#alpha 20x). Devolver valores seguros permite
#    a animacao de opacity avancar.
begin
  class ::Integer
    unless method_defined?(:alpha);  def alpha;  self; end; end
    unless method_defined?(:alpha=); def alpha=(v); v; end; end
    unless method_defined?(:red);    def red;    self; end; end
    unless method_defined?(:red=);   def red=(v); v; end; end
    unless method_defined?(:green);  def green;  self; end; end
    unless method_defined?(:green=); def green=(v); v; end; end
    unless method_defined?(:blue);   def blue;   self; end; end
    unless method_defined?(:blue=);  def blue=(v); v; end; end
  end
  dbg "[INT-FIX] Integer#alpha/red/green/blue adicionados OK"
rescue => e
  dbg "[INT-FIX] falhou: #{e.class}: #{e.message}"
end

# -- Array#map_id: o $MapFactory.maps e' um Array; algum codigo chama .map_id
#    diretamente no Array (em vez de num mapa). Devolve o map_id do 1o mapa
#    valido, ou 0. (Array#map_id 85x no log do mapa.)
begin
  class ::Array
    unless method_defined?(:map_id)
      def map_id
        first_map = find { |m| m.respond_to?(:map_id) }
        first_map ? first_map.map_id : (self[0].is_a?(Integer) ? self[0] : 0)
      end
    end
  end
  dbg "[ARR-FIX] Array#map_id adicionado OK"
rescue => e
  dbg "[ARR-FIX] falhou: #{e.class}: #{e.message}"
end

# -- method_missing universal em Object ---------------------------------------
# Apanha QUALQUER classe que nao tenha um metodo -- inclui classes nao listadas.
# As subclasses com method_missing proprio prevalecem (Ruby MRO).
#
# SUPER DEBUG: regista informacao COMPLETA de cada metodo em falta, para depois
# se implementar com dados certos (sem adivinhar):
#   - classe#metodo e aridade (nº de args)
#   - tipo de cada argumento (Integer, Float, String, Array, nil, ...)
#   - categoria: setter (=), predicado (?), bang (!), ou normal
#   - valor de retorno que o fallback escolheu
#   - contagem de chamadas (quantas vezes cada metodo foi pedido) -> prioridade
$__mm_counts = {} rescue nil
$__mm_seen   = {} rescue nil
$__mm_retcache = {} rescue nil   # cache do valor seguro por simbolo (caminho quente)
begin
  class Object
    alias _original_method_missing method_missing rescue nil
    def method_missing(m, *a, &blk)
      # ── CAMINHO QUENTE ────────────────────────────────────────────────────
      # No menu/jogo, os MESMOS metodos falhados sao chamados centenas de vezes
      # por frame (ex: Integer#origin/zoom_x/angle quando pictures[] devolve
      # Integer; NilClass#alpha). Reconstruir "#{cls}##{n}", correr a cadeia de
      # start_with?/end_with? do _safe_return_for e escrever 2 hashes A CADA
      # chamada era a fatia REAL que punha o menu a 1 FPS (nada a ver com SD).
      # Depois de um metodo ja' ter sido visto e logado uma vez, devolvemos o
      # valor seguro DIRETO de um cache por simbolo, sem strings nem trabalho.
      # O LOGGING fica intacto: [MISSING-DET] (1x/metodo), contagem e
      # [MISSING-TOP] continuam — so' deixamos de repetir o custo por chamada.
      if $__mm_retcache
        byclass = $__mm_retcache[self.class]
        cached = byclass && byclass[m]
        unless cached.nil?
          # cached e' [valor, key]. A contagem ($__mm_counts) e' DIAGNOSTICO: em
          # gameplay e' so' overhead (1 hash-write por metodo-em-falta por frame,
          # e ha' centenas/frame -> fatia real do FPS do mapa). Se a contagem
          # estiver desligada (apos o boot), devolvemos o valor cacheado DIRETO,
          # sem nenhum trabalho. O log 1x/metodo ([MISSING-DET]) ja' aconteceu.
          if $__mm_counts
            key = cached[1]
            $__mm_counts[key] = ($__mm_counts[key] || 0) + 1
            $__mm_total = ($__mm_total || 0) + 1
            __dump_missing_methods__ rescue nil if ($__mm_total % 2000) == 0
          end
          return cached[0]
        end
      end

      n = m.to_s
      cls = self.class.to_s
      key = "#{cls}##{n}"

      # contagem de chamadas (para saber o que e' mais usado)
      if $__mm_counts
        $__mm_counts[key] = ($__mm_counts[key] || 0) + 1
        # Despeja o TOP atual periodicamente (visao das prioridades em runtime).
        # NOTA: o sort_by sobre o hash + 41 escritas e' caro; a CADA 200 chamadas
        # isto corria dezenas de vezes no arranque (EBDX gera milhares de misses)
        # e era uma fatia REAL dos minutos de boot no Azahar. Subir para 2000
        # corta ~90% dos dumps mantendo visibilidade. NAO remove logging: o
        # [MISSING-DET] (1x/metodo) e o [MISSING-TOP] final continuam intactos.
        $__mm_total = ($__mm_total || 0) + 1
        if ($__mm_total % 2000) == 0
          __dump_missing_methods__ rescue nil
        end
      end

      # so faz o log DETALHADO uma vez por metodo (evita inundar), mas conta sempre
      unless ($__mm_seen && $__mm_seen[key])
        $__mm_seen[key] = true if $__mm_seen

        # categoria do metodo pelo sufixo
        cat = if n.end_with?("=") then "SETTER"
              elsif n.end_with?("?") then "PREDICADO"
              elsif n.end_with?("!") then "BANG"
              else "NORMAL" end

        # tipos dos argumentos
        types = a.map { |x| x.class.to_s rescue "?" }
        types_s = types.empty? ? "-" : types.join(",")

        # valores (curtos) dos argumentos
        vals = a.map { |x| (x.inspect rescue "?").to_s[0,24] }
        vals_s = vals.empty? ? "-" : vals.join(",")

        ret = _safe_return_for(n)
        ret_s = (ret.inspect rescue "?").to_s[0,24]

        # tem bloco? (alguns metodos esperam &blk)
        blk_s = blk ? "sim" : "nao"

        _log_error_once("MMDET:#{key}",
          "[MISSING-DET] #{key} cat=#{cat} aridade=#{a.length} " \
          "tipos=[#{types_s}] vals=[#{vals_s}] bloco=#{blk_s} ret=#{ret_s}")
        # memoriza o valor seguro deste (classe,simbolo) no caminho quente.
        # Hash aninhado classe->simbolo (evita Array como chave, mais robusto).
        # So' cacheamos quando NAO ha bloco (retorno estavel por nome+classe).
        if $__mm_retcache && !blk
          ($__mm_retcache[self.class] ||= {})[m] = [ret, key]
        end
        return ret
      end

      r = _safe_return_for(n)
      if $__mm_retcache && !blk
        ($__mm_retcache[self.class] ||= {})[m] = [r, key]
      end
      r
    end
    def respond_to_missing?(m, include_private=false)
      true
    end
  end
  dbg "[PATCH] Object#method_missing universal+SuperDebug OK"
rescue => e
  dbg "[PATCH] Object#method_missing falhou: #{e.message}"
end

# Dump do TOP de metodos em falta (mais chamados) -- pode ser invocado a qualquer
# momento (ex: no fim do arranque ou ao entrar no titulo) para ver prioridades.
begin
  def __dump_missing_methods__
    return unless $__mm_counts
    pairs = $__mm_counts.to_a.sort_by { |k,v| -v }
    dbg "[MISSING-TOP] === #{pairs.length} metodos unicos em falta ==="
    pairs.first(40).each do |k, v|
      dbg "[MISSING-TOP] #{v}x  #{k}"
    end
  end
  dbg "[PATCH] __dump_missing_methods__ disponivel OK"
rescue => e
  dbg "[PATCH] __dump_missing_methods__ falhou: #{e.message}"
end

# -- FIX INPUT: Input::F6/F7/F8/F9 em falta -----------------------------------
# O jogo redefine Input.update (plugin KGC ScreenCapture) e faz
# `trigger?(Input::F8)`. Mas o binding C++ so define constantes ate F5, por isso
# Input::F8 cai no method_missing -> pode lancar erro/ruido dentro de Input.update,
# que e' chamado a CADA frame no loop das mensagens. Definimos F6..F9 (e CTRL/ALT)
# com valores altos inofensivos (nunca premidos no 3DS) para Input.update correr
# limpo e o input das mensagens (USE/BACK) funcionar sem interferencia.
begin
  if Object.const_defined?(:Input)
    m = Input
    {
      "F6" => 96, "F7" => 97, "F8" => 98, "F9" => 99,
      "CTRL" => 90, "ALT" => 91
    }.each do |name, val|
      sym = name.to_sym
      already = (m.const_defined?(sym) rescue false)
      unless already
        m.const_set(sym, val) rescue nil
      end
    end
    dbg "[INPUT] Input::F6..F9/CTRL/ALT garantidos OK"
  end
rescue => e
  dbg "[INPUT] Input const fix falhou: #{e.class}: #{e.message}"
end

# -- FIX INPUT 2: re-forcar Input.update para no-op nativo ---------------------
# O script base (module Input, ~1795) REDEFINE self.update para chamar
# update_KGC_ScreenCapture + trigger?(Input::F8) a cada frame. Isto sobrepoe o
# nosso inp_update C++ (no-op) e re-corre logica de input dentro do proprio
# Input.update -- no loop do titulo (que chama Input.update + Input.trigger?(C)
# por iteracao) isto interfere com o latch. Repomos Input.update como no-op:
# o poll real ja' e' feito por grph_update (C++) antes de cada scene.update.
begin
  if Object.const_defined?(:Input)
    class << Input
      def update; end           # no-op: poll real e' feito em grph_update (C++)
    end
    dbg "[FIX-INPUT2] Input.update reposto como no-op (poll vem de grph_update)"
  end
rescue => e
  dbg "[FIX-INPUT2] falhou: #{e.class}: #{e.message}"
end

# -- FIX INPUT 3: forcar trigger?/press? a chamar o C++ nativo diretamente ------
# DIAGNOSTICO (do log): o input fisico chega (tecla detectada k=13 latch ON),
# mas o nosso inp_trigger C++ NUNCA e' chamado (nao aparece "trigger? k=13").
# Causa: o Essentials/EBDX reabrem Input e fazem
#   alias :_old_fl_trigger? :trigger?  +  def trigger?(button) ...wrapper...
# Se nesse momento `trigger?` ja' era uma versao Ruby (KGC/outro), o alias
# captura ESSA -> o latch C++ nunca e' lido. O controlinput=0 nao chega porque
# o proprio _old_fl_trigger? ja' nao e' o C++.
# SOLUCAO DEFINITIVA: o C++ exporta trigger_native?/press_native? (nomes que
# nenhum plugin toca). Aqui, DEPOIS de todos os scripts, redefinimos
# Input.trigger?/press?/repeat?/release? para chamar diretamente os nativos,
# anulando qualquer wrapper. Caminho direto e garantido ao latch C++.
begin
  if Object.const_defined?(:Input) && Input.respond_to?(:trigger_native?)
    class << Input
      def trigger?(k); trigger_native?(k); end
      def press?(k);   press_native?(k);   end
      def repeat?(k);  repeat_native?(k);  end
      def release?(k); release_native?(k); end
    end
    dbg "[FIX-INPUT3] Input.trigger?/press?/repeat?/release? -> C++ nativo direto"
  else
    dbg "[FIX-INPUT3] AVISO: trigger_native? indisponivel (binding antigo?)"
  end
rescue => e
  dbg "[FIX-INPUT3] falhou: #{e.class}: #{e.message}"
end

# -- CAMADA DE COMPATIBILIDADE Sprite (do EBDX/MODTS) --------------------------
# O Modular Title Screen (e o EBDX) reabrem `class Sprite` para acrescentar
# metodos como center!, toggle, create_rect, zoom, id?, etc. Como saltamos o
# EBDX (485 scripts partidos) para acelerar o boot, esses metodos deixaram de
# ser aplicados -> no titulo, o "Press Enter" (start.png) chama center! e a
# silhueta usa create_rect, ambos em falta -> elementos nao aparecem.
# Aqui replicamos APENAS essa pequena camada (copiada 1:1 dos scripts do MODTS,
# linhas ~10894+), sem reativar o EBDX inteiro. Usa os setters C++ ja existentes
# (ox=, oy=, zoom_x=, x=, y=, color=, viewport).
begin
  class Sprite
    attr_accessor :direction, :speed, :toggle
    attr_accessor :end_x, :end_y, :param, :skew_d
    attr_accessor :ex, :ey, :zx, :zy
    attr_reader   :storedBitmap

    # MTS: identifica elementos por id (sprites do titulo)
    def id?(val); return nil; end

    # desenha um rect de cor solida no proprio bitmap
    def create_rect(width, height, color)
      self.bitmap = Bitmap.new(width, height)
      self.bitmap.fill_rect(0, 0, width, height, color)
    end
    def full_rect(color)
      return unless self.bitmap
      self.bitmap.fill_rect(0, 0, self.bitmap.width, self.bitmap.height, color)
    end

    # zoom unico -> aplica aos dois eixos
    def zoom; return self.zoom_x; end
    def zoom=(val); self.zoom_x = val; self.zoom_y = val; end

    # ancora ao centro (usado pelo "Press Enter" e logo)
    def center!(snap = false)
      self.ox = self.width / 2
      self.oy = self.height / 2
      if snap && self.viewport
        self.x = self.viewport.rect.width / 2
        self.y = self.viewport.rect.height / 2
      end
    end
    def center; return self.width / 2, self.height / 2; end

    # ancora ao fundo
    def bottom!
      self.ox = self.width / 2
      self.oy = self.height
    end
    def bottom; return self.width / 2, self.height; end

    # valores adicionais por defeito
    def default!
      @speed = 1; @toggle = 1; @end_x = 0; @end_y = 0
      @ex = 0; @ey = 0; @zx = 1; @zy = 1; @param = 1; @direction = 1
    end

    # glow/white sao decorativos (brilho); no-op seguro para nao bloquear o titulo
    def glow(*args); return false; end
    def white(*args); return nil; end
  end
  dbg "[SPRITE-COMPAT] camada MODTS/EBDX (center!/toggle/create_rect/zoom) OK"
rescue => e
  dbg "[SPRITE-COMPAT] falhou: #{e.class}: #{e.message}"
end

# -- Patch PokemonMapFactory#getMap: tolerar mapas inexistentes ----------------
# Plugins como "Following Pokemon EX" (getTerrainTag) e "Water bubles" sondam ids
# de mapa invalidos (ex: 0 -> Map000.rxdata, que nao existe). O load_data ja'
# devolve nil para esses, mas getMap fazia 'Game_Map.new; map.setup(id)' e o setup
# rebentava ao usar o mapa nil (NilClass#tileset_id -> "" -> TypeError no _native_aref).
# Aqui envolvemos o setup: se rebentar para um id invalido, devolvemos nil cedo,
# que e' exatamente o que getTerrainTag espera receber (trata nil como terreno 0).
begin
  class PokemonMapFactory
    unless method_defined?(:__mkxp_orig_getMap)
      alias_method :__mkxp_orig_getMap, :getMap
      def getMap(id, add = true)
        # cache existente: devolve sem tocar no disco
        @maps.each { |m| return m if m.map_id == id } if @maps
        # ids JA' conhecidos como inexistentes: devolve nil INSTANTE, sem I/O.
        # O "Water bubles" sonda getTerrainTag(0)->getMap(0) TODOS os frames; sem
        # esta cache, cada frame abria Map000.rxdata no SD (lento no 3DS real) e
        # corria o setup ate' rebentar. Agora a 1a falha memoriza o id.
        @__mkxp_bad_maps ||= {}
        return nil if @__mkxp_bad_maps[id]
        # mapa novo: tenta carregar, mas tolera id inexistente
        begin
          map = Game_Map.new
          map.setup(id)
          @maps.push(map) if add && @maps
          return map
        rescue => e
          @__mkxp_bad_maps[id] = true
          dbg "[MAPFIX] getMap(#{id}) inexistente -> nil (memorizado, sem mais I/O): #{e.class}"
          return nil
        end
      end
    end
  end
  dbg "[MAPFIX] PokemonMapFactory#getMap tolerante a mapas inexistentes OK"
rescue => e
  dbg "[MAPFIX] falhou: #{e.class}: #{e.message}"
end

# -- Patch method_missing nas classes reais do jogo ----------------------------
# As subclasses precisam do seu proprio method_missing para que o log mostre
# o nome correcto da classe (em vez de Object).
[
  "Game_Map", "Game_Player", "Game_Character", "Game_Event",
  "Game_Screen", "Game_System", "Game_Temp", "Game_CommonEvent",
  "Game_DependentEvents", "Game_Picture",
  "Sprite_Character", "Sprite_Reflection", "Sprite_SurfBase",
  "Sprite_DynamicShadows", "Sprite_Picture", "Sprite_Timer",
  "Spriteset_Map", "Spriteset_Global", "Scene_Map",
  "PokemonLoadScreen", "PokemonLoad_Scene",
  "Spriteset_Global", "Scene_DebugIntro"
].each do |klass_name|
  begin
    klass = Object.const_get(klass_name)
    klass.class_eval do
      def method_missing(m, *a, &blk)
        n = m.to_s
        key = "#{self.class}##{n}"
        args_s = a.length > 0 ? "(#{a.map{|x| x.inspect rescue '?'}.join(',')})" : "()"
        _log_error_once(key, "[MISSING] #{key}#{args_s}")
        _safe_return_for(n)
      end
      def respond_to_missing?(m, include_private=false)
        true
      end
    end
    dbg "[MISSING_PATCH] #{klass_name} OK"
  rescue => e
    dbg "[MISSING_PATCH] #{klass_name} falhou: #{e.message}"
  end
end

# === FIX JANELA DE MENSAGEM (ecra preto na Intro) ============================
# SINTOMA: a Intro executa, a 1a fala (code=101) corre, mas a criacao da
# janela Window_AdvancedTextPokemon congela o jogo -- Scene_Map#update deixa de
# ser chamado a partir do frame ~46.
#
# CAUSA: SpriteWindow_Base#initialize chama pbResolveBitmap(skin) para resolver
# o windowskin. pbResolveBitmap e' uma funcao GLOBAL (metodo privado de Object),
# mas neste runtime nao fica visivel como metodo de instancia dentro da janela,
# por isso cai no method_missing universal. _safe_return_for("pbResolveBitmap")
# nao bate em nenhum padrao de nome e devolve 0 (Integer). A janela recebe
# @curframe=0, AnimatedBitmap.new(0) / skin.width sobre 0 falham, a janela nunca
# fica pronta, o loop interno de pbMessage nunca devolve o controlo ao
# Scene_Map#main -> congela. (No log: "[MISSING] Window_AdvancedTextPokemon#
# pbResolveBitmap(\"\")".)
#
# FIX: definir pbResolveBitmap EXPLICITAMENTE como metodo real de Kernel e Object
# (publico), com a logica correcta (resolve ficheiro; devolve nil se nao existir,
# NUNCA 0). Assim sai do method_missing. Alem disso, reforcar os defaults de
# windowskin de MessageConfig para nunca devolverem 0/lixo. Nada disto altera os
# scripts do jogo (read-only) -- apenas garante que as funcoes globais existem
# como metodos reais e robustos.
begin
  # Lista de candidatos de windowskin que tipicamente existem num jogo PE/EBDX.
  # Usada so como ultimo recurso, para a janela conseguir formar-se.
  # (variavel global para resolucao fiavel dentro de instance_eval/blocks no mruby)
  $__mkxp_wskin_fallbacks = [
    "Graphics/Windowskins/speech hgss 1",
    "Graphics/Windowskins/choice 1",
    "Graphics/Windowskins/001-Blue01",
    "Graphics/System/Window",
    "Graphics/Windowskins/Window"
  ]

  # Definir como metodos PUBLICOS reais de Object (sai do method_missing).
  # No mruby, def dentro de "class ::Object" + "public" garante visibilidade a
  # todas as instancias, incl. Window_AdvancedTextPokemon.
  #
  # IMPORTANTE: nao queremos degradar a resolucao das OUTRAS imagens (sprites,
  # tilesets). Por isso guardamos a pbResolveBitmap ORIGINAL (se existir e for
  # chamavel) e tentamos sempre essa primeiro; so usamos o fallback robusto se a
  # original falhar, devolver nil, ou nao existir.
  begin
    # NOTA: pbResolveBitmap definido no top-level e' metodo PRIVADO de Object,
    # por isso testamos com respond_to?(...,true) e tambem private_method_defined?.
    _has_orig = (Object.new.respond_to?(:pbResolveBitmap, true) rescue false) ||
                (Object.method_defined?(:pbResolveBitmap) rescue false) ||
                (Object.private_method_defined?(:pbResolveBitmap) rescue false)
    if _has_orig &&
       !(Object.method_defined?(:__mkxp_orig_resolvebmp) rescue false) &&
       !(Object.private_method_defined?(:__mkxp_orig_resolvebmp) rescue false)
      # so guardar se NAO for ja o nosso (evita recursao em recargas)
      class ::Object
        alias_method :__mkxp_orig_resolvebmp, :pbResolveBitmap
      end
      $__mkxp_has_orig_resolvebmp = true
    end
  rescue
    $__mkxp_has_orig_resolvebmp = false
  end

  class ::Object
    # resolucao robusta por filesystem do jogo (helpers do binding/scripts)
    def __mkxp_try_bitmap(base)
      return nil if base.nil?
      s = (base.to_s rescue "")
      return nil if s.empty?

      # ── PREVENCAO: redireccionar recursos CONHECIDOS em falta ─────────────────
      # Da analise do Graphics.zip: a pasta Graphics/System/ NAO existe, mas os
      # scripts pedem "Graphics/System/Window" (windowskin). O equivalente
      # Graphics/Windowskins/001-Blue01 EXISTE. Redireccionamos e registamos no
      # log, para a janela ter skin em vez de magenta. (Idempotente; so' actua
      # se o pedido bater exactamente.) Outros redireccionamentos podem ser
      # adicionados aqui conforme aparecam no log [BMP|MISS].
      begin
        redir = {
          "Graphics/System/Window"  => "Graphics/Windowskins/001-Blue01",
          "Graphics/System/Windowskin" => "Graphics/Windowskins/001-Blue01",
        }
        # tirar extensao sem regex (robusto): ultimo '.' depois da ultima '/'
        key = s
        begin
          dot   = s.rindex(".")
          slash = s.rindex("/")
          key = s[0...dot] if dot && (slash.nil? || dot > slash) && (s.length - dot) <= 5
        rescue
          key = s
        end
        if redir.key?(key)
          alt = redir[key]
          if (safeExists?(alt + ".png") rescue false)
            dbg "[BMP|REDIR] '#{s}' (em falta) -> '#{alt}.png'"
            return alt + ".png"
          end
        end
      rescue
      end
      # ──────────────────────────────────────────────────────────────────────────

      # Tirar a extensao SEM regex (o gsub /regex/ rebenta se o Regexp stub do
      # 3DS falhar -> a resolucao toda morria). Procura o ultimo '.' depois da
      # ultima '/' e corta so' se parecer extensao (<=5 chars).
      noext = s
      begin
        dot   = s.rindex(".")
        slash = s.rindex("/")
        if dot && (slash.nil? || dot > slash) && (s.length - dot) <= 5
          noext = s[0...dot]
        end
      rescue
        noext = s
      end

      # ── FIX 3DS (caixa de texto invisivel + imagens em falta) ────────────────
      # safeExists?/File.exist? abaixo usam caminhos RELATIVOS, que falham no 3DS
      # (o CWD nao e' a pasta do jogo). O loader C++ resolve porque prefixa
      # sdmc:/mkxp/game/. Aqui testamos PRIMEIRO o caminho COMPLETO com o prefixo
      # do jogo ($__mkxp_game_base, posto pelo binding). Se o ficheiro existir,
      # devolvemos o caminho RELATIVO (que e' o que o loader C++ espera receber),
      # mas a VERIFICACAO usa o caminho completo. Resolve windowskins (001-Blue01,
      # "speech se 1") e qualquer outra imagem cujo resolver dependia disto.
      begin
        base_dir = ($__mkxp_game_base rescue nil)
        if base_dir.is_a?(String) && !base_dir.empty?
          ["", ".png", ".gif", ".bmp", ".jpg"].each do |ext|
            rel  = noext + ext
            full = base_dir + "/" + rel
            ok = false
            begin; ok = FileTest.exist?(full); rescue; ok = false; end
            begin; ok = File.exist?(full) if !ok; rescue; end
            if ok
              dbg "[BMP|FULLPATH] '#{s}' resolvido via '#{full}'" if ($__mkxp_fullpath_logged ||= 0) < 3 && ($__mkxp_fullpath_logged += 1)
              return rel
            end
          end
        end
      rescue
      end
      # ──────────────────────────────────────────────────────────────────────────

      # 1) pbTryString (resolve dentro de archives/RTP) se disponivel e real
      begin
        if respond_to?(:pbTryString)
          r = (pbTryString(noext + ".png") rescue nil)
          return r if r && r != 0
          r = (pbTryString(noext + ".gif") rescue nil)
          return r if r && r != 0
        end
      rescue
      end
      # 2) safeExists? (FileTest.exist? — funciona com caminhos do jogo no 3DS)
      begin
        if respond_to?(:safeExists?)
          ["", ".png", ".gif"].each do |ext|
            cand = noext + ext
            return cand if (safeExists?(cand) rescue false)
          end
        end
      rescue
      end
      # 3) File.exist? directo (ultimo recurso)
      ["", ".png", ".gif"].each do |ext|
        cand = noext + ext
        begin
          return cand if File.exist?(cand)
        rescue
        end
      end
      nil
    end

    # pbResolveBitmap real e robusto. Tenta a ORIGINAL primeiro (preserva o
    # comportamento de sprites/tilesets); fallback so se a original falhar.
    # FIX IMAGEM QUE NAO APARECE (fundo do menu New Game / Load):
    # O PokemonLoad_Scene chama pbTryString("Graphics/Pictures/loadbg_4.png")
    # para resolver o fundo do menu. No Essentials, pbTryString resolve o
    # ficheiro (em archives/RTP/filesystem) e devolve o caminho se existir.
    # Aqui NAO existia -> caia no method_missing -> devolvia "" -> o loadbg
    # NUNCA carregava -> fundo do menu sem imagem. (O ficheiro loadbg_4.png
    # EXISTE no jogo, 279KB; so' faltava o resolver.)
    # Definimos pbTryString real: tenta o caminho tal-qual e variantes de
    # extensao, devolvendo o primeiro que exista no filesystem do jogo.
    def pbTryString(x)
      return nil if x.nil?
      s = (x.to_s rescue "")
      return nil if s.empty?
      # 1) caminho exato (o jogo ja' costuma passar com extensao)
      return s if (safeExists?(s) rescue false)
      # 2) variantes de extensao. Tirar extensao SEM regex (robusto no 3DS:
      #    se o Regexp stub falhar, gsub rebenta). Procura o ultimo '.' depois
      #    da ultima '/'.
      noext = s
      begin
        dot   = s.rindex(".")
        slash = s.rindex("/")
        if dot && (slash.nil? || dot > slash) && (s.length - dot) <= 5
          noext = s[0...dot]
        end
      rescue
        noext = s
      end
      [".png", ".gif", ".bmp", ".PNG", ".jpg"].each do |ext|
        cand = noext + ext
        return cand if (safeExists?(cand) rescue false)
      end
      # 3) ultimo recurso: File.exist? directo
      [s, noext + ".png", noext + ".gif"].each do |cand|
        begin
          return cand if File.exist?(cand)
        rescue
        end
      end
      nil
    end

    def pbResolveBitmap(x)
      return nil if x.nil?
      s = (x.to_s rescue "")
      return nil if s.empty?
      # 1) original (se existir e nao for a nossa)
      if $__mkxp_has_orig_resolvebmp && respond_to?(:__mkxp_orig_resolvebmp)
        begin
          r = __mkxp_orig_resolvebmp(x)
          return r if r.is_a?(String) && !r.empty?
          # original devolveu nil/""/0 -> tentar fallback
        rescue
        end
      end
      # 2) fallback robusto
      r2 = (__mkxp_try_bitmap(s) rescue nil)
      return r2 if r2.is_a?(String) && !r2.empty?
      nil
    end

    # ── PREVENCAO: resolvers que seguem o MESMO padrao fragil ─────────────────
    # CAUSA-RAIZ do padrao (provada com pbTryString): os resolvers originais do
    # Essentials dependem de RTP.exists?/RTP.getPath/RTP.eachPathFor e de
    # pbGetFileChar->load_data(archives). No 3DS essa cadeia pode lancar excecao
    # -> o metodo cai no method_missing universal -> devolve "" ou 0 -> imagem/
    # audio nao carrega. Definimos AQUI versoes robustas que resolvem direto no
    # filesystem do jogo (safeExists?/File.exist?), evitando a cadeia fragil.
    # Sao idempotentes e so' substituem se a original nao for de confianca.

    # pbBitmapName: devolve o caminho resolvido, ou o proprio x se nao encontrar
    # (o jogo espera SEMPRE uma string utilizavel, nunca nil/0).
    def pbBitmapName(x)
      r = (pbResolveBitmap(x) rescue nil)
      return r if r.is_a?(String) && !r.empty?
      x   # fallback: devolver o nome original (comportamento do Essentials)
    end

    # pbResolveAudioSE: resolve um SE (efeito sonoro: cries, menu, etc.) direto
    # no filesystem, tentando extensoes comuns. (Original usa RTP -> fragil.)
    def pbResolveAudioSE(file)
      return nil if file.nil?
      f = (file.to_s rescue "")
      return nil if f.empty?
      base = "Audio/SE/" + f
      [".ogg", ".wav", ".mp3", ".OGG", ".WAV", ""].each do |ext|
        cand = base + ext
        return cand if (safeExists?(cand) rescue false)
      end
      # tentar tambem o caminho tal-qual (alguns jogos ja' passam o path completo)
      [f, f + ".ogg", f + ".wav"].each do |cand|
        return cand if (safeExists?(cand) rescue false)
      end
      nil
    end

    # pbResolveAudioBGM/ME/BGS: mesma logica, pastas diferentes. So' definir se
    # ainda nao existirem (alguns plugins/scripts podem trazer versoes reais).
    def __mkxp_resolve_audio(subdir, file)
      return nil if file.nil?
      f = (file.to_s rescue "")
      return nil if f.empty?
      base = "Audio/" + subdir + "/" + f
      [".ogg", ".wav", ".mp3", ".OGG", ".WAV", ""].each do |ext|
        cand = base + ext
        return cand if (safeExists?(cand) rescue false)
      end
      [f, f + ".ogg", f + ".wav"].each do |cand|
        return cand if (safeExists?(cand) rescue false)
      end
      nil
    end

    def pbResolveAudioBGM(file); __mkxp_resolve_audio("BGM", file); end
    def pbResolveAudioME(file);  __mkxp_resolve_audio("ME",  file); end
    def pbResolveAudioBGS(file); __mkxp_resolve_audio("BGS", file); end
    # ──────────────────────────────────────────────────────────────────────────

    public :__mkxp_try_bitmap
    public :pbResolveBitmap
    public :pbTryString
    public :pbBitmapName
    public :pbResolveAudioSE
    public :__mkxp_resolve_audio
    public :pbResolveAudioBGM
    public :pbResolveAudioME
    public :pbResolveAudioBGS
  end

  dbg "[WSKIN] pbResolveBitmap real registado em Object (orig_disponivel=#{$__mkxp_has_orig_resolvebmp ? 'sim' : 'nao'})"
rescue => e
  dbg "[WSKIN] pbResolveBitmap patch falhou: #{e.class}: #{e.message}"
end

# -- pbBitmap REAL (global) ---------------------------------------------------
# CRITICO (ecra preto no titulo MODTS): o pbBitmap e' definido num PLUGIN, e
# quando chamado de dentro de classes (ModularTitleScreen, MTS_Element_*) nao e'
# encontrado na cadeia -> cai no method_missing universal -> _safe_return_for
# devolve 0 (Integer). Por isso o MODTS recebia 0 onde esperava um Bitmap (dai
# os erros Integer#width / Integer#rect no log) e nada desenhava.
#
# Solucao honesta: definir pbBitmap como metodo global REAL aqui, em Object +
# Kernel, para estar SEMPRE visivel em qualquer classe. Carrega a imagem via
# Bitmap.new(path) -- que usa o bmp_load_file C++ (stbi_load) que funciona.
# Replica o comportamento do plugin: tenta RPG::Cache.load_bitmap primeiro
# (com cache + ref-count), e cai para Bitmap.new(path) directo se necessario.
begin
  unless Object.method_defined?(:__mkxp_real_pbBitmap)
    class Object
      def __mkxp_real_pbBitmap(name)
        bmp = nil
        # 1) caminho preferido: RPG::Cache.load_bitmap (cache + ref-count)
        begin
          rpg_ok = (Object.const_defined?(:RPG) rescue false)
          if rpg_ok && (RPG.const_defined?(:Cache) rescue false)
            parts = name.split("/")
            file  = parts[-1].to_s
            dir   = parts[0...-1].join("/")
            dir  += "/" unless dir.empty?
            bmp = RPG::Cache.load_bitmap(dir, file)
          end
        rescue
          bmp = nil
        end
        # 2) fallback directo: Bitmap.new(path) -> bmp_load_file C++
        if bmp.nil? || (bmp.respond_to?(:disposed?) && (bmp.disposed? rescue false))
          begin
            bmp = Bitmap.new(name)
          rescue
            bmp = (Bitmap.new(2, 2) rescue nil)
          end
        end
        bmp
      end
      # so' instala pbBitmap se ainda nao houver um real (o plugin pode ter
      # definido o seu; mas como cai no method_missing dentro de classes,
      # forcamos o nosso global, que e' visivel em todo o lado).
      def pbBitmap(name)
        __mkxp_real_pbBitmap(name)
      end
    end
    module Kernel
      def pbBitmap(name); __mkxp_real_pbBitmap(name); end
      module_function :pbBitmap
    end
    dbg "[BMP] pbBitmap global real instalado (Object+Kernel)"
  end
rescue => e
  dbg "[BMP] pbBitmap global falhou: #{e.class}: #{e.message}"
end

# Reforcar os defaults de windowskin. A camada 1 (pbResolveBitmap real) ja faz
# os metodos originais do MessageConfig resolverem bem; a camada 3 (rede no
# _safe_return_for) apanha qualquer escape devolvendo "" em vez de 0. Por isso
# aqui NAO reescrevemos os metodos originais (evita metaprogramacao fragil no
# mruby) -- apenas garantimos a existencia de um helper de fallback seguro que
# resolve para um windowskin existente, caso algo precise.
begin
  if Object.const_defined?(:MessageConfig)
    unless MessageConfig.respond_to?(:__mkxp_first_existing_wskin)
      MessageConfig.define_singleton_method(:__mkxp_first_existing_wskin) do
        list = ($__mkxp_wskin_fallbacks || [])
        i = 0
        while i < list.length
          r = (pbResolveBitmap(list[i]) rescue nil)
          return r if r.is_a?(String) && !r.empty?
          i += 1
        end
        ""
      end
    end
    dbg "[WSKIN] MessageConfig helper de fallback OK"
  end
rescue => e
  dbg "[WSKIN] MessageConfig patch falhou: #{e.class}: #{e.message}"
end

# -- FORCAR windowskin resolvido (caixa de texto 32x32 vazia) -----------------
# CAUSA-RAIZ PROVADA (do log): pbResolveBitmap devolve "" para TODOS os
# windowskins, mesmo os que EXISTEM (001-Blue01, "speech se 1") -- a cadeia
# safeExists?/FileTest.exist? falha com caminhos relativos no 3DS. Por isso o
# MessageConfig memoiza @@defaultTextSkin="" -> a janela recebe nome vazio ->
# GifBitmap('/','') -> 32x32 -> caixa de texto invisivel.
#
# MAS o loader C++ (bmp_load_file) RESOLVE estes caminhos (loadPanels carregou
# pelo mesmo mecanismo, prefixando sdmc:/mkxp/game/). Logo o problema NAO e' o
# ficheiro nem o loader -- e' so' o pbResolveBitmap Ruby a falhar a verificacao.
#
# FIX (generico): NAO usar pbResolveBitmap. Setar @@defaultTextSkin / @@systemFrame
# DIRETAMENTE (class_variable_set) com um caminho que o loader C++ resolve. Quando
# a janela carregar a skin (AnimatedBitmap -> GifBitmap -> loader C++), o loader
# acha o ficheiro com o prefixo do jogo. Usa o 1o windowskin configurado do jogo
# e cai para 001-Blue01 (default universal RMXP/Essentials). Se nenhum existir, o
# loader C++ mostra magenta (visivel) em vez de 32x32 (invisivel) -- melhor de
# qualquer forma. So' actua se o valor atual estiver vazio.
begin
  if Object.const_defined?(:MessageConfig)
    # Caminho a forcar: 1o windowskin do jogo (se configurado) + fallback universal.
    skin_path = nil
    begin
      if defined?(Settings) && Settings.const_defined?(:SPEECH_WINDOWSKINS) &&
         Settings::SPEECH_WINDOWSKINS.is_a?(Array) && !Settings::SPEECH_WINDOWSKINS.empty?
        skin_path = "Graphics/Windowskins/" + Settings::SPEECH_WINDOWSKINS[0].to_s
      end
    rescue; end
    skin_path ||= "Graphics/Windowskins/001-Blue01"   # default universal (RESCHECK confirma que existe)

    # Setar DIRETO nas class-vars do MessageConfig, contornando pbResolveBitmap.
    # (pbSetSpeechFrame faria @@x = pbResolveBitmap(v) || "" -> voltaria a "".)
    need_speech = (MessageConfig.pbGetSpeechFrame.to_s.empty? rescue true)
    need_system = (MessageConfig.pbGetSystemFrame.to_s.empty? rescue true)
    begin
      MessageConfig.class_variable_set(:@@defaultTextSkin, skin_path) if need_speech
    rescue; end
    begin
      MessageConfig.class_variable_set(:@@systemFrame, skin_path) if need_system
    rescue; end
    dbg "[WSKIN] forcado DIRETO windowskin='#{skin_path}' (speech=#{need_speech} system=#{need_system})"
    sf = (MessageConfig.pbGetSpeechFrame rescue "?")
    dbg "[WSKIN] pbGetSpeechFrame agora = '#{sf}'"
  end
rescue => e
  dbg "[WSKIN] forcar windowskin falhou: #{e.class}: #{e.message}"
end
# === /FORCAR windowskin ======================================================

# -- PROBE DA CAIXA DE TEXTO (diagnostico do problema "caixa invisivel") -------
# A logica de pbMessage corre mas a janela nao aparece no ecra. Suspeita: a
# windowskin nao carrega (fica 32x32 vazia) -> os sprites da moldura ficam sem
# bitmap -> invisiveis. Este probe envolve SpriteWindow_Base#windowskin= e loga
# o TAMANHO REAL da windowskin aplicada + quantos sprites internos tem bitmap.
# Confirma a causa no log SEM alterar comportamento. So' loga as primeiras 5x.
begin
  if Object.const_defined?(:SpriteWindow_Base)
    class SpriteWindow_Base
      unless method_defined?(:__mkxp_orig_windowskin_set)
        alias __mkxp_orig_windowskin_set windowskin=
        @@__wskin_probe_count = 0
        def windowskin=(value)
          __mkxp_orig_windowskin_set(value)
          begin
            if @@__wskin_probe_count < 5
              @@__wskin_probe_count += 1
              w = (value.width rescue '?')
              h = (value.height rescue '?')
              disp = (value.disposed? rescue '?')
              dbg "[WSKIN-PROBE] ##{@@__wskin_probe_count} windowskin aplicada: #{value.class} #{w}x#{h} disposed=#{disp} rpgvx=#{@rpgvx}"
              # contar sprites internos com bitmap
              if @sprites.is_a?(Hash)
                com = 0; sem = 0; vis = 0
                @sprites.each do |k, sp|
                  next if sp.nil?
                  if (sp.bitmap rescue nil); com += 1; else; sem += 1; end
                  vis += 1 if (sp.visible rescue false)
                end
                dbg "[WSKIN-PROBE]   sprites: #{@sprites.size} total, #{com} com bitmap, #{sem} sem, #{vis} visiveis"
                dbg "[WSKIN-PROBE]   janela: viewport=#{@viewport.nil? ? 'NIL' : (@viewport.rect.inspect rescue '?')} visible=#{self.visible rescue '?'} x=#{self.x rescue '?'} y=#{self.y rescue '?'}"
              end
            end
          rescue => _pe
            dbg "[WSKIN-PROBE] erro: #{_pe.class}: #{_pe.message}"
          end
        end
      end
    end
    dbg "[WSKIN-PROBE] instalado em SpriteWindow_Base#windowskin="
  end
rescue => e
  dbg "[WSKIN-PROBE] instalacao falhou: #{e.class}: #{e.message}"
end
# === /PROBE DA CAIXA DE TEXTO ===============================================

# -- FIX CAIXA DE TEXTO: windowskin a carregar como 32x32 vazio ----------------
# DIAGNOSTICO (do log [WSKIN-PROBE]): a janela de mensagem cria 16 sprites,
# 15 com bitmap, 10 visiveis -- a janela ESTA' a ser desenhada. MAS a windowskin
# e' 32x32 (vazia) em vez do ficheiro real (96x48 "speech se 1"). Sem skin com
# pixels, a moldura desenha transparente -> caixa invisivel.
#
# CAUSA: GifBitmap#initialize (Essentials) faz:
#     begin; @bitmap = RPG::Cache.load_bitmap(dir, file); rescue; @bitmap = nil
#     @bitmap = BitmapWrapper.new(32, 32) if @bitmap.nil?
# Ou seja, se load_bitmap LANCA uma excecao, a skin vira 32x32 SILENCIOSAMENTE
# (o rescue engole o erro). Por isso nao aparece [BMP|MISS] -- a falha e' Ruby,
# nao do loader C++.
#
# FIX (2 partes):
#  1. Envolver GifBitmap#initialize para LOGAR a excecao exata (ver o que falha).
#  2. Se @bitmap ficou 32x32 (fallback), tentar carregar DIRETAMENTE via
#     BitmapWrapper.new(caminho_completo) -- o loader C++ (bmp_load_file) resolve
#     caminhos com espacos e varias extensoes, e SO' devolve 32x32 magenta se o
#     ficheiro mesmo nao existir (e nesse caso loga [BMP|MISS]).
begin
  if Object.const_defined?(:GifBitmap)
    class GifBitmap
      unless method_defined?(:__mkxp_orig_gif_init)
        alias __mkxp_orig_gif_init initialize
        @@__gif_diag_count = 0
        def initialize(dir, filename, hue = 0)
          # tentar o original; capturar a excecao que normalmente e' engolida
          _exc = nil
          begin
            @bitmap   = nil
            @disposed = false
            filename  = "" if !filename
            begin
              @bitmap = RPG::Cache.load_bitmap(dir, filename, hue)
            rescue => _e
              _exc = _e
              @bitmap = nil
            end
            # Se falhou (nil) e ha' nome, tentar carregar DIRETAMENTE pelo loader C++
            if @bitmap.nil? && filename && filename != ""
              full = "#{dir}#{filename}"
              begin
                @bitmap = BitmapWrapper.new(full)
              rescue => _e2
                _exc = _e2
                @bitmap = nil
              end
            end
            # diagnostico das primeiras vezes
            if @@__gif_diag_count < 8
              @@__gif_diag_count += 1
              w = (@bitmap.width rescue '?'); h = (@bitmap.height rescue '?')
              if _exc
                dbg "[GIF-FIX] '#{dir}#{filename}' -> #{w}x#{h} (load_bitmap lancou: #{_exc.class}: #{_exc.message})"
              else
                dbg "[GIF-FIX] '#{dir}#{filename}' -> #{w}x#{h} OK"
              end
            end
            # ultimo recurso: 32x32 (como o original) para nunca rebentar
            @bitmap = BitmapWrapper.new(32, 32) if @bitmap.nil?
            @bitmap.play if @bitmap&.animated?
          rescue => _efatal
            dbg "[GIF-FIX] erro fatal em GifBitmap.new('#{dir}#{filename}'): #{_efatal.class}: #{_efatal.message}"
            @bitmap = BitmapWrapper.new(32, 32)
          end
        end
      end
    end
    dbg "[GIF-FIX] GifBitmap#initialize envolvido (diagnostico+fallback directo)"
  end
rescue => e
  dbg "[GIF-FIX] instalacao falhou: #{e.class}: #{e.message}"
end
# === /FIX CAIXA DE TEXTO ====================================================

# -- Injectar metodos ausentes directamente apos MISSING_PATCH ----------------
# Os scripts do jogo redefinem Game_Player, Game_System, Game_Temp, etc. do
# zero, apagando qualquer patch feito em ios_compat_3ds.rb ou compat_stubs.h.
# Aqui (pos-scripts) adicionamos os metodos reais que faltam e que sao chamados
# a cada frame por Scene_Map/Spriteset_Map -- assim saem do method_missing.

begin
  if Object.const_defined?(:Game_Player)
    class Game_Player
      unless method_defined?(:tile_id)
        def tile_id; 0; end
      end
      unless method_defined?(:screen_x)
        def screen_x; 0; end
      end
      unless method_defined?(:screen_y)
        def screen_y; 0; end
      end
      unless method_defined?(:screen_z)
        def screen_z(h=0); 0; end
      end
      unless method_defined?(:animation_id)
        def animation_id; 0; end
      end
      unless method_defined?(:animation_id=)
        def animation_id=(v); end
      end
      unless method_defined?(:moving?)
        def moving?; false; end
      end
      unless method_defined?(:sprite_size)
        def sprite_size; [32, 32]; end
      end
      unless method_defined?(:sprite_size=)
        def sprite_size=(v); end
      end
    end
    dbg "[PATCH2] Game_Player direct methods OK"
  end
rescue => e
  dbg "[PATCH2] Game_Player direct methods failed: #{e.message}"
end

begin
  if Object.const_defined?(:Game_System)
    class Game_System
      unless method_defined?(:timer_working)
        def timer_working; false; end
      end
      unless method_defined?(:getPlayingBGM)
        def getPlayingBGM; nil; end
      end
    end
    dbg "[PATCH2] Game_System direct methods OK"
  end
rescue => e
  dbg "[PATCH2] Game_System direct methods failed: #{e.message}"
end

begin
  if Object.const_defined?(:Game_Temp)
    class Game_Temp
      unless method_defined?(:to_title)
        def to_title; @to_title ||= false; end
      end
      unless method_defined?(:to_title=)
        def to_title=(v); @to_title = v ? true : false; end
      end
      unless method_defined?(:transition_processing)
        def transition_processing; false; end
      end
      unless method_defined?(:transition_processing=)
        def transition_processing=(v); end
      end
      unless method_defined?(:menu_calling)
        def menu_calling; false; end
      end
      unless method_defined?(:menu_calling=)
        def menu_calling=(v); end
      end
      unless method_defined?(:debug_calling)
        def debug_calling; false; end
      end
      unless method_defined?(:debug_calling=)
        def debug_calling=(v); end
      end
      unless method_defined?(:player_transferring)
        def player_transferring; false; end
      end
      unless method_defined?(:player_transferring=)
        def player_transferring=(v); end
      end
    end
    dbg "[PATCH2] Game_Temp direct methods OK"
  end
rescue => e
  dbg "[PATCH2] Game_Temp direct methods failed: #{e.message}"
end

# NilClass#expired? -- chamado quando getPlayingBGM devolve nil e o jogo
# testa bgm.expired? para saber se o audio acabou.
begin
  class NilClass
    unless method_defined?(:expired?)
      def expired?; false; end
    end
  end
rescue; end

# -- Helper: garante que $data_tilesets tem dados REAIS de Data/Tilesets.rxdata -
# Carrega o ficheiro on-demand (idempotente). SEM este ficheiro o tileset_name
# de cada mapa vem vazio -> pbGetTileset devolve 32x32 -> MAPA SEMPRE PRETO.
# Este helper tambem deixa um aviso claro no log quando o ficheiro falta.
begin
  def __load_tilesets_if_needed__
    return true if $__tilesets_ok__
    # Ja temos dados validos? (alguma entrada com tileset_name nao-vazio)
    if $data_tilesets.is_a?(Array) && $data_tilesets.length > 1
      i = 1
      while i < $data_tilesets.length
        t = $data_tilesets[i]
        if t && ((t.tileset_name rescue "") != "")
          $__tilesets_ok__ = true
          return true
        end
        i += 1
      end
    end
    # So tentamos carregar uma vez (evita spam de erros se o ficheiro faltar)
    return false if $__tilesets_tried__
    $__tilesets_tried__ = true
    begin
      loaded = load_data("Data/Tilesets.rxdata")
      if loaded.is_a?(Array) && loaded.length > 1
        $data_tilesets = loaded
        $__tilesets_ok__ = true
        dbg "[TSET] OK: Data/Tilesets.rxdata carregado (#{loaded.length} tilesets)"
        return true
      else
        dbg "[TSET] AVISO: Tilesets.rxdata com formato inesperado (#{loaded.class rescue '?'})"
      end
    rescue => e
      dbg "[TSET] *** Data/Tilesets.rxdata EM FALTA OU ILEGIVEL (#{e.message}) ***"
      dbg "[TSET] *** Sem tilesets validos o MAPA FICA SEMPRE PRETO. ***"
      dbg "[TSET] *** Copia Data/Tilesets.rxdata do teu projeto PC para o cartao SD (pasta game/Data/). ***"
    end
    false
  end
rescue; end

# -- Patch explicito de metodos em falta na Game_Map real ---------------------
# O method_missing so actua se o metodo NAO existir. Se Game_Map define bridge
# como attr_accessor mas nunca o inicializa, retorna nil -- o que e truthy em
# alguns contextos e falsy noutros, causando crashes inesperados.
# Aqui forcamos valores seguros directamente.
begin
  if Object.const_defined?(:Game_Map)
    class Game_Map
      # bridge: indica se o tile actual e uma ponte (0=nao, 1=sim)
      # Usado por Sprite_Character, Sprite_Reflection, etc. durante update()
      unless method_defined?(:bridge)
        def bridge; 0; end
      end
      # passable?: necessario para movimento do player
      unless method_defined?(:passable?)
        def passable?(*a); true; end
      end
      # valid?: necessario para bounds checking
      unless method_defined?(:valid?)
        def valid?(*a); true; end
      end
      # terrain_tag: necessario para varios sistemas
      unless method_defined?(:terrain_tag)
        def terrain_tag(*a); 0; end
      end
      # events: lista de eventos do mapa
      unless method_defined?(:events)
        def events; @events ||= {}; end
      end
      # name: nome do mapa (usado em varios sitios)
      unless method_defined?(:name)
        def name; @name ||= ""; end
      end
      # tileset_id: ID do tileset no $data_tilesets (critico para tileset_name)
      # NOTA: sem 'unless' -- o Game_Map do Essentials ja define tileset_id mas
      # pode devolver nil/@tileset_id nao inicializado. Reescrevemos sempre.
      def tileset_id
        # NOTA 3DS: NAO usar defined?(@map) -- neste build de mruby o defined?
        # nao funciona como keyword (cai no method_missing e devolve nil), o que
        # fazia este metodo devolver sempre o fallback 1 em vez do tileset_id
        # real do mapa (ex: 4). Referenciar @map directamente e seguro: uma ivar
        # nao definida devolve nil sem erro.
        tid = nil
        tid = (@map.tileset_id rescue nil) if @map
        tid = (@tileset_id rescue nil) if tid.nil?
        (tid.is_a?(Integer) && tid > 0) ? tid : 1
      end
      # tileset_name: CRITICO para Spriteset_Map -- deve devolver nome PNG real.
      # Reescrevemos sempre (sem 'unless') porque o metodo do Essentials pode
      # devolver "" se $data_tilesets nao foi carregado correctamente.
      def tileset_name
        # Garantir que $data_tilesets tem dados reais (carrega Tilesets.rxdata
        # on-demand). Sem isto o nome vem vazio -> tileset 32x32 -> ecra preto.
        __load_tilesets_if_needed__ rescue nil
        begin
          tid = self.tileset_id
          ts  = ($data_tilesets[tid] rescue nil)
          if ts && ts.respond_to?(:tileset_name)
            tn = ts.tileset_name rescue ""
            return tn if tn && tn != ""
          end
        rescue; end
        @tileset_name rescue ""
      end
    end
    dbg "[PATCH] Game_Map metodos essenciais OK"
  end
rescue => e
  dbg "[PATCH] Game_Map patch falhou: #{e.message}"
end

# -- Forcar Game_Map#bridge a devolver sempre Integer -------------------------
# attr_accessor :bridge deixa @bridge=nil se nunca inicializado.
# Reescrevemos directamente sem alias para garantir compatibilidade mruby.
begin
  if Object.const_defined?(:Game_Map)
    class Game_Map
      def bridge
        @bridge.nil? ? 0 : @bridge
      end
      def bridge=(v)
        @bridge = v
      end
    end
    dbg "[PATCH] Game_Map#bridge nil-guard OK"
  end
rescue => e
  dbg "[PATCH] Game_Map#bridge nil-guard falhou: #{e.message}"
end

# -- Forcar $game_map.bridge=0 na instancia actual ----------------------------
begin
  if $game_map && $game_map.respond_to?(:bridge=)
    $game_map.bridge = 0 rescue nil
  end
rescue; end

# -- Game_Player: metodos de movimento/surf em falta --------------------------
# Sprite_Character e Sprite_SurfBase chamam estes metodos durante initialize/update.
# surfing, bob_height, diving, riding -- todos relacionados com movimento especial PE19.
begin
  if Object.const_defined?(:Game_Player)
    class Game_Player
      unless method_defined?(:surfing)
        def surfing;    false; end
      end
      unless method_defined?(:diving)
        def diving;     false; end
      end
      unless method_defined?(:riding)
        def riding;     false; end
      end
      unless method_defined?(:bob_height)
        def bob_height; 0; end
      end
      unless method_defined?(:move_speed)
        def move_speed; 4; end
      end
      unless method_defined?(:follower)
        def follower;   nil; end
      end
      # tile_id: ID do tile actual do player (usado por Spriteset_Map/pbDayNightTint)
      unless method_defined?(:tile_id)
        def tile_id; 0; end
      end
      # sprite_size: tamanho do sprite [w, h] (setter chamado em Spriteset_Map#initialize)
      unless method_defined?(:sprite_size)
        def sprite_size; [32, 32]; end
      end
      unless method_defined?(:sprite_size=)
        def sprite_size=(v); v; end
      end
    end
    dbg "[PATCH] Game_Player metodos surf/mov/tile OK"
  end
rescue => e
  dbg "[PATCH] Game_Player patch falhou: #{e.message}"
end

# -- Game_Character: metodos em falta -----------------------------------------
begin
  if Object.const_defined?(:Game_Character)
    class Game_Character
      unless method_defined?(:bob_height)
        def bob_height; 0; end
      end
      unless method_defined?(:surfing)
        def surfing;    false; end
      end
      unless method_defined?(:diving)
        def diving;     false; end
      end
      unless method_defined?(:follower)
        def follower;   nil; end
      end
      unless method_defined?(:shadow_filename)
        def shadow_filename; nil; end
      end
      unless method_defined?(:shadow_size)
        def shadow_size; 0; end
      end
    end
    dbg "[PATCH] Game_Character metodos em falta OK"
  end
rescue => e
  dbg "[PATCH] Game_Character patch falhou: #{e.message}"
end

# -- Rede de seguranca global: metodos de mapa/player em NilClass/Object --------
# $game_map e $game_player podem ser nil quando createSpritesets corre.
# Definir estes metodos em Object garante que NilClass os herda e nao crasha.
begin
  class Object
    def bridge
      dbg "[BRIDGE-FALLBACK] #{self.class}#bridge -- devolvendo 0"
      0
    end
    def surfing
      dbg "[SURF-FALLBACK] #{self.class}#surfing -- devolvendo false"
      false
    end
    def diving;          false; end
    def riding;          false; end
    def bob_height;          0; end
    def shadow_filename;   nil; end
    def shadow_size;         0; end
    def follower;          nil; end
    def move_speed;          4; end
    def through;         false; end
    def transparent;     false; end
    def bush_depth;          0; end
    def move_route_forcing; false; end
    def character_name;     ""; end
    def character_hue;       0; end
    def direction;           2; end
    def pattern;             0; end
    def opacity;           255; end
    def blend_type;          0; end
    def real_x;              0; end
    def real_y;              0; end
    # FIX: Object#tile_id / screen_x / screen_y / screen_z / animation_id
    # Chamados sobre $game_player quando ainda e nil (NilClass herda de Object).
    # [MFD] Object#tile_id [x3], screen_x [x2], screen_y [x2], screen_z [x2],
    # animation_id [x2], sprite_size=, timer, timer_working -- todos no log.
    def tile_id;              0; end
    def screen_x;             0; end
    def screen_y;             0; end
    def screen_z(*_a);        0; end
    def animation_id;         0; end
    def animation_id=(_v);     end
    def sprite_size;    [32,32]; end
    def sprite_size=(_v);      end
    # FIX: Object#timer / timer_working -- Sprite_Timer acede a $game_system.timer
    # quando $game_system ainda e nil.
    def timer;                0; end
    def timer=(_v);            end
    def timer_working;    false; end
    def timer_working=(_v);    end
    # FIX: Object#weather_duration / tone / flash_color -- aparecem no log como
    # [MISSING] Object#weather_duration() / tone() / flash_color()
    # Sao acedidos sobre $game_screen quando nil. Valores seguros para nao crashar.
    def weather_duration;     0; end
    def weather_duration=(_v); end
    def tone;               nil; end
    def tone=(_v);             end
    def flash_color;        nil; end
    def flash_color=(_v);      end
    # FIX: Object#bicycle / bicycle= / set_movement_type -- chamados sobre
    # $game_player. Se $game_player for nil ou stub incompleto, estes evitam crash.
    def bicycle;          false; end
    def bicycle=(_v);          end
    def set_movement_type(*_a); end
    # FIX: Object#moving? -- chamado em Scene_Map#update sobre $game_player
    def moving?;          false; end
  end
  dbg "[PATCH] Object fallback global OK"
rescue => e
  dbg "[PATCH] Object fallback falhou: #{e.message}"
end

# -- Patch Scene_Map#main: limite de seguranca anti-loop-infinito --------------
# O Scene_Map#main original do Essentials tem um loop interno que so termina
# quando $scene != self. Se os globals estao em estado stub, $scene nunca muda
# e o loop e infinito. Aqui adicionamos um contador de frames com limite
# configuravel. MAX_FRAMES = 300 = ~5 segundos a 60fps -- suficiente para
# confirmar que a scene arranca, mas evita loop eterno em modo de diagnotico.
#
# IMPORTANTE: este patch so actua se Scene_Map ainda nao tiver um update loop
# funcional (detectado pelo facto de $game_player ser um stub/nil apos 1 frame).
# Em producao (com dados reais), o loop termina naturalmente -- este patch e
# apenas uma rede de seguranca para o modo de diagnostico 3DS.
begin
  if Object.const_defined?(:Scene_Map)
    class Scene_Map
      # Guardar o main original se existir
      if method_defined?(:main) && !method_defined?(:__orig_main_3ds)
        alias __orig_main_3ds main

        def main
          dbg "[Scene_Map] main: iniciando (watchdog C++ activado)"

          # ── FIX ECRA PRETO / INTRO DO OAK BLOQUEADA ───────────────────────────
          # CAUSA-RAIZ (provada pelo log: "switches=NilClass variables=NilClass
          # self_switches=NilClass"): o caminho New Game (PokemonLoad_Scene) nao
          # chegou a criar $game_switches/$game_variables/$game_self_switches
          # (o SaveData.load_new_game_values falhou -> $game_player=NilClass).
          # Sem eles, o evento de arranque 'QuickStartActivate' (Map001 id=4) faz
          # "Controlar Variavel[1] += 1" num $game_variables nil -> a variavel
          # fica presa em 0 -> a condicao "Var[1] < 5" e' SEMPRE verdadeira ->
          # loop infinito -> a linha que activa o self-switch A do Evento 1
          # 'Intro' (fala do Prof. Oak) NUNCA e' alcancada -> fundo preto eterno.
          # Criamos aqui os 3 globais reais (idempotente) ANTES do loop de eventos.
          begin
            if $game_switches.nil? || !$game_switches.respond_to?(:[])
              $game_switches = Game_Switches.new
              dbg "[Scene_Map] FIX: $game_switches criado (#{$game_switches.class})"
            end
            if $game_variables.nil? || !$game_variables.respond_to?(:[])
              $game_variables = Game_Variables.new
              dbg "[Scene_Map] FIX: $game_variables criado (#{$game_variables.class})"
            end
            if $game_self_switches.nil? || !$game_self_switches.respond_to?(:[])
              $game_self_switches = Game_SelfSwitches.new
              dbg "[Scene_Map] FIX: $game_self_switches criado (#{$game_self_switches.class})"
            end
            # PREVENCAO: $game_temp e $game_system tambem sao necessarios e podem
            # ficar nil pelo mesmo motivo (load_new_game_values falhou). Sem
            # $game_temp com to_title=false, Scene_Map#update pode disparar
            # pbCallTitle em loop. Sem $game_system, o map_interpreter/contadores
            # de eventos partem. Criamos reais se faltarem (idempotente).
            if ($game_temp.nil? || !$game_temp.respond_to?(:to_title)) && Object.const_defined?(:Game_Temp)
              begin
                $game_temp = Game_Temp.new
                dbg "[Scene_Map] FIX: $game_temp criado (#{$game_temp.class})"
              rescue => _gte
                dbg "[Scene_Map] FIX $game_temp falhou: #{_gte.message}"
              end
            end
            if ($game_system.nil? || !$game_system.respond_to?(:map_interpreter)) && Object.const_defined?(:Game_System)
              begin
                $game_system = Game_System.new
                dbg "[Scene_Map] FIX: $game_system criado (#{$game_system.class})"
              rescue => _gsy
                dbg "[Scene_Map] FIX $game_system falhou: #{_gsy.message}"
              end
            end
          rescue => _ge
            dbg "[Scene_Map] FIX globais falhou: #{_ge.class}: #{_ge.message}"
          end
          # ──────────────────────────────────────────────────────────────────────

          _player_real = $game_player && $game_player.is_a?(Game_Player) rescue false
          dbg "[Scene_Map] $game_player real=#{_player_real}, class=#{$game_player.class rescue 'nil'}"

          # Diagnostico pre-main: verificar classes criticas do RGSS
          dbg "[Scene_Map] Viewport=#{Object.const_defined?(:Viewport) ? 'OK' : 'AUSENTE'}"
          dbg "[Scene_Map] Tilemap=#{Object.const_defined?(:Tilemap)   ? 'OK' : 'AUSENTE'}"
          dbg "[Scene_Map] Plane=#{Object.const_defined?(:Plane)       ? 'OK' : 'AUSENTE'}"
          dbg "[Scene_Map] CustomTilemap=#{Object.const_defined?(:CustomTilemap) ? 'OK' : 'AUSENTE'}"
          dbg "[Scene_Map] Spriteset_Map init_patched=#{Spriteset_Map.method_defined?(:__orig_init_3ds__) rescue 'err'}"

          # Testar Viewport.new isoladamente (falha cedo se o stub foi substituido por versao quebrada)
          begin
            _vp_test = Viewport.new(0, 0, 1, 1)
            dbg "[Scene_Map] Viewport.new(0,0,1,1) OK: #{_vp_test.class}"
          rescue => _vp_err
            dbg "[Scene_Map] AVISO: Viewport.new falhou: #{_vp_err.class}: #{_vp_err.message}"
          end

          # Activar watchdog no C++: a partir daqui, grph_update chama
          # Input.update e $scene.update directamente, garantindo que correm
          # mesmo que o loop Ruby de Scene_Map os salte ou engula erros.
          # O watchdog lanca RuntimeError apos 600 frames sem $scene mudar.
          Graphics.watchdog_enable(600) rescue nil

          begin
            __orig_main_3ds
          rescue RuntimeError => e
            if e.message.include?("[WD]")
              dbg "[Scene_Map] WATCHDOG TIMEOUT -- $scene nao mudou em 600 frames"
            else
              dbg "[Scene_Map] main CRASH: #{e.class}: #{e.message}"
              begin
                bt = e.backtrace
                bt[0,10].each_with_index { |l,i| dbg "[Scene_Map]  bt[#{i}]: #{l}" } if bt
              rescue; end
            end
            $scene = nil
          rescue => e
            dbg "[Scene_Map] main CRASH: #{e.class}: #{e.message}"
            begin
              bt = e.backtrace
              bt[0,10].each_with_index { |l,i| dbg "[Scene_Map]  bt[#{i}]: #{l}" } if bt
            rescue; end
            $scene = nil
          ensure
            Graphics.watchdog_disable rescue nil
          end

          _frames = Graphics.watchdog_count rescue 0
          dbg "[Scene_Map] main terminou: frames=#{_frames} $scene=#{$scene.nil? ? 'nil' : $scene.class}"
        end
      end
    end
    dbg "[PATCH] Scene_Map#main watchdog OK"
  end
rescue => e
  dbg "[PATCH] Scene_Map#main watchdog falhou: #{e.message}"
end

# -- Patch Scene_Map#update: log GRANULAR por secção (suspeitas #1..#4) -------
# Versão cirúrgica: cada etapa do update é envolvida individualmente para
# identificar exactamente onde ocorre a excepção silenciosa.
# Suspeita #1: $game_screen.update (90%) -- #2: $game_system.update (70%)
# Suspeita #3: Spriteset crash antes do SS#update -- #4: Graphics.update não chamado
begin
  if Object.const_defined?(:Scene_Map) && Scene_Map.method_defined?(:update) &&
     !Scene_Map.method_defined?(:__update_orig_diag__)
    class Scene_Map
      alias __update_orig_diag__ update

      @@__update_call_count = 0

      def update
        @@__update_call_count += 1
        n = @@__update_call_count

        # Envelope: primeiras 10 chamadas + heartbeat a cada 100
        if n <= 10
          dbg "[SM#update] chamada ##{n}"
          dbg "[SM#update]   to_title=#{$game_temp.to_title rescue '?'}"
          dbg "[SM#update]   player_transferring=#{$game_temp.player_transferring rescue '?'}"
          dbg "[SM#update]   transition_processing=#{$game_temp.transition_processing rescue '?'}"
          dbg "[SM#update]   @spritesets=#{@spritesets.nil? ? 'NIL' : "Hash(#{@spritesets.size})" rescue 'ERR'}"
          dbg "[SM#update]   @spritesetGlobal=#{@spritesetGlobal.nil? ? 'NIL' : @spritesetGlobal.class rescue 'ERR'}"
          dbg "[SM#update]   $MapFactory=#{$MapFactory.nil? ? 'NIL' : $MapFactory.class}"
          dbg "[SM#update]   $scene=#{$scene.nil? ? 'nil' : $scene.class}"
        elsif n % 100 == 0
          dbg "[SM#update] heartbeat ##{n} $scene=#{$scene.nil? ? 'nil' : $scene.class}"
        end

        # --- Suspeita #2: $game_system.update ---
        begin
          $game_system.update if $game_system
        rescue => _e
          dbg "[SM#update] CRASH em game_system.update: #{_e.class}: #{_e.message}"
          begin; dbg "[SM#update]   bt: #{_e.backtrace.first(3).join(' | ')}"; rescue; end
        end

        # --- Suspeita #1: $game_screen.update ---
        begin
          $game_screen.update if $game_screen
        rescue => _e
          dbg "[SM#update] CRASH em game_screen.update: #{_e.class}: #{_e.message}"
          begin; dbg "[SM#update]   bt: #{_e.backtrace.first(3).join(' | ')}"; rescue; end
        end

        # --- Corpo principal do update original ---
        begin
          __update_orig_diag__
          dbg "[SM#update] __update_orig_diag__ OK ##{n}" if n <= 10
        rescue => _e
          dbg "[SM#update] CRASH no update principal ##{n}: #{_e.class}: #{_e.message}"
          begin
            _bt = _e.backtrace
            _bt[0, 8].each_with_index { |l, i| dbg "[SM#update]   bt[#{i}]: #{l}" } if _bt
          rescue; end
          # Não re-lançar: manter o loop vivo para diagnostico continuado
        end

        dbg "[SM#update] update completo ##{n}" if n <= 10
      end
    end
    dbg "[PATCH] Scene_Map#update granular OK"
  end
rescue => e
  dbg "[PATCH] Scene_Map#update granular falhou: #{e.message}"
end

# -- Patch Scene_Map#createSpritesets: logar crash e estado ------------------
begin
  if Object.const_defined?(:Scene_Map) && Scene_Map.method_defined?(:createSpritesets)
    class Scene_Map
      unless method_defined?(:__orig_createSpritesets_3ds)
        alias __orig_createSpritesets_3ds createSpritesets
        def createSpritesets
          dbg "[createSpritesets] $MapFactory=#{$MapFactory.nil? ? 'NIL' : $MapFactory.class}"
          dbg "[createSpritesets] $game_map=#{$game_map.nil? ? 'NIL' : $game_map.class}"
          begin
            maps = $MapFactory.maps rescue nil
            dbg "[createSpritesets] $MapFactory.maps=#{maps.nil? ? 'NIL' : "Array(#{maps.size})"}"
          rescue => e
            dbg "[createSpritesets] $MapFactory.maps CRASH: #{e.class}: #{e.message}"
          end
          begin
            __orig_createSpritesets_3ds
            dbg "[createSpritesets] OK: @spritesets=#{@spritesets.nil? ? 'NIL' : "Hash(#{@spritesets.size})" rescue 'ERR'}"
            dbg "[createSpritesets] @spritesetGlobal=#{@spritesetGlobal.nil? ? 'NIL' : @spritesetGlobal.class}"
          rescue => e
            dbg "[createSpritesets] CRASH: #{e.class}: #{e.message}"
            begin
              bt = e.backtrace
              bt[0,10].each_with_index { |l,i| dbg "[createSpritesets]   bt[#{i}]: #{l}" } if bt
            rescue; end
          end
        end
      end
    end
    dbg "[PATCH] Scene_Map#createSpritesets diagnostico OK"
  end
rescue => e
  dbg "[PATCH] Scene_Map#createSpritesets diagnostico falhou: #{e.message}"
end

# -- $PokemonGlobal / $PokemonTemp: instanciar se nil -------------------------
# Sem save file estes globals sao nil. Game_Player#update, Overworld e
# DependentEvents chamam .bridge, .surfing, .dependentEvents, etc. a cada
# frame -> NilClass fallbacks imprimem warnings e nunca param.
# Instanciar aqui (pos-scripts, pre-main) com .new seguro.
begin
  if $PokemonGlobal.nil?
    _pg_klass = (Object.const_defined?(:PokemonGlobalMetadata) ? PokemonGlobalMetadata :
                 Object.const_defined?(:PokemonGlobal)         ? PokemonGlobal         : nil)
    if _pg_klass
      begin
        $PokemonGlobal = _pg_klass.new
        dbg "[INIT] $PokemonGlobal = #{_pg_klass}.new OK"
      rescue => e2
        dbg "[INIT] #{_pg_klass}.new falhou (#{e2.message}) -- allocate"
        $PokemonGlobal = _pg_klass.allocate rescue nil
      end
    end
  end
rescue => e
  dbg "[INIT] $PokemonGlobal.new falhou: #{e.message}"
end
begin
  if $PokemonTemp.nil? && Object.const_defined?(:PokemonTemp)
    $PokemonTemp = PokemonTemp.new rescue nil
    dbg "[INIT] $PokemonTemp = PokemonTemp.new OK"
  end
rescue => e
  dbg "[INIT] $PokemonTemp.new falhou: #{e.message}"
end
# Garantir campos criticos de PokemonGlobal mesmo que .new falhe
begin
  if $PokemonGlobal
    $PokemonGlobal.bridge    = 0     unless ($PokemonGlobal.bridge    rescue nil)
    $PokemonGlobal.surfing   = false unless ($PokemonGlobal.surfing   rescue nil)
    $PokemonGlobal.visitedMaps ||= {} rescue nil
    $PokemonGlobal.mapTrail    ||= [] rescue nil
    $PokemonGlobal.stepcount   ||= 0  rescue nil
    $PokemonGlobal.repel       ||= 0  rescue nil
    # dependentEvents deve ser DependentEvents se a classe existir, senso []
    begin
      de = $PokemonGlobal.dependentEvents rescue nil
      if de.nil? || de.is_a?(Integer)
        $PokemonGlobal.dependentEvents = (Object.const_defined?(:DependentEvents) ? DependentEvents.new : []) rescue []
      end
    rescue; end
  end
rescue => e
  dbg "[INIT] PokemonGlobal fields init falhou: #{e.message}"
end
# Garantir campos criticos de PokemonTemp
begin
  if $PokemonTemp
    $PokemonTemp.encounterTriggered       = false rescue nil
    $PokemonTemp.hiddenMoveEventCalling   = false rescue nil
    $PokemonTemp.keyItemCalling           = false rescue nil
    $PokemonTemp.miniupdate               = false rescue nil
  end
rescue => e
  dbg "[INIT] PokemonTemp fields init falhou: #{e.message}"
end

# -- Scene_DebugIntro bypass ---------------------------------------------------
# Na primeira chamada (boot): vai directo para Scene_Map.
# Nas chamadas seguintes (retorno ao titulo): deixa o Load Screen normal correr.
# $__debug_intro_count controla quantas vezes DebugIntro foi chamado.
$__debug_intro_count = 0

# ============================================================================
# INTERRUPTOR: mostrar o fluxo NORMAL do jogo (tela de titulo -> intro do
# professor -> criacao de personagem) em vez do bypass directo para o mapa.
#
# O proprio jogo, quando $DEBUG=true, faz pbCallTitle -> Scene_DebugIntro
# (salta o titulo, modo programador). Nos forcamos $DEBUG=true no arranque, por
# isso nunca se via o titulo. Com USE_REAL_TITLE=true, em vez do bypass corremos
# o Scene_Intro real (a tela "PRESS ENTER") e deixamos o jogo seguir o seu fluxo.
#
# Se o Scene_Intro real der problemas no port, poe USE_REAL_TITLE=false para
# voltar ao bypass que ja funcionava (entrada directa no mapa).
# ============================================================================
USE_REAL_TITLE = true unless defined?(USE_REAL_TITLE)

class Scene_DebugIntro
  def main
    $__debug_intro_count = ($__debug_intro_count || 0) + 1
    dbg "[DebugIntro] chamada ##{$__debug_intro_count}"

    # SMOKE TEST: corre UMA vez, logo na 1a entrada, com os dados ja carregados.
    # Percorre imagens/dados/classes/audio/tiles/animacoes e grava o relatorio
    # em sdmc:/mkxp/smoke_test.log. Controlado por SMOKE_TEST_ON (default true).
    # Para desligar: cria $smoke_done=true antes, ou apaga smoke_test.rb.
    if $__debug_intro_count == 1 && !$__smoke_done
      $__smoke_done = true
      # Reinstalar o nosso pbBitmap REAL depois dos plugins terem corrido (um
      # plugin pode te-lo redefinido sem o fallback Bitmap.new). Garante que o
      # titulo MODTS recebe Bitmaps reais e nao 0.
      begin
        if respond_to?(:__mkxp_real_pbBitmap, true)
          Object.class_eval do
            def pbBitmap(name); __mkxp_real_pbBitmap(name); end
          end
          dbg "[BMP] pbBitmap REAL reinstalado pos-plugins"
        end
      rescue => e
        dbg "[BMP] reinstalar pbBitmap falhou: #{e.message}"
      end
      begin
        on = (defined?(SMOKE_TEST_ON) ? SMOKE_TEST_ON : true)
        if on && defined?(SmokeTest)
          dbg "[SMOKE] a correr smoke test (relatorio em smoke_test.log)..."
          SmokeTest.run_all
          dbg "[SMOKE] smoke test concluido"
        else
          dbg "[SMOKE] smoke test desligado ou nao carregado"
        end
      rescue => e
        dbg "[SMOKE] smoke test rebentou: #{e.class}: #{e.message}"
      end
    end

    # -- Caminho do fluxo NORMAL: correr a tela de titulo real (Scene_Intro) --
    if (USE_REAL_TITLE rescue false) && $__debug_intro_count == 1
      dbg "[DebugIntro] USE_REAL_TITLE: a tentar Scene_Intro (tela de titulo real)"

      # -- INSTRUMENTACAO ModularTitleScreen (Solar Eclipse) -----------------
      # Os scripts do jogo ja estao carregados aqui, por isso ModularTitleScreen
      # ja existe. Envolvemos new/intro/playBGM/update para o log dizer onde
      # rebenta. Objetivo: mostrar a tela bonita, nao simplificar.
      begin
        if defined?(ModularTitleScreen)
          ModularTitleScreen.class_eval do
            unless method_defined?(:__mts_orig_init)
              alias_method :__mts_orig_init, :initialize
              def initialize(*a, &b)
                dbg "[MTS] new START"
                begin
                  __mts_orig_init(*a, &b)
                  dbg "[MTS] new OK sprites=#{(@sprites rescue {}).keys.inspect rescue '?'}"
                rescue => e
                  dbg "[MTS] new CRASH: #{e.class}: #{e.message}"
                  dbg "[MTS] new BT: #{(e.backtrace || [])[0,8].join(' | ')}"
                  raise
                end
              end
            end
          end
          ["playBGM", "intro", "update"].each do |mn|
            next unless ModularTitleScreen.method_defined?(mn.to_sym)
            ModularTitleScreen.class_eval do
              an = "__mts_orig_#{mn}"
              unless method_defined?(an.to_sym)
                alias_method an.to_sym, mn.to_sym
                define_method(mn.to_sym) do |*a, &b|
                  dbg "[MTS] #{mn} START"
                  begin
                    r = send(an.to_sym, *a, &b)
                    dbg "[MTS] #{mn} END"
                    r
                  rescue => e
                    dbg "[MTS] #{mn} CRASH: #{e.class}: #{e.message}"
                    dbg "[MTS] #{mn} BT: #{(e.backtrace || [])[0,8].join(' | ')}"
                    raise
                  end
                end
              end
            end
          end
          dbg "[MTS] instrumentacao instalada OK"
        else
          dbg "[MTS] ModularTitleScreen NAO definido!"
        end
      rescue => e
        dbg "[MTS] instrumentacao falhou: #{e.class}: #{e.message}"
      end
      # -- /INSTRUMENTACAO ---------------------------------------------------

      begin
        # $DEBUG=false para o jogo seguir o fluxo normal (titulo->intro->etc)
        # a partir daqui. Mantemos os outros globals.
        $DEBUG = false
        intro = Scene_Intro.new
        $scene = intro
        dbg "[DebugIntro] Scene_Intro criado OK -> $scene=Scene_Intro"
        return
      rescue => e
        dbg "[DebugIntro] Scene_Intro FALHOU (#{e.class}: #{e.message}) -- a usar bypass"
        $DEBUG = true
        # cai para o bypass abaixo
      end
    end

    # A partir da 2a chamada (retorno ao titulo apos Scene_Map terminar):
    # deixar o Load Screen normal do jogo correr em vez de forcara Scene_Map.
    # Isto quebra o ciclo DebugIntro<->Scene_Map.
    if $__debug_intro_count > 1
      dbg "[DebugIntro] retorno ao titulo -- correndo Load Screen normal"
      begin
        Graphics.transition(0)
        sscene  = PokemonLoad_Scene.new
        sscreen = PokemonLoadScreen.new(sscene)
        sscreen.pbStartLoadScreen
        Graphics.freeze
      rescue => e
        dbg "[DebugIntro] Load Screen falhou (#{e.class}: #{e.message}) -- $scene=nil"
        $scene = nil
      end
      return
    end

    # -- Primeira chamada: inicializar e ir para Scene_Map --
    dbg "[DebugIntro] bypass: setting $scene = Scene_Map"
    Graphics.transition(0)
    begin; $game_temp.common_event_id = 0; rescue; end

    # Inicializar o jogo passo a passo, evitando validate() que falha no mruby.
    # Equivale a Game.start_new mas sem as chamadas validate().
    begin
      dbg "[DebugIntro] init step A: SaveData.load_new_game_values"
      SaveData.load_new_game_values
      dbg "[DebugIntro] init step A OK: $game_player=#{$game_player.class}"
    rescue => e
      dbg "[DebugIntro] SaveData.load_new_game_values falhou (#{e.class}: #{e.message})"
    end

    begin
      dbg "[DebugIntro] init step B: PokemonMapFactory"
      _mf_ok = begin; $MapFactory && $MapFactory.is_a?(PokemonMapFactory); rescue; false; end
      unless _mf_ok
        _map_id = ($data_system.respond_to?(:start_map_id) ? $data_system.start_map_id.to_i : 0)
        _map_id = 1 if _map_id <= 0
        $MapFactory = PokemonMapFactory.new(_map_id)
		dbg "[DebugIntro] PokemonMapFactory.new OK"
		_gm = $MapFactory.map rescue $game_map
		dbg "[MAP] map_id=#{_gm.map_id rescue 'ERR'}"
		dbg "[MAP] tileset_id=#{_gm.tileset_id rescue 'ERR'}"
		dbg "[MAP] tileset_name='#{_gm.tileset_name rescue 'ERR'}'"
		dbg "[MAP] data=#{_gm.data.nil? ? 'NIL' : _gm.data.class}"
		dbg "[MAP] @map=#{(_gm.instance_variable_get(:@map).class rescue 'ERR')}"
		dbg "[MAP] @map.tileset_id=#{(_gm.instance_variable_get(:@map).tileset_id rescue 'ERR')}"
		_ts1 = ($data_tilesets.is_a?(Array) ? $data_tilesets[1] : nil) rescue nil
		dbg "[MAP] data_tilesets[1]=#{_ts1.nil? ? 'NIL' : _ts1.class}"
		dbg "[MAP] data_tilesets[1].tileset_name='#{(_ts1.tileset_name rescue 'ERR')}'"
		dbg "[MAP] $game_map class=#{$game_map.class}"
      end
    rescue => e
      dbg "[DebugIntro] PokemonMapFactory falhou (#{e.class}: #{e.message})"
    end

    begin
      dbg "[DebugIntro] init step C: game_player moveto"
      if $game_player && $data_system
        _sx = ($data_system.respond_to?(:start_x) ? $data_system.start_x.to_i : 0) rescue 0
        _sy = ($data_system.respond_to?(:start_y) ? $data_system.start_y.to_i : 0) rescue 0
        $game_player.moveto(_sx, _sy)
        $game_player.refresh
        dbg "[DebugIntro] moveto OK"
      end
    rescue => e
      dbg "[DebugIntro] moveto falhou (#{e.class}: #{e.message})"
    end

    begin
      dbg "[DebugIntro] init step D: PokemonEncounters"
      $PokemonEncounters = PokemonEncounters.new
      $PokemonEncounters.setup($game_map.map_id)
      dbg "[DebugIntro] PokemonEncounters OK"
    rescue => e
      dbg "[DebugIntro] PokemonEncounters falhou (#{e.class}: #{e.message})"
    end

    begin; $game_map.autoplay; rescue; end
    begin; $game_map.update; rescue; end

    begin
      $PokemonSystem = PokemonSystem.new unless $PokemonSystem
    rescue; end

    # -- $PokemonGlobal / $PokemonTemp: instanciar AQUI, pos-SaveData --
    # SaveData.load_new_game_values pode nao os inicializar (sem save file).
    # Instanciar antes que Scene_Map comece a chamar .bridge/.surfing etc.
    begin
      if $PokemonGlobal.nil?
        klass = (Object.const_defined?(:PokemonGlobalMetadata) ? PokemonGlobalMetadata :
                 Object.const_defined?(:PokemonGlobal)         ? PokemonGlobal         : nil)
        if klass
          begin
            $PokemonGlobal = klass.new
            dbg "[DebugIntro] $PokemonGlobal = #{klass}.new OK"
          rescue => e2
            # initialize chama pbLoadRegionalDexes que pode falhar -- usar stub
            dbg "[DebugIntro] #{klass}.new falhou (#{e2.message}) -- usando stub"
            $PokemonGlobal = klass.allocate rescue nil
          end
        end
      end
    rescue => e
      dbg "[DebugIntro] $PokemonGlobal.new falhou: #{e.class}: #{e.message}"
    end
    begin
      if $PokemonTemp.nil? && Object.const_defined?(:PokemonTemp)
        $PokemonTemp = PokemonTemp.new
        dbg "[DebugIntro] $PokemonTemp = PokemonTemp.new OK"
      end
    rescue => e
      dbg "[DebugIntro] $PokemonTemp.new falhou: #{e.class}: #{e.message}"
    end
    # Garantir campos criticos mesmo que .new falhe ou seja stub
    begin
      if $PokemonGlobal
        $PokemonGlobal.bridge  = 0     rescue nil
        $PokemonGlobal.surfing = false rescue nil
        $PokemonGlobal.visitedMaps ||= {} rescue nil
        $PokemonGlobal.mapTrail    ||= [] rescue nil
        $PokemonGlobal.stepcount   ||= 0  rescue nil
        $PokemonGlobal.repel       ||= 0  rescue nil
        begin
          de = $PokemonGlobal.dependentEvents rescue nil
          if de.nil? || de.is_a?(Integer) || !de.respond_to?(:updateDependentEvents)
            $PokemonGlobal.dependentEvents = (Object.const_defined?(:DependentEvents) ? DependentEvents.new : []) rescue []
          end
        rescue; end
      end
    rescue => e
      dbg "[DebugIntro] PokemonGlobal fields falhou: #{e.message}"
    end
    begin
      if $PokemonTemp
        $PokemonTemp.encounterTriggered     = false rescue nil
        $PokemonTemp.hiddenMoveEventCalling = false rescue nil
        $PokemonTemp.keyItemCalling         = false rescue nil
        $PokemonTemp.miniupdate             = false rescue nil
        begin
          de = $PokemonTemp.dependentEvents rescue nil
          if de.nil? || de.is_a?(Integer) || !de.respond_to?(:updateDependentEvents)
            $PokemonTemp.dependentEvents = (Object.const_defined?(:DependentEvents) ? DependentEvents.new : []) rescue []
          end
        rescue; end
      end
    rescue => e
      dbg "[DebugIntro] PokemonTemp fields falhou: #{e.message}"
    end

    # --- $MapFactory fallback stub (so actua se Game.start_new falhou) ---
    begin
      unless $MapFactory
        _obj = Object.new
        _obj.define_singleton_method(:maps)              { [] }
        _obj.define_singleton_method(:map)               { $game_map }
        _obj.define_singleton_method(:map_id)            { 0 }
        _obj.define_singleton_method(:setSceneStarted)   { |s| }
        _obj.define_singleton_method(:updateMinimap)     { }
        _obj.define_singleton_method(:updateMap)         { }
        _obj.define_singleton_method(:updateMaps)        { |scene| }
        _obj.define_singleton_method(:isPassableStrict?) { |*a| false }
        _obj.define_singleton_method(:setup)             { |*a| }
        _obj.define_singleton_method(:hasMap?)           { |*a| false }
        _obj.define_singleton_method(:getMap)            { |*a| $game_map }
        $MapFactory = _obj
      end
    rescue => e
      dbg "[DebugIntro] MapFactory stub failed: #{e.message}"
    end

    # --- $game_map fallback stub ---
    begin
      unless $game_map
        _obj = Object.new
        _obj.define_singleton_method(:map_id)         { 0 }
        _obj.define_singleton_method(:name)           { "" }
        _obj.define_singleton_method(:update)         { }
        _obj.define_singleton_method(:autoplay)       { }
        _obj.define_singleton_method(:events)         { {} }
        _obj.define_singleton_method(:tileset_name)   { "" }
        _obj.define_singleton_method(:terrain_tag)    { |*a| 0 }
        _obj.define_singleton_method(:bridge)         { 0 }
        _obj.define_singleton_method(:passable?)      { |*a| true }
        _obj.define_singleton_method(:valid?)         { |*a| true }
        $game_map = _obj
      end
    rescue => e
      dbg "[DebugIntro] game_map stub failed: #{e.message}"
    end

    # --- $game_player fallback stub ---
    # NOTA: usar $game_player.nil? em vez de `unless $game_player` -- em Ruby
    # ambos sao equivalentes para nil, mas .nil? e mais explicito e seguro
    # quando o valor pode ter sido substituido por um stub anterior.
    # Tambem tentamos criar Game_Player real primeiro (SaveData pode ter falhado).
begin
      if $game_player.nil? || !$game_player.respond_to?(:update)
        # Tentar criar instancia real antes de usar stub
        begin
          $game_player = Game_Player.new
          begin
            sx = $data_system.respond_to?(:start_x) ? $data_system.start_x.to_i : 0
          rescue
            sx = 0
          end
          begin
            sy = $data_system.respond_to?(:start_y) ? $data_system.start_y.to_i : 0
          rescue
            sy = 0
          end
          $game_player.moveto(sx, sy) rescue nil
          dbg "[DebugIntro] game_player: Game_Player.new OK"
        rescue => gpe
          dbg "[DebugIntro] game_player: Game_Player.new falhou (#{gpe.message}), usando stub"
          _obj = Object.new
          _bicycle = false
          _movement_type = :walking
          _obj.define_singleton_method(:character_name)     { "" }
          _obj.define_singleton_method(:character_hue)      { 0 }
          _obj.define_singleton_method(:direction)          { 2 }
          _obj.define_singleton_method(:x)                  { 0 }
          _obj.define_singleton_method(:y)                  { 0 }
          _obj.define_singleton_method(:real_x)             { 0 }
          _obj.define_singleton_method(:real_y)             { 0 }
          _obj.define_singleton_method(:pattern)            { 0 }
          _obj.define_singleton_method(:opacity)            { 255 }
          _obj.define_singleton_method(:blend_type)         { 0 }
          _obj.define_singleton_method(:bush_depth)         { 0 }
          _obj.define_singleton_method(:transparent)        { false }
          _obj.define_singleton_method(:move_speed)         { 4 }
          _obj.define_singleton_method(:through)            { false }
          _obj.define_singleton_method(:update)             { }
          _obj.define_singleton_method(:move_route_forcing) { false }
          _obj.define_singleton_method(:surfing)            { false }
          _obj.define_singleton_method(:diving)             { false }
          _obj.define_singleton_method(:riding)             { false }
          _obj.define_singleton_method(:bob_height)         { 0 }
          _obj.define_singleton_method(:shadow_filename)    { nil }
          _obj.define_singleton_method(:shadow_size)        { 0 }
          _obj.define_singleton_method(:follower)           { nil }
          _obj.define_singleton_method(:moving?)            { false }
          _obj.define_singleton_method(:tile_id)            { 0 }
          _obj.define_singleton_method(:screen_x)           { 0 }
          _obj.define_singleton_method(:screen_y)           { 0 }
          _obj.define_singleton_method(:screen_z)           { |*_a| 0 }
          _obj.define_singleton_method(:animation_id)       { 0 }
          _obj.define_singleton_method(:animation_id=)      { |v| }
          _obj.define_singleton_method(:sprite_size)        { [32, 32] }
          _obj.define_singleton_method(:sprite_size=)       { |v| }
          _obj.define_singleton_method(:refresh)            { }
          _obj.define_singleton_method(:straighten)         { }
          _obj.define_singleton_method(:moveto)             { |*_a| }
          # FIX: bicycle / bicycle= -- chamados em Overworld_Metadata / Game_Player
          # durante Scene_Map#main. Sem estes, NilClass#bicycle aparece no log
          # significando que $game_player voltou a nil durante o update loop.
          _obj.define_singleton_method(:bicycle)            { _bicycle }
          _obj.define_singleton_method(:bicycle=)           { |v| _bicycle = v ? true : false }
          # FIX: set_movement_type -- chamado em Game_Player#update / pbOnBike etc.
          _obj.define_singleton_method(:set_movement_type)  { |*_a| }
          # FIX: weather_duration / tone / flash_color -- Game_Screen/Map acede
          # a estes atraves do player em alguns contextos do Essentials PE19
          _obj.define_singleton_method(:weather_duration)   { 0 }
          _obj.define_singleton_method(:tone)               { nil }
          _obj.define_singleton_method(:flash_color)        { nil }
          $game_player = _obj
        end
      end
    rescue => e
      dbg "[DebugIntro] game_player stub failed: #{e.message}"
    end

    # --- $game_screen fallback stub ---
    begin
      unless $game_screen
        _obj = Object.new
        _obj.define_singleton_method(:update)         { }
        _obj.define_singleton_method(:weather_type)   { 0 }
        _obj.define_singleton_method(:weather_max)    { 0 }
        _obj.define_singleton_method(:shake)          { 0 }
        _obj.define_singleton_method(:pictures)       {
          @__picstore ||= (MKXPPictureStore.new rescue {})
        }
        $game_screen = _obj
      end
    rescue => e
      dbg "[DebugIntro] game_screen stub failed: #{e.message}"
    end

    # --- $game_system: criar Game_System REAL (tem map_interpreter = Interpreter.new) ---
    # CRITICO: sem um map_interpreter REAL, pbMapInterpreter.update e' no-op (engolido
    # pelo method_missing universal) e os eventos autorun -- intro / criacao de
    # personagem -- NUNCA correm, deixando o ecra preto no mapa de intro (Map001).
    # Por isso preferimos sempre Game_System.new; so' caimos no stub se falhar, e
    # mesmo o stub passa a ter um Interpreter REAL.
    begin
      _mi_ok = begin
        $game_system && $game_system.respond_to?(:map_interpreter) &&
        $game_system.map_interpreter && $game_system.map_interpreter.respond_to?(:update)
      rescue
        false
      end
      unless _mi_ok
        begin
          $game_system = Game_System.new
          dbg "[DebugIntro] $game_system = Game_System.new OK map_interpreter=#{($game_system.map_interpreter.class rescue '?')}"
        rescue => gse
          dbg "[DebugIntro] Game_System.new falhou (#{gse.class}: #{gse.message}) -- stub com Interpreter real"
          _mi = (Interpreter.new(0, true) rescue nil)
          _obj = Object.new
          _obj.define_singleton_method(:bgm_play)          { |*a| }
          _obj.define_singleton_method(:bgs_play)          { |*a| }
          _obj.define_singleton_method(:update)            { }
          _obj.define_singleton_method(:map_interpreter)   { _mi }
          _obj.define_singleton_method(:menu_disabled)     { false }
          _obj.define_singleton_method(:save_disabled)     { false }
          _obj.define_singleton_method(:encounter_disabled){ false }
          $game_system = _obj
        end
      end
    rescue => e
      dbg "[DebugIntro] game_system setup falhou: #{e.message}"
    end

    # Sinalizar "novo jogo" (Game.start_new faz isto) -- a intro pode depender disto.
    begin
      $PokemonTemp.begunNewGame = true if $PokemonTemp.respond_to?(:begunNewGame=)
    rescue
    end

    # --- $game_switches / $game_variables / $game_self_switches ---
    # CRITICO: o SaveData.load_new_game_values falhou (ver $game_player=NilClass),
    # por isso estes globais ficam nil. Sem eles, "Controlar Variaveis" (cmd 122) e
    # pbSetSelfSwitch nao tem onde escrever -> o evento de arranque (QuickStartActivate)
    # nunca completa o contador de 5 frames e NUNCA aciona o Evento 1 'Intro'
    # (criacao de personagem). Criamos os 3 globais reais aqui.
    begin
      if $game_switches.nil? || !$game_switches.respond_to?(:[])
        $game_switches = Game_Switches.new
      end
      if $game_variables.nil? || !$game_variables.respond_to?(:[])
        $game_variables = Game_Variables.new
      end
      if $game_self_switches.nil? || !$game_self_switches.respond_to?(:[])
        $game_self_switches = Game_SelfSwitches.new
      end
      dbg "[DebugIntro] switches=#{$game_switches.class} variables=#{$game_variables.class} self_switches=#{$game_self_switches.class}"
    rescue => e
      dbg "[DebugIntro] game_switches/variables setup falhou: #{e.class}: #{e.message}"
    end

    # --- $game_temp: criar Game_Temp real ou stub completo ---
    # CRITICO: $game_temp DEVE ter to_title=false. Se method_missing devolver
    # 0 (truthy), Scene_Map#update faz $scene=pbCallTitle a cada frame -> loop infinito.
    begin
      # Tentar sempre criar Game_Temp real (pode ja existir mas verificar to_title)
      if !$game_temp || !$game_temp.respond_to?(:to_title)
        begin
          $game_temp = Game_Temp.new
          dbg "[DebugIntro] game_temp: Game_Temp.new OK"
        rescue => gte
          dbg "[DebugIntro] game_temp: Game_Temp.new falhou (#{gte.message}), usando stub"
          _obj = Object.new
          [
            [:common_event_id,         0     ],
            [:message_window_showing,  false ],
            [:to_title,                false ],
            [:player_transferring,     false ],
            [:player_new_map_id,       0     ],
            [:player_new_x,            0     ],
            [:player_new_y,            0     ],
            [:player_new_direction,    0     ],
            [:transition_processing,   false ],
            [:transition_name,         ""    ],
            [:menu_calling,            false ],
            [:debug_calling,           false ],
            [:in_menu,                 false ],
            [:in_battle,               false ],
          ].each do |mname, default|
            val = default
            _obj.define_singleton_method(mname) { val }
            setter = (mname.to_s + "=").to_sym
            _obj.define_singleton_method(setter) { |v| val = v }
          end
          $game_temp = _obj
        end
      end
      # Garantia extra: to_title deve ser false
      if $game_temp.respond_to?(:to_title) && $game_temp.to_title
        $game_temp.to_title = false rescue nil
      end
    rescue => e
      dbg "[DebugIntro] game_temp stub failed: #{e.message}"
    end

    # Criar Scene_Map com protecao -- so se Game.start_new nao o fez ja
    if $scene.nil? || !$scene.is_a?(Scene_Map)
      begin
        $scene = Scene_Map.new
        dbg "[DebugIntro] Scene_Map.new OK"
      rescue => scene_new_err
        _log_error_once("Scene_Map.new:#{scene_new_err.class}",
          "[DebugIntro] Scene_Map.new CRASH: #{scene_new_err.class}: #{scene_new_err.message}")
        begin
          bt = scene_new_err.backtrace
          if bt && bt.is_a?(Array)
            bt[0, 12].each_with_index { |l, i| dbg "[DebugIntro]  bt[#{i}]: #{l}" }
          end
        rescue; end
        $scene = nil
      end
    else
      dbg "[DebugIntro] $scene ja e #{$scene.class} (de Game.start_new), reutilizando"
    end
    dbg "[DebugIntro] success"
  rescue => e
    dbg "[DebugIntro] failed: #{e.class}: #{e.message}"
    begin
      bt = e.backtrace
      if bt && bt.is_a?(Array)
        bt[0, 12].each_with_index { |l, i| dbg "[DebugIntro]  bt[#{i}]: #{l}" }
      end
    rescue; end
    $scene = nil
  end
end

# =============================================================================
# PATCH CRÍTICO: RPG::Cache + BitmapWrapper + @map.events + @@viewports
# =============================================================================

# 1. Inicializar @cache do RPG::Cache (ivar de módulo nunca inicializado)
#    Sem isto: @cache[key] -> NoMethodError nil[] -> engolido -> bitmap=nil
begin
  if Object.const_defined?(:RPG) && RPG.const_defined?(:Cache)
    RPG::Cache.instance_variable_set(:@cache, {}) rescue nil
    dbg "[PATCH] RPG::Cache @cache inicializado OK"
  end
rescue => e
  dbg "[PATCH] RPG::Cache @cache init falhou: #{e.message}"
end

# 2. BitmapWrapper: subclasse de Bitmap usada pelo RPG::Cache.load_bitmap.
#    Se BitmapWrapper não existir ou não tiver os métodos de ref-counting,
#    load_bitmap crasha e retorna nil.
# 2. BitmapWrapper: subclasse de Bitmap usada pelo RPG::Cache.load_bitmap.
#    Se BitmapWrapper não existir ou não tiver os métodos de ref-counting,
#    load_bitmap crasha e retorna nil.
#    FIX CRÍTICO: NUNCA fazer remove_const + recriar -- isso destroi o
#    MRB_TT_DATA que o binding C++ setou, fazendo DATA_PTR devolver nullptr
#    em spr_set_bitmap -> s->bitmap=nil -> g_sprites sem bitmaps -> ecrã preto.
#    Apenas reabrir a classe (ou criar se não existir) e adicionar métodos.
# FIX CRITICO: usar class_eval para adicionar metodos ao BitmapWrapper
# SEM usar "class BitmapWrapper < Bitmap" que pode interferir com MRB_TT_DATA.
begin
  if respond_to?(:__force_bitmapwrapper_tt__, true)
    __force_bitmapwrapper_tt__
    dbg "[PATCH] __force_bitmapwrapper_tt__ OK"
  end
  if respond_to?(:__rebind_bitmapwrapper_init__, true)
    __rebind_bitmapwrapper_init__
    dbg "[PATCH] __rebind_bitmapwrapper_init__ OK (pre-eval)"
  end

  if Object.const_defined?(:BitmapWrapper)
    BitmapWrapper.class_eval do
      unless method_defined?(:refcount)
        def refcount; @refcount; end
      end
      unless method_defined?(:never_dispose)
        def never_dispose; @never_dispose; end
        def never_dispose=(v); @never_dispose = v; end
      end
      unless method_defined?(:dispose)
        def dispose
          return if disposed? rescue false
          @refcount = (@refcount || 1) - 1
          super if @refcount <= 0 && !@never_dispose
        end
      end
      unless method_defined?(:addRef)
        def addRef; @refcount = (@refcount || 0) + 1; end
      end
      unless method_defined?(:resetRef)
        def resetRef; @refcount = 1; end
      end
      unless method_defined?(:copy)
        def copy
          bm = nil
          begin
            bm = BitmapWrapper.new(width, height)
            bm.blt(0, 0, self, self.rect) rescue nil
          rescue
            bm = BitmapWrapper.new(1, 1) rescue nil
          end
          bm.resetRef rescue nil
          bm
        end
      end
    end
    dbg "[PATCH] BitmapWrapper class_eval OK"
    if respond_to?(:__rebind_bitmapwrapper_init__, true)
      __rebind_bitmapwrapper_init__
      dbg "[PATCH] __rebind_bitmapwrapper_init__ OK (post-eval)"
    end
  else
    dbg "[PATCH] AVISO: BitmapWrapper nao encontrado"
  end
rescue => e
  dbg "[PATCH] BitmapWrapper patch falhou: #{e.class}: #{e.message}"
end

# 3. pbGetTileset / pbGetAutotile -- chamados por Spriteset_Map#initialize.
#    IMPORTANTE: estes stubs correm DEPOIS dos 376 scripts do jogo, por isso
#    o unless respond_to? retornava true (jogo ja definia a funcao) e os nossos
#    stubs seguros nunca eram registados. A versao do jogo usa "..." + name sem
#    .to_s, o que rebenta em mruby quando name e Integer.
#    Solucao: forcar sempre a nossa versao segura, independentemente do jogo.
begin
  def pbGetTileset(name)
    name = name.to_s rescue ""
    if name.nil? || name == ""
      dbg "[BMP] pbGetTileset: NOME VAZIO -> fallback 32x32 (mapa fica preto)"
      dbg "[BMP]   Causa provavel: $data_tilesets sem dados reais."
      dbg "[BMP]   Verifica se Data/Tilesets.rxdata existe em game/Data/ no cartao."
      return BitmapWrapper.new(32, 32)
    end
    dbg "[BMP] pbGetTileset: tentando 'Graphics/Tilesets/#{name}'"
    begin
      bm = BitmapWrapper.new("Graphics/Tilesets/#{name}")
    rescue => e
      dbg "[BMP] pbGetTileset ERRO: #{e.class}: #{e.message}"
      bm = BitmapWrapper.new(32, 32)
    end
    dbg "[BMP] pbGetTileset('#{name}') -> #{bm.width rescue '?'}x#{bm.height rescue '?'}"
    bm
  end
  module Kernel
    def pbGetTileset(name)
      name = name.to_s rescue ""
      return BitmapWrapper.new(32, 32) if name.nil? || name == ""
      begin
        BitmapWrapper.new("Graphics/Tilesets/#{name}")
      rescue
        BitmapWrapper.new(32, 32)
      end
    end
    module_function :pbGetTileset
  end
  def pbGetAutotile(name)
    name = name.to_s rescue ""
    if name.nil? || name == ""
      dbg "[BMP] pbGetAutotile: nome vazio -> fallback 96x128"
      return BitmapWrapper.new(96, 128)
    end
    dbg "[BMP] pbGetAutotile: tentando 'Graphics/Autotiles/#{name}'"
    begin
      bm = BitmapWrapper.new("Graphics/Autotiles/#{name}")
    rescue => e
      dbg "[BMP] pbGetAutotile ERRO: #{e.class}: #{e.message}"
      bm = BitmapWrapper.new(96, 128)
    end
    dbg "[BMP] pbGetAutotile('#{name}') -> #{bm.width rescue '?'}x#{bm.height rescue '?'}"
    bm
  end
  module Kernel
    def pbGetAutotile(name)
      name = name.to_s rescue ""
      return BitmapWrapper.new(96, 128) if name.nil? || name == ""
      begin
        BitmapWrapper.new("Graphics/Autotiles/#{name}")
      rescue
        BitmapWrapper.new(96, 128)
      end
    end
    module_function :pbGetAutotile
  end
  dbg "[PATCH] pbGetTileset/pbGetAutotile OK (forcado)"
rescue => e
  dbg "[PATCH] pbGetTileset/pbGetAutotile falhou: #{e.class}: #{e.message}"
end

# 4. $game_map.events vazio -> character_sprites=0 -> nada a desenhar para o player.
#    O $game_player precisa de um Sprite_Character.
#    Injectar um evento sintético para o player se events estiver vazio.
begin
  if $game_map && ($game_map.events.nil? || $game_map.events.empty? rescue true)
    if $game_player && Object.const_defined?(:Game_Event)
      # Criar um evento stub com os dados do player para que Sprite_Character o siga
      _ev_stub = Object.new
      _ev_stub.define_singleton_method(:x)              { $game_player.x rescue 0 }
      _ev_stub.define_singleton_method(:y)              { $game_player.y rescue 0 }
      _ev_stub.define_singleton_method(:real_x)         { $game_player.real_x rescue 0 }
      _ev_stub.define_singleton_method(:real_y)         { $game_player.real_y rescue 0 }
      _ev_stub.define_singleton_method(:direction)      { $game_player.direction rescue 2 }
      _ev_stub.define_singleton_method(:move_speed)     { $game_player.move_speed rescue 4 }
      _ev_stub.define_singleton_method(:step_anime)     { true }
      _ev_stub.define_singleton_method(:walk_anime)     { true }
      _ev_stub.define_singleton_method(:character_name) { $game_player.character_name rescue "" }
      _ev_stub.define_singleton_method(:character_hue)  { $game_player.character_hue rescue 0 }
      _ev_stub.define_singleton_method(:opacity)        { 255 }
      _ev_stub.define_singleton_method(:blend_type)     { 0 }
      _ev_stub.define_singleton_method(:pattern)        { $game_player.pattern rescue 0 }
      _ev_stub.define_singleton_method(:transparent)    { false }
      _ev_stub.define_singleton_method(:through)        { false }
      _ev_stub.define_singleton_method(:tile_id)        { 0 }
      _ev_stub.define_singleton_method(:bush_depth)     { 0 }
      _ev_stub.define_singleton_method(:id)             { -1 }
      begin
        evs = $game_map.events
        if evs.respond_to?(:[]=)
          evs[-1] = _ev_stub
          dbg "[PATCH] events[-1] = player stub injectado (events estava vazio)"
        end
      rescue => _e
        dbg "[PATCH] events inject falhou: #{_e.message}"
      end
    end
  else
    _ev_count = ($game_map.events.size rescue '?')
    dbg "[PATCH] $game_map.events=#{_ev_count} (nao vazio, OK)"
  end
rescue => e
  dbg "[PATCH] events stub inject falhou: #{e.class}: #{e.message}"
end

# 5. Spriteset_Map usa @@viewport0/1/3 criados no corpo da classe.
#    Em mruby, class variables de módulo/classe são partilhadas correctamente,
#    mas se os Viewports forem criados antes de display_3ds_init(), ficam inválidos.
#    Re-criar os viewports agora que display está inicializado.
begin
  if Object.const_defined?(:Spriteset_Map)
    _w = Graphics.width  rescue 400
    _h = Graphics.height rescue 240
    Spriteset_Map.class_eval do
      begin
        @@viewport0 = Viewport.new(0, 0, _w, _h)
        @@viewport0.z = -100
      rescue; end
      begin
        @@viewport1 = Viewport.new(0, 0, _w, _h)
        @@viewport1.z = 0
      rescue; end
      begin
        @@viewport3 = Viewport.new(0, 0, _w, _h)
        @@viewport3.z = 500
      rescue; end
    end rescue nil
    dbg "[PATCH] Spriteset_Map @@viewports reinicializados (#{_w}x#{_h})"
  end
rescue => e
  dbg "[PATCH] Spriteset_Map viewports falhou: #{e.class}: #{e.message}"
end

# -- Wrapper de diagnostico para mainFunctionDebug -----------------------------
begin
  alias __mFD_orig mainFunctionDebug
  def mainFunctionDebug
    step = 0
    begin
      step = 1
      dbg "[MFD] step1: MessageTypes"
      MessageTypes.loadMessageFile("Data/messages.dat") if safeExists?("Data/messages.dat")
      step = 2
      dbg "[MFD] step2: PluginManager"
      PluginManager.runPlugins
      step = 3
      dbg "[MFD] step3: Compiler"
      Compiler.main
      step = 4
      dbg "[MFD] step4: Game.initialize"
      Game.initialize
      step = 5
      dbg "[MFD] step5: Game.set_up_system"
      Game.set_up_system
      # Repor $PokemonSystem depois de Game.set_up_system (pode ter criado instancia nova)
      begin
        $PokemonSystem = PokemonSystem.new unless $PokemonSystem
      rescue; end
      step = 6
      dbg "[MFD] step6: Graphics.update"

		dbg "[CHK] Graphics.class=#{Graphics.class}"
		dbg "[CHK] Graphics.update owner=#{Graphics.method(:update).owner rescue 'ERR'}"
		dbg "[CHK] Graphics.freeze owner=#{Graphics.method(:freeze).owner rescue 'ERR'}"

		dbg "[CHK] calling Graphics.update"
		Graphics.update
		dbg "[CHK] returned from Graphics.update"

		dbg "[MFD] step7: Graphics.freeze"
		Graphics.freeze
      step = 8
      dbg "[MFD] step8: pbCallTitle"
      $scene = pbCallTitle
      dbg "[MFD] step8 done: $scene=#{$scene.nil? ? 'nil' : $scene.class}"
      step = 9
      if $scene.nil?
        dbg "[MFD] WARNING: $scene nil before loop"
      else
        dbg "[MFD] entering scene loop: #{$scene.class}"
        scene_crashes = 0
        max_scene_crashes = 30   # variavel local -- constante dentro de def e SyntaxError em mruby
        scene_iterations = 0
        max_scene_iterations = 200  # limite generoso -- o ciclo e agora quebrado pela logica de $__debug_intro_count
        last_two_scenes = []

        loop do
          break if $scene.nil?
          break if scene_crashes >= max_scene_crashes
          break if scene_iterations >= max_scene_iterations

          scene_iterations += 1

          # Detectar ciclo entre 2 cenas (ex: Scene_Map <-> Scene_DebugIntro)
          klass = $scene.class.to_s
          last_two_scenes.push(klass)
          last_two_scenes.shift if last_two_scenes.length > 4
          if last_two_scenes.length == 4 &&
             last_two_scenes[0] == last_two_scenes[2] &&
             last_two_scenes[1] == last_two_scenes[3]
            dbg "[MFD] CICLO DETECTADO: #{last_two_scenes.join(' -> ')} -- abortando loop"
            $scene = nil
            break
          end
          dbg "[MFD] scene.main start: #{klass}"
          Graphics.transition(0) rescue nil

          begin
            $scene.main
          rescue => scene_err
            scene_crashes += 1
            err_key = "SCENE:#{klass}:#{scene_err.class}:#{scene_err.message[0,60]}"
            _log_error_once(err_key,
              "[SCENE-CRASH ##{scene_crashes}] #{klass}: #{scene_err.class}: #{scene_err.message}")
            begin
              bt = scene_err.backtrace
              if bt && bt.is_a?(Array)
                bt[0, 12].each_with_index { |l, i| dbg "[MFD]  bt[#{i}]: #{l}" }
              end
            rescue; end
            # Nao parar -- tentar avancar para a proxima scene se possivel
            $scene = nil
          end

          dbg "[MFD] scene.main done: #{klass} -> $scene=#{$scene.nil? ? 'nil' : $scene.class}"
        end

        if scene_crashes >= max_scene_crashes
          dbg "[MFD] AVISO: atingido limite de #{max_scene_crashes} crashes de scene"
        end
        dbg "[MFD] scene loop finished (#{scene_crashes} crashes)"
        _dump_error_summary
      end
      step = 10
      dbg "[MFD] step10: Graphics.transition"
      Graphics.transition(20)
      dbg "[MFD] done, returning 1"
      return 1
    rescue Hangup => e
      dbg "[MFD] Hangup at step #{step}: #{e.message}"
      pbPrintException(e) rescue nil
      pbEmergencySave rescue nil
      raise
    rescue => e
      dbg "[MFD] CRASH at step #{step}: #{e.class}: #{e.message}"
      raise RuntimeError, "[STEP #{step}] #{e.class}: #{e.message}"
    end
  end
rescue NameError => e
  dbg "[MFD] alias failed: #{e.message}"
end
)RUBY");
	
	DBGLOG("[TEST] depois do mrb_load_string\n");

	if (mrb->exc) {
		DBGLOG("[TEST] mrb->exc existe\n");

		// 1. Extrair a mensagem da exceção
		mrb_value exc_obj = mrb_obj_value(mrb->exc);
		mrb_value msg_val = mrb_funcall(mrb, exc_obj, "message", 0);
		const char *msg_str = mrb_string_p(msg_val) ? RSTRING_PTR(msg_val) : "sem mensagem";
		DBGLOG("[stubs] EXCEPTION: %s\n", msg_str);

		// 2. Extrair e imprimir o backtrace (as primeiras 8 linhas)
		mrb_value bt_val = mrb_funcall(mrb, exc_obj, "backtrace", 0);
		if (mrb_array_p(bt_val)) {
			int bt_len = RARRAY_LEN(bt_val);
			DBGLOG("[stubs] Backtrace:\n");
			for (int i = 0; i < bt_len && i < 8; i++) {
				mrb_value bt_line = mrb_ary_entry(bt_val, i);
				if (mrb_string_p(bt_line)) {
					DBGLOG("[stubs]   [%d] %s\n", i, RSTRING_PTR(bt_line));
				}
			}
		} else {
			DBGLOG("[stubs] (sem backtrace)\n");
		}

		// 3. Chamar mrb_print_error para o log nativo do mruby (opcional)
		mrb_print_error(mrb);
		DBGLOG("[TEST] mrb_print_error terminou\n");

		// 4. Mostrar erro genérico e limpar
		DBGLOG("[stubs] ERRO ao carregar stubs pre-main!\n");
		show_error(mrb, "pre_main_stubs");
		mrb->exc = 0;
	} else {
		DBGLOG("[stubs] stubs pre-main carregados OK\n");
}

    mrb_sym sym_mF  = mrb_intern_cstr(mrb, "mainFunction");
    mrb_sym sym_mFD = mrb_intern_cstr(mrb, "mainFunctionDebug");
    mrb_value top   = mrb_top_self(mrb);

    bool has_mFD = mrb_respond_to(mrb, top, sym_mFD);
    bool has_mF  = mrb_respond_to(mrb, top, sym_mF);

    DBGLOG("[binding] respond_to: mainFunction=%s mainFunctionDebug=%s\n",
           has_mF ? "yes" : "no", has_mFD ? "yes" : "no");

    if (!has_mF && !has_mFD) {
        DBGLOG("[binding] ERROR: neither mainFunction nor mainFunctionDebug defined\n");
        return;
    }

    mrb_sym entry_sym      = has_mFD ? sym_mFD : sym_mF;
    const char *entry_name = has_mFD ? "mainFunctionDebug" : "mainFunction";

    /* ---------------------------------------------------------------
     * FIX2: reaplicar MRB_TT_DATA + rebind bmp_init antes do loop.
     * check_entry_methods corre patches Ruby que podem ter reaberto
     * BitmapWrapper. bitmapBindingReinit corrige tudo de uma vez.
     * --------------------------------------------------------------- */
    bitmapBindingReinit(mrb);
    DBGLOG("[FIX2] bitmapBindingReinit chamado antes do loop principal OK\n");

    /* Boot terminado: configurar o GC para um HEAP GRANDE (Pokemon Essentials).
     * --------------------------------------------------------------------------
     * DESCOBERTA (analise dos logs): o gargalo dos 220s do CommonEvents e dos
     * 4 FPS NAO e' o malloc nem o parser do marshal -- e' o GARBAGE COLLECTOR a
     * correr vezes sem conta durante a criacao de objetos.
     *
     * Cada mrb_obj_alloc do mruby corre o GC quando 'live > threshold'. A forma
     * como o 'threshold' e' recalculado depende do MODO do GC:
     *   - GERACIONAL  -> threshold = live + 1024 (FIXO!). Ignora interval_ratio.
     *                    Resultado: GC corre a cada 1024 alocacoes, SEMPRE. Com
     *                    o heap grande do Pokemon, isto e' GC-thrashing puro.
     *                    (Foi o erro da tentativa anterior: ligar geracional
     *                    PIOROU porque forcou este threshold fixo baixo.)
     *   - NAO-geracional -> threshold = (live_after_mark/100) * interval_ratio.
     *                    Com interval_ratio MUITO alto, o threshold fica enorme
     *                    -> o GC quase nao corre durante cargas em massa (marshal
     *                    de CommonEvents, mapas, etc.) -> sem thrashing.
     *
     * CORRECAO: modo NAO-geracional + interval_ratio muito alto (mantido durante
     * todo o jogo, nao so' no boot). Trocamos memoria (que sobra nos ~128MB do
     * 3DS) por velocidade. NAO desligamos o GC por completo (mrb_gc_disable)
     * porque isso fazia o arena de objetos novos transbordar no 3DS; com o GC
     * apenas "preguicoso" (threshold alto) o arena continua a ser gerido
     * normalmente pelo read_value (arena_save/restore). */
    {
        int before = (int)mrb->gc.live;
        mrb_full_gc(mrb);                       /* 1 limpeza enquanto o heap e' pequeno */
        /* GC GERACIONAL reativado (era desligado como remendo para a lentidao do
         * boot -- mas essa lentidao eram os SIMBOLOS O(N^2), ja' corrigidos no
         * mruby). O modo geracional e' o normal do mruby e e' o ideal para o
         * jogo: faz GC menor (barato) frequente que limpa o lixo temporario
         * (~750 objs/frame no mapa) SEM varrer o heap todo, evitando o OOM que
         * antes enchia a memoria no mapa ([POOL|GROW] FALHOU). */
        mrb->gc.generational  = TRUE;
        mrb->gc.interval_ratio = 200;           /* default mruby (so' usado no modo nao-geracional) */
        DBGLOG("[GC] boot concluido: GERACIONAL reativado (default) "
               "(full_gc %d -> %d objs vivos) -- evita OOM no mapa\n",
               before, (int)mrb->gc.live);
    }

    DBGLOG("[binding] calling %s loop\n", entry_name);

    /* DESLIGAR o diagnostico do method_missing antes do gameplay. A contagem
     * ($__mm_counts) fazia um hash-write por cada metodo-em-falta por frame;
     * no mapa ha' centenas/frame (ex: Integer#origin chamado milhares de vezes)
     * -> fatia real do FPS. Aqui despejamos o TOP final (mantem a visibilidade
     * do que falta) e DESLIGAMOS so' a contagem. O cache de retorno rapido
     * ($__mm_retcache) e o log 1x/metodo ($__mm_seen, ja' feito no boot) ficam
     * intactos -> os metodos-em-falta passam a devolver o valor seguro DIRETO,
     * sem strings nem hash-writes. NAO remove logging: o [MISSING-TOP] sai aqui
     * uma ultima vez. */
    {
        static const char *mm_off =
            "begin\n"
            "  __dump_missing_methods__ rescue nil\n"
            "  if $__mm_counts\n"
            "    MKXPDebug.log(\"[MISSING] diagnostico desligado p/ gameplay (cache mantido)\") rescue nil\n"
            "    $__mm_counts = nil\n"   /* desliga so' a contagem; retcache continua */
            "    $__mm_total  = nil\n"
            "  end\n"
            "rescue => e\n"
            "  (MKXPDebug.log(\"[MISSING] falha ao desligar diagnostico: #{e.message}\") rescue nil)\n"
            "end\n";
        mrb_load_string(mrb, mm_off);
        if (mrb->exc) mrb->exc = 0;
    }

    /* Diagnóstico Ruby: verificar arity do initialize de BitmapWrapper
     * antes de entrar no loop. Deve ser -1 (C binding) ou 2 (req=1, opt=1).
     * Se for 1, ainda há um initialize Ruby de argumento único activo. */
    {
        static const char *bw_diag =
            "begin\n"
            "  if Object.const_defined?(:BitmapWrapper)\n"
            "    m = BitmapWrapper.instance_method(:initialize) rescue nil\n"
            "    if m\n"
            "      ar = m.arity rescue '?'\n"
            "      owner = m.owner rescue '?'\n"
            "      dbg \"[BW-DIAG] BitmapWrapper#initialize arity=#{ar} owner=#{owner}\"\n"
            "    else\n"
            "      dbg '[BW-DIAG] BitmapWrapper#initialize nao encontrado'\n"
            "    end\n"
            "    # Teste real: BitmapWrapper.new(32,32) deve funcionar\n"
            "    begin\n"
            "      _t = BitmapWrapper.new(32, 32)\n"
            "      dbg \"[BW-DIAG] BitmapWrapper.new(32,32) OK class=#{_t.class} ptr=#{_t.respond_to?(:width) ? _t.width : '?'}\"\n"
            "    rescue => _bw_e\n"
            "      dbg \"[BW-DIAG] BitmapWrapper.new(32,32) FALHOU: #{_bw_e.class}: #{_bw_e.message}\"\n"
            "    end\n"
            "  else\n"
            "    dbg '[BW-DIAG] BitmapWrapper nao existe'\n"
            "  end\n"
            "rescue => e\n"
            "  dbg \"[BW-DIAG] diagnostico falhou: #{e.message}\"\n"
            "end\n";
        mrb_load_string(mrb, bw_diag);
        if (mrb->exc) { mrb->exc = 0; }
    }

    /* ===========================================================================
     * DIAGNOSTICO DE CAIXAS DE DIALOGO (DBG_DIALOG no lado C++ trata texto/blits;
     * aqui ligamos o lado Ruby). Intercepta Window_AdvancedTextPokemon#text= --
     * dispara UMA vez por mensagem (volume baixo, nao por-frame) e regista:
     *   - o TEXTO da mensagem (ate 60 chars, \n visivel)
     *   - a GEOMETRIA da janela (x,y,largura,altura,bordas)
     *   - a windowskin (existe? tamanho) e se e' letra-a-letra / a escrever
     * Combinado com [TXT]/[BLT]/[STRETCH] (C++), da' o quadro completo: que
     * texto, que tamanho de caixa, que imagem, e (pelos intervalos entre logs +
     * o log [INPUT]) se espera pelo A. Tudo defensivo (rescue) -- se a classe
     * nao existir ou algo falhar, so' regista e continua. ====================== */
    {
        const char *dlg_hook =
            "begin\n"
            "  if defined?(Window_AdvancedTextPokemon)\n"
            "    class Window_AdvancedTextPokemon\n"
            "      alias_method :__diag_text_set, :text= unless method_defined?(:__diag_text_set)\n"
            "      def text=(v)\n"
            "        __diag_text_set(v)\n"
            "        begin\n"
            "          t = v.to_s.gsub(\"\\n\", \"\\\\n\")\n"
            "          t = t[0,60] if t.length > 60\n"
            "          sk = (self.windowskin rescue nil)\n"
            "          ss = sk ? \"#{sk.width}x#{sk.height}\" : 'NIL'\n"
            "          bx = (self.borderX rescue '?')\n"
            "          by = (self.borderY rescue '?')\n"
            "          MKXPDebug.log(\"[DLG] msg='#{t}' win=(#{self.x},#{self.y}) #{self.width}x#{self.height} border=#{bx},#{by} skin=#{ss} lbl=#{@letterbyletter} disp=#{@displaying}\")\n"
            "        rescue => e\n"
            "          MKXPDebug.log(\"[DLG] log-err #{e.class}: #{e.message}\")\n"
            "        end\n"
            "      end\n"
            "    end\n"
            "    MKXPDebug.log('[DLG] hook instalado em Window_AdvancedTextPokemon#text=')\n"
            "  else\n"
            "    MKXPDebug.log('[DLG] Window_AdvancedTextPokemon nao existe -- hook nao instalado')\n"
            "  end\n"
            "rescue => e\n"
            "  MKXPDebug.log(\"[DLG] hook falhou: #{e.class}: #{e.message}\")\n"
            "end\n";
        mrb_load_string(mrb, dlg_hook);
        if (mrb->exc) { mrb->exc = 0; }
    }

    /* [PBMSG] Diagnostico do fluxo de mensagens da intro:
     *  - hook em Object#pbMessage  -> capta CADA chamada + texto (a intro chama?)
     *  - hook em Window_AdvancedTextPokemon#initialize -> a janela e' criada?
     * Combinado com o [DLG] (text=), diz-nos onde o texto da intro se perde. */
    {
        static const char *pbmsg_hook = R"PBMSG(
begin
  unless $__pbmsg_hooked
    $__pbmsg_hooked = true
    begin
      class ::Object
        alias_method :__diag_pbmessage, :pbMessage
        def pbMessage(*args, &block)
          begin
            t = (args[0]).to_s
            tt = t.length > 80 ? t[0,80] : t
            tt = tt.gsub("\n", " / ")
            MKXPDebug.log("[PBMSG] len=#{t.length} '#{tt}'")
          rescue
          end
          begin
            __diag_pbmessage(*args, &block)
          rescue Exception => e
            MKXPDebug.log("[PBMSG] EXCECAO no pbMessage: #{e.class}: #{e.message}")
            begin
              bt = e.backtrace
              if bt.is_a?(Array)
                bt[0,8].each_with_index { |l, i| MKXPDebug.log("[PBMSG]   bt[#{i}]: #{l}") }
              end
            rescue
            end
            raise
          end
        end
      end
      MKXPDebug.log('[PBMSG] hook instalado em Object#pbMessage')
    rescue => e
      MKXPDebug.log("[PBMSG] pbMessage indisponivel: #{e.class}: #{e.message}")
    end
    # Hook em pbCreateMessageWindow -- e' AQUI que a janela e' realmente criada.
    begin
      class ::Object
        alias_method :__diag_pbcmw, :pbCreateMessageWindow
        def pbCreateMessageWindow(*args, &block)
          MKXPDebug.log("[PBCMW] pbCreateMessageWindow chamado (args=#{args.length})")
          begin
            r = __diag_pbcmw(*args, &block)
            MKXPDebug.log("[PBCMW] -> #{r.class}")
            r
          rescue Exception => e
            MKXPDebug.log("[PBCMW] EXCECAO: #{e.class}: #{e.message}")
            begin
              (e.backtrace || [])[0,8].each_with_index { |l,i| MKXPDebug.log("[PBCMW]   bt[#{i}]: #{l}") }
            rescue
            end
            raise
          end
        end
      end
      MKXPDebug.log('[PBCMW] hook instalado em Object#pbCreateMessageWindow')
    rescue => e
      MKXPDebug.log("[PBCMW] pbCreateMessageWindow indisponivel: #{e.class}: #{e.message}")
    end
  end
  if defined?(Window_AdvancedTextPokemon)
    class Window_AdvancedTextPokemon
      unless method_defined?(:__diag_winit)
        alias_method :__diag_winit, :initialize
        def initialize(*a)
          __diag_winit(*a)
          begin
            MKXPDebug.log("[DLGINIT] Window_AdvancedTextPokemon.new -> (#{self.x},#{self.y}) #{self.width}x#{self.height}")
          rescue
          end
        end
      end
    end
    MKXPDebug.log('[DLGINIT] hook instalado em Window_AdvancedTextPokemon#initialize')
  end
rescue => e
  MKXPDebug.log("[PBMSG] bloco falhou: #{e.class}: #{e.message}")
end
)PBMSG";
        mrb_load_string(mrb, pbmsg_hook);
        if (mrb->exc) { mrb->exc = 0; }
    }

    for (;;) {
        DBGLOG("[loop] calling %s\n", entry_name);
        mrb_value retval = mrb_funcall_argv(mrb, top, entry_sym, 0, NULL);

        if (mrb->exc) {
            mrb_value exc        = mrb_obj_value(mrb->exc);
            mrb_value msg        = mrb_funcall(mrb, exc, "message", 0);
            mrb_value klass_name = mrb_funcall(mrb,
                                       mrb_obj_value(mrb_class(mrb, exc)),
                                       "name", 0);
            const char *msg_str = mrb_string_p(msg)        ? RSTRING_PTR(msg)        : "unknown";
            const char *cls_str = mrb_string_p(klass_name) ? RSTRING_PTR(klass_name) : "unknown";

            DBGLOG("[loop] ERROR %s: %s\n", cls_str, msg_str);
            show_error(mrb, entry_name);
            mrb->exc = 0;

            if (strstr(cls_str, "NameError")        ||
                strstr(cls_str, "NoMethodError")   ||
                strstr(cls_str, "TypeError")        ||
                strstr(cls_str, "ArgumentError")   ||
                strstr(cls_str, "SyntaxError")     ||
                strstr(cls_str, "ScriptError")     ||
                strstr(cls_str, "LoadError")       ||
                strstr(cls_str, "SystemStackError")) {
                DBGLOG("[loop] Fatal error (%s) -- stopping loop\n", cls_str);
                break;
            }

            svcSleepThread(1000000000LL);
            continue;
        }

        if (mrb_fixnum_p(retval)) {
            mrb_int rv = mrb_fixnum(retval);
            DBGLOG("[loop] %s returned %d\n", entry_name, (int)rv);
            if (rv == 0) {
                DBGLOG("[loop] rv=0, spinning...\n");
                for (;;) {
                    mrb->exc = 0;
                    svcSleepThread(1000000000LL);
                }
            } else if (rv == 1) {
                DBGLOG("[loop] rv=1, exiting cleanly\n");
                break;
            } else {
                DBGLOG("[loop] rv=%d unexpected, exiting\n", (int)rv);
                break;
            }
        } else {
            DBGLOG("[loop] %s returned (non-fixnum), exiting\n", entry_name);
            break;
        }
    }

    DBGLOG("[binding] main loop exited\n");
}
/* =========================================================
 * CACHE DE BYTECODE (acelerar arranque)
 * =========================================================
 * PROBLEMA: a cada arranque, os ~376 scripts (8+ MB de Ruby) sao parseados e
 * compilados para bytecode em runtime pelo mruby, no CPU lento do 3DS -> ~5 min.
 *
 * SOLUCAO: compilar cada script para bytecode .mrb UMA vez, gravar em
 * sdmc:/mkxp/cache/, e nos arranques seguintes carregar o bytecode (salta o
 * parsing) -> segundos.
 *
 * GENERICO / POR-CONTEUDO: a chave da cache deriva do CONTEUDO ja-patchado
 * (nome + tamanho + hash FNV-1a). Por isso:
 *   - funciona para QUALQUER jogo (nao e' especifico deste fan-game);
 *   - cada jogo paga a compilacao so no 1o arranque dele;
 *   - scripts identicos entre jogos reaproveitam a mesma cache;
 *   - se um script mudar, a chave muda -> recompila automaticamente (sem cache
 *     desatualizada).
 *
 * SEGURANCA: se QUALQUER passo da cache falhar (ler, gravar, bytecode invalido,
 * versao mruby diferente), faz fallback transparente a mrb_load_nstring. A cache
 * nunca pode partir o arranque.
 *
 * TODO (UX, quando o emulador estiver avancado): no 1o arranque de um jogo,
 * mostrar uma barra de progresso de compilacao ("A preparar o jogo... N/total")
 * em vez de ecra preto, ja que essa primeira vez demora.
 */

/* Para DESLIGAR a cache (se algum dia causar problemas), basta mudar para 0.
 * Com 0, o arranque volta ao comportamento antigo (mrb_load_nstring direto).
 *
 * NOTA: durante muito tempo julgou-se que MRB_NO_STDIO removia mrb_dump_irep
 * (gravar bytecode). CONFIRMADO QUE NAO: `nm libmruby.a` mostra "T mrb_dump_irep"
 * -- a funcao de dump para MEMORIA existe (MRB_NO_STDIO so remove as variantes
 * que escrevem para FILE* diretamente). Logo a cache de bytecode dos 376 scripts
 * base GERA-SE no proprio 3DS, tal como a dos plugins. Por isso esta LIGADA (1).
 * O bytecode vai para sdmc:/mkxp/cache/scripts/. 1a corrida compila+grava;
 * seguintes carregam .mrb (rapido). Fallback seguro a mrb_load_nstring se a
 * cache faltar/for invalida. */
#ifndef MKXP_BYTECODE_CACHE
#define MKXP_BYTECODE_CACHE 1
#endif

/* Cache dos dados .rxdata desserializados (Tilesets/CommonEvents/species/etc).
 * 1a corrida: grava o resultado em sdmc:/mkxp/cache/rxdata/; seguintes: le de
 * la' e salta o re-parse da fonte. Chave inclui tamanho+mtime -> auto-invalida.
 * Poe a 0 para desligar e medir a diferenca de tempo de arranque. */
#ifndef MKXP_RXDATA_CACHE
#define MKXP_RXDATA_CACHE 1
#endif

/* Saltar plugins pesados que ja' falham por completo (ex.: EBDX, 485 scripts
 * que nem compilam por causa de um SyntaxError no Setup). Compilar 485 scripts
 * so' para falhar e' a maior fatia do tempo de boot. Saltar da' o mesmo estado
 * funcional muito mais rapido. Poe a 0 para voltar a tentar carregar tudo. */
#ifndef MKXP_SKIP_BROKEN_PLUGINS
#define MKXP_SKIP_BROKEN_PLUGINS 1
#endif

/* Garante que toda a estrutura de pastas da cache existe (idempotente).
 * FORA de qualquer #if -- usada tanto pela cache dos scripts como dos plugins:
 *   sdmc:/mkxp/cache/scripts   (376 scripts base)
 *   sdmc:/mkxp/cache/plugins   (559 plugins)
 *   sdmc:/mkxp/cache/rxdata    (dados marshal -- reservado p/ futuro)
 * Chamada uma vez no inicio de cada carregador. */
static void mkxp_cache_ensure_tree(void) {
    mkdir("sdmc:/mkxp",                0777);
    mkdir("sdmc:/mkxp/cache",          0777);
    mkdir("sdmc:/mkxp/cache/scripts",  0777);
    mkdir("sdmc:/mkxp/cache/plugins",  0777);
    mkdir("sdmc:/mkxp/cache/rxdata",   0777);
}

#if MKXP_BYTECODE_CACHE
/* FNV-1a 64-bit -> hex, sobre um buffer (conteudo do script ja patchado). */
static void mkxp_cache_key(const char *name, const unsigned char *data,
                           size_t len, char *out, size_t out_sz) {
    unsigned long long h = 1469598103934665603ULL; /* offset basis */
    /* incorporar o nome para evitar colisoes entre scripts diferentes */
    for (const char *p = name; p && *p; ++p) {
        h ^= (unsigned char)*p;
        h *= 1099511628211ULL;
    }
    h ^= (unsigned long long)len; h *= 1099511628211ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    /* nome "sanitizado" curto + hash, p.ex. cache/Main_a1b2c3d4e5f60718.mrb */
    char safe[40]; size_t si = 0;
    for (const char *p = name; p && *p && si < sizeof(safe) - 1; ++p) {
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9')) safe[si++] = c;
        else safe[si++] = '_';
    }
    safe[si] = 0;
    snprintf(out, out_sz, "sdmc:/mkxp/cache/scripts/%s_%016llx.mrb", safe, h);
}

/* Garante que a pasta da cache existe (idempotente). */
static void mkxp_cache_ensure_dir(void) {
    mkxp_cache_ensure_tree();
}

/* Tenta carregar+executar bytecode da cache. Devolve true se correu (ok ou com
 * excecao tratada); false se a cache nao existe/invalida (=> recompilar). */
static bool mkxp_cache_try_load(mrb_state *mrb, const char *cache_path,
                                const char *script_name) {
    FILE *cf = fopen(cache_path, "rb");
    if (!cf) return false;
    fseek(cf, 0, SEEK_END);
    long csz = ftell(cf);
    fseek(cf, 0, SEEK_SET);
    if (csz <= 8) { fclose(cf); return false; }   /* ficheiro corrompido/curto */
    std::vector<unsigned char> buf((size_t)csz);
    size_t rd = fread(buf.data(), 1, (size_t)csz, cf);
    fclose(cf);
    if (rd != (size_t)csz) return false;

    /* Validar header RITE (".mrb" comeca por "RITE"). Se nao, ignorar. */
    if (csz < 4 || buf[0] != 'R' || buf[1] != 'I' || buf[2] != 'T' || buf[3] != 'E')
        return false;

    int arena = mrb_gc_arena_save(mrb);
    mrb_load_irep_buf(mrb, buf.data(), (size_t)csz);
    if (mrb->exc) {
        /* Excecao na EXECUCAO do bytecode e' normal (mesmos erros que o .rb ja
         * dava) e era tolerada antes; tratamos e consideramos "carregado".    */
        show_error(mrb, script_name);
        mrb->exc = 0;
    }
    mrb_gc_arena_restore(mrb, arena);
    return true;
}

/* Compila a string para irep, GRAVA o bytecode na cache, e executa o irep.
 * Devolve true se compilou/executou; false se a compilacao falhou (=> o
 * chamador faz fallback a mrb_load_nstring). */
static bool mkxp_cache_compile_run(mrb_state *mrb, const char *code, size_t code_len,
                                   const char *cache_path, const char *script_name) {
    int arena = mrb_gc_arena_save(mrb);
    mrbc_context *cxt = mrbc_context_new(mrb);
    if (!cxt) { mrb_gc_arena_restore(mrb, arena); return false; }
    mrbc_filename(mrb, cxt, script_name);
    cxt->no_exec = TRUE;   /* compilar SEM executar -> obtemos o proc/irep */

    mrb_value v = mrb_load_nstring_cxt(mrb, code, code_len, cxt);
    /* Com no_exec=TRUE: parse falhado -> v e' UNDEF (padrao do mruby-require).
     * Sucesso -> v e' um RProc cujo irep podemos despejar.                    */
    if (mrb->exc || mrb_undef_p(v) || mrb_type(v) != MRB_TT_PROC) {
        if (mrb->exc) { show_error(mrb, script_name); mrb->exc = 0; }
        mrbc_context_free(mrb, cxt);
        mrb_gc_arena_restore(mrb, arena);
        return false;   /* parse/codegen falhou -> fallback a nstring */
    }

    struct RProc *proc = mrb_proc_ptr(v);
    const mrb_irep *irep = proc->body.irep;

    /* Gravar bytecode (best-effort: se falhar, ainda executamos na mesma). */
    if (irep) {
        unsigned char *bin = NULL;
        size_t bin_size = 0;
        int dret = mrb_dump_irep(mrb, irep, DUMP_ENDIAN_NAT, &bin, &bin_size);
        if (dret == MRB_DUMP_OK && bin && bin_size > 0) {
            FILE *wf = fopen(cache_path, "wb");
            if (wf) {
                fwrite(bin, 1, bin_size, wf);
                fclose(wf);
            }
        }
        if (bin) mrb_free(mrb, bin);
    }

    /* Executar agora o proc compilado (equivalente a ter corrido o .rb). */
    mrb_vm_run(mrb, proc, mrb_top_self(mrb), 0);
    if (mrb->exc) { show_error(mrb, script_name); mrb->exc = 0; }

    mrbc_context_free(mrb, cxt);
    mrb_gc_arena_restore(mrb, arena);
    return true;
}
#endif /* MKXP_BYTECODE_CACHE */

/* Patch para scripts de PLUGIN. Diferente do patch_script dos 376 scripts base:
 * NAO aplica replace_defined_keyword, porque o Modular Title Screen depende de
 * defined?(MTS_Element_X) -- inclusive dentro de eval("defined?(...)") -- para
 * decidir que elementos visuais carregar. Se trocassemos defined? por nil, a
 * tela ficaria vazia. O mruby 3.2 deste port suporta defined? em runtime (a
 * limitacao antiga era em certos contextos de parsing que aqui nao ocorrem).
 *
 * Por agora nao transforma nada: os plugins correm como vieram. Mantem-se a
 * funcao como ponto unico caso um plugin especifico precise de ajuste futuro
 * (ex: saltar um script que rebente, ou remover um loop bloqueante). */
static void patch_plugin_script(std::vector<Bytef> &decomp, uLong dest_sz,
                                 const char *name) {
    (void)decomp; (void)dest_sz; (void)name;
    /* Sem transformacoes globais. Adicionar aqui ajustes por-plugin se
     * algum vier a precisar (com base no log [PLUGIN-ERROR]). */
}

/* =========================================================
 * CACHE DE BYTECODE DOS PLUGINS (gerada no PC com mrbc)
 * ---------------------------------------------------------
 * Problema: os 559 scripts de plugin sao compilados de TEXTO a cada arranque
 * (~20 min). A libmruby do 3DS foi compilada com MRB_NO_STDIO, logo NAO tem
 * mrb_dump_irep (gravar bytecode) -- nao da para gerar a cache no proprio 3DS.
 * MAS tem mrb_load_irep_buf (LER bytecode), que CHEGA para usar uma cache
 * pre-compilada no PC.
 *
 * Fluxo (sem ferramentas alem do mrbc que ja existe no PC):
 *   1a corrida (sem cache): para cada plugin, alem de o correr por texto, o 3DS
 *      GRAVA o codigo ja-descomprimido-e-patchado em
 *        sdmc:/mkxp/plugin_src/NNNN_nome.rb
 *      (NNNN = indice sequencial, garante ordem de carregamento correcta).
 *   No PC: corre-se UMA vez o mrbc sobre essa pasta -> gera
 *        sdmc:/mkxp/cache/NNNN_nome.mrb
 *   Corridas seguintes: o 3DS encontra o .mrb e carrega bytecode (segundos),
 *      saltando o parsing. Se faltar o .mrb de um script, faz fallback a texto
 *      (e volta a grava-lo em plugin_src para o PC compilar).
 *
 * SEGURANCA: qualquer falha (sem ficheiro, bytecode invalido, versao/boxing
 * incompativel) cai no caminho de texto. A cache nunca parte o arranque.
 * ========================================================= */

/* Liga/desliga a cache de plugins. 1 = tenta usar .mrb e grava .rb fonte. */
#ifndef MKXP_PLUGIN_CACHE
#define MKXP_PLUGIN_CACHE 1
#endif

#if MKXP_PLUGIN_CACHE
/* Garante que um directorio existe (best-effort, ignora se ja existe). */
static void mkxp_ensure_dir(const char* dir) {
    mkdir(dir, 0777);
}

/* Sanitiza o nome do script para nome de ficheiro seguro (sem / \ espacos). */
static void mkxp_safe_name(const char* in, char* out, size_t out_sz) {
    size_t j = 0;
    for (const char* p = in; *p && j + 1 < out_sz; ++p) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = 0;
}

/* Tenta carregar e executar o bytecode .mrb correspondente a este script.
 * Devolve true se carregou e correu (mesmo que o script lance excecao Ruby,
 * que e' tratada/limpa aqui). Devolve false se nao havia cache utilizavel
 * (-> o chamador faz fallback a texto). */
static bool mkxp_plugin_cache_run(mrb_state* mrb, const char* cache_path,
                                  const char* script_name) {
    FILE* cf = fopen(cache_path, "rb");
    if (!cf) return false;

    fseek(cf, 0, SEEK_END);
    long sz = ftell(cf);
    fseek(cf, 0, SEEK_SET);
    if (sz <= 0) { fclose(cf); return false; }

    unsigned char* buf = (unsigned char*)malloc((size_t)sz);
    if (!buf) { fclose(cf); return false; }

    size_t rd = fread(buf, 1, (size_t)sz, cf);
    fclose(cf);
    if (rd != (size_t)sz) { free(buf); return false; }

    int arena = mrb_gc_arena_save(mrb);

    /* mrb_load_irep_buf valida o cabecalho RITE (versao/boxing). Se o bytecode
     * for incompativel devolve um valor sem proc utilizavel e/ou poe mrb->exc.
     * Tratamos ambos como "cache invalida -> fallback". */
    mrb_value v = mrb_load_irep_buf(mrb, buf, (size_t)sz);
    free(buf);

    if (mrb->exc) {
        /* bytecode invalido ou erro de runtime: limpar e cair para texto */
        mrb->exc = 0;
        mrb_gc_arena_restore(mrb, arena);
        return false;
    }
    (void)v;
    mrb_gc_arena_restore(mrb, arena);
    return true;
}

/* Grava o codigo-fonte (ja descomprimido e patchado) para o PC compilar com
 * mrbc. So grava se ainda nao existir o .rb (evita reescrever a cada corrida). */
static void mkxp_plugin_dump_src(const char* src_path,
                                 const unsigned char* code, size_t code_len) {
    FILE* tf = fopen(src_path, "rb");
    if (tf) { fclose(tf); return; }   /* ja existe -> nao reescreve */
    FILE* wf = fopen(src_path, "wb");
    if (!wf) return;
    fwrite(code, 1, code_len, wf);
    fclose(wf);
}

/* ─────────────────────────────────────────────────────────────────────────
 * CACHE AUTOMATICA NO 3DS (sem BAT) -- Tarefa 6
 * Compila o script em bytecode e grava o .mrb DIRETAMENTE no 3DS apos o 1o
 * arranque. Nas corridas seguintes carrega-se o .mrb (rapido).
 *
 * REQUISITO: mrb_dump_irep() so existe se a libmruby for compilada SEM
 * MRB_NO_STDIO (com STDIO). A lib atual do port tem MRB_NO_STDIO, por isso
 * esta funcao fica protegida por MKXP_CAN_DUMP_IREP. Quando recompilares a
 * libmruby com STDIO ativado, define MKXP_CAN_DUMP_IREP=1 (no Makefile ou
 * aqui) e o cache passa a gerar-se SOZINHO no 3DS, sem BAT nenhum.
 *
 * Como ativar (recompilar mruby com dump):
 *   No build_config/nintendo_3ds.rb, remover a flag que define MRB_NO_STDIO
 *   (ou adicionar mruby-bin-mrbc ao conf.gem). Recompilar a lib. Depois
 *   compilar o port com -DMKXP_CAN_DUMP_IREP=1.
 * ───────────────────────────────────────────────────────────────────────── */
#ifndef MKXP_CAN_DUMP_IREP
/* LIGADO por defeito: confirmamos que a libmruby do port EXPORTA mrb_dump_irep
 * (nm libmruby.a mostra "T mrb_dump_irep"), apesar de MRB_NO_STDIO. Logo a cache
 * de bytecode gera-se AUTOMATICAMENTE no 3DS, sem BAT e sem flags de compilacao.
 * Basta `make`. Se algum dia a lib for trocada por uma SEM mrb_dump_irep, o
 * linker vai queixar-se -- nesse caso poe isto a 0 (volta ao metodo do PC). */
#define MKXP_CAN_DUMP_IREP 1
#endif

#if MKXP_CAN_DUMP_IREP
/* mrb_dump_irep vem de <mruby/dump.h> (ja incluido no topo). Confirmado que a
 * lib o exporta (nm: "T mrb_dump_irep"). Usamos as constantes do header
 * (DUMP_ENDIAN_NAT / MRB_DUMP_OK), iguais a' cache dos 376 scripts, para as
 * duas caches serem consistentes e o bytecode compativel. */

/* Compila 'code' e, se compilar limpo, grava o bytecode em cache_path.
 * Devolve true se gerou o .mrb. Best-effort: qualquer falha -> false. */
static bool mkxp_plugin_dump_bytecode(mrb_state* mrb, const char* cache_path,
                                      const unsigned char* code, size_t code_len) {
    int arena = mrb_gc_arena_save(mrb);
    mrbc_context* c = mrbc_context_new(mrb);
    if (!c) { mrb_gc_arena_restore(mrb, arena); return false; }
    c->no_exec = TRUE;   /* so compila, nao executa */

    mrb_value vp = mrb_load_nstring_cxt(mrb, (const char*)code, code_len, c);
    if (mrb->exc || !mrb_proc_p(vp)) {
        mrb->exc = 0;
        mrbc_context_free(mrb, c);
        mrb_gc_arena_restore(mrb, arena);
        return false;
    }
    struct RProc* proc = mrb_proc_ptr(vp);

    unsigned char* bin = NULL;
    size_t   bin_sz = 0;
    int rc = mrb_dump_irep(mrb, proc->body.irep, DUMP_ENDIAN_NAT, &bin, &bin_sz);
    bool ok = false;
    if (rc == MRB_DUMP_OK && bin && bin_sz > 0) {
        FILE* wf = fopen(cache_path, "wb");
        if (wf) {
            ok = (fwrite(bin, 1, bin_sz, wf) == bin_sz);
            fclose(wf);
        }
    }
    if (bin) mrb_free(mrb, bin);
    mrbc_context_free(mrb, c);
    mrb_gc_arena_restore(mrb, arena);
    return ok;
}

/* Compila o codigo UMA SO' VEZ, grava o bytecode (.mrb) E executa o mesmo proc.
 * Substitui o fluxo antigo que compilava DUAS vezes (uma p/ gravar, outra p/
 * executar) -- a compilacao (parse) e' a parte cara (~50% do tempo), por isso
 * fazer 2x desperdicava metade do tempo de cada plugin cache-MISS.
 *
 * GENERICO: serve qualquer script de qualquer jogo. Os que dao SyntaxError
 * falham no unico parse (sem desperdicar o 2o). Devolve true se executou (com ou
 * sem erro de runtime); *out_dumped fica true se o .mrb foi gravado com sucesso
 * (para a proxima corrida ser HIT, mesmo que a execucao agora tenha dado erro). */
static bool mkxp_plugin_compile_dump_run(mrb_state* mrb, const char* cache_path,
                                         const unsigned char* code, size_t code_len,
                                         const char* script_name, bool* out_dumped) {
    if (out_dumped) *out_dumped = false;
    int arena = mrb_gc_arena_save(mrb);
    mrbc_context* c = mrbc_context_new(mrb);
    if (!c) { mrb_gc_arena_restore(mrb, arena); return false; }
    mrbc_filename(mrb, c, script_name ? script_name : "plugin");
    c->no_exec = TRUE;   /* compilar SEM executar -> obtemos o proc/irep p/ gravar E correr */

    mrb_value vp = mrb_load_nstring_cxt(mrb, (const char*)code, code_len, c);
    if (mrb->exc || !mrb_proc_p(vp)) {
        /* erro de COMPILACAO (ex: SyntaxError): nao da' para gravar nem executar
         * o bytecode. Limpar e devolver false -> o chamador cai para texto. */
        if (mrb->exc) { show_error(mrb, script_name); mrb->exc = 0; }
        mrbc_context_free(mrb, c);
        mrb_gc_arena_restore(mrb, arena);
        return false;
    }
    struct RProc* proc = mrb_proc_ptr(vp);
    const mrb_irep* irep = proc->body.irep;

    /* 1) Gravar o bytecode (best-effort). Acontece ANTES de executar, por isso
     *    e' independente de a execucao falhar -> a proxima corrida sera' HIT. */
    if (irep) {
        unsigned char* bin = NULL; size_t bin_sz = 0;
        int rc = mrb_dump_irep(mrb, irep, DUMP_ENDIAN_NAT, &bin, &bin_sz);
        if (rc == MRB_DUMP_OK && bin && bin_sz > 0) {
            FILE* wf = fopen(cache_path, "wb");
            if (wf) {
                if (fwrite(bin, 1, bin_sz, wf) == bin_sz && out_dumped) *out_dumped = true;
                fclose(wf);
            }
        }
        if (bin) mrb_free(mrb, bin);
    }

    /* 2) Executar o MESMO proc ja' compilado (equivale a ter corrido o texto). */
    mrb_vm_run(mrb, proc, mrb_top_self(mrb), 0);
    if (mrb->exc) { show_error(mrb, script_name); mrb->exc = 0; }

    mrbc_context_free(mrb, c);
    mrb_gc_arena_restore(mrb, arena);
    return true;
}
#endif /* MKXP_CAN_DUMP_IREP */
#endif /* MKXP_PLUGIN_CACHE */


/* =========================================================
 * CARREGADOR DE PLUGINS (PluginScripts.rxdata)
 * ---------------------------------------------------------
 * O PluginManager original do Essentials esta desligado (stub vazio) neste
 * port. Por isso os plugins (incluindo o Modular Title Screen, que desenha a
 * tela bonita do Solar Eclipse) nunca eram carregados, e o jogo caia na tela
 * de titulo base (splashes do Essentials).
 *
 * Esta funcao faz o que o PluginManager.runPlugins original faz:
 *   - carrega Data/PluginScripts.rxdata (Ruby Marshal)
 *   - formato: Array de plugins. Cada plugin = [nome, meta, scripts]
 *     onde scripts = Array de [caminho, codigo_comprimido_zlib]
 *   - descomprime cada script com zlib e executa-o com mrb_load_nstring
 *   - aplica os mesmos patches (patch_script) que os scripts base, para
 *     manter as mesmas correcoes/skip-list a funcionar nos plugins
 *
 * Abordagem honesta: executa o codigo real do plugin, sem reimplementar a
 * tela a mao. Se um plugin especifico falhar, regista [PLUGIN-ERROR] e
 * continua (como o loop dos 376 scripts), para o boot nao morrer.
 * ========================================================= */
static void run_plugin_scripts(mrb_state* mrb, const char* game_path) {
    char path[256];
    snprintf(path, sizeof(path), "%s/Data/PluginScripts.rxdata", game_path);
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        DBGLOG("[PLUGINS] PluginScripts.rxdata nao encontrado -- sem plugins\n");
        return;
    }

    mrb_value plugins = marshalLoadInt(mrb, fp);
    fclose(fp);

    /* GUARDA: se o marshal lancou excecao (ficheiro corrompido/formato
     * inesperado), limpar antes de continuar para nao contaminar os
     * mrb_load_nstring seguintes. */
    if (mrb->exc) {
        show_error(mrb, "PluginScripts.rxdata marshal");
        mrb->exc = 0;
        return;
    }

    if (!mrb_array_p(plugins)) {
        DBGLOG("[PLUGINS] PluginScripts.rxdata nao e um array -- ignorado\n");
        return;
    }

    mrb_gc_register(mrb, plugins);
    int num_plugins = (int)RARRAY_LEN(plugins);
    DBGLOG("[PLUGINS] %d plugins a carregar\n", num_plugins);

    int ok_scripts = 0, fail_scripts = 0;

#if MKXP_PLUGIN_CACHE
    /* Preparar a estrutura de pastas da cache (scripts/plugins/rxdata).
     * Tudo dentro de sdmc:/mkxp/cache/. seq = indice global para garantir que a
     * ordem de carregamento e' sempre a mesma (critico: os plugins dependem uns
     * dos outros e das classes base). Com a cache automatica (mrb_dump_irep) os
     * .mrb geram-se no proprio 3DS; o .rb so se grava como fallback p/ o PC. */
    mkxp_cache_ensure_tree();
    int cache_seq = 0;
    int cache_hits = 0, cache_miss = 0;
    int cache_dumped = 0;
#endif

    for (int pi = 0; pi < num_plugins; pi++) {
        mrb_value plugin = mrb_ary_entry(plugins, pi);
        if (!mrb_array_p(plugin) || RARRAY_LEN(plugin) < 3) {
            DBGLOG("[PLUGINS] plugin %d invalido (estrutura)\n", pi);
            continue;
        }

        /* plugin = [name, meta, scripts] */
        mrb_value name_val    = mrb_ary_entry(plugin, 0);
        mrb_value scripts_val = mrb_ary_entry(plugin, 2);

        const char* pname = mrb_string_p(name_val) ? RSTRING_PTR(name_val) : "?";

        if (!mrb_array_p(scripts_val)) {
            DBGLOG("[PLUGINS] plugin '%s' sem array de scripts\n", pname);
            continue;
        }

        int nscr = (int)RARRAY_LEN(scripts_val);

#if MKXP_SKIP_BROKEN_PLUGINS
        /* SALTAR plugins pesados que JA' falham por completo -----------------
         * O EBDX (Elite Battle: DX) sao 485 dos 559 scripts (87%!) e o seu
         * EBDX Setup.rb da SyntaxError -> cascata -> o plugin INTEIRO ja' nao
         * funciona hoje. Compilar/executar 485 scripts so' para falhar custa a
         * maior fatia do tempo de boot (cada script = parse + bytecode no mruby).
         * Saltar da' EXATAMENTE o mesmo estado funcional (batalha base do
         * Essentials, que ja' e' o que ha' sem EBDX) mas muito mais rapido.
         * Lista por prefixo de nome; poe MKXP_SKIP_BROKEN_PLUGINS a 0 p/ reativar. */
        {
            static const char* kSkip[] = {
                "Elite Battle: DX",   /* 485 scripts, Setup.rb SyntaxError, nao funciona */
                0
            };
            bool skip = false;
            for (int k = 0; kSkip[k]; k++)
                if (strcmp(pname, kSkip[k]) == 0) { skip = true; break; }
            if (skip) {
                DBGLOG("[PLUGINS] SKIP '%s' (%d scripts) -- plugin partido, "
                       "saltado p/ acelerar boot\n", pname, nscr);
                continue;
            }
        }
#endif

        DBGLOG("[PLUGINS] plugin '%s' (%d scripts)\n", pname, nscr);

        for (int si = 0; si < nscr; si++) {
            mrb_value scr = mrb_ary_entry(scripts_val, si);
            if (!mrb_array_p(scr) || RARRAY_LEN(scr) < 2) continue;

            /* scr = [filepath, zlib_code] */
            mrb_value spath_val = mrb_ary_entry(scr, 0);
            mrb_value code_val  = mrb_ary_entry(scr, 1);
            if (!mrb_string_p(code_val)) continue;

            const char* compressed = RSTRING_PTR(code_val);
            int compressed_len     = (int)RSTRING_LEN(code_val);

            /* nome do ficheiro do script (so a parte final, p/ logs/patches) */
            char sname[128] = "plugin_script";
            if (mrb_string_p(spath_val)) {
                const char* sp = RSTRING_PTR(spath_val);
                const char* base = sp;
                for (const char* c = sp; *c; c++) {
                    if (*c == '/' || *c == '\\') base = c + 1;
                }
                snprintf(sname, sizeof(sname), "%s", base);
            }

            /* PROFILING por fase (igual aos scripts-base). */
            uint64_t pl_t0 = prof_tick();

            /* descomprimir zlib (igual ao loop dos 376 scripts) */
            uLongf dest_len = (compressed_len > 0 ? compressed_len : 1) * 4;
            std::vector<Bytef> decomp(dest_len);
            int ret = uncompress(decomp.data(), &dest_len,
                                 (const Bytef*)compressed, compressed_len);
            while (ret == Z_BUF_ERROR) {
                dest_len *= 2;
                decomp.resize(dest_len);
                ret = uncompress(decomp.data(), &dest_len,
                                 (const Bytef*)compressed, compressed_len);
            }
            if (ret != Z_OK) {
                DBGLOG("[PLUGIN-ERROR] uncompress falhou em '%s'/'%s'\n", pname, sname);
                fail_scripts++;
                continue;
            }
            decomp.resize(dest_len);
            double pl_decomp_ms = prof_ticks_to_ms(prof_tick() - pl_t0);

            /* patch especifico de plugins: NAO mexe em defined? (o MODTS
             * precisa dele). Ver patch_plugin_script. */
            uint64_t pl_t1 = prof_tick();
            patch_plugin_script(decomp, (uLong)decomp.size(), sname);
            double pl_patch_ms = prof_ticks_to_ms(prof_tick() - pl_t1);

            uint64_t pl_bytes = (uint64_t)decomp.size();
            int    pl_hit = 0;
            double pl_exec_ms = 0.0, pl_compile_ms = 0.0;
            uint64_t pl_t2 = prof_tick();   /* inicio da fase exec/compile (fora do #if p/ robustez) */
            bool executed = false;          /* fora do #if: o fallback abaixo precisa dela sempre */

#if MKXP_PLUGIN_CACHE
            /* Chave de ficheiro: NNNN_nome (indice sequencial preserva ordem). */
            int seq = cache_seq++;
            char safe[96];
            mkxp_safe_name(sname, safe, sizeof(safe));
            /* Remover a extensao .rb do nome (o sname ja a inclui), senao os
             * ficheiros ficavam com extensao dupla: 0127_GRUDGE.rb.rb /
             * 0127_GRUDGE.rb.mrb. Cortamos no ultimo ".rb" final. */
            {
                size_t sl = strlen(safe);
                if (sl >= 3 && strcmp(safe + sl - 3, ".rb") == 0) {
                    safe[sl - 3] = 0;
                }
            }
            char cache_path[256];
            char src_path[256];
            snprintf(cache_path, sizeof(cache_path),
                     "sdmc:/mkxp/cache/plugins/%04d_%s.mrb", seq, safe);
            snprintf(src_path, sizeof(src_path),
                     "sdmc:/mkxp/cache/plugins/%04d_%s.rb", seq, safe);

            /* 1) Tentar bytecode pre-compilado (rapido). */
            if (mkxp_plugin_cache_run(mrb, cache_path, sname)) {
                cache_hits++;
                ok_scripts++;
                pl_hit = 1;
                pl_exec_ms = prof_ticks_to_ms(prof_tick() - pl_t2);
                prof_script_record(sname, pl_bytes, pl_hit,
                                   pl_decomp_ms, pl_patch_ms, pl_exec_ms, pl_compile_ms);
                continue;
            }

            /* 2) Sem cache (MISS): gravar o fonte para o PC compilar com mrbc... */
            cache_miss++;
            mkxp_plugin_dump_src(src_path, decomp.data(), decomp.size());

#if MKXP_CAN_DUMP_IREP
            /* ...e, se a lib suportar dump, COMPILAR UMA SO' VEZ: gera o .mrb
             * (best-effort) E executa o mesmo proc. Antes compilava-se 2x (uma
             * p/ gravar, outra p/ executar) -- a compilacao e' a parte cara, por
             * isso isto poupa ~50% do tempo de cada plugin MISS. Na proxima
             * corrida o .mrb existe -> HIT (mesmo que a execucao agora dê erro
             * de runtime, pois o .mrb e' gravado ANTES de executar). */
            {
                bool dumped = false;
                if (mkxp_plugin_compile_dump_run(mrb, cache_path,
                                                 decomp.data(), decomp.size(),
                                                 sname, &dumped)) {
                    executed = true;
                    if (dumped) cache_dumped++;
                    /* a contagem ok/fail e' feita por show_error dentro da funcao;
                     * aqui consideramos "processado". Se houve excecao de runtime
                     * ela ja' foi logada e limpa la' dentro. */
                    ok_scripts++;
                }
            }
#endif
#endif /* MKXP_PLUGIN_CACHE */

            /* Fallback: se nao havia dump-irep (ou a compilacao falhou), correr
             * por texto (compila+executa em runtime, sem cache). */
            if (!executed) {
                mrb_value result = mrb_load_nstring(mrb, (const char*)decomp.data(),
                                                    decomp.size());
                (void)result;
                if (mrb->exc) {
                    char ctx[200];
                    snprintf(ctx, sizeof(ctx), "plugin '%s' / %s", pname, sname);
                    show_error(mrb, ctx);
                    mrb->exc = 0;
                    fail_scripts++;
                } else {
                    ok_scripts++;
                }
            }
            pl_compile_ms = prof_ticks_to_ms(prof_tick() - pl_t2);
            prof_script_record(sname, pl_bytes, pl_hit,
                               pl_decomp_ms, pl_patch_ms, pl_exec_ms, pl_compile_ms);
        }
    }

    mrb_gc_unregister(mrb, plugins);
#if MKXP_PLUGIN_CACHE
    DBGLOG("[PLUGINS] cache: %d hits (.mrb), %d miss (texto)\n",
           cache_hits, cache_miss);
#if MKXP_CAN_DUMP_IREP
    DBGLOG("[PLUGINS] cache: %d .mrb gerados AUTOMATICAMENTE no 3DS\n", cache_dumped);
    if (cache_dumped > 0)
        DBGLOG("[PLUGINS] >> proxima corrida sera' rapida (sem BAT)\n");
#else
    if (cache_miss > 0) {
        DBGLOG("[PLUGINS] >> Fontes em sdmc:/mkxp/plugin_src/ (compilar no PC com mrbc)\n");
        DBGLOG("[PLUGINS] >> Ou recompilar libmruby c/ STDIO + -DMKXP_CAN_DUMP_IREP=1 p/ cache automatica\n");
    }
#endif
#endif
    /* Relatorio de profiling dos plugins (igual aos scripts-base): hits/miss,
     * tempo por fase, top 10 mais lentos. */
    prof_report_scripts("plugins");
    prof_scripts_reset();

    DBGLOG("[PLUGINS] concluido: %d scripts OK, %d falharam\n",
           ok_scripts, fail_scripts);
}

/* =========================================================
 * ENTRY POINT
 * ========================================================= */
 
static void run_rmxp_scripts(mrb_state* mrb, const char* game_path) {
    char path[256];
    snprintf(path, sizeof(path), "%s/Data/Scripts.rxdata", game_path);
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        DBGLOG("[ERROR] Cannot open %s\n", path);
        return;
    }

    // Le o ficheiro inteiro com marshalLoadInt -> array de [id, nome, codigo]
    mrb_value scripts_array = marshalLoadInt(mrb, fp);
    fclose(fp);

    if (!mrb_array_p(scripts_array)) {
        DBGLOG("[ERROR] Scripts.rxdata is not an array\n");
        return;
    }

    mrb_gc_register(mrb, scripts_array);
    int num_scripts = (int)RARRAY_LEN(scripts_array);
        DBGLOG("[SCRIPTS] Loading %d scripts\n", num_scripts);

#if MKXP_BYTECODE_CACHE
    /* Criar pasta de cache de bytecode (idempotente). Se a cache funcionar,
     * os arranques seguintes saltam o parsing e sao MUITO mais rapidos. */
    mkxp_cache_ensure_dir();
    DBGLOG("[CACHE] cache de bytecode: sdmc:/mkxp/cache/\n");
#endif

    /* Carregar rgss_stubs.rb antes dos scripts do jogo */
    {
        FILE *sf = fopen("sdmc:/mkxp/rgss_stubs.rb", "rb");
        if (!sf) sf = fopen("romfs:/rgss_stubs.rb", "rb");
        if (!sf) sf = fopen("sdmc:/3ds/mkxp-3ds/rgss_stubs.rb", "rb");
        if (!sf) sf = fopen("sdmc:/3ds/mkxp/rgss_stubs.rb", "rb");
        if (sf) {
            fseek(sf, 0, SEEK_END); long ssz = ftell(sf); fseek(sf, 0, SEEK_SET);
            char *sbuf = (char*)malloc(ssz + 1);
            fread(sbuf, 1, ssz, sf); fclose(sf); sbuf[ssz] = 0;
            mrb_load_string(mrb, sbuf); free(sbuf);
            if (mrb->exc) { show_error(mrb, "rgss_stubs"); mrb->exc = 0; }
            else DBGLOG("[STUBS] rgss_stubs.rb loaded\n");
        } else {
            DBGLOG("[STUBS] WARN: rgss_stubs.rb not found\n");
        }
    }

    /* [REGEXP-REAL] Injecao do stub no-op de Regexp removida. O mruby agora tem
     * Regexp real (mruby-onig-regexp), por isso o ios_compat_3ds.rb passa a usar
     * o regex nativo em vez do stub que devolvia nil/false. */

    /* Carregar ios_compat_3ds.rb -- compat layer portado do iOS */
    {
        FILE *sf = fopen("sdmc:/mkxp/ios_compat_3ds.rb", "rb");
        if (!sf) sf = fopen("romfs:/ios_compat_3ds.rb", "rb");
        if (sf) {
            fseek(sf, 0, SEEK_END); long ssz = ftell(sf); fseek(sf, 0, SEEK_SET);
            char *sbuf = (char*)malloc(ssz + 1);
            fread(sbuf, 1, ssz, sf); fclose(sf); sbuf[ssz] = 0;
            mrb_load_string(mrb, sbuf); free(sbuf);
            if (mrb->exc) { show_error(mrb, "ios_compat_3ds"); mrb->exc = 0; }
            else DBGLOG("[STUBS] ios_compat_3ds.rb loaded\n");
        } else {
            DBGLOG("[STUBS] WARN: ios_compat_3ds.rb not found\n");
        }
    }

	extern const char *COMPAT_STUBS_RUBY;  // declarado em compat_stubs.h
	mrb_load_string(mrb, COMPAT_STUBS_RUBY);
	if (mrb->exc) {
		show_error(mrb, "compat_stubs");
		mrb->exc = 0;
	}
	/* NOTA: BitmapWrapper NÃO existe aqui ainda -- é criado pelos scripts do jogo.
	 * O fix MRB_TT_DATA tem de ser aplicado DEPOIS de run_rmxp_scripts(). */
    for (int i = 0; i < num_scripts; i++) {
        mrb_value entry = mrb_ary_entry(scripts_array, i);
        /* PROGRESSO VISIVEL: a cada 25 scripts (e no 1o), uma linha com a
         * contagem. Com o log line-buffered, isto prova que o carregamento
         * esta' a AVANCAR (vs travado). Se o log parar em "script 150/376",
         * sabes que travou no script ~150. */
        if (i == 0 || (i % 25) == 0) {
            DBGLOG("[SCRIPTS] progresso: %d/%d...\n", i, num_scripts);
        }
        if (!mrb_array_p(entry) || RARRAY_LEN(entry) < 3) {
            DBGLOG("[ERROR] Script %d is not a valid array\n", i);
            continue;
        }

        /* id_val (entry[0]) não é usado neste path — suprimir warning */
        (void)mrb_ary_entry(entry, 0);
        mrb_value name_val  = mrb_ary_entry(entry, 1);
        mrb_value code_val  = mrb_ary_entry(entry, 2);

        if (!mrb_string_p(name_val) || !mrb_string_p(code_val)) {
            DBGLOG("[ERROR] Script %d has non-string name/code\n", i);
            continue;
        }

        const char* script_name = RSTRING_PTR(name_val);
        const char* compressed  = RSTRING_PTR(code_val);
        int compressed_len      = RSTRING_LEN(code_val);

        /* PROFILING por fase (descomprimir / patch / compilar / executar) — o
         * equivalente ao BREAKDOWN dos rxdata, para scripts. */
        uint64_t ph_t0 = prof_tick();

        // Descomprimir com zlib
        uLongf dest_len = compressed_len * 4;
        std::vector<Bytef> decomp(dest_len);
        int ret = uncompress(decomp.data(), &dest_len, (const Bytef*)compressed, compressed_len);
        while (ret == Z_BUF_ERROR) {
            dest_len *= 2;
            decomp.resize(dest_len);
            ret = uncompress(decomp.data(), &dest_len, (const Bytef*)compressed, compressed_len);
        }
        if (ret != Z_OK) {
            DBGLOG("[ERROR] uncompress failed for script '%s'\n", script_name);
            continue;
        }
        decomp.resize(dest_len);
        double prof_decomp_ms = prof_ticks_to_ms(prof_tick() - ph_t0);

        // Aplicar patches (Main, etc.)
        uint64_t ph_t1 = prof_tick();
        patch_script(decomp, (uLong)decomp.size(), script_name);
        double prof_patch_ms = prof_ticks_to_ms(prof_tick() - ph_t1);

        uint64_t script_bytes = (uint64_t)decomp.size();
        int    prof_hit = 0;
        double prof_exec_ms = 0.0, prof_compile_ms = 0.0;

#if MKXP_BYTECODE_CACHE
        // --- CACHE DE BYTECODE -------------------------------------------
        // Chave por conteudo JA-PATCHADO. 1a vez: compila + grava .mrb.
        // Seguintes: carrega bytecode (rapido). Fallback seguro a nstring.
        char cache_path[320];
        mkxp_cache_key(script_name, decomp.data(), decomp.size(),
                       cache_path, sizeof(cache_path));

        uint64_t ph_t2 = prof_tick();
        bool done = mkxp_cache_try_load(mrb, cache_path, script_name);
        if (done) {
            /* HIT: o tempo foi so' executar o bytecode. */
            prof_hit = 1;
            prof_exec_ms = prof_ticks_to_ms(prof_tick() - ph_t2);
        } else {
            /* MISS: compilar + gravar + executar. Aqui o tempo agrega compilacao
             * e execucao; o relatorio mostra-o como compile (otimizavel se a cache
             * gravasse). */
            done = mkxp_cache_compile_run(mrb, (const char*)decomp.data(),
                                          decomp.size(), cache_path, script_name);
            prof_compile_ms = prof_ticks_to_ms(prof_tick() - ph_t2);
        }
        if (!done) {
            // ultimo recurso: caminho antigo (compila+executa, sem cache)
            uint64_t ph_t3 = prof_tick();
            mrb_value result = mrb_load_nstring(mrb, (const char*)decomp.data(), decomp.size());
            (void)result;
            if (mrb->exc) {
                show_error(mrb, script_name);
                mrb->exc = 0;   // continuar mesmo com erro
            }
            prof_compile_ms += prof_ticks_to_ms(prof_tick() - ph_t3);
        }
        // --- /CACHE DE BYTECODE ------------------------------------------
#else
        // Caminho antigo (cache desligada): compila+executa em runtime.
        uint64_t ph_t2 = prof_tick();
        mrb_value result = mrb_load_nstring(mrb, (const char*)decomp.data(), decomp.size());
        (void)result;
        if (mrb->exc) {
            show_error(mrb, script_name);
            mrb->exc = 0;
        }
        prof_compile_ms = prof_ticks_to_ms(prof_tick() - ph_t2);
#endif
        /* Registar este script no profiling. */
        prof_script_record(script_name, script_bytes, prof_hit,
                           prof_decomp_ms, prof_patch_ms, prof_exec_ms, prof_compile_ms);
    }

    /* Relatorio agregado dos 376 scripts: hits/miss, tempo por fase, top 10
     * mais lentos. Responde se a cache funciona e onde vao os ~95s. */
    prof_report_scripts("scripts-base");
    prof_scripts_reset();   /* limpar para a fase de plugins reusar a estrutura */

    mrb_gc_unregister(mrb, scripts_array);
    DBGLOG("[SCRIPTS] Loaded %d scripts\n", num_scripts);

    /* CARREGAR PLUGINS (PluginScripts.rxdata) -- DEPOIS dos 376 scripts base.
     * Os plugins dependem das classes base (Sprite, Viewport, GameData, etc),
     * por isso tem de correr aqui. E' isto que define ModularTitleScreen e a
     * tela bonita do Solar Eclipse. Sem isto o jogo cai na tela base. */
    /* [DEFINED-EARLY] Instalar defined?/const_missing/MKXP_UNDEF ANTES dos plugins.
     * Os plugins (ex: Modular Title Screen via eval, e 003_Dialogue_Specific da
     * intro) usam defined? em RUNTIME. O [DEFINED-FIX] do bloco MFD so corre
     * DEPOIS dos plugins -> tarde demais -> NoMethodError 'defined?' e a fala da
     * intro nunca e criada. Mesma definicao, instalada aqui antes dos plugins.
     * (O [DEFINED-FIX] posterior reinstala de forma idempotente -- inofensivo.) */
    {
        static const char *defined_early = R"DEFEARLY(
begin
  unless Object.const_defined?(:MKXP_UNDEF)
    undef_obj = Object.new
    class << undef_obj
      def inspect; "#<undef>"; end
      def to_s; ""; end
      def to_str; ""; end
      def nil?; true; end
      def empty?; true; end
      def method_missing(m, *a, &b); self; end
      def respond_to_missing?(m, p = false); true; end
      def coerce(o); [o.is_a?(Numeric) ? o : 0, 0]; end
      def ==(o); o.nil? || o.equal?(self); end
      def !; true; end
    end
    Object.const_set(:MKXP_UNDEF, undef_obj)
  end
  module ::Kernel
    def defined?(*args)
      return nil if args.empty?
      args[0].equal?(MKXP_UNDEF) ? nil : "expression"
    end
  end
  class ::Object
    def self.const_missing(name)
      MKXP_UNDEF
    end
  end
rescue
end
)DEFEARLY";
        mrb_load_string(mrb, defined_early);
        if (mrb->exc) { show_error(mrb, "defined_early"); mrb->exc = 0; }
        else DBGLOG("[DEFINED-EARLY] defined?/const_missing instalados ANTES dos plugins\n");
    }

    run_plugin_scripts(mrb, game_path);

    /* Carregar debug_probe.rb DEPOIS dos scripts do jogo.
     * CRITICO: tem de ser aqui. Scene_Map, mainFunctionDebug e todas as
     * classes do jogo so existem depois dos 376 scripts terem sido
     * executados. Se carregado antes (posicao anterior), os patches do
     * probe tentavam alias/respond_to? em classes inexistentes e falhavam
     * silenciosamente nos blocos rescue -- aparecia "[PROBE] carregado"
     * mas nenhum patch ficava activo.
     * Para desactivar: apagar ou renomear para debug_probe.rb.off      */
    {
        FILE *sf = fopen("sdmc:/mkxp/debug_probe.rb", "rb");
        if (sf) {
            fseek(sf, 0, SEEK_END); long ssz = ftell(sf); fseek(sf, 0, SEEK_SET);
            char *sbuf = (char*)malloc(ssz + 1);
            fread(sbuf, 1, ssz, sf); fclose(sf); sbuf[ssz] = 0;
            mrb_load_string(mrb, sbuf); free(sbuf);
            if (mrb->exc) { show_error(mrb, "debug_probe"); mrb->exc = 0; }
            else DBGLOG("[PROBE] debug_probe.rb carregado\n");
        } else {
            DBGLOG("[PROBE] debug_probe.rb nao encontrado -- modo normal\n");
        }
    }

    /* Carregar smoke_test.rb (define o modulo SmokeTest). NAO corre aqui --
     * a execucao (SmokeTest.run_all) e' feita mais tarde, no Scene_DebugIntro,
     * quando GameData e todas as classes ja estao prontos. Aqui so' define.
     * Para desactivar: apagar/renomear smoke_test.rb. */
    {
        FILE *kf = fopen("sdmc:/mkxp/smoke_test.rb", "rb");
        if (kf) {
            fseek(kf, 0, SEEK_END); long ksz = ftell(kf); fseek(kf, 0, SEEK_SET);
            char *kbuf = (char*)malloc(ksz + 1);
            fread(kbuf, 1, ksz, kf); fclose(kf); kbuf[ksz] = 0;
            mrb_load_string(mrb, kbuf); free(kbuf);
            if (mrb->exc) { show_error(mrb, "smoke_test"); mrb->exc = 0; }
            else DBGLOG("[SMOKE] smoke_test.rb carregado (corre no Scene_DebugIntro)\n");
        } else {
            DBGLOG("[SMOKE] smoke_test.rb nao encontrado -- sem smoke test\n");
        }
    }
}

/* base path do jogo (ex.: "sdmc:/mkxp/game"), usado pelo load_data real */
static char g_game_base[300] = {0};
/* Caminho do ficheiro de cache rxdata pendente de gravacao (preenchido na
 * leitura do load_data quando ha MISS; usado no fim para gravar o resultado). */
char g_rxcache_pending[600] = {0};

/* Cache de mapas inexistentes (anti-I/O-storm). Plugins sondam Map000 e outros
 * ids invalidos todos os frames; sem isto cada sonda fazia um fopen() falhado.
 * Guardamos os nomes (curtos, ex "Data/Map000.rxdata") numa lista fixa. */
static char s_bad_maps[64][96];
static int  s_bad_maps_n = 0;
static bool mkxp_is_known_bad_map(const char *fname) {
    if (!fname) return false;
    if (!strstr(fname, "Map") || !strstr(fname, ".rxdata")) return false;
    for (int i = 0; i < s_bad_maps_n; i++)
        if (strcmp(s_bad_maps[i], fname) == 0) return true;
    return false;
}
static void mkxp_register_bad_map(const char *fname) {
    if (!fname) return;
    if (!strstr(fname, "Map") || !strstr(fname, ".rxdata")) return;
    if (mkxp_is_known_bad_map(fname)) return;
    if (s_bad_maps_n < 64) {
        snprintf(s_bad_maps[s_bad_maps_n], 96, "%s", fname);
        s_bad_maps_n++;
    }
}

int binding_3ds_run(const char *game_path) {
    dbglog_open();
    DBGLOG("binding_3ds_run: %s\n", game_path);
    snprintf(g_game_base, sizeof(g_game_base), "%s", game_path ? game_path : "");

    /* CRÍTICO: inicializar render targets ANTES de qualquer binding.
     * gfxInitDefault + C3D_Init + C2D_Init + C2D_Prepare já foram chamados
     * em main.cpp::init_services(). display_3ds_init() cria os RenderTargets
     * (s_top / s_bottom). Sem isto, display_3ds_begin_frame chama
     * C2D_TargetClear(nullptr) → comportamento indefinido → ecrã preto. */
    display_3ds_init();
    DBGLOG("[DISPLAY] display_3ds_init OK\n");

    /* Inicializar o pool allocator ANTES de abrir o mruby. Todas as alocacoes
     * do mruby passam a vir dos pools (alloc/free O(1)) em vez do malloc do
     * sistema -- e' isto que elimina o O(n^2) do boot no 3DS (ver explicacao
     * na definicao de mkxp_pool, no topo do ficheiro).
     * MARCADORES DETALHADOS: cada passo loga ANTES de comecar, para que se o
     * 3DS travar saibas EXATAMENTE em qual passo parou (o log e' line-buffered,
     * por isso a ultima linha do ficheiro = onde parou). */
    DBGLOG("[BOOT] passo 1/4: mkxp_pool::init()...\n");
    mkxp_pool::init();
#if USE_POOL_ALLOCATOR
    DBGLOG("[BOOT] passo 1/4 OK. passo 2/4: mrb_open_allocf (criar mruby COM pool)...\n");
    mrb_state *mrb = mrb_open_allocf(mkxp_pool::allocf, NULL);
    if (!mrb) {
        /* FALLBACK: se o pool allocator falhar no 3DS, tentar o mrb_open()
         * normal. O boot fica mais lento mas pelo menos arranca. */
        DBGLOG("[BOOT] passo 2/4 FALHOU com pool -- a tentar mrb_open() normal (fallback)...\n");
        mrb = mrb_open();
        if (!mrb) { DBGLOG("[ERROR] mrb_open (fallback) tambem falhou\n"); dbglog_close(); return -1; }
        DBGLOG("[BOOT] passo 2/4 OK via FALLBACK (mrb_open normal -- boot mais lento)\n");
    } else {
        DBGLOG("[BOOT] passo 2/4 OK (mruby criado com pool, allocf chamado %ld vezes)\n",
               mkxp_pool::s_allocf_calls);
    }

    /* O marshal tem um cache de resolucao de classe (nome -> RClass*) que acelera
     * o load de TODOS os ficheiros .rxdata/.dat em ~150x. Esses ponteiros so' sao
     * validos para ESTA sessao mruby; limpar o cache agora garante que uma nova
     * sessao (reiniciar/trocar de jogo) nunca reutiliza ponteiros antigos. */
    marshal_class_cache_reset();   /* limpa o cache de classes (ver declaracao no topo) */

    /* Expor o caminho base do jogo ao Ruby como $__mkxp_game_base. O pbResolveBitmap
     * do Essentials usa FileTest.exist? com caminhos RELATIVOS, que falham no 3DS
     * (o CWD nao e' a pasta do jogo) -> nenhum windowskin/imagem resolve -> caixa
     * de texto invisivel. Com esta global, __mkxp_try_bitmap pode testar o caminho
     * COMPLETO (g_game_base + "/" + rel), tal como o loader C++ ja' faz. */
    {
        mrb_value gb = mrb_str_new_cstr(mrb, g_game_base);
        mrb_gv_set(mrb, mrb_intern_lit(mrb, "$__mkxp_game_base"), gb);
        DBGLOG("[WSKIN] $__mkxp_game_base = '%s' (p/ resolver imagens por caminho completo)\n", g_game_base);
    }
    DBGLOG("[POOL] allocator de pools ativo (alloc O(1), evita malloc O(n) do 3DS)\n");
#else
    /* TESTE DE ISOLAMENTO: mrb_open() normal, SEM o pool. Se o jogo arrancar
     * assim (e travava com o pool), confirma que o problema esta' no pool.
     * O boot sera' mais lento (malloc O(n) do 3DS no CommonEvents etc), mas
     * permite chegar ao jogo e validar tudo o resto (audio, ecra, etc). */
    DBGLOG("[BOOT] passo 1/4 OK. passo 2/4: mrb_open() NORMAL (pool DESLIGADO p/ teste)...\n");
    mrb_state *mrb = mrb_open();
    if (!mrb) { DBGLOG("[ERROR] mrb_open failed\n"); dbglog_close(); return -1; }
    DBGLOG("[BOOT] passo 2/4 OK (mruby criado SEM pool -- boot mais lento mas estavel)\n");
    DBGLOG("[POOL] DESLIGADO (USE_POOL_ALLOCATOR=0) -- a usar malloc do sistema\n");
#endif


    /* ── CHECK DE RECURSOS CRITICOS (diagnostico/prevencao) ────────────────────
     * Da analise do Graphics.zip (18.661 ficheiros): alguns recursos que os
     * scripts pedem NAO existem. Verificamos cedo e documentamos no log, para
     * saberes imediatamente o que vai aparecer magenta/sem imagem e porque.
     * Isto NAO bloqueia nada -- apenas regista. Caminhos relativos a' raiz do
     * jogo (sdmc:/mkxp/game/). */
    {
        struct ResCheck { const char *path; const char *nota; };
        static const ResCheck checks[] = {
            { "sdmc:/mkxp/game/Graphics/System/Window.png",      "windowskin base (REDIRECIONADO p/ Windowskins/001-Blue01)" },
            { "sdmc:/mkxp/game/Graphics/Windowskins/001-Blue01.png", "windowskin fallback (deve existir)" },
            { "sdmc:/mkxp/game/Graphics/System/Iconset.png",     "icones de itens <img=...> (sem alternativa -> magenta)" },
            { "sdmc:/mkxp/game/Graphics/Pictures/loadbg.png",    "fundo do menu load (deve existir)" },
            { "sdmc:/mkxp/game/Graphics/Pictures/loadbg_4.png",  "fundo do menu New Game (deve existir)" },
            { "sdmc:/mkxp/game/Data/CommonEvents.rxdata",        "eventos comuns (critico p/ intro)" },
        };
        DBGLOG("[RESCHECK] a verificar recursos criticos conhecidos...\n");
        for (size_t i = 0; i < sizeof(checks)/sizeof(checks[0]); i++) {
            FILE *rf = fopen(checks[i].path, "rb");
            if (rf) {
                fclose(rf);
                DBGLOG("[RESCHECK]   OK    %s\n", checks[i].path);
            } else {
                DBGLOG("[RESCHECK]   FALTA %s  -- %s\n", checks[i].path, checks[i].nota);
            }
        }
        DBGLOG("[RESCHECK] concluido (FALTA = sera' logado [BMP|MISS]/[BMP|REDIR] quando pedido)\n");
    }

    /* ── AJUSTE DO GC (anti-O(n^2)) ────────────────────────────────────────
     * CAUSA RAIZ da lentidao do boot (CommonEvents 220s, Animations 18s p/
     * 43KB, etc): o mruby ja' usa GC geracional, mas com interval_ratio=200
     * dispara major-GCs frequentes que varrem a heap viva INTEIRA. A heap
     * cresce durante o boot (376 scripts + 54 plugins + data = centenas de
     * milhares de objetos), por isso cada varrimento fica mais caro -> O(n^2).
     *
     * PROBLEMA CRITICO descoberto pelo profiling: durante o boot o GC estava
     * GERACIONAL, e o modo geracional IGNORA o interval_ratio (dispara um minor
     * GC a cada ~1024 allocs, threshold FIXO). Por isso pôr interval_ratio=8000
     * no inicio NAO bastava -- o GC continuava a correr a cada 1024 objetos e a
     * varrer a heap viva. So' no FIM do boot e' que se punha NAO-geracional.
     *
     * CORRECAO: pôr NAO-geracional + interval_ratio gigante JA' AQUI, no inicio
     * do boot (nao so' no fim). Assim os 376 scripts, os plugins e o 1o load de
     * GameData (species 7.7s, Animations 14s, moves, items...) correm todos com
     * o GC raro. Medido no mruby 3.2.0: criar objetos com a heap cheia e
     * interval_ratio=40000 e' ~2x mais rapido que com o default, e MUITO mais
     * rapido que geracional. SEGURO: nao desliga o GC (gc.disabled corrompia o
     * array de topo no 3DS) nem mexe no arena; so' torna o GC menos frequente.
     * O full_gc final (apos o boot, antes do 1o frame) compacta tudo. */
    int  gc_saved_interval = mrb->gc.interval_ratio;
    mrb_bool gc_saved_gen  = mrb->gc.generational;
    mrb->gc.generational   = FALSE;   /* CRITICO: geracional ignora interval_ratio e corre a cada 1024 allocs */
    mrb->gc.interval_ratio = 40000;   /* threshold gigante -> GC raro durante as cargas em massa do boot */
    DBGLOG("[GC] boot inicio: NAO-geracional, interval_ratio %d -> %d, generational %d -> 0 (anti-O(n^2))\n",
           gc_saved_interval, mrb->gc.interval_ratio, (int)gc_saved_gen);

    inputBindingInit(mrb);
    graphicsBindingInit(mrb);

    /* Registar aliases _3ds_internal para freeze e transition,
     * tal como graphicsBindingInit ja fez para update_3ds_internal.
     * O rebind pos-scripts vai redirecionar os metodos Ruby para estes. */
    {
        static const char *aliases_ruby =
            "module Graphics\n"
            "  class << self\n"
            "    alias transition_3ds_internal transition\n"
            "    alias freeze_3ds_internal     freeze\n"
            "  end\n"
            "end\n";
        mrb_load_string(mrb, aliases_ruby);
        if (mrb->exc) {
            DBGLOG("[ALIAS] ERRO ao criar aliases _3ds_internal\n");
            mrb->exc = 0;
        } else {
            DBGLOG("[ALIAS] transition_3ds_internal e freeze_3ds_internal OK\n");
        }

        /* ── PICFIX-LATE: pictures store robusto ───────────────────────────────
         * CAUSA-RAIZ do menu lento: o jogo faz $game_screen.pictures[n].origin/
         * .zoom_x/.angle/... Quando pictures[] devolvia nil/Integer, cada acesso
         * caía no method_missing universal MILHARES de vezes por frame (no log:
         * Integer#origin 1821x e a subir) -> menu a 1-8 FPS.
         * A 1a tentativa (PICFIX cedo) FALHOU: corria antes de 'dbg' existir e o
         * alias_method :pictures rebentava (rescue engolia tudo). Aqui corre TARDE
         * -- 'dbg' e as classes do jogo ja' existem -- e NAO depende do pictures
         * original (define de fresco). Resultado: pictures[n] devolve sempre um
         * Game_Picture valido, custo O(1), zero method_missing. */
        static const char *picfix_ruby = R"PICRUBY(
# __pf_log: log seguro que NUNCA rebenta (nao depende de 'dbg', que no 3DS
# pode cair no method_missing universal e levantar excecao -> o erro escapava
# o rescue e o C++ via mrb->exc setado -> "[PICFIX-LATE] ERRO" mesmo com o
# patch aplicado. Aqui usamos um log proprio protegido.
__pf_log = lambda do |msg|
  begin
    MKXPDebug.log(msg.to_s)
  rescue
    nil
  end
end

begin
  # 1) MKXPPicture: a NOSSA classe, com TODOS os atributos RGSS garantidos.
  unless Object.const_defined?(:MKXPPicture)
    klass = Class.new do
      attr_accessor :number, :name, :origin, :x, :y, :zoom_x, :zoom_y,
                    :opacity, :blend_type, :tone, :angle
      def initialize(n = 0)
        @number = n; @name = ""; @origin = 0; @x = 0.0; @y = 0.0
        @zoom_x = 100.0; @zoom_y = 100.0; @opacity = 255.0
        @blend_type = 1; @angle = 0.0
        @tone = (Tone.new(0,0,0,0) rescue nil)
      end
      def show(name, origin, x, y, zx, zy, op, bt)
        @name=name; @origin=origin; @x=x; @y=y
        @zoom_x=zx; @zoom_y=zy; @opacity=op; @blend_type=bt; @angle=0.0
      end
      def move(dur, origin, x, y, zx, zy, op, bt)
        @origin=origin; @x=x; @y=y; @zoom_x=zx; @zoom_y=zy; @opacity=op; @blend_type=bt
      end
      def rotate(s); @rotate_speed=s; end
      def start_tone_change(t, d); @tone=t; end
      def erase; @name=""; end
      def update; end
    end
    Object.const_set(:MKXPPicture, klass)
    __pf_log.call("[PICFIX-LATE] MKXPPicture criada (atributos RGSS garantidos)")
  end

  # 2) Store que NUNCA devolve nil: cria MKXPPicture a pedido.
  unless Object.const_defined?(:MKXPPictureStore)
    store = Class.new do
      def initialize; @h = {}; end
      def [](i); @h[i] ||= MKXPPicture.new(i); end
      def []=(i, v); @h[i] = v; end
      def each(&b); @h.values.each(&b); end
      def size; @h.size; end
      def length; @h.size; end
    end
    Object.const_set(:MKXPPictureStore, store)
  end

  # 3) Game_Screen#pictures -> store. Define de FRESCO (sem alias, sem defined?).
  if Object.const_defined?(:Game_Screen)
    Game_Screen.class_eval do
      def pictures
        @__mkxp_picstore ||= MKXPPictureStore.new
      end
    end
    __pf_log.call("[PICFIX-LATE] Game_Screen#pictures -> store robusto OK")
  end

  # 4) Se $game_screen ja' existe mas nao e' Game_Screen (stub), garante pictures.
  gs_is_real = begin
    $game_screen.is_a?(Game_Screen)
  rescue
    false
  end
  if $game_screen && !gs_is_real
    begin
      class << $game_screen
        def pictures
          @__mkxp_picstore ||= MKXPPictureStore.new
        end
      end
    rescue
      nil
    end
  end
rescue => e
  __pf_log.call("[PICFIX-LATE] falhou (rescue)")
end
)PICRUBY";
        /* limpa qualquer exc residual de blocos anteriores antes de avaliar,
         * para o teste a seguir refletir SO' o resultado do picfix. */
        mrb->exc = 0;
        mrb_load_string(mrb, picfix_ruby);
        if (mrb->exc) {
            /* Houve exc: limpa e verifica se mesmo assim o patch ficou aplicado
             * (um rescue interno pode ter deixado exc setado sem impedir o
             * patch). So' reportamos ERRO real se Game_Screen NAO ganhou
             * o metodo pictures do nosso store. */
            mrb->exc = 0;
            int ok = 0;
            if (mrb_class_defined(mrb, "MKXPPictureStore")) ok = 1;
            if (ok) {
                DBGLOG("[PICFIX-LATE] aplicado OK (exc residual ignorado)\n");
            } else {
                DBGLOG("[PICFIX-LATE] ERRO ao aplicar (exc) -- pictures pode ficar lento\n");
            }
            mrb->exc = 0;
        } else {
            DBGLOG("[PICFIX-LATE] aplicado OK\n");
        }

    }
    etcBindingInit(mrb);
    bitmapBindingInit(mrb);
    spriteBindingInit(mrb);
	tilemapBindingInit(mrb);

    /* ---------------------------------------------------------------
     * FIX5 (CRITICO): load_data real.
     *
     * O rgss_stubs.rb so define `def load_data(path); []; end` SE o
     * binding C nao registar load_data primeiro ("unless respond_to?").
     * Como nunca era registado, TODAS as chamadas devolviam [] -> os
     * tilesets e mapas vinham vazios (tileset_name='', @map/data=Array
     * vazio), o grafico do tileset nunca carregava e o mapa compunha
     * nada -> ecra preto. (A correccao do RPG::Table no marshal.cpp e
     * necessaria mas inutil enquanto load_data nem sequer abrir o ficheiro.)
     *
     * Implementacao RGSS real: resolve <base>/<path> e desserializa via
     * marshalLoadInt (que ja reconstroi RPG::Table nativamente). Registado
     * ANTES de run_rmxp_scripts -> o stub do rgss_stubs.rb e ignorado.
     *
     * CACHE RXDATA (MKXP_RXDATA_CACHE): a 1a corrida grava o resultado ja
     * desserializado em sdmc:/mkxp/cache/rxdata/<chave>.mrx via marshalDumpInt;
     * as seguintes leem desse ficheiro e SALTAM o .rxdata da fonte. A chave
     * inclui tamanho+mtime do rxdata original, por isso se editares os Data/
     * a cache invalida-se sozinha. Logs [RXCACHE] permitem medir o ganho.
     * Poe MKXP_RXDATA_CACHE a 0 para desligar e comparar tempos. */
    mrb_define_method(mrb, mrb->kernel_module,
        "load_data",
        [](mrb_state *mrb2, mrb_value) -> mrb_value {
            const char *fname = 0;
            mrb_get_args(mrb2, "z", &fname);

            char path[512];
            if (fname && (fname[0]=='/' || strstr(fname, ":/")))
                snprintf(path, sizeof(path), "%s", fname);            /* ja absoluto */
            else
                snprintf(path, sizeof(path), "%s/%s", g_game_base, fname ? fname : "");

            /* --- cache de mapas INEXISTENTES (anti-I/O-storm + anti-log-spam)
             * Plugins ("Water bubles" / "Following Pokemon EX") sondam Map000
             * (e outros ids invalidos) TODOS os frames. Cada sonda fazia um
             * fopen() falhado E um DBGLOG (escrita no SD, lentissima no 3DS)
             * -> centenas de escritas/frame -> 2700 ms/frame. Verificamos ANTES
             * de qualquer log: a 1a sonda loga e regista; as seguintes saem
             * AQUI, sem log e sem tocar no disco. (helpers a nivel de ficheiro) */
            if (mkxp_is_known_bad_map(fname)) {
                g_rxcache_pending[0] = 0;
                return mrb_nil_value();
            }

            DBGLOG("[load_data] req='%s' -> '%s'\n", fname ? fname : "(nil)", path);

#if MKXP_RXDATA_CACHE
            /* --- tentativa de leitura da cache de rxdata -------------------
             * Chave: nome sanitizado + tamanho + mtime do ficheiro fonte.
             * Se a fonte nao existir (ex.: path do romfs), saltamos a cache. */
            g_rxcache_pending[0] = 0;
            {
                struct stat st_src;
                if (fname && stat(path, &st_src) == 0) {
                    char safe[256]; size_t si = 0;
                    for (const char *p = fname; *p && si < sizeof(safe)-1; ++p)
                        safe[si++] = (*p=='/'||*p=='\\'||*p==':') ? '_' : *p;
                    safe[si] = 0;
                    snprintf(g_rxcache_pending, sizeof(g_rxcache_pending),
                             "sdmc:/mkxp/cache/rxdata/%s.%lu.%lu.v2.mrx",
                             safe, (unsigned long)st_src.st_size,
                             (unsigned long)st_src.st_mtime);

                    FILE *cf = fopen(g_rxcache_pending, "rb");
                    if (cf) {
                        DBGLOG("[RXCACHE] HIT '%s'\n", fname);
                        mrb_value cv = mrb_nil_value();
                        bool ok = true;
                        /* IMPORTANTE: marshalLoadInt NAO lanca excecao C++ quando o
                         * marshal falha -- faz raise mruby (seta mrb2->exc) e devolve
                         * nil. Por isso o catch(...) sozinho NAO deteta cache corrompida.
                         * Temos de inspecionar mrb2->exc explicitamente E limpa-lo, senao
                         * o erro pendente contamina o estado e rebenta frames depois
                         * (era a causa do "New Game sai do jogo": Map001.mrx truncado). */
                        mrb2->exc = NULL;                 /* limpa estado antes de tentar */
                        try { cv = marshalLoadInt(mrb2, cf); }
                        catch (...) { ok = false; }       /* defensivo: throw improvavel */
                        fclose(cf);
                        if (mrb2->exc) {                  /* houve raise mruby -> cache ma' */
                            ok = false;
                            mrb2->exc = NULL;             /* descarta o erro pendente */
                        }
                        if (ok && !mrb_nil_p(cv)) {
                            DBGLOG("[RXCACHE]   load OK (sem re-parse da fonte)\n");
                            g_rxcache_pending[0] = 0;   /* ja' resolvido */
                            return cv;
                        }
                        DBGLOG("[RXCACHE]   CORROMPIDA '%s' -> apaga e recarrega da fonte\n",
                               g_rxcache_pending);
                        remove(g_rxcache_pending);
                        /* g_rxcache_pending fica setado de proposito: ao recarregar da
                         * fonte abaixo, o bloco de gravacao regenera o .mrx correto. */
                    } else {
                        DBGLOG("[RXCACHE] MISS '%s' -> grava apos load\n", fname);
                    }
                }
            }
#endif

            FILE *fp = fopen(path, "rb");
            const char *src = "game";
            if (!fp && fname) {
                /* fallback: ficheiros embebidos no romfs do emulador */
                char rpath[512];
                snprintf(rpath, sizeof(rpath), "romfs:/%s", fname);
                fp = fopen(rpath, "rb");
                src = "romfs";
            }
            if (!fp) {
                /* Mapas inexistentes (MapNNN.rxdata) NAO devem rebentar: plugins
                 * como "Following Pokemon EX" (getTerrainTag) e "Water bubles" sondam
                 * ids de mapa invalidos de proposito (ex: Map000). Rebentar aqui
                 * matava o Spriteset_Global inteiro -> ecra preto apos New Game.
                 * Para mapas em falta devolvemos nil (o codigo Ruby ja' tolera nil);
                 * para qualquer outro ficheiro essencial mantemos o raise, para nao
                 * mascarar bugs reais de dados em falta. */
                bool is_map = fname &&
                    (strstr(fname, "Map") != NULL) &&
                    (strstr(fname, ".rxdata") != NULL);
                if (is_map) {
                    DBGLOG("[load_data] mapa inexistente '%s' -> nil (sonda de plugin, ignorado)\n",
                           path);
                    mkxp_register_bad_map(fname);   /* proxima sonda: nil sem I/O */
                    g_rxcache_pending[0] = 0;   /* nao gravar cache de um nil */
                    return mrb_nil_value();
                }
                DBGLOG("[load_data] ERRO: ficheiro nao encontrado '%s'\n", path);
                mrb_raise(mrb2, mrb_class_get(mrb2, "RuntimeError"),
                          "load_data: cannot open file");
            }
            { long pz; fseek(fp,0,SEEK_END); pz=ftell(fp); fseek(fp,0,SEEK_SET);
              DBGLOG("[load_data] aberto (%s, %ld bytes)\n", src, pz); }

            mrb_value v = mrb_nil_value();
            try {
                v = marshalLoadInt(mrb2, fp);
            } catch (...) {
                fclose(fp);
                DBGLOG("[load_data] ERRO: marshal falhou em '%s' (ver [marshal] acima)\n", path);
                mrb_raise(mrb2, mrb_class_get(mrb2, "RuntimeError"),
                          "load_data: marshal error");
            }
            fclose(fp);

            /* diagnostico do resultado: tipo + forma */
            if (mrb_array_p(v)) {
                int n = (int)RARRAY_LEN(v);
                DBGLOG("[load_data] '%s' -> Array(len=%d)\n", fname ? fname : "?", n);
                for (int k = 0; k < n && k < 4; k++) {
                    mrb_value e = mrb_ary_entry(v, k);
                    if (!mrb_nil_p(e)) {
                        const char *en = mrb_class_name(mrb2, mrb_class(mrb2, e));
                        DBGLOG("[load_data]   [%d] = %s\n", k, en ? en : "?");
                    }
                }
            } else if (mrb_hash_p(v)) {
                DBGLOG("[load_data] '%s' -> Hash\n", fname ? fname : "?");
            } else {
                const char *cn = mrb_class_name(mrb2, mrb_class(mrb2, v));
                DBGLOG("[load_data] '%s' -> %s\n", fname ? fname : "?", cn ? cn : "?");
            }

#if MKXP_RXDATA_CACHE
            /* --- grava o resultado na cache de rxdata (apos load da fonte) -
             * Escreve para um ficheiro temporario e so' o renomeia no fim,
             * para que uma escrita interrompida nao deixe cache corrompida. */
            if (g_rxcache_pending[0] && !mrb_nil_p(v)) {
                char tmp[640];
                snprintf(tmp, sizeof(tmp), "%s.tmp", g_rxcache_pending);
                FILE *wf = fopen(tmp, "wb");
                if (!wf) {
                    /* RECUPERACAO: a pasta sdmc:/mkxp/cache/rxdata/ pode nao existir
                     * (apagada manualmente, ou mkdir do boot falhou). Sem isto, TODOS
                     * os load_data ficam MISS para sempre -> cada arranque/mapa re-
                     * processa do zero (cache morto = lentidao permanente). Garante a
                     * arvore de pastas e tenta abrir o tmp uma segunda vez. */
                    mkdir("sdmc:/mkxp",              0777);
                    mkdir("sdmc:/mkxp/cache",        0777);
                    mkdir("sdmc:/mkxp/cache/rxdata", 0777);
                    wf = fopen(tmp, "wb");
                    if (wf) DBGLOG("[RXCACHE] pasta recriada -> tmp abre agora OK\n");
                    else    DBGLOG("[RXCACHE] tmp falha mesmo apos mkdir: '%s'\n", tmp);
                }
                if (wf) {
                    bool ok = true;
                    bool complete = false;
                    try { complete = marshalDumpInt(mrb2, wf, v); }
                    catch (...) { ok = false; }
                    fclose(wf);
                    if (ok && complete) {
                        remove(g_rxcache_pending);          /* Windows/3DS: rename falha se destino existe */
                        if (rename(tmp, g_rxcache_pending) == 0)
                            DBGLOG("[RXCACHE] gravado '%s' (proxima corrida le da cache)\n",
                                   g_rxcache_pending);
                        else { DBGLOG("[RXCACHE] rename falhou -> apaga tmp\n"); remove(tmp); }
                    } else if (ok && !complete) {
                        /* dump tinha tipos nao-serializaveis (Tables/Data). Gravar
                         * isto daria cache corrompido -> EOF ao reler -> jogo fecha.
                         * Descartamos o tmp; este ficheiro le-se sempre da fonte. */
                        DBGLOG("[RXCACHE] '%s' tem tipos nao-serializaveis -> cache IGNORADO (le da fonte)\n",
                               g_rxcache_pending);
                        remove(tmp);
                    } else {
                        DBGLOG("[RXCACHE] dump falhou -> apaga tmp\n");
                        remove(tmp);
                    }
                } else {
                    DBGLOG("[RXCACHE] nao consegui abrir tmp para escrita\n");
                }
                g_rxcache_pending[0] = 0;
            }
#endif
            return v;
        },
        MRB_ARGS_REQ(1));
    DBGLOG("[FIX5] load_data real registado em Kernel OK\n");

    /* ---------------------------------------------------------------
     * FIX: rand/oldRand recursion (SystemStackError)
     *
     * Registar __native_rand__ como wrapper C puro ANTES dos scripts.
     * O bloco Ruby em check_entry_methods() redefine rand e oldRand
     * para delegarem para __native_rand__, quebrando a recursao.
     * NAO usamos khash/kh_get (internals do mruby nao exportadas).
     * --------------------------------------------------------------- */
    mrb_define_module_function(mrb, mrb->kernel_module,
        "__native_rand__",
        [](mrb_state *mrb2, mrb_value) -> mrb_value {
            mrb_value arg;
            mrb_int argc = mrb_get_argc(mrb2);
            if (argc == 0) {
                return mrb_float_value(mrb2,
                    (double)rand() / ((double)RAND_MAX + 1.0));
            }
            mrb_get_args(mrb2, "o", &arg);
            mrb_int n = mrb_integer_p(arg) ? mrb_integer(arg) : 1;
            if (n <= 0) n = 1;
            return mrb_int_value(mrb2, (mrb_int)(rand() % (int)n));
        },
        MRB_ARGS_OPT(1));
    DBGLOG("[rand-fix] __native_rand__ registado em Kernel OK\n");

    /* ---------------------------------------------------------------
     * FIX3: registar __force_bitmapwrapper_tt__ como função C pura.
     *
     * O bloco MFD em check_entry_methods reabre "class BitmapWrapper < Bitmap"
     * e isso repõe MRB_TT_OBJECT na RClass, deitando fora o que [FIX] e
     * [FIX2] fizeram. Esta função C é chamada do Ruby logo após o reopen,
     * garantindo que MRB_TT_DATA fica gravado no estado FINAL da classe,
     * depois de qualquer reopen que ainda possa ocorrer.
     * --------------------------------------------------------------- */
    mrb_define_module_function(mrb, mrb->kernel_module,
        "__force_bitmapwrapper_tt__",
        [](mrb_state *mrb2, mrb_value) -> mrb_value {
            mrb_sym bw_sym = mrb_intern_lit(mrb2, "BitmapWrapper");
            if (mrb_const_defined(mrb2, mrb_obj_value(mrb2->object_class), bw_sym)) {
                mrb_value bw_val = mrb_const_get(mrb2, mrb_obj_value(mrb2->object_class), bw_sym);
                if (mrb_class_p(bw_val)) {
                    struct RClass *bw = mrb_class_ptr(bw_val);
                    MRB_SET_INSTANCE_TT(bw, MRB_TT_DATA);
                    printf("[FIX3] __force_bitmapwrapper_tt__: MRB_TT_DATA forcado OK\n");
                    fflush(stdout);
                    return mrb_true_value();
                }
            }
            printf("[FIX3] __force_bitmapwrapper_tt__: BitmapWrapper nao encontrado!\n");
            fflush(stdout);
            return mrb_false_value();
        },
        MRB_ARGS_NONE());
    DBGLOG("[FIX3] __force_bitmapwrapper_tt__ registado em Kernel OK\n");

    /* ---------------------------------------------------------------
     * FIX4: registar __rebind_bitmapwrapper_init__ como função C pura.
     *
     * Chamada do Ruby (check_entry_methods) imediatamente após o reopen
     * "class BitmapWrapper < Bitmap" para re-registar bmp_init como
     * initialize. Garante que o bind C++ prevalece mesmo que o reopen
     * Ruby tenha instalado um initialize Ruby de assinatura errada.
     * --------------------------------------------------------------- */
    mrb_define_module_function(mrb, mrb->kernel_module,
        "__rebind_bitmapwrapper_init__",
        [](mrb_state *mrb2, mrb_value) -> mrb_value {
            mrb_sym bw_sym = mrb_intern_lit(mrb2, "BitmapWrapper");
            if (mrb_const_defined(mrb2, mrb_obj_value(mrb2->object_class), bw_sym)) {
                mrb_value bw_val = mrb_const_get(mrb2, mrb_obj_value(mrb2->object_class), bw_sym);
                if (mrb_class_p(bw_val)) {
                    struct RClass *bw = mrb_class_ptr(bw_val);
                    MRB_SET_INSTANCE_TT(bw, MRB_TT_DATA);
                    mrb_define_method(mrb2, bw, "initialize", bmp_init,
                                      MRB_ARGS_ANY());
                    MRB_SET_INSTANCE_TT(bw, MRB_TT_DATA);
                    printf("[FIX4] __rebind_bitmapwrapper_init__: bmp_init + MRB_TT_DATA OK\n");
                    fflush(stdout);
                    return mrb_true_value();
                }
            }
            printf("[FIX4] __rebind_bitmapwrapper_init__: BitmapWrapper nao encontrado!\n");
            fflush(stdout);
            return mrb_false_value();
        },
        MRB_ARGS_NONE());
    DBGLOG("[FIX4] __rebind_bitmapwrapper_init__ registado em Kernel OK\n");

    // Carregar scripts (ainda vazia, mas vamos preencher)
    DBGLOG("[BOOT] passo 3/4: run_rmxp_scripts (carregar+correr 376 scripts -- FASE MAIS LONGA)...\n");
    run_rmxp_scripts(mrb, game_path);
    DBGLOG("[BOOT] passo 3/4 OK (scripts carregados e executados).\n");

    /* ---------------------------------------------------------------
     * REINIT CRÍTICO: Bitmap + BitmapWrapper + Sprite
     *
     * Os 376 scripts do jogo reabrem "class BitmapWrapper < Bitmap" e
     * "class Sprite", sobrescrevendo os initialize C++ com versões Ruby
     * de aridade errada. Em mruby, um reopen Ruby de subclasse C-DATA
     * repõe MRB_TT_OBJECT, fazendo DATA_PTR devolver nullptr em toda a
     * criação subsequente -> s->bitmap=nil -> 0 blits -> ecra preto.
     *
     * bitmapBindingReinit e spriteBindingReinit forcam MRB_TT_DATA e
     * rebind dos initialize C++ DEPOIS de todos os scripts terem corrido.
     * --------------------------------------------------------------- */
    bitmapBindingReinit(mrb);
    spriteBindingReinit(mrb);

    if (mrb->exc) {
        show_error(mrb, "top-level");
    } else {
        /* ---------------------------------------------------------------
         * REBIND CRITICO: re-registar Graphics.update/freeze/transition
         * DEPOIS de todos os 376 scripts carregarem.
         *
         * Alguns scripts reabrem o modulo Graphics em Ruby e sobrescrevem
         * estes metodos por cima do binding C++. Usamos Ruby inline para
         * redirecionar para os aliases _3ds_internal registados em C.
         * graphicsBindingInit ja registou update_3ds_internal; registamos
         * aqui os aliases para freeze e transition.
         * --------------------------------------------------------------- */
        {
            static const char *rebind_ruby =
                "module Graphics\n"
                "  def self.update\n"
                "    update_3ds_internal\n"
                "  end\n"
                "  def self.transition(*a)\n"
                "    transition_3ds_internal(*a)\n"
                "  end\n"
                "  def self.freeze\n"
                "    freeze_3ds_internal\n"
                "  end\n"
                "end\n";
            mrb_load_string(mrb, rebind_ruby);
            if (mrb->exc) {
                DBGLOG("[REBIND] ERRO ao re-bind Graphics via Ruby\n");
                show_error(mrb, "rebind_graphics");
                mrb->exc = 0;
            } else {
                DBGLOG("[REBIND] Graphics.update/freeze/transition re-bound OK\n");
            }
        }

        // Entrar no loop principal (mainFunction/mainFunctionDebug)
        check_entry_methods(mrb, 0, "Main");

    }

    mrb_close(mrb);
    dbglog_close();
    return 0;
}

void binding_3ds_cleanup() {}