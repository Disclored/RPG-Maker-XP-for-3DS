#include <3ds.h>
#include <mruby.h>
#include <mruby/string.h>
#include <mruby/array.h>
#include <time.h>
#include <mruby/compile.h>
#include <mruby/class.h>
#include <mruby/variable.h>
#include <mruby/error.h>
#include <mruby/dump.h>
#include <mruby/irep.h>
#include <mruby/proc.h>
#include <mruby/data.h>   /* [FILE-IO] File real (Save/Load) */
#include <zlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <vector>
#include <malloc.h>   /* SUPERDBG: mallinfo p/ MKXPDebug.mem_info (heap livre/usado) */
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
 * 1 = usar o pool (mrb_open_allocf). NECESSARIO no HARDWARE 3DS real: o malloc
 *     do 3DS e' O(n) e sem o pool o boot demora minutos / fragmenta. PARA O
 *     BUILD DE HARDWARE, POR ISTO A 1.
 * 0 = mrb_open() normal (malloc do sistema). NO AZAHAR (emulador em PC) o malloc
 *     e' o do PC -- rapido (O(1) amortizado), por isso a pool NAO da' beneficio
 *     nenhum e so' DESPERDICA ~28MB. Com 0, esses ~28MB ficam livres p/ assets
 *     (tilesets, introPictures, outfits) -> deixam de dar magenta por falta de
 *     RAM. ALEM disso, o GC liberta memoria de volta ao malloc (a pool nao),
 *     por isso tilesets orfaos do mapa anterior libertam-se de facto.
 *     >>> USAR 0 PARA TESTAR NO AZAHAR. Voltar a 1 para o hardware. <<<
 * ─────────────────────────────────────────────────────────────────────────── */
#define USE_POOL_ALLOCATOR 0


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
            bool show = (s_allocf_calls <= 3) || ((s_allocf_calls % 500000) == 0);
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

    /* ===== DEBUG LOG CERTEIRO (read-only, 0 risco) ============================
     * Decide o fix CEGO C (release de chunks vazios do pool). So' CONTA, nunca
     * liberta. Reporta, por classe: chunks vivos, slots usados/livres, e quantos
     * KB estao presos em slots LIVRES -- que e' EXATAMENTE o que um release-de-
     * chunks devolveria ao malloc. Se o total "libertavel" for grande e a sonda
     * de malloc abaixo mostrar um bloco maximo < 12MB, entao o release vale o
     * risco; se for pequeno, o pool esta' cheio de objetos VIVOS e a unica saida
     * e' pre-escalar as imagens (fix B). */
    static void pool_diag_full(const char *quando) {
        if (!::g_dbglog) return;
        long total_bytes = 0, free_bytes = 0, used_bytes = 0;
        for (int i = 0; i < kNumClasses; i++) {
            Pool *p = &s_pools[i];
            long total_slots = (long)p->chunk_count * (long)kChunkObjs;
            long free_slots = 0;
            for (FreeNode *n = p->free_list; n; n = n->next) free_slots++;
            long used_slots = total_slots - free_slots;
            long cls_bytes = total_slots * (long)p->obj_size;
            long cls_free  = free_slots  * (long)p->obj_size;
            long cls_used  = used_slots  * (long)p->obj_size;
            total_bytes += cls_bytes; free_bytes += cls_free; used_bytes += cls_used;
            fprintf(::g_dbglog,
                "[POOL|FULL] %s cls%d(%uB) chunks=%d slots=%ld usados=%ld livres=%ld "
                "-> %ldKB total / %ldKB livres-presos\n",
                quando ? quando : "?", i, (unsigned)p->obj_size,
                p->chunk_count, total_slots, used_slots, free_slots,
                cls_bytes/1024, cls_free/1024);
        }
        fprintf(::g_dbglog,
            "[POOL|FULL] %s === pool TOTAL=%ldKB (~%ldMB) | VIVOS=%ldKB | "
            "LIVRES-PRESOS=%ldKB (~%ldMB) <- isto e' o que o release devolveria ===\n",
            quando ? quando : "?", total_bytes/1024, total_bytes/(1024*1024),
            used_bytes/1024, free_bytes/1024, free_bytes/(1024*1024));
        fflush(::g_dbglog);
    }

    /* SONDA DE MALLOC: maior bloco contiguo alocavel AGORA (= o que o decode do
     * stb consegue obter). Tenta tamanhos decrescentes e liberta logo. Diz-nos
     * o quao perto/longe estamos dos ~12MB que a intro picture precisa. */
    static void malloc_probe(const char *quando) {
        if (!::g_dbglog) return;
        static const size_t sizes[] = {
            24u*1024*1024, 20u*1024*1024, 16u*1024*1024, 12u*1024*1024,
            10u*1024*1024,  8u*1024*1024,  6u*1024*1024,  4u*1024*1024,
             2u*1024*1024,  1u*1024*1024,  512u*1024,     256u*1024 };
        size_t maior = 0;
        for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
            void *q = malloc(sizes[i]);
            if (q) { free(q); maior = sizes[i]; break; }
        }
        fprintf(::g_dbglog, "[MALLOC|PROBE] %s maior bloco alocavel ~%zuKB (~%zuMB) "
                "[decode da intro precisa ~12MB]\n",
                quando ? quando : "?", maior/1024, maior/(1024u*1024u));
        fflush(::g_dbglog);
    }
}

/* Wrappers globais (chamaveis de outros .cpp, ex. bitmap_binding no OOM). */
void mkxp_pool_diag_full(const char *q) { mkxp_pool::pool_diag_full(q); }
void mkxp_malloc_probe(const char *q)   { mkxp_pool::malloc_probe(q); }

/* =========================================================
 * SUPER DEBUG HELPERS
 * ========================================================= */

/* Ficheiro de log global para dump detalhado */
FILE *g_dbglog = NULL;
/* Ponteiro global para o mrb_state principal. Exposto para o bitmap loader
 * (bitmap_binding_3ds.cpp) poder forcar um mrb_full_gc no PONTO EXATO em que um
 * tileset/imagem grande nao cabe na memoria -- isso liberta bitmaps ORFAOS (ex:
 * o tileset do mapa anterior que ficou desreferenciado mas nao disposto, a
 * ocupar malloc) e permite RETENTAR a alocacao. So' e' usado nesse caminho de
 * falha (asset grande), nunca no fluxo normal. */
mrb_state *g_mkxp_mrb = NULL;
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
/* [LIMPEZA 2026-07-05] espelha $MKXP_VERBOSE p/ o C poder suprimir os seus
 * diagnosticos runtime periodicos (GFX|RENDER/SUPERDBG/HEARTBEAT/WD/CHARBATCH|FUNIL/
 * GHOST-KILL/marshal-Table/load) sem ler uma global Ruby a cada frame. Set em
 * MKXPDebug.log (que ja' le $MKXP_VERBOSE). Default false = silencio;
 * $MKXP_VERBOSE=true reactiva tudo. */
bool g_log_verbose = false;
/* [BOOT-LEAN 2026-07-05] enquanto false, o boot mostra SO' plugins + GameData +
 * erros (pedido do user). Vira true quando o scene loop arranca (set em
 * MKXPDebug.log ao ver "entering scene loop"). Depois: filtro runtime normal. */
bool g_boot_done = false;
/* [PLUGIN-RETRY 2026-07-05] scripts de plugin (EBDX) que dao excecao na 1a
 * execucao por ORDEM-DE-LOAD (alias :M_ebdx :M com M ainda nao definido). Sao
 * coletados e RE-CORRIDOS depois de TODOS os plugins carregarem (M ja' existe). */
#include <vector>
#include <string>
#include <utility>
bool g_plugin_exec_failed = false;
std::vector<std::pair<std::string, std::string>> g_plugin_retry;
/* Blacklist de tags que passam pelo DBGLOG (teem [t=] stamp): marshal por-mapa
 * (Table reconstruido/load OK/dump->port) enche o log em cada carregamento. */
static inline bool dbg_fmt_suppressed(const char *fmt){
    if (g_log_verbose || !fmt) return false;
    static const char *BL[] = {
        "[marshal] Table", "[marshal] load OK", "[marshal] dump->port",
        "[PROFILE|marshal]", "[RXCACHE]",
        /* confirmacoes de boot (uma vez cada; ruido quando o boot ja' e' estavel) */
        "[SKIP-PATCH]", "[SCRIPTS]", "[RESCHECK]",
        "[LOADORDER]", "[DISPLAY|BLACK]", "[DISPLAY|INIT]", "[BOOT]",
        0
    };
    for (int i=0; BL[i]; i++) if (strstr(fmt, BL[i])) return true;
    return false;
}
/* Durante o boot, suprime linhas C DIRETAS (nao-[MFD]) que nao sejam plugins/erros.
 * As linhas [MFD] (MKXPDebug.log) sao filtradas la'; aqui so' tocamos nas C-puras
 * (GFX|*, BMP|REINIT, etc.) que de outra forma sujavam o boot. */
static inline bool dbg_boot_suppressed(const char *fmt){
    if (g_log_verbose || !fmt) return false;
    if (strncmp(fmt, "[MFD] ", 6) == 0) return false;   /* [MFD] filtrado em MKXPDebug.log */
    /* erros passam sempre */
    if (strstr(fmt,"FALH") || strstr(fmt,"ERRO") || strstr(fmt,"WARN") ||
        strstr(fmt,"CRASH")) return false;
    /* no boot tambem plugins + dados; em RUNTIME (overworld) NADA -> 0 escritas SD */
    if (!g_boot_done && (strstr(fmt,"[PLUGINS]") || strstr(fmt,"[Game.init]"))) return false;
    return true;
}
#define DBGLOG(fmt, ...) \
    do { \
        if (dbg_fmt_suppressed(fmt) || dbg_boot_suppressed(fmt)) break; \
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

    /* #4: erros benignos de ORDEM DE LOAD -- o metodo EXISTE e funciona em runtime
     * (verificado em base_scripts/plugins); o plugin so' o chamou antes de a classe
     * o ter definido. Rebaixar de [ERROR] p/ [LOADORDER] e NAO despejar backtrace,
     * para nao alarmar nem floodar. Lista fechada de metodos confirmados. */
    {
        static const char *benign_lo[] = {
            "pbGetMoveScoreFunctionCode", "compile_encounters", "convert_files",
            "setUp", "DependentEventSprites", "heal_party", NULL
        };
        if (mrb_string_p(mesg)) {
            for (int bi = 0; benign_lo[bi]; bi++) {
                if (strstr(msg, benign_lo[bi])) {
                    DBGLOG("[LOADORDER] %s: %s (metodo existe em runtime -- benigno)\n", ctx_msg, msg);
                    return;
                }
            }
        }
    }
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

/* [DEFINED-RESCUE 2026-07-06] Variante IDEMPOTENTE de replace_defined_keyword.
 * Em vez de trocar defined?(X) -> nil (que MATA o guard `unless defined?(X_ebdx)`
 * -> o alias re-instala sempre -> com o PLUGIN-RETRY corre 2x -> ATAQUES/ITENS a
 * duplicar), envolve cada `defined?(X)` em `(defined?(X) rescue nil)`. Assim:
 *   - X indefinido e o defined? do mruby LEVANTA -> rescue -> nil (falsy) -> como
 *     CRuby -> o alias instala na 1a passagem (batalha continua a funcionar);
 *   - X JA' definido -> defined?(X) devolve "method"/"expression" (truthy) -> o
 *     guard bloqueia a 2a instalacao no RETRY -> SEM duplicacao.
 * Cresce o buffer (~13 chars por ocorrencia). So' apanha a forma com parenteses
 * `defined?(` (igual ao original). Ignora ocorrencias em comentario de linha. */
static void wrap_defined_rescue(std::vector<Bytef> &decomp) {
    const char needle[] = "defined?(";
    const size_t nlen = sizeof(needle) - 1;
    const char *buf = (const char*)decomp.data();
    const size_t len = decomp.size();
    std::string out;
    out.reserve(len + 128);
    size_t i = 0;
    while (i < len) {
        if (i + nlen <= len && memcmp(buf + i, needle, nlen) == 0) {
            /* FRONTEIRA DE PALAVRA: `defined?(` e' substring de `method_defined?(`,
             * `const_defined?(`, `instance_method_defined?(`, etc. So' e' o KEYWORD
             * `defined?` se o char anterior NAO for de identificador nem '.'. Sem isto,
             * envolviamos o sufixo -> `self.method_(defined?(:x) rescue nil)` = SyntaxError. */
            bool boundary = true;
            if (i > 0) {
                char pc = buf[i - 1];
                if ((pc >= 'a' && pc <= 'z') || (pc >= 'A' && pc <= 'Z') ||
                    (pc >= '0' && pc <= '9') || pc == '_' || pc == '.') boundary = false;
            }
            if (!boundary) { out += buf[i]; i++; continue; }
            /* nao dentro de comentario de linha */
            size_t ls = i;
            while (ls > 0 && buf[ls - 1] != '\n') ls--;
            bool in_comment = false;
            for (size_t c = ls; c < i; c++) { if (buf[c] == '#') { in_comment = true; break; } }
            if (!in_comment) {
                /* fecha o parentesis correspondente */
                size_t scan = i + nlen;
                int depth = 1;
                while (scan < len && depth > 0) {
                    if      (buf[scan] == '(') depth++;
                    else if (buf[scan] == ')') { depth--; if (depth == 0) break; }
                    scan++;
                }
                if (depth == 0) {
                    out += '(';
                    out.append(buf + i, scan - i + 1);   /* "defined?(...)" */
                    out += " rescue nil)";
                    i = scan + 1;
                    continue;
                }
            }
        }
        out += buf[i];
        i++;
    }
    decomp.resize(out.size());
    if (!out.empty()) memcpy(decomp.data(), out.data(), out.size());
}

/* =========================================================
 * PATCH SCRIPT
 * ========================================================= */
static void patch_script(std::vector<Bytef> &decomp, uLong dest_sz, const char *name) {
    char *buf = (char*)decomp.data();

    if (strcmp(name, "PluginManager") == 0 ||
        // [BISSECAO 2026-07-09] Intl re-skip TESTADO: NAO e' o veneno (probe continuou
        // morto) -> revertido; o INTL-UNSKIP fica ATIVO (nomes reais).
        // [INTL-UNSKIP 2026-07-08] Intl_Messages DES-SKIPADO. O skip era a RAIZ dos
        // nomes "0": este script define OrderedHash/MessageTypes/pbGetMessage --
        // sem ele, o auto-stub devolvia 0 p/ pbGetMessage -> TODOS os nomes = "0"
        // (+ cascata de tipos na batalha: Integer#force_encoding, Move_000).
        // Compila LIMPO no mrbc host (Syntax OK, verificado 2026-07-08); sem codigo
        // top-level (so' define classes/metodos; os regex pesados sao ferramentas de
        // dev nunca chamadas em runtime). O messages.dat (400x 'Iu' OrderedHash)
        // requer o [IU-USERDEF] no marshal.cpp (mesma data).
        strcmp(name, "Compiler") == 0 ||
        strcmp(name, "Compiler_CompilePBS") == 0 ||
        strcmp(name, "Compiler_WritePBS") == 0 ||
        strcmp(name, "Compiler_MapsAndEvents") == 0 ||
        // Fix 5: script 189 demasiado grande para codegen mruby (131 KB)
        // Fix 5: script 191 depende do 189 que falhou
        strcmp(name, "AI_Move_EffectScores") == 0 ||
        strcmp(name, "AI_Move_EffectScores_Gen8") == 0 ||
        // [UNSKIP 2026-07-09] "EBDX Wrapper Compatibility" DES-SKIPADO (GAP #3): o
        // "SyntaxError" era FALSO (mrbc: Syntax OK — 3o caso do padrao, apos EBDX e
        // Intl_Messages). E' o unico dono de EBDXBitmapWrapper -> sem ele, Pokedex/
        // Sumario/PC usam sprites errados/crasham. Top-level: PluginManager.register
        // (stub do motor L~2226 ✓) + Essentials::ERROR_TEXT (001 nao-skipado ✓).
        // [SAVE-VALUES-FIX 2026-07-03] Game_SaveValues DES-SKIPPADO: o skip antigo
        // ("apenas registam campos de save; nao necessarios p/ boot") era A RAIZ dos
        // saves de 101 bytes -- sem os 15 registos core (player/switches/map_factory/
        // bag/...), o compile_save_hash so' tinha os 2 valores de plugins -> save
        // esqueletico -> load = ecra preto. O ArgumentError historico de validate/
        // register ja' nao se reproduz (mruby/ambiente amadureceu); validado por
        // self-run (SELFTEST agora conta os valores registados).
        strcmp(name, "Game_SaveConversions") == 0 ||
        // [RE-SKIP 2026-07-09 BISSECAO] os 2 Deprecated ERRAM no load (egg?/full_name
        // "nao definidos" — sub-misterio) e sao o suspeito nº1 de envenenar a VM p/
        // os mrb_load_string seguintes (probe morto). Re-skipados ate resolver o
        // sub-misterio; o EBDX Wrapper Compat (carrega limpo) fica des-skipado.
        strcmp(name, "Player_Deprecated") == 0 ||
        strcmp(name, "Pokemon_Deprecated") == 0 ||
        // [UNSKIP 2026-07-09] (histórico) Player/Pokemon_Deprecated (GAP #2):
        // deprecated_method_alias EXISTE (016_Deprecation, nao-skipado, kwargs OK em
        // mruby 3.2; mrbc: Syntax OK em ambos). deprecate_constant era o que faltava
        // -> stub no-op no PRE-FIX (Module#deprecate_constant). Sem estes scripts,
        // pbGenPkmn crashava Mystery Gift/Rotom e pbHasSpecies?/pbHasType? (auto-stub
        // 0) tornavam 3 paginas de evento SEMPRE-ATIVAS.
        // Fix 7: Sprite_DynamicShadows usa APIs graficas avancadas nao suportadas
        strcmp(name, "Sprite_DynamicShadows") == 0 ||
        // Fix 8 REVERTIDO: apagar o script inteiro tirava o addEvent REAL (com push),
        // initialize, eachEvent, pbEnsureEvent, getEventByName -> o seguidor (Giltbert e
        // pokemon-seguidor) nunca era adicionado (dep 0->0). Os plugins reabrem a classe mas
        // so' adicionam pbMove/createEvent, nunca addEvent. Deixamos o 050 carregar (como no
        // Windows). Se reaparecer "uninitialized constant" no load, o log nomeia-a -> define-se
        // essa constante antes do 050 em vez de apagar o script todo.
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
			"          (@viewport2.z = 200) rescue nil  # [ROUPEIRO-FIX] pictures acima dos personagens\n"
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
            g_log_verbose = verbose;   /* espelha p/ os diagnosticos C-puros runtime */
            { /* [BLITDIAG] switch runtime p/ o diagnostico GPU dos battlers */
              extern bool g_blitdiag;
              mrb_value bd = mrb_gv_get(mrb, mrb_intern_lit(mrb, "$MKXP_BLITDIAG"));
              g_blitdiag = mrb_test(bd); }
            extern bool g_boot_done;
            if (s && !g_boot_done && (strstr(s, "entering scene loop") ||
                                      strstr(s, "scene.main start") ||
                                      strstr(s, "step8 done")))
                g_boot_done = true;
            /* [BOOT-LEAN] durante o boot mostra SO' o carregamento de dados
             * ([Game.init]/GameData) + erros; os fixes/steps sao suprimidos.
             * (Os plugins vem por DBGLOG/C, tratados a' parte.) */
            if (!verbose && s && !g_boot_done) {
                if (!(strstr(s,"[Game.init]") || strstr(s,"FALH") ||
                      strstr(s,"ERRO") || strstr(s,"WARNING")))
                    return mrb_nil_value();
            }
            if (!verbose && s && g_boot_done) {
                /* [OVERWORLD-SILENT 2026-07-05] pedido do user: 0 escritas no SD
                 * durante a jogatina. Em runtime so' passam ERROS/crashes. Reativar
                 * o resto: $MKXP_VERBOSE=true. */
                if (!(strstr(s,"FALH")||strstr(s,"ERRO")||strstr(s,"WARNING")||
                      strstr(s,"CRASH")||strstr(s,"Exception")||strstr(s,"crash")))
                    return mrb_nil_value();
                /* prefixos de alta-frequencia a suprimir quando NAO-verbose.
                 * Estes probes correm por-frame/por-redraw e cada linha escreve
                 * ao SD (~ms). Sao SO diagnostico -> suprimidos por defeito agora
                 * que o jogo arranca. $MKXP_VERBOSE=true reativa-os.
                 * Prefixos sem ']' apanham variantes (ex. [DIAG-DT]/[DIAG-DT-TXT]). */
                static const char *noisy[] = {
                    "[SM#update]", "[PRB]", "[MISSING-DET]", "[MISSING]",
                    "[SM#update", "[CHK]",
                    "[SPRDUMP", "[DLG", "[DIAG", "[TF]", "[TCMD]", "[MOVEDBG",
                    "[WSKIN-PROBE", "[CFIX", "[CORNERFIX", "[BACKFILL", "[BOXFIT",
                    "[PLANE-DIAG", "[MAPDUMP", "[MEVENT", "[SEQ>", "[TSTAT",
                    "[SHEET?", "[CARTAO", "[SPRGUARD", "[DEPGUARD",
                    /* [LIMPEZA 2026-07-05] diagnosticos de bugs JA' RESOLVIDOS ou
                     * profilers granulares -- so' ruido/escrita-SD agora. Reativar
                     * TUDO com $MKXP_VERBOSE=true. MANTIDOS: [FPS], [FPSPROBE],
                     * [DIAG-PERF], [POKEDBG], [NESTED-AUTODEF], [SAVE-REPAIR],
                     * [GHOST-KILL], [ITEMNAME-FIX] e erros. */
                    "[ARVDBG", "[GILBERTDIAG", "[SAPDBG", "[SUPERDBG",
                    "[RUBYPROF", "[VWPATH", "[TILEPROF", "[PERF-RESTO",
                    "[PBMSG", "[OUTFITDIAG", "[PBCMW", "[GIF-FIX",
                    "[CHURN", "[HEARTBEAT", "[NAMEIN", "[TILEANIM",
                    "[SPR|DUMP", "[SPR|SET_BMP", "[SPR|INIT",
                    "[J3]", "[BMP|WSKIN-TRUST", "[EBDXPROF",
                    "[MISSING_PATCH", "[PATCH", "[WSKIN", "[BMP|FULLPATH",
                    "[MISSING-TOP", "[DIAG-SPRITE0", "[FIX-GRELHA7",
                    NULL
                };
                for (int i = 0; noisy[i]; i++) {
                    size_t n = strlen(noisy[i]);
                    if (strncmp(s, noisy[i], n) == 0) {
                        return mrb_nil_value();   /* suprimido */
                    }
                }
                /* [LIMPEZA 2026-07-05] confirmacoes de instalacao de fixes no boot
                 * ("...  OK", "... instalad*", "... adicionad*") -- uma vez cada,
                 * so' ruido quando o boot ja' e' estavel. $MKXP_VERBOSE=true reactiva. */
                size_t sl = strlen(s);
                if (sl >= 3 && s[sl-1]=='K' && s[sl-2]=='O' && s[sl-3]==' ')
                    return mrb_nil_value();
                if (strstr(s, " instalad") || strstr(s, " adicionad"))
                    return mrb_nil_value();
            }
            DBGLOG("[MFD] %s\n", s);
        }
        return mrb_nil_value();
    }, MRB_ARGS_REQ(1));

    /* [BATTLER-MARK] o probe marca os sprites-battler (identificacao deterministica) */
    mrb_define_module_function(mrb, dbg_mod, "mark_battler",
        [](mrb_state *mrb, mrb_value) -> mrb_value {
            mrb_value sp; mrb_get_args(mrb, "o", &sp);
            if (mrb_type(sp) == MRB_TT_DATA || mrb_type(sp) == MRB_TT_OBJECT) {
                Sprite3DS *s = (Sprite3DS*)DATA_PTR(sp);
                if (s) s->is_battler = true;
            }
            return mrb_nil_value();
        }, MRB_ARGS_REQ(1));

    /* [PLANE-WRAP] o stub Ruby do Plane marca o seu sprite interno -> o blit
     * final tila o bitmap pelo ecra (fog/panorama com wrap RGSS). */
    mrb_define_module_function(mrb, dbg_mod, "mark_plane",
        [](mrb_state *mrb, mrb_value) -> mrb_value {
            mrb_value sp; mrb_get_args(mrb, "o", &sp);
            if (mrb_type(sp) == MRB_TT_DATA || mrb_type(sp) == MRB_TT_OBJECT) {
                Sprite3DS *s = (Sprite3DS*)DATA_PTR(sp);
                if (s) s->is_plane = true;
            }
            return mrb_nil_value();
        }, MRB_ARGS_REQ(1));

    /* [TITLE-SWEEP] esconder os paineis do titulo (z>=990) ao sair do Scene_Intro */
    mrb_define_module_function(mrb, dbg_mod, "sweep_title",
        [](mrb_state*, mrb_value) -> mrb_value {
            extern void sprites_sweep_title_leftovers();
            sprites_sweep_title_leftovers();
            return mrb_nil_value();
        }, MRB_ARGS_NONE());

    /* ── [SUPERDBG] MKXPDebug.mem_info -> string com a fotografia da memoria ────
     * Linear livre (decodes de tilesets), heap malloc (livre/usado via mallinfo),
     * texturas vivas (contagem + bytes POT + VRAM) e Bitmaps vivos. O probe chama
     * isto no [SNAP] periodico. Tudo o que e' preciso p/ caçar fugas/fragmentacao
     * num so' sitio, sem por-frame. */
    mrb_define_module_function(mrb, dbg_mod, "mem_info",
        [](mrb_state *mrb, mrb_value) -> mrb_value {
            extern int g_live_bitmaps;
            size_t tex_n = 0, tex_bytes = 0, tex_vram = 0;
            display_3ds_tex_stats(&tex_n, &tex_bytes, &tex_vram);
            struct mallinfo mi = mallinfo();
            char buf[320];
            snprintf(buf, sizeof(buf),
                "linear_free=%.0fKB heap_free=%.0fKB heap_used=%.1fMB "
                "texturas=%u tex_mem=%.0fKB tex_vram=%.0fKB bitmaps=%d",
                linearSpaceFree() / 1024.0,
                (double)mi.fordblks / 1024.0,
                (double)mi.uordblks / 1048576.0,
                (unsigned)tex_n, tex_bytes / 1024.0, tex_vram / 1024.0,
                g_live_bitmaps);
            return mrb_str_new_cstr(mrb, buf);
        }, MRB_ARGS_NONE());

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
        /* [AUDIO-FADE 2026-07-02] fades REAIS (antes: no-op -> pbBGMFade nunca
         * parava a musica; BGM sobreposta em transicoes). Semantica RGSS:
         * Audio.bgm_fade(ms). Rampa de volume 1x/frame + stop no fim (audio_3ds). */
        mrb_define_module_function(mrb, audio_mod, "bgm_fade",
            [](mrb_state *m, mrb_value) -> mrb_value {
                mrb_int ms = 0; mrb_get_args(m, "|i", &ms);
                audio_3ds_bgm_fade((int)ms); return mrb_nil_value();
            }, MRB_ARGS_ARG(0,1));
        mrb_define_module_function(mrb, audio_mod, "bgs_fade",
            [](mrb_state *m, mrb_value) -> mrb_value {
                mrb_int ms = 0; mrb_get_args(m, "|i", &ms);
                audio_3ds_bgs_fade((int)ms); return mrb_nil_value();
            }, MRB_ARGS_ARG(0,1));
        mrb_define_module_function(mrb, audio_mod, "me_fade",
            [](mrb_state *m, mrb_value) -> mrb_value {
                mrb_int ms = 0; mrb_get_args(m, "|i", &ms);
                audio_3ds_me_fade((int)ms); return mrb_nil_value();
            }, MRB_ARGS_ARG(0,1));
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
        # args = [coord1, (coord2), (coord3), valor] -> o ultimo e' SEMPRE o valor,
        # os anteriores sao coordenadas. Nao de' o nr de coords pela presenca de
        # args[1], senao a forma 1D `t[i]=v` (=[]=(i,v)) trata o VALOR v como coord-y
        # -> idx=v*@x+i (fora dos limites) -> escrita DESCARTADA (paredes passaveis).
        v   = args.last.to_i
        x   = args[0].to_i
        y   = (args.length > 2 ? args[1] : 0).to_i
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

# -- CORRECAO #4a/#4b/#4-heal: metodos em falta (ANTES dos scripts/plugins) -----
# round/Numeric e Dir.delete faltam mesmo no port; heal_party e' chamado por um
# plugin sem receiver (devia ser $Trainer.heal_party). Definidos cedo para os
# plugins (Tileset Rearranger / Marin's Map Exporter / Following Pkmn) carregarem
# sem erro. Guardas 'unless' garantem que NAO substituem nada que ja' exista.
begin
  class Numeric
    unless method_defined?(:round)
      def round(ndigits = 0)
        if ndigits <= 0
          (self >= 0 ? (self + 0.5).to_i : (self - 0.5).to_i)
        else
          f = 10.0 ** ndigits
          v = self * f
          v = (v >= 0 ? (v + 0.5).to_i : (v - 0.5).to_i)
          v / f
        end
      end
    end
  end
rescue; end
begin
  if Object.const_defined?(:Dir)
    class << Dir
      unless respond_to?(:delete)
        def delete(*); 0; end            # no-op seguro (ferramentas de export; sem efeito no jogo)
        alias_method :rmdir,  :delete rescue nil
        alias_method :unlink, :delete rescue nil
      end
    end
  end
rescue; end
begin
  module Kernel
    unless method_defined?(:heal_party) || private_method_defined?(:heal_party)
      def heal_party
        t = ($Trainer rescue nil); t = ($player rescue nil) if t.nil?
        t.heal_party if t.respond_to?(:heal_party)
      end
      module_function :heal_party
    end
  end
rescue; end
# -- FIM CORRECAO #4a/#4b/#4-heal -----------------------------------------------

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
# FIX RAIZ (recursos em subpastas que nao carregavam, ex: teclado "Naming/bg_2"):
# antes estes metodos devolviam SEMPRE false -> safeExists?/pbResolveBitmap davam
# nil -> addBackgroundPlane fazia bitmap=nil -> Plane invisivel -> faixa amber
# visivel por tras. Agora exist?/file? perguntam ao loader C++ nativo
# (Bitmap._native_exist?), que testa o ficheiro nas raizes do jogo + extensoes --
# exactamente o que o loader de imagens faz. Assim o que existe no SD passa a ser
# encontrado, em qualquer subpasta. Generico p/ todo o jogo.
begin
  module FileTest
    def self.__mkxp_exist?(p)
      return false if !p
      s = p.to_s
      return false if s.empty?
      (Bitmap._native_exist?(s) rescue false)
    end
    def self.exist?(p);       __mkxp_exist?(p); end
    def self.exists?(p);      __mkxp_exist?(p); end
    def self.file?(p);        __mkxp_exist?(p); end
    def self.directory?(p)
      return false if !p
      s = p.to_s
      return false if s.empty?
      (Bitmap._native_dir_exist?(s) rescue false)
    end
    def self.audio_exist?(p); __mkxp_exist?(p); end
    def self.size?(p);        nil;   end
    def self.zero?(p);        true;  end
    def self.readable?(p);    __mkxp_exist?(p); end
    def self.writable?(p);    false; end
    def self.executable?(p);  false; end
  end
rescue; end

# -- [DAYNIGHT 2026-07-11] pbDayNightTint com cache por-frame (pos-scripts) -----
# ANTES: no-op total ("precisa de Bitmap/Tone reais") -> dia/noite MORTO em todo
# o jogo. A justificacao caducou: o [SPRFX] aplica sprite.tone no blit e o
# [TONE-DUAL] le Tone Ruby-puro -> a cadeia inteira funciona:
#   pbDayNightTint(self) por sprite (061:154) + pbDayNightTint(@tilemap) (066:79)
#   -> tone.set barato; CustomTilemap propaga @tone as suas sprites (075:973-979)
#   -> SPRFX tinge personagens E chao. O calculo caro (MapMetadata/getTone) e'
# feito UMA vez por frame ($__dnt_frame) e o CHARBATCH ja consome $__dnt_tone
# nativamente no fast-path. Kill-switch sem rebuild: $MKXP_DAYNIGHT=false.
begin
  unless $__dnt_installed
    $__dnt_installed = true
    $__dnt_frame = -1
    $__dnt_apply = false
    $__dnt_tone  = nil
    def pbDayNightTint(object)
      return if $MKXP_DAYNIGHT == false
      return if !$scene.is_a?(Scene_Map)
      fc = (Graphics.frame_count rescue 0)
      if fc != $__dnt_frame
        $__dnt_frame = fc
        begin
          if (Settings::TIME_SHADING && $PokemonSystem.daytone == 0) &&
             GameData::MapMetadata.exists?($game_map.map_id) &&
             GameData::MapMetadata.get($game_map.map_id).outdoor_map
            t = PBDayNight.getTone
            $__dnt_tone  = [t.red, t.green, t.blue, t.gray]
            $__dnt_apply = true
          else
            $__dnt_apply = false
          end
        rescue
          $__dnt_apply = false
        end
      end
      begin
        if $__dnt_apply && $__dnt_tone
          object.tone.set($__dnt_tone[0], $__dnt_tone[1], $__dnt_tone[2], $__dnt_tone[3])
        else
          object.tone.set(0, 0, 0, 0)
        end
      rescue
      end
    end
    (MKXPDebug.log("[DAYNIGHT] pbDayNightTint cache reativado (era no-op)") rescue nil)
  end
rescue; end

# -- pbGetTileBitmap: cache do TILESET de extracao (pos-scripts) ----------------
# RAIZ PROVADA do lag de ~7s ao entrar numa sala (e provavel causa do quarto
# preto). Eventos com grafico de TILE (tile_id>=384, ex: moveis) fazem
# Sprite_Character -> pbGetTileBitmap -> RPG::Cache.tileEx, que para CADA tile_id
# diferente executa o bloco { AnimatedBitmap.new(tileset).deanimate } e a seguir
# faz `tileset.dispose`. Ou seja: cada movel recarrega o tileset INTEIRO (Gen4
# Interior 256x17024 = 8.3MB, ~1.4s) e descarta-o -> 5 moveis = 5 cargas = ~7s +
# 41MB de churn de memoria + varios GC-on-OOM no meio (que podem fragmentar/varrer
# a camada de tiles -> preto).
# FIX: cachear o tileset COMPLETO por nome e marca-lo never_dispose=true. Assim o
# `tileset.dispose` do tileEx vira no-op (BitmapWrapper#dispose so' liberta se
# refcount<=0 E !never_dispose) e os N moveis partilham UMA so' carga. Na mudanca
# de mapa, forca-se o dispose e limpa-se (senao seria fuga: 8.3MB por mapa).
# Seguro: este tileset e' SO' fonte de extracao (nunca e' o @tilemap.tileset), e
# os tiles extraidos sao copias independentes (blt) que nao o referenciam.
begin
  $__mkxp_tilebmp_ts  ||= {}
  $__mkxp_tilebmp_map ||= -999
  unless $__mkxp_tilebmp_patched
    $__mkxp_tilebmp_patched = true
    class Object
      def pbGetTileBitmap(filename, tile_id, hue, width = 1, height = 1)
        mid = ($game_map.map_id rescue 0)
        if mid != $__mkxp_tilebmp_map
          # mapa mudou -> dispor mesmo (anular never_dispose) e esvaziar o cache
          begin
            $__mkxp_tilebmp_ts.each_value do |b|
              begin
                b.never_dispose = false
                b.dispose
              rescue; end
            end
          rescue; end
          $__mkxp_tilebmp_ts = {}
          $__mkxp_tilebmp_map = mid
        end
        return RPG::Cache.tileEx(filename, tile_id, hue, width, height) { |f|
          # [TILEBMP v2 2026-07-02] PRIMEIRO o tileset RESIDENTE via pbGetTileset
          # (TSCACHE, never_dispose) -> ZERO re-decode. RAIZ PROVADA no log 08:39:
          # este bloco fazia AnimatedBitmap.new(...) RAW p/ o 2o decode do
          # 'Gen4 Outside Town' (15.5MB) com a arena ja' ocupada -> PNG4444-ERR
          # -> fallback magenta + NCACHE envenenado + cascata GC/EVICT (3.4 fps).
          # (Este bloco corre DEPOIS do probe -> e' o dono final de pbGetTileBitmap;
          # o TSC-PBGET do probe e o [TILEBMP-FIX] antigo eram sobrepostos por ele.)
          ts = (pbGetTileset(f) rescue nil)
          if ts.nil? || (ts.disposed? rescue true)
            # fallback antigo (so' se pbGetTileset falhar): cache local por mapa
            ts = $__mkxp_tilebmp_ts[filename]
            if ts.nil? || (ts.disposed? rescue true)
              ts = AnimatedBitmap.new("Graphics/Tilesets/" + filename).deanimate
              (ts.never_dispose = true) rescue nil   # tileEx vai chamar dispose -> no-op
              $__mkxp_tilebmp_ts[filename] = ts
            end
          end
          ts
        }
      end
    end
    (MKXPDebug.log("[TILEBMP] pbGetTileBitmap com cache de tileset (anti-lag 5x + anti-churn)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[TILEBMP] patch falhou: #{e.class}: #{e.message}") rescue nil)
end

# -- TileWrap: blt SEMPRE direto (tileset un-wrapped) --------------------------
# BUG RAIZ do quarto/vila PRETO (confirmado por [TMDBG]: @tileset valido 4444,
# layer valida, refresh corre, SEM excecao -> logo o erro esta' no proprio blt).
# Com mega?=false (ver bmp_max_size=1024) o tileset fica UN-wrapped: 256 x
# altura_total (ex: 256x17024). Mas o TileWrap::blitWrappedPixels ORIGINAL, para
# qualquer tile com srcrect.y >= MAX_TEX_SIZE (=1024), aplica getWrappedRect, que
# mapeia o x para col*256 (col = y/1024 -> 4096, 8192, ...) -- FORA dos 256px de
# largura do tileset un-wrapped. O bmp_blt le fora dos limites (sxx>=src->width ->
# salta) -> a tile sai PRETA. So' as tiles das primeiras ~32 linhas (y<1024)
# desenhavam (e' a faixa que se ve' no canto). O quarto usa tiles por todo o
# tileset de 532 linhas -> quase tudo preto.
# Como NENHUM tileset e' wrapped aqui (shouldWrap=false sempre), o blt correto e'
# SEMPRE o direto: o bmp_blt nativo recorta o tile 32x32 a QUALQUER y (ate' 31040)
# sem precisar de wrapping. Sobrepomos os dois metodos para nunca aplicar o wrap.
begin
  if Object.const_defined?(:TileWrap)
    module TileWrap
      def self.blitWrappedPixels(destX, destY, dest, src, srcrect)
        dest.blt(destX, destY, src, srcrect)
      end
      def self.stretchBlitWrappedPixels(destrect, dest, src, srcrect)
        dest.stretch_blt(destrect, src, srcrect)
      end
    end
    (MKXPDebug.log("[TILEWRAP] blt direto (un-wrapped) instalado -- corrige quarto/vila preto") rescue nil)
  else
    (MKXPDebug.log("[TILEWRAP] TileWrap ausente -- override NAO aplicado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[TILEWRAP] override falhou: #{e.class}: #{e.message}") rescue nil)
end

# -- CustomTilemap: forcar extracao dos tiles de PRIORIDADE (pos-scripts) ------
# RAIZ PROVADA (casa invisivel / arvore cortada a meio / objetos cortados): os
# tilesets do jogo sao gigantes (ex: 256x31040) e excedem o limite de textura do
# 3DS (1024) -> ficam sem textura GPU. No getRegularTile, com @diffsizes=false (o
# caso normal: tile 32 == fonte 32), o sprite do tile de PRIORIDADE aponta DIRETO
# ao tileset gigante (que nao tem textura) -> nao desenha. Por isso todo o topo
# (telhados, copas de arvores, topo de moveis) fica invisivel, enquanto o layer0
# (chao, composto por blt software, sem precisar de textura) aparece bem.
# FIX: forcar o caminho @diffsizes -> o Essentials EXTRAI cada tile para um bitmap
# 32x32 proprio (que TEM textura), usando stretchBlitWrappedPixels (que o override
# de TileWrap acima ja' torna num blt direto, valido a qualquer Y ate' 31040).
# Aplica-se a TODAS as instancias de CustomTilemap e auto-corrige no refresh
# seguinte. So' mexe nos tiles de prioridade; o chao continua exatamente igual.
begin
  if Object.const_defined?(:CustomTilemap)
    class CustomTilemap
      alias_method :__df_orig_getRegularTile, :getRegularTile
      def getRegularTile(sprite, id)
        @diffsizes = true
        __df_orig_getRegularTile(sprite, id)
      end

      # -- PERF (quarto/interiores a 20fps): saltar a RE-RENDERIZACAO periodica
      # de autotiles quando o mapa NAO tem autotiles animados. -----------------
      # O Essentials, em CustomTilemap#update, corre `repaintAutotiles; refresh(true)`
      # a cada Animated_Autotiles_Frames (=5 frames a 30fps) para animar agua/etc.
      # refresh(true)->refreshLayer0(true) re-blita a camada de chao (ex: 832x704)
      # a partir do tileset gigante. Num mapa sem nada animado (um quarto interior)
      # isto re-desenha SEMPRE o mesmo -> ~40ms/frame de pico medido = ~20 fps mesmo
      # sem nada a acontecer.
      # refresh_autotiles JA' calcula quantos frames tem cada autotile (@framecount
      # e a var local 'hasanimated'); replicamos esse teste, guardamo-lo, e tornamos
      # as duas chamadas periodicas NO-OP quando nao ha animacao. O render estatico
      # inicial (feito uma vez no load) mantem-se; o SCROLL (refresh(false)) e os
      # mapas COM agua ficam exatamente iguais. Self-corrige: se houver mesmo um
      # autotile animado, @__mkxp_hasanim fica true e tudo corre como o original.
      alias_method :__mkxp_orig_refresh_autotiles, :refresh_autotiles
      def refresh_autotiles
        __mkxp_orig_refresh_autotiles
        @__mkxp_hasanim = false
        fc = @framecount
        if fc.is_a?(Array)
          anim = [false, false, false, false, false, false, false]
          any = false
          i = 0
          while i < 7
            if fc[i] && fc[i] >= 2 ; anim[i] = true ; any = true ; end
            i += 1
          end
          if any
            # [TILEANIM v2 2026-07-11] o gate por-TILESET era quase morto: o
            # 'Gen4 Interior' (236 mapas) tem o autotile 'Calm Water2' animado,
            # logo hasanim=true e TODOS os interiores repintavam o chao por
            # frame (~40ms) sem agua nenhuma colocada. Agora verificamos se
            # algum autotile ANIMADO esta' COLOCADO nos dados DESTE mapa:
            # 1 passagem com early-exit (mapas com agua saem nos primeiros
            # tiles; custo unico no load). ids de autotile: 48..383, slot
            # = (id/48)-1. Fallback conservador: qualquer duvida -> true.
            begin
              raw = @map.data.instance_variable_get(:@data)
              if raw.is_a?(Array)
                raw.each do |t|
                  next if !t.is_a?(Integer) || t < 48 || t >= 384
                  s = (t / 48) - 1
                  if s >= 0 && s < 7 && anim[s]
                    @__mkxp_hasanim = true
                    break
                  end
                end
              else
                @__mkxp_hasanim = true
              end
            rescue
              @__mkxp_hasanim = true
            end
          end
        else
          @__mkxp_hasanim = true   # sem info -> nao arriscar, deixa correr o original
        end
        (MKXPDebug.log("[TILEANIM v2] mapa hasanim=#{@__mkxp_hasanim}") rescue nil)
      end

      alias_method :__mkxp_orig_repaintAutotiles, :repaintAutotiles
      def repaintAutotiles
        return if @__mkxp_hasanim == false   # mapa sem autotiles animados -> nada a repintar
        __mkxp_orig_repaintAutotiles
      end

      alias_method :__mkxp_orig_refresh_anim, :refresh
      def refresh(autotiles = false)
        return if autotiles && @__mkxp_hasanim == false   # so' salta a re-render de ANIMACAO
        __mkxp_orig_refresh_anim(autotiles)
      end
    end
    (MKXPDebug.log("[DIFFSIZES] getRegularTile -> extracao p/ bitmap pequeno (tiles de prioridade aparecem)") rescue nil)
    (MKXPDebug.log("[TILEANIM] refresh/repaintAutotiles guardados (no-op sem autotiles animados -> interiores deixam de re-renderizar por frame)") rescue nil)
  else
    (MKXPDebug.log("[DIFFSIZES] CustomTilemap ausente -- override NAO aplicado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[DIFFSIZES] override falhou: #{e.class}: #{e.message}") rescue nil)
end

module PluginManager
  def self.register(hash); end
  def self.runPlugins; end
  def self.installed?(name); false; end
  def self.checkDependencies; end
  def self.requiredVersion(name, ver); end
  def self.version(name); "1.0"; end   # [EBDX-SETUP] plugins interpolam em strings
  def self.plugins; []; end            # [P5] ecra de creditos itera os plugins
end

# [P5 AI-SHIM 2026-07-10] o script AI (189, Gen8) esta' SKIPADO -> a IA da
# batalha chama pbGetMoveScoreFunctionCode e caia no auto-stub (0-truthy
# imprevisivel). Definir so' se nao existir (sonda por alias em begin/rescue --
# NUNCA respond_to?, o method_missing universal mente).
begin
  alias __mkxp_probe_pgmsfc pbGetMoveScoreFunctionCode
rescue NameError
  def pbGetMoveScoreFunctionCode(*a); 0; end
  (MKXPDebug.log("[AI-SHIM] pbGetMoveScoreFunctionCode -> 0 (script AI skipado)") rescue nil)
end

# [NAMEIN-HARD 2026-07-11] o default do jogo e' textinput=1 (teclado fisico,
# 284_UI_Options:47) e o dispatch (0094:795) abriria PokemonEntryScene com
# Input.text_input, que NAO existe no 3DS -> soft-lock no 1o nickname. Forcar o
# getter a 0 = cena por grelha de setas (Window_CharacterEntry, input nativo).
# Complementa o [NAMEIN] USEKEYBOARD=false; a opcao no menu Options fica inerte
# (correto num handheld sem teclado).
begin
  class PokemonSystem
    def textinput; 0; end
  end
rescue; end

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
    begin
      $data_animations = load_data('Data/Animations.rxdata')
    rescue => e
      # [ANIMRETRY 2026-07-02] o load falhava com 'allocation failure' no boot (heap
      # fragmentada apos os 376 scripts) -> $data_animations=nil -> pbExclaim/anims
      # partidos (e um Integer chegava a Sprite#bitmap= -> [BMP|UNWRAP] FALHOU).
      # Retry unico apos full-GC: no boot o heap recupera o suficiente p/ 43KB.
      dbg "[Game.init] Animations: #{e.message} -- retry pos-GC"
      begin
        GC.start
        $data_animations = load_data('Data/Animations.rxdata')
        dbg "[Game.init] Animations: OK na 2a tentativa (pos-GC)"
      rescue => e2
        dbg "[Game.init] Animations: 2a tentativa falhou: #{e2.message}"
      end
    end
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
    # ---- [EBDX-SETUP v2 2026-07-10] RAIZ da batalha vazia ----
    # O EBDX pendura o seu setupData (que executa os blocos configProcess a
    # carregar TODOS os .ebdx: metrics=vetores de camara, ui=databoxes,
    # maps=cenarios, animationmap=animacoes de ataque, pokemon/trainers/rules)
    # num alias de Game.initialize (0062_EBDX_Setup:87-104). Este Game.initialize
    # do motor SUBSTITUI esse wrapper (e carregado depois dos plugins), logo os
    # blocos NUNCA corriam. Executamos os blocos aqui, um-a-um, na MESMA posicao
    # do fluxo Essentials real (logo apos o GameData carregar).
    # v2 (provado no log do user 2026-07-10 21:13):
    #  (a) @compiled=true ANTES dos blocos. O add_data (0061:279) tem o desvio
    #      "if !@compiled && $DEBUG -> @cachedData.push; return" que CACHEIA em
    #      vez de processar; so o Compiler real poe @compiled=true (0060:66) e
    #      esse nunca corre no port -> NADA era processado (pokemon/trainers/
    #      ui/environments vazios = sem sprites/cenario/nomes na batalha,
    #      apesar dos vetores OK -- add_vector nao passa pelo add_data).
    #      Os .ebdx JA SAO os dados compilados -> compiled=true e' a verdade.
    #  (b) NAO chamar CompilerEBDX.addFromCached: itera @cachedData enquanto o
    #      add_data (ainda !@compiled) re-push no MESMO array -> loop infinito
    #      -> "array size too big" (t=11.5s) -> array gigante vivo ->
    #      NoMemoryError (t=24.8s) -> restart do jogo. Com (a) nada e' cacheado.
    #  (c) guarda run-once $__ebdx_setup_done: o mainFunctionDebug reinicia o
    #      jogo apos crash e re-chama Game.initialize -> sem guarda os blocos
    #      re-corriam (e no log falharam os 6 que usam add_data).
    #      instance_variable_set direto: o set() do EBDX passa por parse() que
    #      usa eval("defined?(@x)") -- nao-fiavel no mruby.
    # [EBDX-DEFER 2026-07-11 — leitura integral do log 739 linhas] o setup EBDX
    # (9.6s, ~6MB de objetos residentes) corria AQUI (boot, t=18s) e enchia o
    # malloc ANTES do titulo -> os paineis do titulo (11MB) iam TODOS para o
    # linear -> ao chegar ao mapa restavam ~200KB -> nem os buffers de leitura
    # dos tilesets (231KB) cabiam -> NCACHE -> mapa preto/magenta. Os dados
    # EBDX so' sao precisos NA BATALHA -> diferir para a entrada no 1o mapa
    # (createSpritesets, pos-titulo e pos-sweep): o titulo corre com malloc
    # LIVRE, os paineis vao p/ malloc, o linear sobrevive p/ os tilesets.
    # $MKXP_EBDX_EAGER=true reverte para o comportamento antigo.
    $__ebdx_setup_pending = true
    $__mkxp_ebdx_setup = lambda do
      next if $__ebdx_setup_done
      begin
      if Object.const_defined?(:EliteBattle)
        if false
          nil
        else
          EliteBattle.instance_variable_set(:@compiled, true)
          EliteBattle.instance_variable_set(:@cachedData, [])
          __cfg = (EliteBattle.class_variable_get(:@@configData) rescue nil)
          if __cfg.is_a?(Hash)
            __ok = []; __bad = []
            __cfg.keys.each do |k|
              begin
                __cfg[k].call
                __ok.push(k)
              rescue => e
                __bad.push(k)
                dbg "[Game.init][EBDX-SETUP] bloco #{k} FALHOU: #{e.class}: #{e.message}"
              end
            end
            $__ebdx_setup_done = true
            dbg "[Game.init][EBDX-SETUP] blocos OK=#{__ok.inspect} FALH=#{__bad.inspect}"
            begin
              __vk = EliteBattle.instance_variable_get(:@vectors)
              dbg "[Game.init][EBDX-SETUP] vetores registados: #{__vk.is_a?(Hash) ? __vk.keys.inspect : __vk.inspect}"
            rescue => e
              dbg "[Game.init][EBDX-SETUP] leitura vetores: #{e.message}"
            end
            # prova de que o add_data PROCESSOU (na v1 cacheava e ficava tudo 0)
            begin
              __pd = EliteBattle.instance_variable_get(:@pokemonData)
              __td = EliteBattle.instance_variable_get(:@trainerData)
              __md = EliteBattle.instance_variable_get(:@mapData)
              __cc = EliteBattle.instance_variable_get(:@cachedData)
              dbg "[Game.init][EBDX-SETUP] dados processados: pokemon=#{__pd.is_a?(Hash) ? __pd.size : __pd.class} trainers=#{__td.is_a?(Hash) ? __td.size : __td.class} maps=#{__md.is_a?(Hash) ? __md.size : __md.class} cache_residual=#{__cc.is_a?(Array) ? __cc.size : __cc.class}"
            rescue => e
              dbg "[Game.init][EBDX-SETUP] leitura dados: #{e.message}"
            end
          else
            dbg "[Game.init][EBDX-SETUP] @@configData ausente (#{__cfg.class}) -- EBDX sem blocos?"
          end
        end
      end
      $__ebdx_setup_pending = false
      rescue => e
        dbg "[EBDX-SETUP] geral: #{e.class}: #{e.message}"
      end
    end
    # boot: so' corre ja' se o utilizador pedir explicitamente
    $__mkxp_ebdx_setup.call if $MKXP_EBDX_EAGER == true
    # ---- #7 GPS-FIX: get_player robusto, AGORA que GameData::Metadata.load correu ----
    # O picker "what picture you'd like to use?" constroi o caminho da imagem a
    # partir de GameData::Metadata.get_player(i)[0], que DEVIA ser o simbolo do
    # treinador. O marshal do port le' o [0] como 0 -> pede 'PlayerSprites/0' ->
    # falha -> paineis vazios. Aqui (logo apos o load do Metadata, ~t=6.46s, muito
    # antes do picker abrir ~t=46s) instalamos um get_player que, se o [0] vier bom,
    # confia; senao recupera o nome do sprite a partir dos PROPRIOS dados do jogo
    # (charset "trainer_<TIPO>" em meta[1], ou varrendo a pasta PlayerSprites/),
    # preservando o resto do array. Sem nomes hardcoded -> serve qualquer jogo.
    # Idempotente (guarda no alias).
    begin
      if defined?(GameData) && GameData.const_defined?(:Metadata)
        __MD = GameData::Metadata
        # NB: NAO usar respond_to? aqui -- o method_missing universal faz
        # respond_to_missing? devolver true p/ tudo, logo respond_to? seria sempre
        # true e o fix NUNCA instalava. method_defined? ve' metodos REAIS.
        unless __MD.singleton_class.method_defined?(:__mkxp_orig_get_player)
          __MD.singleton_class.send(:alias_method, :__mkxp_orig_get_player, :get_player)
          __MD.define_singleton_method(:get_player) do |id|
            meta = (__mkxp_orig_get_player(id) rescue nil)
            if meta.is_a?(Array) && meta[0] && !meta[0].is_a?(Integer)
              next meta
            end
            # [0] veio como Integer => o marshal estragou o simbolo do treinador.
            # Recuperar o nome do sprite SEM hardcoding (serve QUALQUER jogo):
            #  (1) do proprio array: o charset do treinador (meta[1]) segue a
            #      convencao Essentials "trainer_<TIPO>"; tirar o prefixo da' o
            #      nome usado em PlayerSprites/ (== nome do TrainerType).
            #  (2) se falhar, varrer Graphics/Pictures/PlayerSprites/ e usar o
            #      id-esimo sprite base (sem sufixo de outfit _N).
            __sym = nil
            if meta.is_a?(Array) && meta[1].is_a?(String) && !meta[1].empty?
              __nm = meta[1].sub(/\Atrainer_/i, "")
              __sym = __nm.to_sym unless __nm.empty?
            end
            if __sym.nil? && id.is_a?(Integer) && id >= 0
              __base = (Dir.entries("Graphics/Pictures/PlayerSprites") rescue [])
              __base = __base.select { |f| f =~ /\.(png|gif)\z/i }
                             .map    { |f| f.sub(/\.[^.]+\z/, "") }
                             .reject { |f| f =~ /_\d+\z/ }
                             .uniq.sort
              __sym = __base[id].to_sym if __base[id]
            end
            if __sym
              if (($__mkxp_gpsfix_log ||= 0) < 3) && ($__mkxp_gpsfix_log += 1)
                dbg "[GPS-FIX] meta[0] veio Integer p/ id=#{id}; recuperado :#{__sym} (meta=#{meta.inspect[0,80]})"
              end
              rest = (meta.is_a?(Array) && meta.size > 1) ? meta[1..-1] : []
              next ([__sym] + rest)
            end
            meta
          end
          dbg "[GPS-FIX] get_player robusto instalado (em Game.init, Metadata carregado)"
        end
      else
        dbg "[GPS-FIX] AVISO: GameData::Metadata ausente em Game.init"
      end
    rescue => e
      dbg "[GPS-FIX] falhou em Game.init: #{e.class}: #{e.message}"
    end
    # [SAVETIME-FIX 2026-07-02] O jogo FECHAVA no arranque em get_newest_save_slot:
    # "comparison of Integer with Time". Um slot devolve last_time_saved como Integer
    # (epoch de um save legado), e SaveData.get_newest_save_slot faz
    # `save_time > Time.at(0)` -> Integer#> nao conhece a classe Time do motor (este
    # build nao tem mruby-time) -> ArgumentError -> Scene_Intro morre -> o jogo fecha.
    # A tentativa no debug_probe.rb NAO pegava: o probe corre a t~3.8s, ANTES de Player
    # existir, e os seus deferidos so' disparam no 1o frame de Scene_Map (overworld),
    # que nunca chega porque o crash e' no TITULO. AQUI (Game.init, t~6.7s, Player ja'
    # definido -- o GPS-FIX acima prova-o -- e ANTES do titulo/closeTitle) normalizamos
    # o getter: se last_time_saved for numerico devolve Time.at(v) -> a comparacao passa
    # a Time#> (Comparable, robusto). So' age quando o valor esta' errado -> zero mudanca
    # no caso bom. Idempotente (guarda pelo alias). Compat com saves que guardaram epoch.
    begin
      if defined?(Player) && Player.method_defined?(:last_time_saved) &&
         !Player.method_defined?(:__mkxp_orig_lts)
        Player.send(:alias_method, :__mkxp_orig_lts, :last_time_saved)
        Player.send(:define_method, :last_time_saved) do
          v = __mkxp_orig_lts
          (v.is_a?(Integer) || v.is_a?(Float)) ? (Time.at(v) rescue v) : v
        end
        dbg "[SAVETIME-FIX] Player#last_time_saved coage epoch->Time (get_newest_save_slot nao rebenta)"
      else
        dbg "[SAVETIME-FIX] Player/last_time_saved ausente ou ja' patchado em Game.init (nada a fazer)"
      end
    rescue => e
      dbg "[SAVETIME-FIX] falhou: #{e.class}: #{e.message}"
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
    # even?/odd? NAO existem no mruby 3.2.0. Sem eles, codigo como
    # "x = 16 + (i.even? ? 0 : 358)" (plugin Gender selection) lanca
    # NoMethodError -> apanhado pelo method_missing -> o ternario cai sempre no
    # ramo errado e TODOS os bonecos vao para a mesma coluna (sobrepostos).
    # Registar aqui resolve isso de forma generica p/ qualquer plugin RMXP.
    unless method_defined?(:even?);  def even?; (self % 2) == 0; end; end
    unless method_defined?(:odd?);   def odd?;  (self % 2) != 0; end; end
  end
  dbg "[INT-FIX] Integer#alpha/red/green/blue/even?/odd? adicionados OK"
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

      # ── FIX REGRESSAO (dialogo com skin errado) ───────────────────────────────
      # O setSkin() do Essentials chama pbResolveBitmap() ANTES de criar o
      # AnimatedBitmap e faz "return if nil_or_empty?". No 3DS, FileTest.exist?/
      # File.exist? falham para alguns windowskins (ex. "speech se 1") mesmo
      # existindo -- entao o setSkin aborta e a janela fica com o skin do
      # initialize (pbGetSystemFrame = "choice se 1", 48x48). Resultado: o
      # DIALOGO (que faz setSkin("speech se 1")) ficava com o skin de ESCOLHA.
      # Os windowskins sao carregados pelo loader C++ (o @sysframe ja' o prova:
      # AnimatedBitmap.new("choice se 1") funciona). Por isso, para caminhos
      # "Graphics/Windowskins/*", confiamos no loader e devolvemos o caminho SEM
      # verificar a existencia (se faltar, o loader mostra fallback visivel).
      # Resolve o setSkin do dialogo e mantem o Yes/No correto. Generico p/ Essentials.
      begin
        sw = s.gsub("\\", "/")
        if sw.start_with?("Graphics/Windowskins/") || sw.start_with?("graphics/Windowskins/")
          noext_ws = sw
          begin
            dot   = sw.rindex(".")
            slash = sw.rindex("/")
            noext_ws = sw[0...dot] if dot && slash && dot > slash && (sw.length - dot) <= 5
          rescue
            noext_ws = sw
          end
          # ── Redirect de windowskins CONHECIDAS em falta no JOGO ──────────────
          # 'speech black' e' pedida pela janela de NOME do orador (plugin, quando
          # isDark==true: setSkin("Graphics/Windowskins/speech black")), mas esse
          # ficheiro NAO existe nos Graphics deste jogo -> a box do nome ("Auntie")
          # ficava magenta/invisivel. Caimos numa skin EXISTENTE para a box
          # aparecer. So' actua se 'speech black' realmente NAO existir: se a
          # adicionares a Graphics/Windowskins/, passa a usar a tua.
          begin
            __ws_missing = { "Graphics/Windowskins/speech black" => "Graphics/Windowskins/speech bw 1" }
            if __ws_missing.key?(noext_ws)
              # [WSKIN-CACHE 2026-07-02] memoizar o resultado: isto corre a CADA
              # mensagem (setSkin do dialogo) e re-fazia stat+log de ~100ms por caixa
              # (10x no log 08:39). 1a vez resolve e guarda; seguintes = lookup O(1).
              $__ws_redir_cache ||= {}
              if $__ws_redir_cache.key?(noext_ws)
                __c = $__ws_redir_cache[noext_ws]
                return __c if __c
              elsif !(safeExists?(noext_ws + ".png") rescue false)
                __ws_alt = __ws_missing[noext_ws]
                if (safeExists?(__ws_alt + ".png") rescue false)
                  dbg "[BMP|WSKIN-REDIR] '#{noext_ws}' (em falta no jogo) -> '#{__ws_alt}' (cacheado)"
                  $__ws_redir_cache[noext_ws] = __ws_alt
                  return __ws_alt
                else
                  $__ws_redir_cache[noext_ws] = nil
                end
              else
                $__ws_redir_cache[noext_ws] = nil
              end
            end
          rescue
          end
          dbg "[BMP|WSKIN-TRUST] '#{s}' -> '#{noext_ws}' (confia no loader)" if ($__mkxp_wskin_trust_logged ||= 0) < 4 && ($__mkxp_wskin_trust_logged += 1)
          return noext_ws
        end
      rescue
      end
      # ──────────────────────────────────────────────────────────────────────────

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
    # P4 (PROVADO via Scripts.rxdata 000_Settings.rb + 080_MessageConfig.rb):
    # ha DOIS frames distintos no Essentials:
    #   - SPEECH frame: dialogo (Window_AdvancedTextPokemon). pbGetSpeechFrame ->
    #     SPEECH_WINDOWSKINS[textskin]. Indice 0 = "speech se 1" (96x48).
    #   - SYSTEM frame: menus e Yes/No (Window_CommandPokemonEx, via
    #     SpriteWindow_Base#initialize -> pbGetSystemFrame -> MENU_WINDOWSKINS[frame]).
    #     Indice 0 = "choice se 1" (48x48).
    # O bug: estavamos a forcar AMBOS para SPEECH_WINDOWSKINS[0], pondo o Yes/No
    # com o skin de dialogo (96x48) em vez do de escolha (48x48). Resultado: caixa
    # Yes/No com geometria errada. Fix: cada frame usa o seu proprio array.
    speech_path = nil
    system_path = nil
    begin
      if defined?(Settings) && Settings.const_defined?(:SPEECH_WINDOWSKINS) &&
         Settings::SPEECH_WINDOWSKINS.is_a?(Array) && !Settings::SPEECH_WINDOWSKINS.empty?
        speech_path = "Graphics/Windowskins/" + Settings::SPEECH_WINDOWSKINS[0].to_s
      end
    rescue; end
    begin
      if defined?(Settings) && Settings.const_defined?(:MENU_WINDOWSKINS) &&
         Settings::MENU_WINDOWSKINS.is_a?(Array) && !Settings::MENU_WINDOWSKINS.empty?
        system_path = "Graphics/Windowskins/" + Settings::MENU_WINDOWSKINS[0].to_s
      end
    rescue; end
    speech_path ||= "Graphics/Windowskins/001-Blue01"   # default universal (RESCHECK confirma que existe)
    system_path ||= speech_path                          # se nao houver MENU_WINDOWSKINS, cai no speech

    # Setar DIRETO nas class-vars do MessageConfig, contornando pbResolveBitmap.
    # (pbSetSpeechFrame faria @@x = pbResolveBitmap(v) || "" -> voltaria a "".)
    need_speech = (MessageConfig.pbGetSpeechFrame.to_s.empty? rescue true)
    need_system = (MessageConfig.pbGetSystemFrame.to_s.empty? rescue true)
    begin
      MessageConfig.class_variable_set(:@@defaultTextSkin, speech_path) if need_speech
    rescue; end
    begin
      MessageConfig.class_variable_set(:@@systemFrame, system_path) if need_system
    rescue; end
    dbg "[WSKIN] forcado DIRETO speech='#{speech_path}' system='#{system_path}' (speech=#{need_speech} system=#{need_system})"
    sf = (MessageConfig.pbGetSpeechFrame rescue "?")
    syf = (MessageConfig.pbGetSystemFrame rescue "?")
    dbg "[WSKIN] pbGetSpeechFrame='#{sf}' pbGetSystemFrame='#{syf}'"
  end
rescue => e
  dbg "[WSKIN] forcar windowskin falhou: #{e.class}: #{e.message}"
end
# === /FORCAR windowskin ======================================================

# -- WSKIN DINAMICO: usar o textskin/frame REAIS do save (nao o indice 0) ------
# O patch acima (FORCAR windowskin) memoiza @@defaultTextSkin com o INDICE 0
# ("speech se 1"), porque o pbGetSpeechFrame original falha (pbResolveBitmap nao
# resolve caminhos relativos no 3DS e devolve "" -> caixa invisivel). Mas isso
# IGNORA a skin que o jogador escolheu (Settings::SPEECH_WINDOWSKINS[textskin]).
# Aqui substituimos pbGetSpeechFrame/pbGetSystemFrame para devolverem o caminho
# DIRETO da skin do textskin/frame ATUAIS (lidos do save), sem pbResolveBitmap
# (o loader C++ resolve o caminho na mesma, prefixando sdmc:/mkxp/game/). Assim
# a caixa de fala usa exatamente a skin do jogo, igual ao PC. Re-resolve sempre
# (nao memoiza) -> se o jogador mudar a skin nas opcoes, atualiza logo.
# class << MessageConfig: funciona quer MessageConfig seja module quer class.
begin
  if Object.const_defined?(:MessageConfig) && defined?(Settings) && Settings.const_defined?(:SPEECH_WINDOWSKINS)
    class << MessageConfig
      def pbGetSpeechFrame
        arr = Settings::SPEECH_WINDOWSKINS
        return "Graphics/Windowskins/001-Blue01" if !arr.is_a?(Array) || arr.empty?
        idx = ($PokemonSystem.textskin rescue 0) || 0
        idx = 0 if idx < 0 || idx >= arr.length
        return "Graphics/Windowskins/" + arr[idx].to_s
      end
      def pbGetSystemFrame
        arr = (defined?(Settings) && Settings.const_defined?(:MENU_WINDOWSKINS)) ? Settings::MENU_WINDOWSKINS : nil
        return MessageConfig.pbGetSpeechFrame if !arr.is_a?(Array) || arr.empty?
        idx = ($PokemonSystem.frame rescue 0) || 0
        idx = 0 if idx < 0 || idx >= arr.length
        return "Graphics/Windowskins/" + arr[idx].to_s
      end
    end
    (MKXPDebug.log("[WSKIN-DYN] speech/system frame dinamicos (usam textskin/frame REAIS do save)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[WSKIN-DYN] override falhou: #{e.class}: #{e.message}") rescue nil)
end
# === /WSKIN DINAMICO =========================================================

# -- SOMBRA do JOGADOR: seguir a posicao REAL do player no ecra ----------------
# A sombra (Sprite_OWShadow) posiciona-se em @rsprite.x/y (o sprite principal do
# personagem). No teu jogo o sprite principal do player fica DESALINHADO (a
# sombra aparece no centro/mesa enquanto o player visivel esta' noutro sitio).
# So' para a sombra do PLAYER (@event == $game_player), forcamos a posicao a
# partir de $game_player.screen_x/screen_y -- onde o player e' REALMENTE
# desenhado. No caso normal isto da' o mesmo valor (sem mexer); no caso bugado,
# corrige. NPCs ficam intactos na logica original. Idempotente (alias 1x).
begin
  if Object.const_defined?(:Sprite_OWShadow)
    class Sprite_OWShadow
      unless method_defined?(:__df_orig_owshadow_update)
        alias_method :__df_orig_owshadow_update, :update
        def update
          __df_orig_owshadow_update
          begin
            if @sprite && !@sprite.disposed? && @event && @event == $game_player
              @sprite.x = @event.screen_x
              @sprite.y = @event.screen_y
            end
          rescue
          end
        end
      end
    end
    (MKXPDebug.log("[OWSHADOW-FIX] sombra do player segue screen_x/y de $game_player") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[OWSHADOW-FIX] falhou: #{e.class}: #{e.message}") rescue nil)
end
# === /SOMBRA do JOGADOR ======================================================

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

# === CFIX: recorte de windowskin GENERICO (corrige molduras pretas) =========
# O CORNERFIX/BACKFILL original (mais abaixo) so' actua em
# Window_AdvancedTextPokemon#text= -> so' o DIALOGO fica bem. As outras janelas
# (Window_CommandPokemon=Yes/No, Window_TextEntry e Window_UnformattedTextPokemon
# do teclado de nome) herdam de SpriteWindow_Base mas NUNCA passam por esse hook,
# por isso a moldura sai PRETA no 3DS (os cantos ficam com a skin 96x48 inteira em
# vez de recortada). Aqui aplicamos o MESMO recorte ao nivel da BASE, mas de forma
# OTIMIZADA para o ARM11: event-driven (corre 1x no fim do initialize de cada
# janela + 1x se a skin mudar via windowskin=), NUNCA por-frame. Idempotente: sai
# de imediato se os cantos ja' estiverem recortados (src_rect < skin). Custo por
# frame = ZERO. Generico p/ qualquer jogo RMXP/Essentials.
begin
  if Object.const_defined?(:SpriteWindow_Base)
    class SpriteWindow_Base
      def __mkxp_cfix!
        sp = (@sprites rescue nil); sk = (self.windowskin rescue nil)
        return unless sp.is_a?(Hash) && sk && !(sk.disposed? rescue true)
        skw = (sk.width rescue 0); skh = (sk.height rescue 0)
        return unless skw.is_a?(Integer) && skh.is_a?(Integer) && skw > 0 && skh > 0
        wx = (self.x rescue 0); wy = (self.y rescue 0)
        # DIAGNOSTICO (teclado): despejar as pecas das janelas de entrada de
        # texto para localizar os "quadrados brancos" da img3. So' as classes do
        # teclado, e so' as primeiras vezes (limite por classe), p/ nao poluir.
        begin
          cn = self.class.to_s
          if cn == 'Window_CharacterEntry' || cn == 'Window_TextEntry'
            $__mkxp_kbdump ||= {}
            if ($__mkxp_kbdump[cn] ||= 0) < 1
              $__mkxp_kbdump[cn] += 1
              dbg "[KBDUMP] === #{cn} skin=#{skw}x#{skh} win=(#{wx},#{wy}) #{(self.width rescue '?')}x#{(self.height rescue '?')} bopac=#{(self.back_opacity rescue '?')} opac=#{(self.opacity rescue '?')} ==="
              sp.each do |kk, ss|
                next if ss.nil?
                bb2 = (ss.bitmap rescue nil)
                bs2 = (bb2 && !(bb2.disposed? rescue true)) ? "#{(bb2.width rescue '?')}x#{(bb2.height rescue '?')}" : "nil"
                sr2 = (ss.src_rect rescue nil)
                srs = sr2 ? "[#{(sr2.x rescue '?')},#{(sr2.y rescue '?')},#{(sr2.width rescue '?')},#{(sr2.height rescue '?')}]" : "[-]"
                # cor central do bitmap da peca: revela se alguma peca tem pixels
                # dourados (origem do axadrezado amarelo do teclado, ainda nao provada)
                pxc = "-"
                if bb2 && !(bb2.disposed? rescue true)
                  pw = (bb2.width rescue 0); ph = (bb2.height rescue 0)
                  if pw > 0 && ph > 0
                    cc = (bb2.get_pixel(pw/2, ph/2) rescue nil)
                    pxc = cc ? "#{(cc.red rescue '?')},#{(cc.green rescue '?')},#{(cc.blue rescue '?')},a#{(cc.alpha rescue '?')}" : "?"
                  end
                end
                dbg "[KBDUMP]  #{kk}: bmp=#{bs2} src=#{srs} xy=(#{(ss.x rescue '?')},#{(ss.y rescue '?')}) o=(#{(ss.ox rescue '?')},#{(ss.oy rescue '?')}) z=#{(ss.z rescue '?')} op=#{(ss.opacity rescue '?')} vis=#{(ss.visible rescue '?')} cxc=#{pxc}"
              end
            end
          end
        rescue
        end
        ccx = wx + (self.width rescue 0) / 2; ccy = wy + (self.height rescue 0) / 2
        # ============================================================
        # FORMATO DA SKIN (PROVADO via codigo Essentials 082_SpriteWindow
        # + medicao dos pixels das skins reais):
        # As skins "speech se 1" (96x48) e "choice se 1" (48x48) NAO sao
        # windowskins de 9 fatias de 16px. O Essentials trata-as com
        # skinformat=1 (em __setWindowskin: tudo o que nao e' 192x128 nem
        # 128x128 vira skinformat=1). Com skinformat=1, o privRefresh usa:
        #   skinrect = ((skw-16)/2, (skh-16)/2, 16, 16)
        #   sx,sy = skinrect.x, skinrect.y ; cx,cy = sx+16, sy+16
        #   endX,endY = skw-cx, skh-cy
        # e recorta as 9 pecas com cantos ASSIMETRICOS. Para 96x48 os cantos
        # sao 40x16 (nao 16x16!); para 48x48 sao 16x16. Replicamos isso
        # EXATAMENTE -- e' o que o jogo desenha no PC. Aplicar a tira/back
        # esticada por cima garante o miolo cheio mesmo se a skin for so' fundo.
        # ============================================================
        # GUARDA skinformat: este recorte assimetrico SO' se aplica a skins
        # skinformat=1. O Essentials (__setWindowskin) define skinformat=0 para
        # 192x128 (RPGXP) e 128x128 (RPGVX) -- essas usam cantos FIXOS 16x16 e o
        # proprio jogo ja' as recorta bem. Aplicar a logica (skw-16)/2 a elas
        # daria cantos 56x56/88x56 (errado). So' actuamos no formato pequeno.
        __is_fmt0 = ((skw == 192 && skh == 128) || (skw == 128 && skh == 128))
        if __is_fmt0
          # skinformat=0: nao mexer nos cantos (o jogo recorta a 16x16 sozinho).
          sx = 16; sy = 16; cxr = skw - 16; cyr = skh - 16
          endX = 16; endY = 16
          lW = 16; rW = 16; tH = 16; bH = 16
          nfx = 0
        else
        sx = (skw - 16) / 2; sy = (skh - 16) / 2
        sx = 0 if sx < 0; sy = 0 if sy < 0
        cxr = sx + 16; cyr = sy + 16
        endX = skw - cxr; endY = skh - cyr
        endX = 0 if endX < 0; endY = 0 if endY < 0
        # lW/rW/tH/bH = espessura das bordas (p/ posicionar o miolo). No
        # skinformat=1 a borda esq/dir tem largura sx/endX e topo/base sy/endY.
        lW = sx; rW = endX; tH = sy; bH = endY
        lW = 16 if lW <= 0; rW = 16 if rW <= 0; tH = 16 if tH <= 0; bH = 16 if bH <= 0
        # Mapa dos 4 cantos: [chave] => [src_x, src_y, src_w, src_h]
        __corner_src = {
          'corner0' => [0,    0,    sx,   sy  ],
          'corner1' => [cxr,  0,    endX, sy  ],
          'corner2' => [0,    cyr,  sx,   endY],
          'corner3' => [cxr,  cyr,  endX, endY],
        }
        __side_src = {
          'side0' => [sx,   0,    16,   sy  ],  # topo
          'side1' => [0,    sy,   sx,   16  ],  # esquerda
          'side2' => [cxr,  sy,   endX, 16  ],  # direita
          'side3' => [sx,   cyr,  16,   endY],  # base
        }
        nfx = 0
        sp.each do |k, s|
          next if s.nil?
          ks = k.to_s
          cb = (s.bitmap rescue nil); next if cb.nil?
          next unless ((cb.width rescue 0) == skw && (cb.height rescue 0) == skh)
          rr = __corner_src[ks] || __side_src[ks]
          next if rr.nil?
          # Aplicar SEMPRE (idempotente): o jogo as vezes deixa src vazio
          # (teclado Window_TextEntry mostra src=[-]), outras recorta mal.
          # Recortar a regiao EXATA do skinformat=1 corrige ambos os casos.
          next if rr[2] <= 0 || rr[3] <= 0
          s.src_rect = Rect.new(rr[0], rr[1], rr[2], rr[3]); nfx += 1
        end
        end
        # NOTA: nao fazemos 'return if nfx == 0' aqui. Mesmo que os cantos ja'
        # estejam recortados (idempotencia), o MIOLO pode ainda nao estar
        # esticado (a janela cresce de tamanho DEPOIS do initialize). Por isso
        # o stretch da 'back' abaixo corre sempre que houver tamanho valido.
        # NOTA P1 + Regressao teclado (PROVADO via SPRDUMP + pixels das skins):
        # NAO pintamos as 'side' (isso destruia as bordas -> FAIXAS PRETAS).
        # MAS a peca 'back' (bitmap 4x64) e' uma fatia da COLUNA ESQUERDA da skin,
        # que NEM SEMPRE e' o fundo: em "speech se 1" (96x48) a coluna esq e'
        # opaca (fundo), mas em "choice se 1" (48x48) e' transparente+borda branca
        # -> estica-la dava o miolo AXADREZADO do teclado. O CENTRO da skin e'
        # que e' SEMPRE o fundo (medido: (7,7,7,224) em ambas). Por isso pintamos
        # SO' a 'back' (uma peca, nao as side -> sem faixas) com a cor central da
        # skin e esticamo-la para cobrir o miolo. Robusto p/ qualquer skin.
        fillc = (sk.get_pixel(skw / 2, skh / 2) rescue nil)
        fillc = Color.new(8, 8, 8, 255) if fillc.nil?
        fillc = Color.new(8, 8, 8, 224) if (fillc.alpha rescue 0) < 8
        bk = sp['back']
        if bk
          bb = (bk.bitmap rescue nil)
          if bb && !(bb.disposed? rescue true)
            bw = (bb.width rescue 0); bh = (bb.height rescue 0)
            # ROBUSTEZ MAXIMA (PROVADO via FRAMEDUMP): as 'side' tem bitmap proprio
            # transparente -> as faixas entre cantos e miolo ficavam vazias. Em vez
            # de depender das side, esticamos o 'back' para cobrir a JANELA INTEIRA
            # (full width/height, na posicao wx,wy). Assim NAO ha transparencia em
            # lado nenhum. Os cantos (opacos, da skin) desenham POR CIMA nas 4
            # esquinas (z igual, mas back e' criado/desenhado primeiro -> fica atras),
            # dando o detalhe de canto. O texto (contents) desenha por cima de tudo.
            iw = (self.width rescue 0)
            ih = (self.height rescue 0)
            if bw > 0 && bh > 0 && iw > 0 && ih > 0
              vx = 0; vy = 0
              vp = (bk.viewport rescue nil)
              if vp && (vp.rect rescue nil); vx = (vp.rect.x rescue 0); vy = (vp.rect.y rescue 0); end
              (bb.fill_rect(0, 0, bw, bh, fillc) rescue nil)
              (bk.src_rect = Rect.new(0, 0, bw, bh) rescue nil)
              (bk.zoom_x = iw.to_f / bw rescue nil)
              (bk.zoom_y = ih.to_f / bh rescue nil)
              (bk.ox = 0 rescue nil); (bk.oy = 0 rescue nil)
              (bk.x = (wx - vx) rescue nil)
              (bk.y = (wy - vy) rescue nil)
              dbg "[CFIX] #{self.class} back FULL #{iw}x#{ih} (bmp #{bw}x#{bh} fill=#{fillc.red rescue '?'},#{fillc.green rescue '?'},#{fillc.blue rescue '?'},a#{fillc.alpha rescue '?'})" if ($__cfix_logn ||= 0) < 24 && ($__cfix_logn += 1)
            end
          end
        end
        # PREENCHER AS 4 SIDE (faixas entre cantos e miolo) -- PROVADO via FRAMEDUMP:
        # no formato skinformat=1 destas skins, as 'side' top/left/right/bottom tem
        # BITMAP PROPRIO (480x16, 40x64, etc) que fica TRANSPARENTE (alpha 0) -> as
        # faixas entre os cantos e o miolo ficavam vazias e via-se o fundo/personagem.
        # Cada side e' um bitmap independente; basta pinta-lo opaco com a cor central
        # da skin (a mesma do miolo). Os cantos (opacos) desenham por cima nas
        # esquinas, por isso nao ha sobreposicao errada. So' actua no formato pequeno
        # (skinformat=1); para 128/192 (skinformat=0) as side vem da skin e nao se tocam.
        if !__is_fmt0
          ['side0','side1','side2','side3'].each do |sk_name|
            ss = sp[sk_name]
            next if ss.nil?
            sbb = (ss.bitmap rescue nil)
            next if sbb.nil? || (sbb.disposed? rescue true)
            # SO' pintar se a side tem bitmap PROPRIO (nao a windowskin recortada):
            # se o bitmap for a skin (96x48), ja' foi recortado acima e nao se pinta.
            sbw = (sbb.width rescue 0); sbh = (sbb.height rescue 0)
            next if sbw <= 0 || sbh <= 0
            next if (sbw == skw && sbh == skh)   # e' a skin recortada -> nao mexer
            (sbb.fill_rect(0, 0, sbw, sbh, fillc) rescue nil)
            # garantir que desenha o bitmap todo (sem src_rect a cortar)
            (ss.src_rect = Rect.new(0, 0, sbw, sbh) rescue nil)
          end
          dbg "[CFIX] #{self.class} sides preenchidas (fill=#{fillc.red rescue '?'},#{fillc.green rescue '?'},#{fillc.blue rescue '?'},a#{fillc.alpha rescue '?'})" if ($__cfix_logn ||= 0) < 24 && ($__cfix_logn += 1)
        end
        dbg "[CFIX] #{self.class} #{nfx} cantos recortados (skin #{skw}x#{skh})" if nfx > 0 && ($__cfix_logn ||= 0) < 24 && ($__cfix_logn += 1)
      end
      unless method_defined?(:__mkxp_cfix_init)
        alias_method :__mkxp_cfix_init, :initialize
        def initialize(*a, &b)
          __mkxp_cfix_init(*a, &b)
          (__mkxp_cfix! rescue nil)
        end
        alias_method :__mkxp_cfix_wsset, :windowskin=
        def windowskin=(v)
          __mkxp_cfix_wsset(v)
          (__mkxp_cfix! rescue nil)
        end
        # Re-correr quando a janela MUDA DE TAMANHO: muitas janelas (Yes/No,
        # comandos, dialogo) nascem pequenas no initialize e so' recebem o
        # tamanho final mais tarde via width=/height=. Sem isto, o miolo ficava
        # do tamanho inicial (pequeno) e o centro aparecia vazio. Event-driven:
        # so' dispara quando o valor MUDA mesmo (nao por-frame). Custo ~zero.
        if method_defined?(:width=)
          alias_method :__mkxp_cfix_wset, :width=
          def width=(v)
            __ov = (@width rescue nil)
            r = __mkxp_cfix_wset(v)
            (__mkxp_cfix! rescue nil) if __ov != v
            r
          end
        end
        if method_defined?(:height=)
          alias_method :__mkxp_cfix_hset, :height=
          def height=(v)
            __ov = (@height rescue nil)
            r = __mkxp_cfix_hset(v)
            (__mkxp_cfix! rescue nil) if __ov != v
            r
          end
        end
        # Re-correr no UPDATE apenas para as janelas de ENTRADA DE TEXTO (teclado).
        # PROVADO (KBDUMP + img4): no teclado (Window_TextEntry/CharacterEntry) as
        # pecas teem back GRANDE (ex 448x160) e o proprio Window faz refresh INTERNO
        # que re-blitta a skin DEPOIS do CFIX do initialize, desfazendo o recorte
        # dos cantos e o preenchimento do miolo -> cantos 48x48 brancos + miolo
        # axadrezado. O dialogo nao tem este problema porque o BACKFILL re-aplica a
        # cada text=. Aqui damos o mesmo tratamento: re-aplicar o CFIX no update,
        # LIMITADO as primeiras chamadas (a janela estabiliza depressa) para nao
        # pesar por-frame no ARM11. So' afeta classes de teclado (zero custo nas
        # outras janelas, que nem entram neste ramo).
        if method_defined?(:update)
          alias_method :__mkxp_cfix_update, :update
          def update(*a, &b)
            r = __mkxp_cfix_update(*a, &b)
            begin
              cn = self.class.to_s
              if cn == 'Window_CharacterEntry' || cn == 'Window_TextEntry' ||
                 cn == 'Window_TextEntry_Keyboard'
                n = (@__mkxp_cfix_upd ||= 0)
                if n < 4
                  @__mkxp_cfix_upd = n + 1
                  (__mkxp_cfix! rescue nil)
                end
              end
            rescue; end
            r
          end
        end
      end
    end
    dbg "[CFIX] recorte de windowskin instalado em SpriteWindow_Base (event-driven, 0 custo/frame)"
  end
rescue => e
  dbg "[CFIX] instalacao falhou: #{e.class}: #{e.message}"
end
# === /CFIX ==================================================================

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
        # [CACHE-SAFE-DISPOSE 2026-07-05] RAIZ do "Gilbert invisivel na costura"
        # (provada pelo XRAY: bmp 256x256 -> DISPOSED apos DES#refresh):
        # @bitmap vem DIRETO da RPG::Cache (partilhado por todos os sprites com o
        # mesmo charset); o dispose original fazia @bitmap.dispose -> matava o
        # bitmap NA CACHE -> o proximo pbGetCharset devolvia o cadaver e TODOS os
        # sprites desse charset ficavam invisiveis p/ sempre. No RGSS real o
        # AnimatedBitmap e' wrapper por-sprite; aqui o dispose passa a marcar
        # apenas. (Os raros fallbacks BitmapWrapper diretos ficam para o GC.)
        def dispose
          @disposed = true
        end
        def disposed?
          @disposed ? true : false
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
          # ===== [EBDX-DEFER] carregar dados de batalha AGORA (pos-titulo) =====
          ($__mkxp_ebdx_setup.call rescue nil) if $__ebdx_setup_pending && !$__ebdx_setup_done && $__mkxp_ebdx_setup
          # ===== [TITLE-SWEEP v7 2026-07-11] despojar paineis do titulo AQUI =====
          # O gancho antigo (scene loop pos Scene_Intro) NUNCA dispara no fluxo
          # real; createSpritesets corre em TODAS as entradas de mapa. O sweep
          # esconde+DESPOJA (pixels+tex) so' sprites com a assinatura dos
          # paineis (viewport>=99990 e z 990..99989) — liberta os MBs que
          # afogavam a composicao do chao. Idempotente e barato (so' varre).
          (MKXPDebug.sweep_title rescue nil)
          # ===== [TSCACHE-EVICT 2026-07-11] RAM: despejar tilesets GIGANTES =====
          # O RPG::Cache guarda cada tileset para sempre -> apos 3 mapas havia
          # 3 gigantes residentes (Interior+Outside+Route = ~30MB = 47% da RAM
          # total!) e o linear morria (battlers sem textura, atlas furado).
          # Na (re)construcao dos spritesets: manter SO' os tilesets dos mapas
          # ATUAIS do factory; dispor os restantes >=2M px (so' gigantes; os
          # pequenos nao pesam e beneficiam do cache). O proximo uso re-carrega
          # do SD (~0.5s) -- raro (so' ao voltar a uma zona antiga).
          begin
            __keep = {}
            begin
              if $MapFactory && $MapFactory.respond_to?(:maps)
                $MapFactory.maps.each do |gm|
                  tn = (gm.tileset_name.to_s rescue "")
                  __keep["Graphics/Tilesets/" + tn] = true unless tn.empty?
                end
              end
            rescue; end
            begin
              tn = ($game_map.tileset_name.to_s rescue "")
              __keep["Graphics/Tilesets/" + tn] = true unless tn.empty?
            rescue; end
            __cc = (RPG::Cache.instance_variable_get(:@cache) rescue nil)
            if __cc.is_a?(Hash) && !__keep.empty?
              __cc.keys.each do |k|
                ks = (k.is_a?(Array) ? k[0].to_s : k.to_s) rescue next
                next unless ks.include?("Tilesets/")
                next if __keep[ks]
                b = __cc[k]
                next if b.nil?
                bw = (b.width rescue 0); bh = (b.height rescue 0)
                next if bw * bh < 2_000_000
                # [v2 2026-07-11] NAO dispor: o CustomTilemap pode ainda usar
                # este bitmap como fonte do compose (dispor deixava o chao
                # PRETO permanente). Basta soltar a referencia do cache — a
                # memoria e' recuperada pelo GIANT-LRU do motor (por recencia
                # de leitura) ou pelo GC quando o spriteset largar a dele.
                __cc.delete(k)
                dbg "[TSCACHE-EVICT v2] ref de cache do tileset '#{ks}' (#{bw}x#{bh}) solta"
              end
            end
          rescue => _ete
            dbg "[TSCACHE-EVICT] falhou: #{_ete.class}: #{_ete.message}"
          end
          # ===== FIX ECRA PRETO (entrar mapa ligado c/ tileset vizinho falhado) =====
          # O wrapper INTERNO do porte (__orig_update_3ds__) foi instalado no BOOT,
          # ANTES dos plugins. O Following Pokemon EX / EBDX embrulham
          # Spriteset_Map#update POR CIMA dele, por isso os crashes deles (ex:
          # "cannot set instance variable" quando o tileset do mapa vizinho falha
          # -> magenta) ESCAPAM ao guard interno e MATAM o createSpritesets ->
          # Graphics.transition() nunca corre -> ECRA PRETO/CONGELADO.
          # Aqui instalamos (1x, ja' com os plugins carregados) um wrapper EXTERNO
          # que apanha esses crashes: o mapa ACTUAL continua a desenhar; so' o
          # update do vizinho partido e' saltado. Resolve o preto sem tocar no jogo.
          unless $__ss_outer_guard_installed
            $__ss_outer_guard_installed = true
            begin
              if Object.const_defined?(:Spriteset_Map) && Spriteset_Map.method_defined?(:update) &&
                 !Spriteset_Map.method_defined?(:__ss_outer_orig_update)
                Spriteset_Map.class_eval do
                  alias_method :__ss_outer_orig_update, :update
                  def update(*a)
                    begin
                      __ss_outer_orig_update(*a)
                    rescue => _e
                      $__ss_outer_n = ($__ss_outer_n || 0) + 1
                      (dbg "[SS-OUTER-GUARD] update protegido (#{$__ss_outer_n}): #{_e.class}: #{_e.message}") if $__ss_outer_n <= 3
                      nil
                    end
                  end
                end
                dbg "[SS-OUTER-GUARD] wrapper EXTERNO de Spriteset_Map#update instalado (apanha plugins: Following Pkmn EX / EBDX)"
              end
            rescue => _ge
              dbg "[SS-OUTER-GUARD] instalacao falhou: #{_ge.class}: #{_ge.message}"
            end
          end
          begin
            maps = $MapFactory.maps rescue nil
            dbg "[createSpritesets] $MapFactory.maps=#{maps.nil? ? 'NIL' : "Array(#{maps.size})"}"
          rescue => e
            dbg "[createSpritesets] $MapFactory.maps CRASH: #{e.class}: #{e.message}"
          end
          # ===== [DISPOSE-FIRST 2026-07-02] RAIZ dos "2 Gilberts" =====
          # O createSpritesets do jogo (032_Scene_Map.rb:18) faz `@spritesets = {}`
          # -> LARGA os Spriteset_Map antigos SEM os dispor. Os Sprite_Character
          # antigos ficam registados no motor (g_sprites) e continuam a desenhar ->
          # PROVADO: [GILSPR] TOTAL=2 <<< DUPLICADO com 1 so' evento RIVAL1 vivo.
          # (O TSFIX do probe devia tratar isto mas nao instalou -- 0 no log.) Aqui,
          # no wrapper do motor que COMPROVADAMENTE corre, chamamos disposeSpritesets
          # ANTES de recriar: os sprites antigos sao libertados. So' quando ha'
          # spritesets antigos nao-vazios (a 1a criacao nao dispoe nada).
          # [DISPOSE-FIRST v2] robusto: sem respond_to? (nao fiavel neste mruby) e com
          # log de ESTADO sempre (p/ ver porque nao disparava). Dispoe cada spriteset
          # antigo diretamente; o disposeSpritesets do jogo so' poe [i]=nil, por isso
          # chamamos .dispose em cada VALOR nao-nil (liberta os Sprite_Character/texturas
          # duplicados que causam os 2 Gilberts E o chao preto por 26MB de texturas).
          begin
            _ss = @spritesets
            if _ss.is_a?(Hash) && _ss.size > 0
              _nd = 0
              _ss.each_value do |sset|
                next if sset.nil?
                (sset.dispose rescue nil)
                _nd += 1
              end
              @spritesets = {}
              dbg "[DISPOSE-FIRST] dispostos #{_nd}/#{_ss.size} spriteset(s) antigo(s) antes de recriar (anti 2-Gilberts + memoria)"
            else
              dbg "[DISPOSE-FIRST] nada a dispor (@spritesets=#{_ss.class} size=#{(_ss.size rescue '?')})"
            end
          rescue => _dfe
            dbg "[DISPOSE-FIRST] falhou: #{_dfe.class}: #{_dfe.message}"
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
            # RECUPERACAO anti-ecra-preto: libertar o freeze mesmo que algo escape.
            begin
              Graphics.transition(0)
              dbg "[createSpritesets] RECUPERACAO: Graphics.transition(0) forcado (anti-ecra-preto)"
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

    # -- [TRY-CONTINUE] (diagnostico, gated): replicar o fluxo REAL do Continuar --
    # Em vez do titulo, faz exatamente o que o Load Screen faz ao escolher o slot:
    # read_from_file + Game.load (load_all_values c/ SAVE-REPAIR + load_map +
    # Scene_Map). Se $MKXP_TRY_CONTINUE for [x,y], teleporta o jogador (para p'or
    # a camara em cima de um alvo de diagnostico, ex. a arvore do monumento).
    # Ativa-se no debug_probe.rb; OFF por defeito -> zero efeito no jogo normal.
    if ($MKXP_TRY_CONTINUE rescue nil) && $__debug_intro_count == 1
      begin
        _fp = nil
        begin
          _slot = SaveData.get_newest_save_slot
          _fp = SaveData::SAVE_DIR + "/" + _slot.to_s + ".rxdata" if _slot
        rescue; end
        _fp = (SaveData::FILE_PATH rescue nil) if _fp.nil? || !(File.file?(_fp) rescue false)
        if _fp && (File.file?(_fp) rescue false)
          dbg "[TRY-CONTINUE] a carregar #{_fp} pelo fluxo real (Game.load)"
          _sv = SaveData.read_from_file(_fp)
          Game.load(_sv)
          if $MKXP_TRY_CONTINUE.is_a?(Array) && $MKXP_TRY_CONTINUE.size == 2
            _tx, _ty = $MKXP_TRY_CONTINUE
            ($game_player.moveto(_tx, _ty) rescue nil)
            ($game_player.center(_tx, _ty) rescue nil)
            dbg "[TRY-CONTINUE] teleporte diagnostico p/ (#{_tx},#{_ty})"
          end
          dbg "[TRY-CONTINUE] Game.load OK: map=#{($game_map.map_id rescue '?')} player=(#{($game_player.x rescue '?')},#{($game_player.y rescue '?')}) scene=#{$scene.class} events=#{($game_map.events.size rescue '?')}"
          return
        else
          dbg "[TRY-CONTINUE] sem save legivel (#{_fp.inspect}) -> fluxo normal"
        end
      rescue => e
        dbg "[TRY-CONTINUE] FALHOU: #{e.class}: #{e.message.to_s[0,120]}"
        dbg "[TRY-CONTINUE] BT: #{(e.backtrace || [])[0,6].join(' | ')}"
        # cai para o fluxo normal (titulo) para o arranque nao morrer
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
      end
      # FIX #5: o Essentials define never_dispose como attr_accessor (allscripts
      # L15612), por isso o guard 'unless method_defined?' acima SALTA o nosso setter.
      # Redefinimos o setter INCONDICIONALMENTE aqui (corre depois do attr_accessor,
      # pos-scripts -> prevalece): never_dispose=true liga prefer_vram p/ a textura do
      # tileset ir para VRAM (libertando o heap linear p/ a camada de 4MB). Mantemos a
      # semantica (so' poe @never_dispose) e juntamos o hook.
      def never_dispose=(v); @never_dispose = v; (self.prefer_vram = !!v rescue nil); end
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
      unless $__mkxp_autotile_empty_logged
        $__mkxp_autotile_empty_logged = true
        dbg "[BMP] pbGetAutotile: nome vazio -> fallback 96x128 (slot autotile vazio = NORMAL; log 1x)"
      end
      return BitmapWrapper.new(96, 128)
    end
    # [LOG-LEAN] "tentando/resultado" por-autotile removido (2 linhas x ~8/mapa)
    begin
      bm = BitmapWrapper.new("Graphics/Autotiles/#{name}")
    rescue => e
      dbg "[BMP] pbGetAutotile ERRO: #{e.class}: #{e.message}"   # KEEP: erro
      bm = BitmapWrapper.new(96, 128)
    end
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
        # [ROUPEIRO-FIX] @@viewport2 = viewport das PICTURES. O patch SALTAVA-o -> ficava
        # z=0, EMPATADO com @@viewport1 (personagens). Os Sprite_Character always_on_top
        # (z=999) ganhavam o desempate e desenhavam POR CIMA das Pictures do menu (sintoma
        # do roupeiro). z=200 (default RGSS: pictures acima dos personagens) poe-nas no topo.
        @@viewport2 = Viewport.new(0, 0, _w, _h)
        @@viewport2.z = 200
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
                bt[0, 12].each_with_index { |l, i| dbg "[MFD] WARNING bt[#{i}]: #{l}" }
              end
            rescue; end
            # [INTRO-LEAK-FIX 2026-07-10] o crash salta o disposeTitle -> os paineis
            # do titulo (328x192/344x208, z=999) ficam VIVOS para sempre e desenham
            # por cima de TUDO (era o que tapava os battlers na batalha!). Dispor
            # explicitamente no crash.
            begin
              $scene.disposeTitle if $scene && $scene.respond_to?(:disposeTitle)
            rescue; end
            # [INTRO-RECOVER] intro crashou -> em vez de fechar o jogo, cair no ecra
            # de load (o mesmo destino do closeTitle normal). Se falhar, sai como antes.
            recovered = false
            begin
              if klass.to_s == "Scene_Intro" && defined?(PokemonLoadScreen) && defined?(PokemonLoad_Scene)
                dbg "[MFD] WARNING [INTRO-RECOVER] a recuperar do crash do intro -> load screen"
                $scene = nil
                sscreen = PokemonLoadScreen.new(PokemonLoad_Scene.new)
                sscreen.pbStartLoadScreen
                recovered = !$scene.nil?
              end
            rescue => _re
              dbg "[MFD] WARNING [INTRO-RECOVER] falhou: #{_re.class}: #{_re.message}"
            end
            # Nao parar -- tentar avancar para a proxima scene se possivel
            $scene = nil unless recovered
          end

          dbg "[MFD] scene.main done: #{klass} -> $scene=#{$scene.nil? ? 'nil' : $scene.class}"
          # [TITLE-SWEEP] o titulo (Modular Title Screen) NUNCA dispoe os paineis
          # z=998/999 -> tapavam os battlers na batalha. Ao sair do Scene_Intro
          # (qualquer caminho: normal, delete, crash-recover) esconde-os no motor.
          (MKXPDebug.sweep_title if klass.to_s == "Scene_Intro") rescue nil
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

# ─── CHAMAR O INSTALL DEFERIDO (RUBYPROF + tint cache + J3) ───────────────────
# Estamos no FIM do bloco MFD (~t=8.7s): os 376 scripts ja' correram e este bloco
# ja' corrigiu/definiu Spriteset_Map, Sprite_Character, etc. 'dbg' tambem ja'
# existe. E' o momento CERTO para instalar os patches que dependem das classes do
# jogo -- coisa que o compat_stubs.h (corre a t=0.5s, antes dos scripts) NAO podia
# fazer. A lambda foi registada la' em $__mkxp_perf_install; chamamo-la agora.
# Cada ramo loga para o caso de erro se isolar no log sem adivinhar.
begin
  if $__mkxp_perf_install
    dbg "[PERF] a chamar install deferido (RUBYPROF + J3)..."
    $__mkxp_perf_install.call
    dbg "[PERF] install deferido concluido OK"
  else
    dbg "[PERF] AVISO: $__mkxp_perf_install AUSENTE -> compat_stubs.h antigo/errado no build"
  end
rescue => e
  dbg "[PERF] install deferido FALHOU: #{e.class}: #{e.message}"
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
        /* [GC-FPS v2] Gameplay: GERACIONAL + major automatico TRAVADO.
         * v1 (NAO-geracional, interval_ratio=150) ficou aquem (update so' 46->40ms,
         * valvula ainda dispara 50x). Medido: ~6500 allocs/frame de lixo de vida
         * curtissima (a valvula recupera ~270k em ~41 frames). Em nao-geracional o
         * ciclo incremental marca o LIVE-SET INTEIRO (~222k base ESTAVEL) quase
         * todos os frames -> desperdicio puro (re-marca dados que nao sao lixo).
         * CORRECAO: GERACIONAL. O minor-GC (unica recolha por-frame) so' marca a
         * young-gen (~1024 objs) + write-barriers old->young; NAO re-marca os 222k
         * base -> minors baratos (~2-5ms/frame). O que era caro era o MAJOR-GC
         * automatico (dispara a ~2x live = ~464k e varre tudo); fixamos
         * majorgc_old_threshold MUITO alto para ele NUNCA disparar no gameplay.
         * A old-gen e' drenada onde o hitch e' INVISIVEL: mrb_full_gc do grph_freeze
         * (cada transicao, ecra congelado) + a valvula GC (rede de seguranca).
         * Reverter: generational=FALSE. (interval_ratio e' irrelevante em geracional:
         * o minor usa threshold fixo ~1024.) */
        mrb->gc.generational          = TRUE;
        mrb->gc.majorgc_old_threshold = 3000000;   /* trava o major automatico in-frame */
        DBGLOG("[GC] boot concluido: GERACIONAL + majorgc_old_threshold=3000000 "
               "(minors baratos sobre young-gen; major automatico travado; "
               "old-gen drenada no freeze/valvula) (full_gc %d -> %d objs)\n",
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
            "          __by = (self.borderY rescue 32)\n"
            "          __ch = (self.contents.height rescue 0)\n"
            "          __min = 64 + __by\n"
            "          __need = (__ch > 0) ? [__min, __ch + __by].max : __min\n"
            "          if __need != self.height\n"
            "            __gh = (Graphics.height rescue 384)\n"
            "            self.height = __need\n"
            "            self.y = __gh - self.height\n"
            "            self.y = 0 if self.y < 0\n"
            "            MKXPDebug.log(\"[BOXFIT] caixa -> #{self.width}x#{self.height} @(#{self.x},#{self.y}) (contents h=#{__ch} min=#{__min})\")\n"
            "          end\n"
            "        rescue => e\n"
            "          MKXPDebug.log(\"[BOXFIT] err #{e.class}: #{e.message}\")\n"
            "        end\n"
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
            "        begin\n"
            "          __n = (@@__sprdump_n rescue 0)\n"
            "          if __n < 2\n"
            "            @@__sprdump_n = __n + 1\n"
            "            __rv = (@rpgvx rescue '?')\n"
            "            __sk = (self.windowskin rescue nil)\n"
            "            __sks = __sk ? \"#{__sk.width}x#{__sk.height}\" : 'nil'\n"
            "            MKXPDebug.log(\"[SPRDUMP] === ##{@@__sprdump_n} rpgvx=#{__rv} skin=#{__sks} win=(#{self.x},#{self.y}) #{self.width}x#{self.height} ===\")\n"
            "            __sp = (@sprites rescue nil)\n"
            "            if __sp.is_a?(Hash)\n"
            "              __sp.each do |k, s|\n"
            "                next if s.nil?\n"
            "                __x=(s.x rescue '?'); __y=(s.y rescue '?'); __z=(s.z rescue '?')\n"
            "                __ox=(s.ox rescue 0); __oy=(s.oy rescue 0)\n"
            "                __r=(s.src_rect rescue nil)\n"
            "                __rs = __r ? \"#{__r.x},#{__r.y},#{__r.width},#{__r.height}\" : '-'\n"
            "                __b=(s.bitmap rescue nil)\n"
            "                __bd = (__b.disposed? rescue true)\n"
            "                __bs = (__b && !__bd) ? \"#{__b.width}x#{__b.height}\" : 'nil'\n"
            "                __v=(s.visible rescue '?')\n"
            "                MKXPDebug.log(\"[SPRDUMP]  H['#{k}'] @(#{__x},#{__y}) z=#{__z} o=(#{__ox},#{__oy}) src=[#{__rs}] bmp=#{__bs} vis=#{__v}\")\n"
            "              end\n"
            "            else\n"
            "              MKXPDebug.log(\"[SPRDUMP]  sem @sprites Hash\")\n"
            "            end\n"
            "          end\n"
            "        rescue => e\n"
            "          MKXPDebug.log(\"[SPRDUMP] err #{e.class}: #{e.message}\")\n"
            "        end\n"
            "        begin\n"
            "          __csp = (@sprites rescue nil)\n"
            "          __csk = (self.windowskin rescue nil)\n"
            "          if __csp.is_a?(Hash) && __csk && !(__csk.disposed? rescue true)\n"
            "            __skw = __csk.width; __skh = __csk.height\n"
            "            __wx=(self.x rescue 0); __wy=(self.y rescue 0)\n"
            "            __ccx = __wx + (self.width rescue 0)/2; __ccy = __wy + (self.height rescue 0)/2\n"
            "            # GUARDA skinformat: recorte assimetrico SO' p/ skinformat=1.\n"
            "            # 192x128 e 128x128 sao skinformat=0 (cantos fixos 16x16, o jogo\n"
            "            # recorta-os sozinho) -- nao mexer.\n"
            "            __isfmt0 = ((__skw == 192 && __skh == 128) || (__skw == 128 && __skh == 128))\n"
            "            if __isfmt0\n"
            "              __lW = 16; __rW = 16; __tH = 16; __bH = 16; __nfx = 0\n"
            "            else\n"
            "            __sx = (__skw - 16)/2; __sy = (__skh - 16)/2\n"
            "            __sx = 0 if __sx < 0; __sy = 0 if __sy < 0\n"
            "            __cxr = __sx + 16; __cyr = __sy + 16\n"
            "            __endX = __skw - __cxr; __endY = __skh - __cyr\n"
            "            __endX = 0 if __endX < 0; __endY = 0 if __endY < 0\n"
            "            __lW = __sx; __rW = __endX; __tH = __sy; __bH = __endY\n"
            "            __lW = 16 if __lW <= 0; __rW = 16 if __rW <= 0; __tH = 16 if __tH <= 0; __bH = 16 if __bH <= 0\n"
            "            __csrc = {\n"
            "              'corner0' => [0,      0,      __sx,   __sy  ],\n"
            "              'corner1' => [__cxr,  0,      __endX, __sy  ],\n"
            "              'corner2' => [0,      __cyr,  __sx,   __endY],\n"
            "              'corner3' => [__cxr,  __cyr,  __endX, __endY],\n"
            "              'side0'   => [__sx,   0,      16,     __sy  ],\n"
            "              'side1'   => [0,      __sy,   __sx,   16    ],\n"
            "              'side2'   => [__cxr,  __sy,   __endX, 16    ],\n"
            "              'side3'   => [__sx,   __cyr,  16,     __endY],\n"
            "            }\n"
            "            __nfx = 0\n"
            "            __csp.each do |__k, __s|\n"
            "              next if __s.nil?\n"
            "              __ks = __k.to_s\n"
            "              __cb = (__s.bitmap rescue nil); next if __cb.nil?\n"
            "              __cbw = (__cb.width rescue 0); __cbh = (__cb.height rescue 0)\n"
            "              next unless (__cbw == __skw && __cbh == __skh)\n"
            "              __rr = __csrc[__ks]\n"
            "              next if __rr.nil?\n"
            "              next if __rr[2] <= 0 || __rr[3] <= 0\n"
            "              __s.src_rect = Rect.new(__rr[0], __rr[1], __rr[2], __rr[3])\n"
            "              __nfx += 1\n"
            "            end\n"
            "            end\n"
            "            MKXPDebug.log(\"[CORNERFIX] #{__nfx} pecas recortadas skinfmt1 (L=#{__lW} R=#{__rW} T=#{__tH} B=#{__bH} skin=#{__skw}x#{__skh})\") if __nfx > 0 && ($__cbf_logn ||= 0) < 24 && ($__cbf_logn += 1)\n"
            "            __fillc = (__csk.get_pixel(__skw/2, __skh/2) rescue nil)\n"
            "            __fillc = Color.new(8,8,8,255) if __fillc.nil?\n"
            "            __fillc = Color.new(8,8,8,224) if (__fillc.alpha rescue 0) < 8\n"
            "            __filled = 0\n"
            "            # P1+teclado: NAO pintar as 'side' (faixas pretas). Pintar SO' a\n"
            "            # 'back' com a cor CENTRAL da skin (sempre fundo) e estica-la: o\n"
            "            # miolo fica cheio mesmo quando a coluna esq da skin e' transparente.\n"
            "            __bk = __csp['back']\n"
            "            if __bk\n"
            "              __bb = (__bk.bitmap rescue nil)\n"
            "              if __bb && !(__bb.disposed? rescue true)\n"
            "                __bw = (__bb.width rescue 0); __bh = (__bb.height rescue 0)\n"
            "                __iw = (self.width rescue 0)\n"
            "                __ih = (self.height rescue 0)\n"
            "                if __bw > 0 && __bh > 0 && __iw > 0 && __ih > 0\n"
            "                  __vx = 0; __vy = 0\n"
            "                  __vp = (__bk.viewport rescue nil)\n"
            "                  if __vp && (__vp.rect rescue nil); __vx = (__vp.rect.x rescue 0); __vy = (__vp.rect.y rescue 0); end\n"
            "                  (__bb.fill_rect(0, 0, __bw, __bh, __fillc) rescue nil)\n"
            "                  (__bk.src_rect = Rect.new(0, 0, __bw, __bh) rescue nil)\n"
            "                  (__bk.zoom_x = __iw.to_f / __bw rescue nil)\n"
            "                  (__bk.zoom_y = __ih.to_f / __bh rescue nil)\n"
            "                  (__bk.ox = 0 rescue nil); (__bk.oy = 0 rescue nil)\n"
            "                  (__bk.x = (__wx - __vx) rescue nil)\n"
            "                  (__bk.y = (__wy - __vy) rescue nil)\n"
            "                  __filled += 1\n"
            "                end\n"
            "              end\n"
            "            end\n"
            "            MKXPDebug.log(\"[BACKFILL] back pint+esticado (#{__fillc.red rescue '?'},#{__fillc.green rescue '?'},#{__fillc.blue rescue '?'},a#{__fillc.alpha rescue '?'})\") if __filled > 0 && ($__cbf_logn ||= 0) < 24 && ($__cbf_logn += 1)\n"
            "            # PREENCHER AS 4 SIDE (faixas entre cantos e miolo). No formato\n"
            "            # skinformat=1 as side tem bitmap PROPRIO transparente -> faixas\n"
            "            # vazias. Pintar cada uma opaca com a cor central. So' skinfmt1.\n"
            "            if !__isfmt0\n"
            "              ['side0','side1','side2','side3'].each do |__sn|\n"
            "                __ss = __csp[__sn]\n"
            "                next if __ss.nil?\n"
            "                __sbb = (__ss.bitmap rescue nil)\n"
            "                next if __sbb.nil? || (__sbb.disposed? rescue true)\n"
            "                __sbw = (__sbb.width rescue 0); __sbh = (__sbb.height rescue 0)\n"
            "                next if __sbw <= 0 || __sbh <= 0\n"
            "                next if (__sbw == __skw && __sbh == __skh)\n"
            "                (__sbb.fill_rect(0, 0, __sbw, __sbh, __fillc) rescue nil)\n"
            "                (__ss.src_rect = Rect.new(0, 0, __sbw, __sbh) rescue nil)\n"
            "              end\n"
            "              MKXPDebug.log(\"[CORNERFIX] sides preenchidas\")\n"
            "            end\n"
            "          end\n"
            "        rescue => __ce\n"
            "          MKXPDebug.log(\"[CORNERFIX] err #{__ce.class}: #{__ce.message}\")\n"
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

    /* [TRAINERID-N 2026-07-09] MIGRACAO probe->motor do TRAINERIDFIX (raiz #4,
     * overflow MRB_INT32). Trainer#initialize do jogo faz
     *   @id = rand(2**16) | rand(2**16) << 16    (max 2^32-1)
     * mas o mrb_int e' int32 COM SINAL -> o <<16 estoura -> promove a Float ->
     * `Integer | Float` = TypeError -> Player.new falha -> $Trainer nil -> cartao
     * de treinador vazio (excecao engolida por rescues do jogo). FIX: @id com
     * secret_ID a 15 bits (max = INT32_MAX exato); public_ID mantem 16 bits.
     * Mesma guarda $__traineridfix_installed do probe: o motor corre PRIMEIRO ->
     * o bloco equivalente do probe ve a guarda e salta (migracao limpa; remover
     * do probe apos 2 ciclos confirmados, regra §9.4). Regiao pos-plugins: nada
     * de plugin redefine Trainer#initialize (verificado nos dumps). */
    {
        static const char *trainerid_fix = R"TIDFIX(
begin
  if defined?(Trainer) && Trainer.method_defined?(:initialize) && !$__traineridfix_installed
    $__traineridfix_installed = true
    class Trainer
      def initialize(name, trainer_type)
        @trainer_type = GameData::TrainerType.get(trainer_type).id
        @name         = name
        @id           = rand(2 ** 16) | (rand(2 ** 15) << 16)   # int32-safe
        @language     = (pbGetLanguage rescue 2)
        @party        = []
      end
    end
    MKXPDebug.log("[TRAINERID-N] Trainer#initialize int32-safe instalado pelo MOTOR (probe passa a saltar)") rescue nil
  end
rescue => e
  MKXPDebug.log("[TRAINERID-N] falhou: #{e.class}: #{e.message}") rescue nil
end
)TIDFIX";
        mrb_load_string(mrb, trainerid_fix);
        if (mrb->exc) { mrb->exc = 0; }
    }

    /* [FOLLOWER-SHIM + CBWRAP 2026-07-10] Os 2 desbloqueadores da BATALHA
     * (diagnostico BATALHA_DIAGNOSTICO_FINAL.md, 5 agentes):
     * FIX1: $PokemonGlobal.follower_toggled nao declarado -> auto-stub devolve 0
     *   (truthy!) -> EliteBattle.follower()=0 -> send-out do player faz next/early
     *   -return -> SEM sprite do treinador, SEM pokebola, SEM reveal, databox unica
     *   ("Pikachu 20/20" = a caixa do FOLLOWER do jogador; o foe sempre foi o Mareep).
     *   Shim: attr_accessor real -> nil genuino -> follower()=nil -> fluxo normal.
     * FIX2: CallbackWrapper#execute fazia instance_variable_set num ARRAY (mruby:
     *   RArray sem iv_tbl -> "cannot set instance variable", o [IVSET-DIAG] @scene
     *   em Array) -> TODAS as animacoes comuns (pokebola, captura, transicoes custom)
     *   abortavam. Substituir por um contexto-objeto. Pos-plugins (0015 ja definiu). */
    {
        static const char *battle_unblockers = R"BUNBLK(
begin
  if defined?(PokemonGlobalMetadata)
    class PokemonGlobalMetadata
      attr_accessor :follower_toggled unless method_defined?(:follower_toggled)
    end
    MKXPDebug.log("[FOLLOWER-SHIM] WARNING follower_toggled real (nil) -- send-out do player desbloqueado") rescue nil
  end
rescue; end
begin
  # [FOLLOWER-KILL] ha' DUAS definicoes de EliteBattle.follower (0061 + 0077
  # FollowingPkmn-compat, a ultima vence) e a 0077 esta' envenenada por OUTRO stub
  # (FollowingPkmn.active? -> 0 truthy) -> early-return no send-out na mesma.
  # O "walk into battle" do follower nao funciona no port de qualquer forma ->
  # matar deterministicamente AS DUAS no motor: follower() = nil SEMPRE.
  if defined?(EliteBattle)
    (class << EliteBattle; self; end).class_eval do
      define_method(:follower) { |*a| nil }
    end
    MKXPDebug.log("[FOLLOWER-KILL] WARNING EliteBattle.follower()=nil definitivo (ambas as defs mortas)") rescue nil
  end
rescue; end
begin
  # [EVUPD-MEMO 2026-07-11, achado da pesquisa] Game_Event#should_update?
  # (046:245) corre POR EVENTO POR FRAME e faz `@event.name[/update/i]` — um
  # REGEX sobre um nome IMUTAVEL, 30-60x por frame. Memoizar o regex (1x por
  # evento) corta ~0.5-2ms/frame no updateMaps. Corpo identico ao v19.1, so' o
  # regex vira flag cacheada. Sonda por alias (nunca respond_to?).
  if Object.const_defined?(:Game_Event)
    class Game_Event
      begin
        alias __mkxp_memo_probe should_update?
        unless method_defined?(:__mkxp_upd_memoized)
          def __mkxp_upd_memoized; true; end
          def should_update?(recalc = false)
            return @to_update if !recalc
            return true if @trigger && (@trigger == 3 || @trigger == 4)
            return true if @move_route_forcing
            if @__mkxp_name_upd.nil?
              @__mkxp_name_upd = (@event.name[/update/i] ? true : false) rescue false
            end
            return true if @__mkxp_name_upd
            range = 2
            return false if self.screen_x - @sprite_size[0]/2 > Graphics.width + range * Game_Map::TILE_WIDTH
            return false if self.screen_x + @sprite_size[0]/2 < -range * Game_Map::TILE_WIDTH
            return false if self.screen_y_ground - @sprite_size[1] > Graphics.height + range * Game_Map::TILE_HEIGHT
            return false if self.screen_y_ground < -range * Game_Map::TILE_HEIGHT
            return true
          end
          (MKXPDebug.log("[EVUPD-MEMO] should_update? com regex memoizado") rescue nil)
        end
      rescue NameError
        # jogo sem should_update? (nao e' v19) -> nada a fazer
      end
    end
  end
rescue; end
begin
  # [EBDX-FRAME0 2026-07-11] RAM da batalha: BitmapEBDX#refresh construia TODOS
  # os frames do strip (ate 104 x Bitmap 140x140 = 8MB por battler) MESMO com
  # @@disableBitmapAnimation=true (a flag so travava o update). Com a animacao
  # de frames desligada no port (EBDXFIX do probe), construir SO' o frame 0:
  # -6 a -30MB por batalha. Reversivel: reativar animacao remove o cap.
  if Object.const_defined?(:BitmapEBDX)
    class BitmapEBDX
      unless method_defined?(:__mkxp_f0_orig_refresh)
        alias_method :__mkxp_f0_orig_refresh, :refresh
        def refresh(bitmaps = nil)
          __f0 = (self.class.class_variable_get(:@@disableBitmapAnimation) rescue false)
          if __f0 && bitmaps.nil? && @bitmapFile.is_a?(String)
            self.dispose
            f_bmp = Bitmap.new(@bitmapFile)
            @width  = f_bmp.height * @scale
            @height = f_bmp.height * @scale
            bitmap = Bitmap.new(@width, @height)
            bitmap.stretch_blt(Rect.new(0, 0, @width, @height), f_bmp,
                               Rect.new(0, 0, f_bmp.height, f_bmp.height))
            @bitmaps.push(bitmap)
            f_bmp.dispose
            if @bitmaps.length < 1 && !self.is_bitmap?
              EliteBattle.log.error("Unable to construct bitmap sheet (frame0) `#{@bitmapFile}`") rescue nil
            end
            if !self.is_bitmap?
              @totalFrames = @bitmaps.length
              @animationFrames = @totalFrames * @frames
              @tempBmp = Bitmap.new(@bitmaps[0].width, @bitmaps[0].width)
            end
          else
            __mkxp_f0_orig_refresh(bitmaps)
          end
        end
        MKXPDebug.log("[EBDX-FRAME0] WARNING refresh frame-0-only armado (poupa ate 30MB/batalha)") rescue nil
      end
    end
  end
rescue => e
  (MKXPDebug.log("[EBDX-FRAME0] falhou: #{e.class}: #{e.message}") rescue nil)
end
begin
  # [FPKMN-OFF] FollowingPkmn.active? stub-0-truthy envenena varios ramos do EBDX.
  # Se o modulo existir mas active? vier do stub, define-o real: false.
  if defined?(FollowingPkmn)
    _real = (FollowingPkmn.respond_to?(:active?) rescue false)
    unless _real
      (class << FollowingPkmn; self; end).class_eval do
        define_method(:active?) { |*a| false }
      end
      MKXPDebug.log("[FPKMN-OFF] WARNING FollowingPkmn.active?=false real definido") rescue nil
    end
  end
rescue; end
begin
  if defined?(CallbackWrapper)
    class CallbackContext
      def initialize(args); @args = args; end
    end
    class CallbackWrapper
      def execute(block, *args)
        ctx = CallbackContext.new(args)
        (@params || {}).each { |k, v| ctx.instance_variable_set("@#{k}", v) }
        ctx.instance_eval(&block)
      end
    end
    MKXPDebug.log("[CBWRAP] WARNING CallbackWrapper sem ivars-em-Array -- animacoes comuns desbloqueadas") rescue nil
  end
rescue; end
)BUNBLK";
        mrb_load_string(mrb, battle_unblockers);
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

  # ---- DIAGNOSTICO TEMPORARIO: entrada de nome (teclado BW Text Entry) ----
  # O teclado de nome no Essentials passa por pbEnterText -> cria uma cena
  # (PokemonEntryScene / PokemonEntryScene2) e chama pbStartScene. O ecra fica
  # preso quando essa cena nao desenha ou rebenta em pbStartScene. Este bloco:
  #  (1) LISTA que metodos/classes de entrada existem mesmo (sabermos o nome real)
  #  (2) faz hook a pbStartScene das cenas p/ apanhar ENTROU/OK/CRASH+backtrace.
  begin
    cand_m = [:pbEnterText, :pbEnterPlayerName, :pbChooseGender, :pbTrainerName, :pbProcessPlayerInput]
    presentes = cand_m.select { |m| (Object.method_defined?(m) || (Object.allocate.respond_to?(m, true) rescue false)) }
    MKXPDebug.log("[NAMEIN] metodos-entrada presentes: #{presentes.inspect}")
    cand_c = ["PokemonEntryScene","PokemonEntryScene2","PokemonEntryScene3","PokemonEntry","PokemonEntry2"]
    cls = cand_c.select { |c| (Object.const_defined?(c) rescue false) }
    MKXPDebug.log("[NAMEIN] classes-entrada presentes: #{cls.inspect}")
  rescue => e
    MKXPDebug.log("[NAMEIN] inventario falhou: #{e.class}: #{e.message}")
  end

  # ---- CORRECAO: pbTrainerName robusto -------------------------------------
  # O log provou: o evento da Trainer Card chama pbTrainerName(nil,0) via um
  # "Script" event command (command_355). A versao do jogo rebenta logo no corpo
  # com "ArgumentError: comparison of String with Integer failed" (um "<" que
  # compara o nome String com um Integer tipo MAX_PLAYER_NAME_SIZE). Como o eval
  # rebenta e o motor re-tenta o command_355 todos os frames, o jogo fica PRESO e
  # o teclado nunca chega a ser instanciado. Nao podemos editar o script do jogo,
  # por isso substituimos pbTrainerName por uma versao a prova de tipos que usa o
  # teclado (pbEnterPlayerName/pbEnterText, ja carregados) com min/max Integer
  # garantidos, e aplica o nome a $player/$Trainer. Generico p/ qualquer jogo
  # RMXP/Essentials. O hook [NAMEIN] global instalado a seguir envolve esta versao
  # (mantendo o logging CHAMADO/-> intacto).
  begin
    if Object.method_defined?(:pbTrainerName) && !Object.method_defined?(:__pretn_orig)
      Object.send(:alias_method, :__pretn_orig, :pbTrainerName)
    end
    def __pretn_int(v, dflt)
      return v if v.is_a?(Integer)
      begin
        iv = Integer(v) rescue nil
        iv.is_a?(Integer) ? iv : dflt
      rescue
        dflt
      end
    end
    def pbTrainerName(*args)
      MKXPDebug.log("[NAMEIN] pbTrainerName(robusto) args=#{args.inspect}") rescue nil
      name = args[0]
      if name.is_a?(String) && name.length > 0
        newname = name
      else
        maxlen = 12
        begin
          if defined?(Settings) && Settings.const_defined?(:MAX_PLAYER_NAME_SIZE)
            maxlen = __pretn_int(Settings::MAX_PLAYER_NAME_SIZE, 12)
          end
        rescue
          maxlen = 12
        end
        maxlen = 12 if !maxlen.is_a?(Integer) || maxlen <= 0
        help = "Your name?"
        newname = nil
        if Object.method_defined?(:pbEnterPlayerName) || (respond_to?(:pbEnterPlayerName, true) rescue false)
          begin
            newname = pbEnterPlayerName(help, 1, maxlen)
          rescue => e
            MKXPDebug.log("[NAMEIN] pbEnterPlayerName falhou: #{e.class}: #{e.message}") rescue nil
            newname = nil
          end
        end
        if !(newname.is_a?(String) && newname.length > 0)
          begin
            newname = pbEnterText(help, 1, maxlen, "")
          rescue => e
            MKXPDebug.log("[NAMEIN] pbEnterText falhou: #{e.class}: #{e.message}") rescue nil
            newname = nil
          end
        end
        newname = "Player" if !(newname.is_a?(String) && newname.length > 0)
      end
      outfit_val = (args[1].is_a?(Integer) ? args[1] : 0)
      # CORRECAO: neste port o setup de novo-jogo nao chegou a criar $Trainer
      # (SaveData.load_new_game_values / bloco :player nao correu), por isso o
      # nome nao tinha onde ser guardado e o menu/Trainer Card/save ficam sem
      # treinador ("The player trainer was not defined"). Criamos aqui o
      # $Trainer espelhando o new_game_value do Essentials (Player.new("Unnamed",
      # primeiro trainer_type)), damos-lhe identidade/sprite com pbChangePlayer(0)
      # e marcamos begunNewGame. Generico p/ qualquer jogo RMXP/Essentials.
      begin
        if !((defined?($Trainer) && $Trainer) || (defined?($player) && $player)) && defined?(Player)
          tt = nil
          (GameData::TrainerType.each { |t| tt = t.id; break }) rescue nil
          tr = (Player.new("Unnamed", tt) rescue (Player.new("Unnamed", 0) rescue nil))
          if tr
            $Trainer = tr
            $player  = tr
            MKXPDebug.log("[NAMEIN] $Trainer CRIADO (estava nil) ttype=#{tt.inspect}") rescue nil
            begin
              pbChangePlayer(0) if (respond_to?(:pbChangePlayer, true) rescue false) && tr.respond_to?(:character_ID) && tr.character_ID < 0
              MKXPDebug.log("[NAMEIN] pbChangePlayer(0) char_ID=#{(tr.character_ID rescue '?')}") rescue nil
            rescue => e3
              MKXPDebug.log("[NAMEIN] pbChangePlayer falhou: #{e3.class}: #{e3.message}") rescue nil
            end
          else
            MKXPDebug.log("[NAMEIN] Player.new falhou (tt=#{tt.inspect}) - $Trainer continua nil") rescue nil
          end
        end
      rescue => e
        MKXPDebug.log("[NAMEIN] criar $Trainer falhou: #{e.class}: #{e.message}") rescue nil
      end
      begin
        tgt = ((defined?($player) && $player) || (defined?($Trainer) && $Trainer) || nil)
        if tgt && tgt.respond_to?(:name=)
          tgt.name = newname
          tgt.outfit = outfit_val if tgt.respond_to?(:outfit=)
          $PokemonTemp.begunNewGame = true if defined?($PokemonTemp) && $PokemonTemp && $PokemonTemp.respond_to?(:begunNewGame=)
          MKXPDebug.log("[NAMEIN] nome aplicado a #{tgt.class}: #{newname.inspect} outfit=#{outfit_val}") rescue nil
        else
          MKXPDebug.log("[NAMEIN] AVISO: sem $Trainer/$player para aplicar nome") rescue nil
        end
      rescue => e
        MKXPDebug.log("[NAMEIN] aplicar nome falhou: #{e.class}: #{e.message}") rescue nil
      end
      MKXPDebug.log("[NAMEIN] pbTrainerName(robusto) -> #{newname.inspect}") rescue nil
      newname
    end
    MKXPDebug.log("[NAMEIN] PATCH pbTrainerName robusto instalado OK")
  rescue => e
    MKXPDebug.log("[NAMEIN] PATCH pbTrainerName falhou: #{e.class}: #{e.message}")
  end

  # ---- CORRECAO: teclado BW -> modo SETAS (input nativo 3DS) ----------------
  # O plugin "BW Text Entry Scene" (PokemonEntryScene) tem USEKEYBOARD=true, que
  # escolhe o caminho pbEntry1: le o nome de um TECLADO FISICO de PC via
  #   Input.text_input = true ; Input.triggerex?(:RETURN) ; Input.triggerex?(:ESCAPE)
  # Nada disto existe no 3DS (sem teclado fisico) nem no nosso binding nativo, por
  # isso o loop nunca recebe input: nao escreve e A/Enter nunca confirma -> PRESO.
  # Forcando USEKEYBOARD=false, o plugin usa pbEntry2 (teclado de SETAS on-screen)
  # que le SO Input.trigger?(USE) e Input.repeat?(UP/DOWN/LEFT/RIGHT) -- tudo ja
  # suportado nativamente em C++. E' tambem o unico modo navegavel num 3DS (d-pad
  # + A). Generico p/ qualquer jogo que use este plugin. remove_const evita o aviso
  # de redefinicao. Idempotente (so' age se ainda for !=false).
  begin
    if Object.const_defined?(:PokemonEntryScene)
      kls = Object.const_get(:PokemonEntryScene)
      if kls.is_a?(Class) && kls.const_defined?(:USEKEYBOARD) && kls::USEKEYBOARD != false
        kls.send(:remove_const, :USEKEYBOARD)
        kls.const_set(:USEKEYBOARD, false)
        MKXPDebug.log("[NAMEIN] PATCH USEKEYBOARD=false (teclado de setas, input nativo 3DS) OK")
      end
    end
  rescue => e
    MKXPDebug.log("[NAMEIN] PATCH USEKEYBOARD falhou: #{e.class}: #{e.message}")
  end
  # ---- FIM CORRECAO ---------------------------------------------------------

  # ---- DIAGNOSTICO appearance (Gender selection) ----------------------------
  # So' LOGGING -- nao altera comportamento. Confirma quantos bonecos a cena
  # "Select your appearance" mostra. A duvida: no video PC vem-se 10 bonecos,
  # mas no 3DS so' 4. A matematica (abaixo, no log) prova que os 10 CABEM no
  # ecra fisico, logo NAO e' o emulador a corta-los: o numero vem de
  # @realquant = GenderPickSelection.quant_registered_player (metadata do jogo).
  # Este hook regista o valor real em runtime + a posicao fisica de cada boneco
  # (VIS/FORA), para decidir com prova em vez de adivinhar. Idempotente,
  # protegido por begin/rescue, e so' age se o plugin existir. Usa (i % 2 == 0)
  # em vez de i.even? por compatibilidade. Generico: nao mexe no jogo.
  begin
    if Object.const_defined?(:GenderPickSelection)
      __gps = Object.const_get(:GenderPickSelection)
      if __gps.const_defined?(:Show)
        __gkls = __gps.const_get(:Show)
        if __gkls.is_a?(Class) && __gkls.method_defined?(:create_scene) &&
           !__gkls.method_defined?(:__diag_create_scene)
          __gkls.send(:alias_method, :__diag_create_scene, :create_scene)
          __gkls.send(:define_method, :create_scene) do
            r = __diag_create_scene
            begin
              rq = (instance_variable_get(:@realquant) rescue nil)
              pl = (instance_variable_get(:@player) rescue nil)
              MKXPDebug.log("[APPEAR] @realquant=#{rq.inspect} @player.size=#{(pl ? pl.size : nil).inspect}")
              if rq.is_a?(Integer)
                rq.times { |i|
                  x = 16 + ((i % 2 == 0) ? 0 : 358)
                  y = 52 + 64 * (i / 2)
                  fx = (x * 0.625 + 40).to_i
                  fy = (y * 0.625).to_i
                  vis = (fx < 400 && fy < 240) ? "VIS" : "FORA"
                  MKXPDebug.log("[APPEAR]   boneco #{i}: log(#{x},#{y}) fis(#{fx},#{fy}) #{vis}")
                }
              end
            rescue => e
              MKXPDebug.log("[APPEAR] log falhou: #{e.class}: #{e.message}")
            end
            r
          end
          MKXPDebug.log("[APPEAR] hook GenderPickSelection::Show#create_scene instalado OK")
        end
      end
    end
  rescue => e
    MKXPDebug.log("[APPEAR] PATCH falhou: #{e.class}: #{e.message}")
  end
  # APPEAR2: inspecionar os SPRITES dos bonecos depois de criados, para saber
  # PORQUE so' metade aparece. Hookamos update_player (onde o plugin posiciona
  # e poe o bitmap de cada boneco) e, na 1a passagem, logamos por boneco: se o
  # bitmap carregou (largura), z, visibilidade e posicao. Se os da esquerda
  # (pares) tiverem bmp=nil/0x0 -> e' carregamento de asset; se tiverem bitmap
  # OK mas nao aparecem -> e' z/ordem/tapados. So' loga 1x (@__appear_dumped).
  begin
    if Object.const_defined?(:GenderPickSelection)
      __gps2 = Object.const_get(:GenderPickSelection)
      if __gps2.const_defined?(:Show)
        __gk2 = __gps2.const_get(:Show)
        if __gk2.is_a?(Class) && __gk2.method_defined?(:update_player) &&
           !__gk2.method_defined?(:__diag_update_player)
          __gk2.send(:alias_method, :__diag_update_player, :update_player)
          __gk2.send(:define_method, :update_player) do
            r = __diag_update_player
            begin
              __cnt = (instance_variable_get(:@__appear_dumps) rescue 0) || 0
              if __cnt < 6
                instance_variable_set(:@__appear_dumps, __cnt + 1)
                sp = (instance_variable_get(:@sprites) rescue nil)
                rq = (instance_variable_get(:@realquant) rescue 0)
                bg = (instance_variable_get(:@bg) rescue '?')
                MKXPDebug.log("[APPEAR2] dump##{__cnt+1} @bg=#{bg} @realquant=#{rq.inspect}")
                if sp.is_a?(Hash) && rq.is_a?(Integer)
                  rq.times { |i|
                    s = sp["player #{i}"]
                    if s.nil?
                      MKXPDebug.log("[APPEAR2] boneco #{i}: SEM SPRITE")
                    else
                      b = (s.bitmap rescue nil)
                      bs = (b && !(b.disposed? rescue true)) ? "#{(b.width rescue '?')}x#{(b.height rescue '?')}" : "nil"
                      MKXPDebug.log("[APPEAR2] boneco #{i}: bmp=#{bs} z=#{(s.z rescue '?')} vis=#{(s.visible rescue '?')} xy=(#{(s.x rescue '?')},#{(s.y rescue '?')}) o=(#{(s.ox rescue '?')},#{(s.oy rescue '?')})")
                    end
                  }
                end
              end
            rescue => __e2
              MKXPDebug.log("[APPEAR2] dump falhou: #{__e2.class}: #{__e2.message}")
            end
            r
          end
          MKXPDebug.log("[APPEAR2] hook update_player instalado OK")
        end
      end
    end
  rescue => __e2
    MKXPDebug.log("[APPEAR2] PATCH falhou: #{__e2.class}: #{__e2.message}")
  end
  # ---- FIM DIAGNOSTICO appearance -------------------------------------------

  # ---- CORRECAO #7: ecra preto do picker GenderPickSelection -----------------
  # Causa: a cena "what picture you'd like to use?" constroi o caminho da imagem
  # a partir de GameData::Metadata.get_player(i)[0], que DEVIA ser o simbolo do
  # treinador (ex. :POKEMONTRAINER_Sorin). No metadata.dat o dado esta' correto
  # (player_A..player_H + player_K/_L = 10 jogadores), MAS o marshal do port le'
  # o simbolo DENTRO do array como 0 -> meta[0]=0 -> pede "PlayerSprites/0" ->
  # falha -> paineis vazios/escuros -> ecra preto.
  # Fix (so' no port, runtime): se o [0] ja' veio bom, confia; senao recupera o
  # nome do sprite a partir dos PROPRIOS dados do jogo (charset "trainer_<TIPO>"
  # em meta[1], ou varrendo a pasta PlayerSprites/), PRESERVANDO o resto do array
  # (charsets) quando o marshal so' estragou o [0]. Idempotente e protegido.
  # Sem nomes hardcoded -> serve qualquer jogo Essentials.
  begin
    if defined?(GameData) && GameData.const_defined?(:Metadata)
      __MD = GameData::Metadata
      unless __MD.respond_to?(:__mkxp_orig_get_player)
        __MD.singleton_class.send(:alias_method, :__mkxp_orig_get_player, :get_player)
        __MD.define_singleton_method(:get_player) do |id|
          meta = (__mkxp_orig_get_player(id) rescue nil)
          # [0] valido (simbolo ou string, NAO inteiro) -> usar tal e qual
          if meta.is_a?(Array) && meta[0] && !meta[0].is_a?(Integer)
            next meta
          end
          # [0] veio como Integer => o marshal estragou o simbolo do treinador.
          # Recuperar o nome do sprite SEM hardcoding (serve QUALQUER jogo):
          #  (1) charset do treinador (meta[1]) segue a convencao Essentials
          #      "trainer_<TIPO>"; tirar o prefixo da' o nome de PlayerSprites/.
          #  (2) senao, varrer Graphics/Pictures/PlayerSprites/ e usar o id-esimo
          #      sprite base (sem sufixo de outfit _N).
          __sym = nil
          if meta.is_a?(Array) && meta[1].is_a?(String) && !meta[1].empty?
            __nm = meta[1].sub(/\Atrainer_/i, "")
            __sym = __nm.to_sym unless __nm.empty?
          end
          if __sym.nil? && id.is_a?(Integer) && id >= 0
            __base = (Dir.entries("Graphics/Pictures/PlayerSprites") rescue [])
            __base = __base.select { |f| f =~ /\.(png|gif)\z/i }
                           .map    { |f| f.sub(/\.[^.]+\z/, "") }
                           .reject { |f| f =~ /_\d+\z/ }
                           .uniq.sort
            __sym = __base[id].to_sym if __base[id]
          end
          if __sym
            if (($__mkxp_gpsfix_log ||= 0) < 3) && ($__mkxp_gpsfix_log += 1)
              MKXPDebug.log("[GPS-FIX] meta[0] veio Integer p/ id=#{id}; recuperado :#{__sym} (meta=#{meta.inspect[0,80]})") rescue nil
            end
            rest = (meta.is_a?(Array) && meta.size > 1) ? meta[1..-1] : []
            next ([__sym] + rest)
          end
          meta  # nil para id>=10 -> quant_registered_player para' aqui (=> @realquant=10)
        end
        MKXPDebug.log("[GPS-FIX] get_player robusto instalado (picker usa nomes reais de treinador)") rescue nil
      end
    end
  rescue => __egps
    MKXPDebug.log("[GPS-FIX] falhou: #{__egps.class}: #{__egps.message}") rescue nil
  end
  # ---- FIM CORRECAO #7 -------------------------------------------------------

  # ---- CORRECAO #8: crash terrain_tag->valid? (Integer < nil) no mapa exterior ----
  # Overworld Shadows chama terrain_tag(x,y) durante o Spriteset init; se a coord
  # OU width/height vier nil (mapa ainda nao pronto), valid? faz "Integer < nil" e
  # rebenta (ArgumentError). Guarda: coord/dimensao nil => tile invalido (false).
  # So' previne o crash -- uma query a coordenada nil nunca seria valida na mesma.
  begin
    if Object.const_defined?(:Game_Map) && Game_Map.method_defined?(:valid?) &&
       !Game_Map.method_defined?(:__mkxp_orig_valid?)
      Game_Map.send(:alias_method, :__mkxp_orig_valid?, :valid?)
      Game_Map.send(:define_method, :valid?) do |x, y|
        next false if x.nil? || y.nil?
        w = (width rescue nil); h = (height rescue nil)
        next false if w.nil? || h.nil?
        (x >= 0 && x < w && y >= 0 && y < h)
      end
      MKXPDebug.log("[VALIDFIX] Game_Map#valid? robusto a coord/dimensao nil (anti-crash owshadow)") rescue nil
    end
  rescue => __ev8
    MKXPDebug.log("[VALIDFIX] falhou: #{__ev8.class}: #{__ev8.message}") rescue nil
  end
  # ---- FIM CORRECAO #8 -------------------------------------------------------

  # ---- LOG CIRURGICO #9: 'cannot set instance variable' (Following Pkmn) --------
  # Nao-fatal (o port recupera) mas queremos a causa EXACTA: que classe e que ivar.
  # Envolve Object#instance_variable_set SO' para apanhar a 1a falha, regista
  # classe+ivar+receiver, e depois RESTAURA o metodo original (zero peso por-frame
  # dai em diante). Re-lanca o erro -> comportamento inalterado (o port recupera).
  begin
    unless $__mkxp_ivset_diag
      $__mkxp_ivset_diag = true
      class Object
        alias_method :__mkxp_orig_ivset, :instance_variable_set
        def instance_variable_set(name, val)
          __mkxp_orig_ivset(name, val)
        rescue => __eiv
          begin
            MKXPDebug.log("[IVSET-DIAG] FALHOU set #{name} em classe=#{self.class} " \
                          "val=#{val.class} (#{__eiv.class}: #{__eiv.message})")
          rescue; end
          # auto-remover: repor o metodo C original para nao pesar mais
          Object.send(:alias_method, :instance_variable_set, :__mkxp_orig_ivset) rescue nil
          raise
        end
      end
      MKXPDebug.log("[IVSET-DIAG] sonda instance_variable_set instalada (auto-removivel)") rescue nil
    end
  rescue => __ed9
    MKXPDebug.log("[IVSET-DIAG] instalacao falhou: #{__ed9.class}: #{__ed9.message}") rescue nil
  end
  # ---- FIM LOG #9 ------------------------------------------------------------

  # ---- PERF: throttle de Sprite_Character FORA do ecra (mapa ativo) -----------
  # O J3 trata dos mapas VIZINHOS, mas no mapa exterior o mapa ATIVO tem dezenas
  # de NPCs (no log: 54). Sprite_Character#update corre p/ TODOS a 30fps, mesmo
  # os que estao muito fora do ecra (invisiveis). Aqui saltamos o update desses
  # sprites em 2 de cada 3 frames -- so' os CLARAMENTE fora do ecra (margem
  # generosa de ~3 tiles para nunca afetar nada visivel). Quando o sprite entra
  # na margem, volta a 30fps e sincroniza ANTES de ficar visivel (sem "pop").
  # A posicao logica do NPC (real_x/real_y) e' do jogo e nao e' tocada -- so' a
  # SINCRONIZACAO do sprite e' adiada. Reversivel: $MKXP_PERF_OFFSCREEN=false.
  begin
    $MKXP_PERF_OFFSCREEN = true if $MKXP_PERF_OFFSCREEN.nil?
    $__os_skipped = 0; $__os_ran = 0
    if Object.const_defined?(:Sprite_Character) &&
       Sprite_Character.method_defined?(:update) &&
       !Sprite_Character.method_defined?(:__mkxp_os_orig_update)
      Sprite_Character.send(:alias_method, :__mkxp_os_orig_update, :update)
      Sprite_Character.send(:define_method, :update) do |*a|
        if $MKXP_PERF_OFFSCREEN
          c = (@character rescue nil)
          if c
            sx = (c.screen_x rescue nil); sy = (c.screen_y rescue nil)
            # ecra util 0..512 x 0..384; margem 96px (~3 tiles) p/ seguranca
            if sx && sy && (sx < -96 || sx > 608 || sy < -96 || sy > 480)
              @__os_n = (@__os_n || 0) + 1
              if (@__os_n % 3) != 0
                $__os_skipped += 1
                next   # fora do ecra: salta 2 de 3 frames
              end
            end
          end
        end
        $__os_ran += 1
        __mkxp_os_orig_update(*a)
      end
      MKXPDebug.log("[OFFSCREEN] throttle de Sprite_Character fora-do-ecra instalado (every=3, flag $MKXP_PERF_OFFSCREEN)") rescue nil
    end
  rescue => __eos
    MKXPDebug.log("[OFFSCREEN] instalacao falhou: #{__eos.class}: #{__eos.message}") rescue nil
  end
  # ---- FIM PERF offscreen ----------------------------------------------------

  # ---- PERF FPS (Fase 1): throttle de Game_Event#update FORA do ecra ----------
  # MEDIDO no log (FPSPROBE, steady-state a andar): update=19ms = updateMaps~10.5
  # (Game_Map#update) + updateSpritesets~8.5 (ja' aliviado pelo OFFSCREEN acima) +
  # resto~0.5. O frame real e' ~40ms: 19ms Ruby + ~16-21ms GPU/compositing/swap.
  # A 60Hz, um frame de ~35-40ms perde o vsync de 33ms e salta para 50ms = 20fps.
  # Para ficar SOB 33ms (=30fps) basta cortar ~9ms ao update Ruby.
  # updateMaps = @events.each{ e.update }. O OFFSCREEN ja' aliviou os SPRITES dos
  # eventos invisiveis, mas a LOGICA (Game_Event#update: movimento/animacao) corre
  # para TODOS a 30fps. Aqui saltamos 2 de 3 frames a logica dos eventos CLARAMENTE
  # fora do ecra -- com as MESMAS margens do OFFSCREEN (consistente) e guardas duras:
  #   * NUNCA saltar trigger paralelo(3)/autorun(4)  -> logica de fundo intacta
  #   * NUNCA saltar com @move_route_forcing          -> cutscenes/movimentos guiados
  #   * so' eventos a >96px fora do ecra util (512x384) -> nada visivel e' afetado
  # A posicao logica (real_x/real_y) e' do jogo; so' adiamos quem nao se ve. Quando
  # o evento entra na margem volta a 30fps. Reversivel: $MKXP_PERF_EVENTSKIP=false.
  begin
    $MKXP_PERF_EVENTSKIP = true if $MKXP_PERF_EVENTSKIP.nil?
    $__ev_skipped = 0; $__ev_ran = 0
    if Object.const_defined?(:Game_Event) &&
       Game_Event.method_defined?(:update) &&
       !Game_Event.method_defined?(:__mkxp_ev_orig_update)
      Game_Event.send(:alias_method, :__mkxp_ev_orig_update, :update)
      Game_Event.send(:define_method, :update) do |*a|
        if $MKXP_PERF_EVENTSKIP
          tr = (@trigger rescue nil)              # 3=paralelo, 4=autorun
          if tr != 3 && tr != 4 && !(@move_route_forcing rescue false)
            sx = (screen_x rescue nil); sy = (screen_y rescue nil)
            if sx && sy && (sx < -96 || sx > 608 || sy < -96 || sy > 480)
              @__ev_n = (@__ev_n || 0) + 1
              if (@__ev_n % 3) != 0
                $__ev_skipped += 1
                next                              # fora do ecra: salta 2 de 3 frames
              end
            end
          end
        end
        $__ev_ran += 1
        __mkxp_ev_orig_update(*a)
      end
      MKXPDebug.log("[EVSKIP] throttle de Game_Event#update fora-do-ecra instalado (every=3; nao mexe em paralelo/autorun/move-route; flag $MKXP_PERF_EVENTSKIP)") rescue nil
    end
  rescue => __evs
    MKXPDebug.log("[EVSKIP] instalacao falhou: #{__evs.class}: #{__evs.message}") rescue nil
  end
  # ---- FIM PERF FPS Fase 1 ---------------------------------------------------

  # ---- PERF + CORRECAO: screen_y_ground nativo & offset de escadas ------------
  # O acelerador (compat_stubs.h, corre antes a ~t=3.95s) ja' poe screen_x/y/z
  # do Game_Character em C nativo. Mas ficaram DOIS problemas, que tratamos aqui
  # (este bloco corre DEPOIS, a ~t=4.0s, e e' independente da ordem porque so'
  # usa __gc_native_* que sao registados no arranque):
  #
  #  (1) PERF: screen_y_ground NAO foi acelerado e e' chamado p/ os 54 eventos
  #      TODOS os frames dentro de Game_Event#should_update? (a par de screen_x).
  #      Ou seja, metade da verificacao de visibilidade dos eventos ficou em
  #      Ruby. Aqui aceleramo-la SEM tocar no essentials_accel_3ds.cpp: quando o
  #      character NAO esta' a saltar, __gc_native_screen_y devolve exatamente
  #      acc_screen_y_ground (== screen_y_ground). A saltar (raro, e quase sempre
  #      on-screen) devolve nil e caimos no Ruby. Valores IDENTICOS ao Ruby.
  #
  #  (2) CORRECAO de compatibilidade: a ligacao do compat_stubs devolve screen_x
  #      e screen_y do NATIVO, que NAO inclui o offset do plugin "Terrain Tag
  #      Side Stairs" (@real_offset_x/@real_offset_y). Resultado: nas escadas, os
  #      sprites ficam desalinhados ate' alguns pixels durante o movimento. Aqui
  #      re-definimos screen_x/screen_y = nativo + offset -> rapido E correto.
  #      O caso de salto cai no fallback (que ja' inclui o offset e o arco).
  #
  # Idempotente. Reversivel: $MKXP_PERF_SCREENFIX=false (volta ao estado do
  # compat_stubs, i.e. nativo sem offset + screen_y_ground em Ruby).
  begin
    $MKXP_PERF_SCREENFIX = true if $MKXP_PERF_SCREENFIX.nil?
    if $MKXP_PERF_SCREENFIX &&
       Object.const_defined?(:Game_Character) &&
       Game_Character.method_defined?(:__gc_native_screen_x) &&
       Game_Character.method_defined?(:__gc_native_screen_y) &&
       Game_Character.method_defined?(:screen_y_ground) &&
       !Game_Character.method_defined?(:__mkxp_screenfix)
      class Game_Character
        def __mkxp_screenfix; true; end
        # fallbacks = versao atual (a do compat_stubs p/ x/y; a base Ruby p/ y_ground)
        alias_method :__sfx_fallback_sx,  :screen_x
        alias_method :__sfx_fallback_sy,  :screen_y
        alias_method :__sfx_fallback_syg, :screen_y_ground
        def screen_x
          n = __gc_native_screen_x(self)
          return __sfx_fallback_sx if n.nil?   # sem mapa: Ruby (ja' c/ offset)
          n + (@real_offset_x || 0)            # nativo + offset escadas
        end
        def screen_y
          n = __gc_native_screen_y(self)       # nil se a saltar / sem mapa
          return __sfx_fallback_sy if n.nil?   # salto: Ruby (arco + offset)
          n + (@real_offset_y || 0)            # nativo + offset escadas
        end
        def screen_y_ground
          n = __gc_native_screen_y(self)       # NAO salta: == screen_y_ground
          n.nil? ? __sfx_fallback_syg : n      # salto/sem-mapa: Ruby
        end
      end
      MKXPDebug.log("[SCREENFIX] screen_x/y nativos+offset-escadas e screen_y_ground nativo instalados") rescue nil
    end
  rescue => __esfx
    MKXPDebug.log("[SCREENFIX] instalacao falhou: #{__esfx.class}: #{__esfx.message}") rescue nil
  end
  # ---- FIM PERF screenfix ----------------------------------------------------

  # ---- [NATMOVE] Game_Character#update_move nativo (miolo do movimento) -------
  # PERF: o passo real_x/real_y por-frame de TODOS os characters em movimento
  # custa 9-14ms nos picos (player+seguidor). __gc_native_move_step (C) faz o
  # miolo; casos raros (salto, ivars por memoizar) devolvem nil -> original.
  # SPLICE INTELIGENTE: o plugin "Terrain Tag Side Stairs" faz
  #   alias updatemovestairs update_move ; def update_move (logica escadas + chama o alias)
  # Se embrulhassemos update_move por FORA, o fast-path saltava as escadas.
  # Por isso, se updatemovestairs existir, redefinimos ESSE (o apontador p/ a
  # base original) -> o wrapper das escadas continua a correr por fora, intacto.
  # Introspecao por instance_methods.include? (respond_to?/method_defined? nao
  # sao fiaveis neste mruby). Reversivel: $MKXP_PERF_NATMOVE=false.
  begin
    $MKXP_PERF_NATMOVE = true if $MKXP_PERF_NATMOVE.nil?
    if $MKXP_PERF_NATMOVE && Object.const_defined?(:Game_Character)
      _nk = Object.const_get(:Game_Character)
      _nim = _nk.instance_methods
      if _nim.include?(:update_move) && _nim.include?(:__gc_native_move_step) &&
         !_nim.include?(:__mkxp_natmove_base)
        _splice = _nim.include?(:updatemovestairs) ? "updatemovestairs" : "update_move"
        _nk.class_eval(
          "alias_method :__mkxp_natmove_base, :" + _splice + "\n" +
          "def " + _splice + "\n" +
          "  _r = __gc_native_move_step(self)\n" +
          "  return __mkxp_natmove_base if _r.nil?\n" +
          "  if _r == 1\n" +
          "    Events.onStepTakenFieldMovement.trigger(self, self)\n" +
          "    calculate_bush_depth\n" +
          "    @stopped_this_frame = true\n" +
          "    @anime_count += 1 if @walk_anime || @step_anime\n" +
          "    @moved_this_frame = true\n" +
          "  elsif _r == 2\n" +
          "    calculate_bush_depth\n" +
          "  end\n" +
          "end")
        MKXPDebug.log("[NATMOVE] update_move nativo instalado (splice=" + _splice + ")") rescue nil
      else
        MKXPDebug.log("[NATMOVE] nao instalado (update_move/__gc_native_move_step ausente)") rescue nil
      end
    end
  rescue => __enm
    MKXPDebug.log("[NATMOVE] instalacao falhou: #{__enm.class}: #{__enm.message}") rescue nil
  end
  # ---- FIM NATMOVE -------------------------------------------------------------

  # ---- [SAVE-NEWEST] get_newest_save_slot ROBUSTO (raiz do "sem Continuar") ----
  # PROVADO nos logs: o original rebenta com "comparison of Integer with Time"
  # porque um slot vazio/corrompido da' temp_save_data[:player]=nil e
  # nil.last_time_saved (via method_missing universal) devolve um Integer -> a
  # comparacao com Time.at(0) explode -> a rede de seguranca devolvia nil p/
  # TODOS os slots -> nunca ha' "Continuar" mesmo com um save bom ao lado.
  # Reimplementacao: salta slots inexistentes/0-bytes/ilegiveis, compara por
  # to_i (epoch) -- nunca levanta. Corre pos-plugins (SaveData ja' existe).
  begin
    if Object.const_defined?(:SaveData)
      _gsm = (SaveData.singleton_class.instance_methods rescue [])
      if _gsm.include?(:get_newest_save_slot) && _gsm.include?(:get_full_path) &&
         !$__mkxp_gnss_robusto
        $__mkxp_gnss_robusto = true
        def SaveData.get_newest_save_slot
          _newest_t = -1
          _newest = nil
          begin
            (SaveData::AUTO_SLOTS + SaveData::MANUAL_SLOTS).each do |_f|
              _fp = get_full_path(_f)
              next unless (File.file?(_fp) rescue false)
              next if (File.size(_fp) rescue 0).to_i <= 0
              begin
                _d  = load_save_file(_fp)
                _pl = _d.is_a?(Hash) ? _d[:player] : nil
                _t  = _pl ? _pl.last_time_saved : nil
                _ti = (_t.to_i rescue nil)
                _ti = 0 unless _ti.is_a?(Integer)
                if _ti > _newest_t
                  _newest_t = _ti
                  _newest = _f
                end
              rescue
                next   # slot corrompido: ignora e segue (nao mata os outros)
              end
            end
          rescue
          end
          MKXPDebug.log("[SAVE-NEWEST] slot mais recente: #{_newest.inspect} (t=#{_newest_t})") rescue nil
          _newest
        end
        MKXPDebug.log("[SAVE-NEWEST] get_newest_save_slot robusto instalado (salta 0-bytes/corrompidos; compara to_i)") rescue nil
      end
    end
  rescue => __egn
    MKXPDebug.log("[SAVE-NEWEST] instalacao falhou: #{__egn.class}: #{__egn.message}") rescue nil
  end
  # ---- FIM SAVE-NEWEST ---------------------------------------------------------

  # ---- [SAVE-REPAIR] recuperar saves com mapas class=Object (ecra preto no load) --
  # PROVADO (raio-X byte a byte do save do user, 2026-07-03): o ficheiro de 179KB
  # tem EXATAMENTE 1 objeto corrompido -- $MapFactory.@maps[0] gravado com classe
  # literal "Object" (o resto do grafo, 54 Game_Event + RPG::Map + Game_Player,
  # esta' perfeito). map_id(metodo)->method_missing->0 -> $game_map=mapa 0 -> preto.
  # Aqui, apos load_all_values, reconstruimos cada @maps[i] Object num Game_Map REAL
  # (allocate + copiar ivars) -> os metodos voltam -> o save carrega no sitio certo.
  # INSTALADO ANTES da bateria SELFTEST/READSAVE para o diagnostico ja' exercitar o
  # wrapper (na 1a validacao o READSAVE-LOAD correu ANTES da instalacao -> map_id=0).
  begin
    # GUARD SEM respond_to?: neste mruby o respond_to? e' NAO-FIAVEL (foi por isso
    # que este wrapper nunca se instalou na 1a tentativa -- log sem a linha de
    # instalacao). Padrao fiavel: const_defined? + flag em ivar + alias dentro de
    # begin/rescue (NameError se o metodo nao existir).
    if Object.const_defined?(:SaveData) &&
       !(SaveData.instance_variable_get(:@__mkxp_repair_on) rescue false)
      class << SaveData
        alias_method :__mkxp_orig_load_all, :load_all_values
        def load_all_values(save_data)
          __mkxp_orig_load_all(save_data)
          begin
            # [SAVE-REPAIR v3 2026-07-04] reparacao PROFUNDA. O v2 (so' @maps[i])
            # deixava fantasmas vivos que PROPAGAVAM pela gravacao seguinte
            # (raio-X do save novo: mapa Object OUTRA VEZ + 3 eventos Object +
            # $game_system.@map_interpreter Object -> NPCs a deslizar com a
            # camara (@map dos eventos aponta ao mapa-fantasma, display=0) e
            # portas mortas (interpreter zumbi engole os eventos de transfer)).
            # Agora: mapas + eventos + @map dos eventos re-apontado + interpreter
            # (e child) + $game_player. Tudo por allocate+ivars (padrao provado).
            _fx = []
            _mk = lambda do |_o, _ks|
              _kk = Object.const_get(_ks)
              _n = _kk.allocate
              _o.instance_variables.each { |_iv| _n.instance_variable_set(_iv, _o.instance_variable_get(_iv)) }
              _n
            end
            _mf = ($MapFactory rescue nil)
            _mps = _mf ? (_mf.instance_variable_get(:@maps) rescue nil) : nil
            if _mps.is_a?(Array) && Object.const_defined?(:Game_Map)
              _mps.each_with_index do |_m, _i|
                next if _m.nil?
                if _m.class == Object
                  _m = _mk.call(_m, :Game_Map)
                  _mps[_i] = _m
                  _fx << "map#{(_m.instance_variable_get(:@map_id) rescue '?')}"
                end
                # eventos: reconstruir os Object e RE-APONTAR @map de TODOS ao
                # mapa (reparado) a que pertencem — mata o "deslizar com a camara"
                _evs = (_m.instance_variable_get(:@events) rescue nil)
                if _evs.is_a?(Hash) && Object.const_defined?(:Game_Event)
                  _evs.keys.each do |_k2|
                    _e = _evs[_k2]
                    next if _e.nil?
                    if _e.class == Object
                      _e = _mk.call(_e, :Game_Event)
                      _evs[_k2] = _e
                      _fx << "ev#{_k2}"
                    end
                    _em = (_e.instance_variable_get(:@map) rescue nil)
                    if !_em.nil? && !_em.equal?(_m)
                      _e.instance_variable_set(:@map, _m)
                      _fx << "ev#{_k2}@map"
                    end
                  end
                end
              end
            end
            # interpreter zumbi (portas/eventos mortos) + child
            _gs = ($game_system rescue nil)
            if _gs && Object.const_defined?(:Interpreter)
              _ip = (_gs.instance_variable_get(:@map_interpreter) rescue nil)
              if _ip && _ip.class == Object
                _ip = _mk.call(_ip, :Interpreter)
                _gs.instance_variable_set(:@map_interpreter, _ip)
                _fx << "interp"
              end
              _ci = (_ip ? _ip.instance_variable_get(:@child_interpreter) : nil rescue nil)
              if _ci && _ci.class == Object
                (_ip.instance_variable_set(:@child_interpreter, _mk.call(_ci, :Interpreter)) rescue nil)
                _fx << "interp.child"
              end
            end
            _fixed = _fx.size
            if _fixed > 0
              MKXPDebug.log("[SAVE-REPAIR] v3 profundo: #{_fixed} reparacoes -> #{_fx.first(12).join(',')}#{_fx.size > 12 ? '...' : ''}") rescue nil
            end
            if _mps.is_a?(Array)
              # [TSC-REVERT 2026-07-04] RAIZ da "arvore cortada" no Continuar:
              # o TSC (compactacao de tilesets) remapeia os IDs DENTRO de @map.data
              # e o save persiste isso (@__tsc_data_done + tabelas compactas). No
              # load, o TSC reconstroi o atlas a partir dos dados JA remapeados ->
              # tiles altos (copa da arvore, casas) desenham arte das posicoes
              # ORIGINAIS baixas = transparente/errada (provado: TSC-PIXDBG so viu
              # ids<=754 quando o mapa original vai ate 7865+). Fix: repor o
              # RPG::Map pristino do disco + tabelas originais do tileset e limpar
              # a flag -> o TSC desta sessao compacta de fresco e fica consistente.
              _mps.each do |_m|
                next if _m.nil?
                begin
                  next unless (_m.instance_variable_get(:@__tsc_data_done) rescue nil)
                  _mid2 = _m.instance_variable_get(:@map_id)
                  next unless _mid2.is_a?(Integer) && _mid2 > 0
                  _rpg = (load_data(sprintf("Data/Map%03d.rxdata", _mid2)) rescue nil)
                  next unless _rpg
                  _m.instance_variable_set(:@map, _rpg)
                  _ts2 = ($data_tilesets[_rpg.tileset_id] rescue nil)
                  if _ts2
                    _m.instance_variable_set(:@passages,     _ts2.passages)
                    _m.instance_variable_set(:@priorities,   _ts2.priorities)
                    _m.instance_variable_set(:@terrain_tags, _ts2.terrain_tags)
                  end
                  _m.instance_variable_set(:@__tsc_data_done, nil)
                  MKXPDebug.log("[SAVE-REPAIR] TSC-REVERT map=#{_mid2}: dados/tabelas pristinos repostos (o save trazia tiles remapeados)") rescue nil
                rescue => _rte
                  MKXPDebug.log("[SAVE-REPAIR] TSC-REVERT falhou num mapa: #{_rte.class}: #{_rte.message.to_s[0,70]}") rescue nil
                end
              end
            end
            # reconstruir tambem $game_player se vier Object (metodos do jogador)
            if Object.const_defined?(:Game_Player) && ($game_player rescue nil) && $game_player.class != Game_Player
              _op = $game_player; _np = Game_Player.allocate
              (_op.instance_variables.each { |_iv| _np.instance_variable_set(_iv, _op.instance_variable_get(_iv)) } rescue nil)
              $game_player = _np
              MKXPDebug.log("[SAVE-REPAIR] $game_player Object -> Game_Player reconstruido") rescue nil
            end
          rescue => _re
            MKXPDebug.log("[SAVE-REPAIR] erro: #{_re.class}: #{_re.message.to_s[0,80]}") rescue nil
          end
        end
      end
      SaveData.instance_variable_set(:@__mkxp_repair_on, true)
      MKXPDebug.log("[SAVE-REPAIR] wrapper de reparacao instalado (Object->Game_Map/Game_Player no load)") rescue nil
    end
  rescue => _esr
    MKXPDebug.log("[SAVE-REPAIR] instalacao falhou: #{_esr.class}: #{_esr.message}") rescue nil
  end
  # ---- FIM SAVE-REPAIR -------------------------------------------------------------

  # ---- [SELFTEST-SAVE] bateria de arranque (valida o pipeline SEM jogar) --------
  # Corre pos-plugins e prova no log, a cada boot: (1) que SAVE_DIR do Auto Multi
  # Save apanhou a pasta certa (era '.' por causa do File.directory? envenenado);
  # (2) que Marshal.dump->ficheiro escreve BYTES REAIS (era 0 por causa do stub);
  # (3) que o round-trip load devolve os dados. Se algo falhar, o log diz ONDE,
  # sem precisar de gravar dentro do jogo.
  begin
    _sd_dir   = (System.data_directory rescue nil)   # a versao dos SCRIPTS do jogo (<game>/Save)
    _dir_ok   = (File.directory?(_sd_dir) rescue false)
    _sd_const = (SaveData::SAVE_DIR rescue "(sem plugin)")
    MKXPDebug.log("[SELFTEST-SAVE] data_dir=#{_sd_dir.inspect} File.directory?=#{_dir_ok} SaveData::SAVE_DIR=#{_sd_const.inspect} #{_sd_const == _sd_dir ? '(OK: plugin apanhou a pasta certa)' : '(PROBLEMA: plugin vai gravar fora de saves/)'}") rescue nil
    _tp = (_sd_dir || ".") + "/__selftest.bin"
    _payload = { "n" => 123, "s" => "selftest", "a" => [1, 2.5, "x"] }
    File.open(_tp, "wb") { |_f| Marshal.dump(_payload, _f) }
    _sz = (File.size(_tp) rescue -1)
    _back = nil
    File.open(_tp, "rb") { |_f| _back = Marshal.load(_f) }
    (File.delete(_tp) rescue nil)
    _ok = _sz.to_i > 0 && _back.is_a?(Hash) && _back["n"] == 123 && _back["a"].is_a?(Array) && _back["a"][1] > 2
    MKXPDebug.log("[SELFTEST-SAVE] round-trip dump->ficheiro->load: bytes=#{_sz} load=#{_back.class} => #{_ok ? 'PASS' : 'FALHOU'}") rescue nil
    # [SAVE-VALUES-FIX] contar os registos do SaveData: 17 (15 core + 2 plugins) = OK;
    # 2 = a seccao Game_SaveValues nao correu (saves esqueleticos de 101 bytes).
    _nv = (SaveData.instance_variable_get(:@values).size rescue -1)
    MKXPDebug.log("[SELFTEST-SAVE] SaveData valores registados: #{_nv} #{_nv.to_i >= 15 ? '(OK: core presente)' : '(PROBLEMA: registos core em falta -> save esqueletico)'}") rescue nil
    # [PERF 2026-07-04] Bateria PROFUNDA (IVAR/TABLE/FILE/READSAVE/GMCLASS) gateada:
    # a raiz do save foi FECHADA (SAVE-REPAIR + TSC-REVERT provados) e o READSAVE
    # fazia Marshal.load do save REAL inteiro (179KB, ~1-2s de boot no 3DS) em
    # TODOS os arranques. Opt-in: $MKXP_DIAG_SAVE=true reativa tudo tal-e-qual.
    if ($MKXP_DIAG_SAVE == true)
    # [SELFTEST-IVAR 2026-07-03] O load restaura o mapa ERRADO (map=0) porque o
    # $MapFactory.@mapIndex volta a 0. Testar se o marshal preserva os IVARS de um
    # objeto com varios ivars + hash aninhado (a estrutura do PokemonMapFactory).
    # Se @mapIndex nao sobreviver ao round-trip -> bug do marshal a caçar.
    begin
      Object.const_set(:MkxpIvarProbe, Class.new do
        attr_accessor :mapIndex, :maps, :a, :b, :c
      end) unless Object.const_defined?(:MkxpIvarProbe)
      _o = MkxpIvarProbe.new
      _o.mapIndex = 43; _o.a = "x"; _o.b = 999; _o.c = [1,2,3]
      _o.maps = { 43 => "map43data", 44 => "map44data" }
      _s2 = Marshal.dump(_o)
      _o2 = Marshal.load(_s2)
      _mi = (_o2.mapIndex rescue nil)
      _mk = (_o2.maps.is_a?(Hash) ? _o2.maps.keys.sort : nil)
      _okv = (_mi == 43 && _mk == [43,44] && _o2.b == 999)
      MKXPDebug.log("[SELFTEST-IVAR] objeto c/ ivars+hash: mapIndex=#{_mi.inspect} maps.keys=#{_mk.inspect} b=#{(_o2.b rescue nil).inspect} => #{_okv ? 'PASS' : 'FALHOU (marshal perde ivars -> RAIZ do map=0 no load!)'}") rescue nil
    rescue => _ei
      MKXPDebug.log("[SELFTEST-IVAR] EXCECAO: #{_ei.class}: #{_ei.message.to_s[0,80]}") rescue nil
    end
    # [SELFTEST-TABLE 2026-07-03] O caso REAL: um objeto com uma Table GIGANTE (como
    # Game_Map#@data 65x70x3) SEGUIDA de um ivar (como @mapIndex). Se o marshal ler
    # mal os bytes da Table, o offset desliza e o ivar seguinte fica corrompido/0 ->
    # e' EXATAMENTE o "map=0 no load" ($MapFactory.@mapIndex volta a 0). PROVA direta.
    begin
      Object.const_set(:MkxpTableProbe, Class.new do
        attr_accessor :data, :mapIndex, :tail
      end) unless Object.const_defined?(:MkxpTableProbe)
      _tp = MkxpTableProbe.new
      _t = Table.new(65, 70, 3)   # mesmas dims do Map043
      _t[10, 20, 1] = 7851; _t[0, 0, 0] = 384; _t[64, 69, 2] = 999
      _tp.data = _t; _tp.mapIndex = 43; _tp.tail = "APOS_TABLE"
      _s3 = Marshal.dump(_tp)
      _tp2 = Marshal.load(_s3)
      _mi3 = (_tp2.mapIndex rescue nil)
      _tl3 = (_tp2.tail rescue nil)
      _tv  = (_tp2.data[10,20,1] rescue nil)
      _okt = (_mi3 == 43 && _tl3 == "APOS_TABLE" && _tv == 7851)
      MKXPDebug.log("[SELFTEST-TABLE] ivar APOS Table gigante: mapIndex=#{_mi3.inspect} tail=#{_tl3.inspect} table[10,20,1]=#{_tv.inspect} => #{_okt ? 'PASS' : 'FALHOU (offset da Table desliza -> RAIZ do map=0 no load!)'}") rescue nil
    rescue => _et
      MKXPDebug.log("[SELFTEST-TABLE] EXCECAO: #{_et.class}: #{_et.message.to_s[0,80]}") rescue nil
    end
    # [SELFTEST-FILE 2026-07-03] O caminho EXATO do save real: um HASH {chave=>objeto
    # com Table+ivar} escrito e lido por FICHEIRO (nao string). E' a unica diferenca
    # entre os testes que passam e o save real que perde o @mapIndex. Se o @mapIndex
    # sobreviver a' STRING mas NAO ao FICHEIRO -> o bug esta' no port read/write.
    begin
      _fp2 = (_sd_dir || ".") + "/__selftest_map.bin"
      _mo = MkxpTableProbe.new
      _mt = Table.new(65, 70, 3); _mt[10, 20, 1] = 7851
      _mo.data = _mt; _mo.mapIndex = 43; _mo.tail = "TAIL"
      _payload2 = { :map_factory => _mo, :game_player => "player_stub", :switches => [true, false, true] }
      File.open(_fp2, "wb") { |_f| Marshal.dump(_payload2, _f) }
      _szf = (File.size(_fp2) rescue -1)
      _back2 = nil
      File.open(_fp2, "rb") { |_f| _back2 = Marshal.load(_f) }
      (File.delete(_fp2) rescue nil)
      _mf2 = _back2.is_a?(Hash) ? _back2[:map_factory] : nil
      _mif = (_mf2.mapIndex rescue nil)
      _tlf = (_mf2.tail rescue nil)
      _swf = (_back2.is_a?(Hash) ? _back2[:switches].inspect : "?") rescue "?"
      _okf = (_mif == 43 && _tlf == "TAIL")
      MKXPDebug.log("[SELFTEST-FILE] hash{map_factory:obj+Table} por FICHEIRO (#{_szf}B): mapIndex=#{_mif.inspect} tail=#{_tlf.inspect} switches=#{_swf} => #{_okf ? 'PASS' : 'FALHOU (port read/write perde ivars apos Table -> RAIZ do map=0!)'}") rescue nil
    rescue => _ef
      MKXPDebug.log("[SELFTEST-FILE] EXCECAO: #{_ef.class}: #{_ef.message.to_s[0,80]}") rescue nil
    end
    # [READSAVE 2026-07-03] DECISIVO: abrir o SAVE REAL com o marshal do motor (que
    # os 4 selftests provaram bom) e ver que map_id ele contem. Se @maps tiver o mapa
    # 43 e @mapIndex apontar la' -> o load parte-o. Se ja' tiver map_id=0 -> o SAVE
    # foi feito partido (transfer TRANSFERFIX). Nao carrega o jogo; so' inspeciona.
    begin
      _rsp = (_sd_dir || ".") + "/File 01.rxdata"
      if (File.file?(_rsp) rescue false) && (File.size(_rsp) rescue 0) > 4096
        _sv = nil
        File.open(_rsp, "rb") { |_f| _sv = Marshal.load(_f) }
        if _sv.is_a?(Hash)
          _mf = _sv[:map_factory]
          _gp = _sv[:game_player]
          _mi = (_mf.instance_variable_get(:@mapIndex) rescue nil)
          _mps = (_mf.instance_variable_get(:@maps) rescue nil)
          _ids = (_mps.is_a?(Array) ? _mps.map { |_m| _m ? (_m.instance_variable_get(:@map_id) rescue '?') : nil } : '?') rescue '?'
          _px = (_gp.instance_variable_get(:@x) rescue '?'); _py = (_gp.instance_variable_get(:@y) rescue '?')
          MKXPDebug.log("[READSAVE] save REAL: chaves=#{_sv.size} | map_factory.@mapIndex=#{_mi.inspect} @maps.map_ids=#{_ids.inspect} | player@(#{_px},#{_py})") rescue nil
          # [READSAVE-GM] isolar o Game_Map: metodo map_id vs ivar @map_id. Se diferirem
          # -> bug de interning de simbolos (o marshal escreve num @map_id que o metodo
          # attr_accessor nao le'). Testa tb outro ivar (@display_x) p/ ver se e' geral.
          if _mps.is_a?(Array) && _mps[0]
            _gm0 = _mps[0]
            _meth = (_gm0.map_id rescue 'ERRO'); _ivar = (_gm0.instance_variable_get(:@map_id) rescue 'ERRO')
            _ivnames = (_gm0.instance_variables.inspect rescue '?')
            MKXPDebug.log("[READSAVE-GM] Game_Map: map_id(metodo)=#{_meth.inspect} @map_id(ivar)=#{_ivar.inspect} class=#{_gm0.class} | instance_variables=#{_ivnames}") rescue nil
            MKXPDebug.log("[READSAVE-GM] VEREDITO: se metodo!=ivar -> INTERNING de simbolos (o marshal grava ivars num simbolo que o codigo nao le)") rescue nil
          end
          # [READSAVE-CLS] Game_Map esta' DEFINIDA no momento do load? Se nao ->
          # class_from_path falha -> Object. Confirma timing vs bug de resolucao.
          MKXPDebug.log("[READSAVE-CLS] no load: const_defined?(:Game_Map)=#{Object.const_defined?(:Game_Map)} const_defined?(:PokemonMapFactory)=#{Object.const_defined?(:PokemonMapFactory)} Game_Map.class=#{(Game_Map.class rescue 'ERRO')}") rescue nil
          # [READSAVE-LOAD] DESLIGADO por defeito ($MKXP_TRY_LOAD): o load_all_values
          # muda estado E pode envenenar a cache de classes do marshal. So' diagnostico.
          if ($MKXP_TRY_LOAD == true) && (defined?(SaveData) && SaveData.respond_to?(:load_all_values) rescue false)
            _m0 = ($game_map.map_id rescue '?')
            begin
              SaveData.load_all_values(_sv)
              _mf3 = ($MapFactory rescue nil)
              _same = (_mf3.equal?(_sv[:map_factory]) rescue false)   # $MapFactory É o do save?
              _mi3 = (_mf3.instance_variable_get(:@mapIndex) rescue '?')
              _mps3 = (_mf3.instance_variable_get(:@maps) rescue nil)
              _ids3 = (_mps3.is_a?(Array) ? _mps3.map { |_m| _m ? (_m.instance_variable_get(:@map_id) rescue '?') : nil } : '?') rescue '?'
              _m1 = ($MapFactory.map.map_id rescue '?')
              MKXPDebug.log("[READSAVE-LOAD] $MapFactory===save? #{_same} | @mapIndex=#{_mi3} @maps.map_ids=#{_ids3.inspect} | map().map_id=#{_m1}") rescue nil
              MKXPDebug.log("[READSAVE-LOAD] VEREDITO: same=false -> :map_factory NAO foi atribuido (validacao/exception saltou-o); same=true mas map_id=0 -> map() ou @maps quebrou") rescue nil
            rescue => _el
              MKXPDebug.log("[READSAVE-LOAD] load_all_values REBENTOU: #{_el.class}: #{_el.message.to_s[0,90]}") rescue nil
            end
          end
        else
          MKXPDebug.log("[READSAVE] save real nao e' Hash: #{_sv.class}") rescue nil
        end
      else
        MKXPDebug.log("[READSAVE] sem save real >4KB para inspecionar") rescue nil
      end
    rescue => _ers
      MKXPDebug.log("[READSAVE] EXCECAO ao ler save real: #{_ers.class}: #{_ers.message.to_s[0,80]}") rescue nil
    end
    # [SELFTEST-GMCLASS 2026-07-03] DECISIVO: round-trip de um Game_Map REAL agora.
    # Se voltar como Object -> o MARSHAL mangla a classe (bug write ou read/cache).
    # Se voltar Game_Map com map_id=77 -> marshal OK e o save nasceu ja' partido.
    begin
      if Object.const_defined?(:Game_Map)
        _rgm = Game_Map.new
        _rgm.instance_variable_set(:@map_id, 77)
        _dd2 = Marshal.dump(_rgm)
        _ll2 = Marshal.load(_dd2)
        MKXPDebug.log("[SELFTEST-GMCLASS] Game_Map round-trip: classe_volta=#{_ll2.class} map_id(metodo)=#{(_ll2.map_id rescue 'ERRO')} @map_id(ivar)=#{(_ll2.instance_variable_get(:@map_id) rescue '?')} => #{(_ll2.class.to_s == 'Game_Map' && (_ll2.map_id rescue nil) == 77) ? 'PASS (marshal OK, save nasceu partido)' : 'FALHOU: marshal mangla a classe -> RAIZ!'}") rescue nil
      end
    rescue => _egc
      MKXPDebug.log("[SELFTEST-GMCLASS] EXCECAO: #{_egc.class}: #{_egc.message.to_s[0,80]}") rescue nil
    end
    end   # fim do gate $MKXP_DIAG_SAVE (bateria profunda opt-in)
  rescue => _est
    MKXPDebug.log("[SELFTEST-SAVE] EXCECAO: #{_est.class}: #{_est.message.to_s[0, 80]}") rescue nil
  end
  # ---- FIM SELFTEST-SAVE ---------------------------------------------------------

  # ---- [NEIGHBOR-REFRESH] estatua do Gilbert (evento fantasma no mapa vizinho) --
  # RAIZ (provada por analise estatica dos scripts): TODOS os pontos do jogo fazem
  # `$game_map.need_refresh = true` -- SO' o mapa atual. Com MapFactory (mapas
  # ligados), o VIZINHO nunca recebe need_refresh -> os eventos dele mantem a
  # pagina antiga (ex: ev7 RIVAL1 continua com o grafico do Gilbert em vez de
  # refrescar para vazio) -> "estatua" duplicada ao lado do Gilbert real.
  # FIX: propagar need_refresh=true a todos os mapas do factory. Custo: so'
  # quando need_refresh muda p/ true (raro). Gate: $MKXP_FIX_NEIGHBOR_REFRESH.
  begin
    $MKXP_FIX_NEIGHBOR_REFRESH = true if $MKXP_FIX_NEIGHBOR_REFRESH.nil?
    if $MKXP_FIX_NEIGHBOR_REFRESH && Object.const_defined?(:Game_Map) &&
       Game_Map.instance_methods.include?(:need_refresh=) &&
       !Game_Map.instance_methods.include?(:__mkxp_orig_set_nr)
      class Game_Map
        alias_method :__mkxp_orig_set_nr, :need_refresh=
        def need_refresh=(v)
          __mkxp_orig_set_nr(v)
          if v && $MapFactory
            begin
              _ms = $MapFactory.maps
              if _ms.is_a?(Array)
                _ms.each do |_m|
                  next if _m.nil? || _m.equal?(self)
                  (_m.__mkxp_orig_set_nr(true) rescue nil)
                end
              end
            rescue
            end
          end
          v
        end
      end
      MKXPDebug.log("[NEIGHBOR-REFRESH] need_refresh propaga a todos os mapas do factory (anti-estatua)") rescue nil
    end
  rescue => __enr
    MKXPDebug.log("[NEIGHBOR-REFRESH] falhou: #{__enr.class}: #{__enr.message}") rescue nil
  end
  # ---- FIM NEIGHBOR-REFRESH ------------------------------------------------------

  # ---- [FOLLOWER-LEAK] anti-orfaos de Sprite_Character (estatua do Gilbert) -----
  # RAIZ (provada nos scripts): (1) o Spriteset_Map#dispose do jogo (065+066) morre
  # a MEIO se um bitmap ja' foi disposto -> os @usersprites (DependentEventSprites,
  # que seguram o sprite do SEGUIDOR) nunca sao dispostos -> sprite imortal no
  # viewport = estatua. (2) createSingleSpriteset faz restoreAnimations(temp) que
  # SUBSTITUI @usersprites acabados de criar sem os dispor -> DES orfao (o
  # Sprite_Character#initialize faz update -> ja' nasce visivel) = estatua.
  # FIX A: dispose blindado -- se rebentar, varre usersprites+chars na mesma.
  # FIX B: restoreAnimations preserva DES (dono continua o spriteset) e dispoe o
  # resto que ia ser descartado. Ambos no-op no fluxo saudavel.
  begin
    if Object.const_defined?(:Spriteset_Map)
      if Spriteset_Map.method_defined?(:dispose) &&
         !Spriteset_Map.method_defined?(:__mkxp_safe_dispose)
        class Spriteset_Map
          alias_method :__mkxp_safe_dispose, :dispose
          def dispose
            begin
              __mkxp_safe_dispose
            rescue => _ed
              MKXPDebug.log("[FOLLOWER-LEAK] dispose rebentou a meio (#{_ed.class}: #{_ed.message.to_s[0,60]}) -> varredura de seguranca") rescue nil
              ((@usersprites || []).each { |_u| (_u.dispose rescue nil) } rescue nil)
              ((@character_sprites || []).each { |_c| (_c.dispose rescue nil) } rescue nil)
              (@usersprites.clear rescue nil)
              (@character_sprites.clear rescue nil)
            end
          end
        end
      end
      if Spriteset_Map.method_defined?(:restoreAnimations) &&
         !Spriteset_Map.method_defined?(:__mkxp_orig_restanims)
        class Spriteset_Map
          alias_method :__mkxp_orig_restanims, :restoreAnimations
          def restoreAnimations(anims)
            _cur = @usersprites
            __mkxp_orig_restanims(anims)
            begin
              if _cur.is_a?(Array) && !_cur.equal?(@usersprites)
                _cur.each do |_u|
                  next if _u.nil?
                  next if @usersprites.is_a?(Array) && @usersprites.any? { |_k| _k.equal?(_u) }
                  if _u.class.to_s == "DependentEventSprites"
                    @usersprites.push(_u) if @usersprites.is_a?(Array)   # preservar o dono
                  else
                    (_u.dispose rescue nil)   # animacao descartada -> dispor (anti-orfao)
                  end
                end
              end
            rescue; end
          end
        end
      end
      MKXPDebug.log("[FOLLOWER-LEAK] dispose blindado + restoreAnimations anti-orfao instalados") rescue nil
    end
  rescue => _efl
    MKXPDebug.log("[FOLLOWER-LEAK] instalacao falhou: #{_efl.class}: #{_efl.message}") rescue nil
  end
  # ---- FIM FOLLOWER-LEAK -----------------------------------------------------------

  # ---- [EVSC] cache de compilacao de scripts de evento (picos do interpreter) ----
  # PROVADO (auditoria 2026-07-04): Interpreter#execute_script faz eval(script)
  # puro (mruby-eval SEM cache: parse+codegen completos por chamada) e os
  # chamadores quentes reavaliam POR FRAME — Conditional Branch [script] em
  # eventos paralelos (command_111 caso 12), Script command (command_355) e os
  # switches "s:" (switchIsOn?). Resultado: 1-N compilacoes mruby POR FRAME =
  # os picos de 26-87ms do interpreter + o churn que levou o GC a 887k vivos.
  # FIX: memoizar o Proc compilado por string e correr com instance_exec (self
  # identico ao eval). FAIL-OPEN: falha de compilacao OU de execucao cai no
  # caminho original -> erro/report 100% identico ao Essentials.
  begin
    _ie_ok = (Object.new.instance_exec { 42 } == 42 rescue false)
    if _ie_ok && Object.const_defined?(:Interpreter) &&
       Interpreter.instance_methods.include?(:execute_script) &&
       !Interpreter.instance_methods.include?(:__evsc_orig_execute_script)
      $__evsc_cache = {}
      $__evsc_fail  = {}
      class Interpreter
        # compilar num scope LIMPO onde o unico local e' `script` — exatamente o
        # mesmo ambiente lexical que o eval(script) original via.
        def self.__evsc_compile(script)
          eval("Proc.new {\n" + script + "\n}")
        end
        alias_method :__evsc_orig_execute_script, :execute_script
        def execute_script(script)
          return __evsc_orig_execute_script(script) if $__evsc_fail[script]
          _pr = $__evsc_cache[script]
          if _pr.nil?
            begin
              _pr = Interpreter.__evsc_compile(script)
            rescue Exception
              _pr = nil
            end
            if _pr.nil?
              $__evsc_fail[script.dup] = true
              return __evsc_orig_execute_script(script)
            end
            $__evsc_cache[script.dup] = _pr
          end
          begin
            instance_exec(&_pr)
          rescue Exception
            # re-corre pelo caminho original: erro identico + report do Essentials
            __evsc_orig_execute_script(script)
          end
        end
      end
      MKXPDebug.log("[EVSC] cache de compilacao de scripts de evento instalada (fail-open)") rescue nil
    else
      MKXPDebug.log("[EVSC] NAO instalada (instance_exec=#{_ie_ok})") rescue nil
    end
  rescue => _ee
    MKXPDebug.log("[EVSC] instalacao falhou: #{_ee.class}: #{_ee.message}") rescue nil
  end
  # ---- FIM EVSC ----------------------------------------------------------------------

  # ---- [SSW] cache dos switches por script "s:" (refresh de eventos) --------------
  # Game_Event#switchIsOn? e Game_CommonEvent#switchIsOn? fazem eval($~.post_match)
  # por condicao de pagina em CADA refresh de mapa — e command_122 (variaveis)
  # marca need_refresh em QUALQUER escrita -> em cutscene isto recompila os
  # switches "s:" de todos os eventos POR FRAME. Mesma tecnica do EVSC: Proc
  # memoizado + instance_exec(self=evento), fail-open para o caminho original.
  begin
    _ie_ok2 = (Object.new.instance_exec { 42 } == 42 rescue false)
    if _ie_ok2
      $__ssw_cache = {}
      [:Game_Event, :Game_CommonEvent].each do |_kn|
        next unless Object.const_defined?(_kn)
        _k = Object.const_get(_kn)
        next unless _k.instance_methods.include?(:switchIsOn?)
        next if _k.instance_methods.include?(:__ssw_orig_switchIsOn?)
        _k.class_eval do
          alias_method :__ssw_orig_switchIsOn?, :switchIsOn?
          def switchIsOn?(id)
            _sw = $data_system.switches[id]
            # [SWITCHNAME-FIX 2026-07-05] RAIZ do NEW GAME preso: System.rxdata
            # falha no boot (allocation failure) -> $data_system.switches[id]=nil
            # (nome em falta) -> o `return false if !_sw` original dava FALSE p/
            # TODOS os switches globais mesmo LIGADOS -> condicoes de pagina de
            # eventos (EV26 'INTRO' cond ShowOpening=switch50) nunca cumpriam ->
            # player preso nas arvores. nome nil = switch NORMAL = valor real.
            return ($game_switches[id] ? true : false) if !_sw
            # [SSW-FIX 2026-07-05] switches "s:" (script) DELEGADOS ao eval original.
            # RAIZ do "new game preso no intro": o meu instance_exec de um Proc
            # compilado em scope limpo NAO era equivalente ao eval original (binding
            # diferente) -> switchIsOn?("s:...") do switch 50 devolvia FALSE ->
            # a condicao da pagina do EV26 (autorun do intro) falhava -> o intro
            # nunca corria. Correcao > micro-otimizacao: so' os switches NORMAIS
            # usam o fast-path ($game_switches direto); os "s:" usam o original.
            if _sw[/^s\:/]
              return __ssw_orig_switchIsOn?(id)
            end
            return $game_switches[id]
          end
        end
        MKXPDebug.log("[SSW] cache de switches s: instalada em #{_kn}") rescue nil
      end
    end
  rescue => _es2
    MKXPDebug.log("[SSW] instalacao falhou: #{_es2.class}: #{_es2.message}") rescue nil
  end
  # ---- FIM SSW -------------------------------------------------------------------

  # ---- [CHARBATCH] seam do update nativo do Sprite_Character ----------------------
  # O corpo vanilla (061) custa ~45-55 dispatches + 10-12 setters C++ POR SPRITE POR
  # FRAME; o batch nativo (__mkxp_char_batch, sprite_binding) trata o caso comum em
  # 1 chamada. Seam no 'shadow_update' (a alias MAIS INTERIOR da cadeia de wrappers
  # 067<-0004<-0028<-0037<-0557): os plugins (bolhas/sombras/pegadas/followingpkmn)
  # continuam TODOS a correr por cima; so' o corpo vanilla e' substituido; fallback
  # = o proprio vanilla via alias (zero replica). DEFAULT OFF ate' o modo verify
  # passar limpo num self-run: $MKXP_NATIVE_CHAR_BATCH = 1 liga, 2 = verify
  # (corre o vanilla E compara — zero mudanca de comportamento, loga [CHARBATCH|DIFF]).
  begin
    # [2026-07-04c] GUARD FINAL: a reflexao (instance_method/instance_methods) e'
    # envenenada neste engine por um method_missing universal dos stubs (provado
    # em self-run: instance_method de metodo INEXISTENTE nao levanta -> guards
    # todos "true" e por_instalar sempre false). Sonda fiavel = o proprio
    # alias_method (NameError real se shadow_update nao existir — e' o padrao de
    # todos os fixes que funcionam) + flag em ivar de classe p/ idempotencia
    # (padrao SAVE-REPAIR). O batch nativo nao se testa: se faltar em runtime,
    # o rescue do seam desliga o modo e cai no vanilla.
    # [2026-07-04d] shadow_update NAO existe no runtime: Sprite_DynamicShadows
    # esta' na SKIP-LIST do motor (linha ~775). A cadeia real de aliases e':
    #   vanilla(061) <- water_buble_update(0004) <- sh_update(0028)
    #                <- footsteps_update(0037) <- __followingpkmn__update(0557)
    # O seam tem de agarrar a alias MAIS INTERIOR EXISTENTE (interior->exterior;
    # agarrar uma exterior saltaria plugins = mudanca de comportamento). O
    # alias_method dentro de rescue e' a sonda (NameError real, provado).
    _cb_on = (Sprite_Character.instance_variable_get(:@__mkxp_cb_on) rescue false)
    if Object.const_defined?(:Sprite_Character) && !_cb_on
      _seam = nil
      [:shadow_update, :water_buble_update, :sh_update,
       :footsteps_update, :__followingpkmn__update].each do |_cand|
        break if _seam
        begin
          Sprite_Character.class_eval { alias_method :__mkxp_vanilla_upd, _cand }
          _seam = _cand
        rescue Exception
        end
      end
      if _seam
        Sprite_Character.class_eval(<<-MKXPSEAM)
        def #{_seam}
          _m = $MKXP_NATIVE_CHAR_BATCH
          if _m == 1 || _m == true
            begin
              _r = __mkxp_char_batch(1)
              return if _r == 2
              if _r == 1
                SpriteAnimation.clear
                return
              end
            rescue Exception
              unless $__cb_err
                $__cb_err = true
                MKXPDebug.log("[CHARBATCH] excecao -> desligado (fallback permanente)") rescue nil
              end
              $MKXP_NATIVE_CHAR_BATCH = false
            end
          elsif _m == 2
            __mkxp_vanilla_upd
            (__mkxp_char_batch(2) rescue nil)
            return
          end
          __mkxp_vanilla_upd
        end
        MKXPSEAM
        Sprite_Character.instance_variable_set(:@__mkxp_cb_on, true)
        MKXPDebug.log("[CHARBATCH] seam instalado em '#{_seam}' (modo=#{$MKXP_NATIVE_CHAR_BATCH.inspect}; 1=on 2=verify nil/false=off)") rescue nil
      else
        MKXPDebug.log("[CHARBATCH] NENHUMA ancora de alias encontrada -> nao instalado (vanilla intacto)") rescue nil
      end
    else
      MKXPDebug.log("[CHARBATCH] seam ja' instalado ou Sprite_Character ausente") rescue nil
    end
  rescue Exception => _cbe
    MKXPDebug.log("[CHARBATCH] seam falhou: #{_cbe.class}: #{_cbe.message.to_s[0,80]} (shadow_update ausente?)") rescue nil
  end
  # ---- FIM CHARBATCH ---------------------------------------------------------------

  # ---- [AUTOFADE-CACHE] transicao de mapa: load_data redundante do autofade -------
  # PROVADO no log (t=332-334): cada transicao carrega o MESMO Map###.rxdata 2-3x;
  # um deles e' o Scene_Map#autofade, que faz load_data COMPLETO (60-90ms mesmo em
  # cache) so' para ler autoplay_bgm/bgs + nomes. Memo por map_id (dados estaticos
  # do mapa) -> 1 load por mapa por sessao. Semantica identica (mesmas decisoes de
  # fade). Fail-open: qualquer erro -> caminho original.
  begin
    if Object.const_defined?(:Scene_Map) &&
       Scene_Map.instance_methods.include?(:autofade) &&
       !Scene_Map.instance_methods.include?(:__mkxp_orig_autofade)
      $__af_cache = {}
      class Scene_Map
        alias_method :__mkxp_orig_autofade, :autofade
        def autofade(mapid)
          begin
            playingBGM = $game_system.playing_bgm
            playingBGS = $game_system.playing_bgs
            return if !playingBGM && !playingBGS
            _c = $__af_cache[mapid]
            if _c.nil?
              _map = load_data(sprintf("Data/Map%03d.rxdata", mapid))
              _c = [_map.autoplay_bgm, (_map.bgm.name rescue ""),
                    _map.autoplay_bgs, (_map.bgs.name rescue "")]
              $__af_cache[mapid] = _c
            end
            if playingBGM && _c[0]
              if (PBDayNight.isNight? rescue false)
                pbBGMFade(0.8) if playingBGM.name != _c[1] && playingBGM.name != _c[1] + "_n"
              else
                pbBGMFade(0.8) if playingBGM.name != _c[1]
              end
            end
            if playingBGS && _c[2]
              pbBGSFade(0.8) if playingBGS.name != _c[3]
            end
            Graphics.frame_reset
          rescue Exception
            __mkxp_orig_autofade(mapid)   # fail-open: comportamento original
          end
        end
      end
      MKXPDebug.log("[AUTOFADE-CACHE] autofade com memo por map_id (1 load_data por mapa/sessao)") rescue nil
    end
  rescue => _eaf
    MKXPDebug.log("[AUTOFADE-CACHE] falhou: #{_eaf.class}: #{_eaf.message}") rescue nil
  end
  # ---- FIM AUTOFADE-CACHE ------------------------------------------------------------

  # ---- [PLAYERPACK 2026-07-05] 4 cortes no Game_Player#update (9-16ms na rua) ------
  # Auditoria por leitura provou os 4 quentes: (1) pbTerrainTag recalculado por
  # frame (plugin Following) e 3-5x por passo; (2) regex trainer()/counter()
  # RECOMPILADA por evento por passo (mruby nao cacheia literais) em varrimentos
  # de TODOS os eventos; (3) display_x=/display_y= -> 2x metadata lookup + 2x
  # setMapsInRange POR FRAME a andar; (4) update_pattern corre a cauda float
  # em TODOS os characters PARADOS. Semantica exata; tudo por alias/fail-open.
  # [TTCACHE] pbTerrainTag memoizado por [map,x,y,bridge] (globals, nunca ivars:
  # player/map sao marshalados no save)
  begin
    $MKXP_PERF_TTCACHE = true if $MKXP_PERF_TTCACHE.nil?
    if $MKXP_PERF_TTCACHE && Object.const_defined?(:Game_Player) &&
       Game_Player.instance_methods.include?(:pbTerrainTag) &&
       !Game_Player.instance_methods.include?(:__ttc_orig)
      class Game_Player
        alias_method :__ttc_orig, :pbTerrainTag
        def pbTerrainTag(countBridge = false)
          _mid = self.map.map_id
          _br  = ($PokemonGlobal ? $PokemonGlobal.bridge : 0)
          _c = countBridge ? $__ttc_b : $__ttc_a
          return _c[4] if _c && _c[0] == _mid && _c[1] == @x && _c[2] == @y && _c[3] == _br
          _r = __ttc_orig(countBridge)
          _slot = [_mid, @x, @y, _br, _r]
          if countBridge then $__ttc_b = _slot else $__ttc_a = _slot end
          _r
        end
      end
      MKXPDebug.log("[TTCACHE] Game_Player#pbTerrainTag memoizado por posicao") rescue nil
    end
  rescue => __ettc
    MKXPDebug.log("[TTCACHE] falhou: #{__ettc.class}: #{__ettc.message}") rescue nil
  end
  # [EVDIST] distancias trainer()/counter() cacheadas por evento (nome imutavel)
  begin
    if Object.const_defined?(:Game_Player) &&
       !Game_Player.instance_methods.include?(:__evd_get)
      $__evdist = {}
      (Events.onMapChange += proc { $__evdist.clear }) rescue nil
      class Game_Player
        EVD_TRAINER_RE = /trainer\((\d+)\)/i
        EVD_COUNTER_RE = /counter\((\d+)\)/i
        def __evd_get(event)
          _k = ($game_map.map_id << 20) | event.id
          _d = $__evdist[_k]
          if _d.nil?
            _n = event.name.to_s
            _mt = EVD_TRAINER_RE.match(_n)
            _mc = EVD_COUNTER_RE.match(_n)
            _d = [_mt ? _mt[1].to_i : false, _mc ? _mc[1].to_i : false]
            $__evdist[_k] = _d
          end
          _d
        end
        def pbTriggeredTrainerEvents(triggers, checkIfRunning = true)
          result = []
          return result if checkIfRunning && $game_system.map_interpreter.running?
          for event in $game_map.events.values
            distance = __evd_get(event)[0]
            next if !distance
            if pbEventCanReachPlayer?(event, self, distance) && triggers.include?(event.trigger)
              result.push(event) if !event.jumping? && !event.over_trigger?
            end
          end
          return result
        end
        def pbTriggeredCounterEvents(triggers, checkIfRunning = true)
          result = []
          return result if checkIfRunning && $game_system.map_interpreter.running?
          for event in $game_map.events.values
            distance = __evd_get(event)[1]
            next if !distance
            if pbEventFacesPlayer?(event, self, distance) && triggers.include?(event.trigger)
              result.push(event) if !event.jumping? && !event.over_trigger?
            end
          end
          return result
        end
      end
      MKXPDebug.log("[EVDIST] trainer/counter regex -> cache por evento") rescue nil
    end
  rescue => __eevd
    MKXPDebug.log("[EVDIST] falhou: #{__eevd.class}: #{__eevd.message}") rescue nil
  end
  # [DISPXY] guard barato primeiro + display X/Y fundidos (1 setMapsInRange/frame)
  begin
    if Object.const_defined?(:Game_Map) && Object.const_defined?(:Game_Player) &&
       Game_Player.instance_methods.include?(:update_screen_position) &&
       !Game_Map.instance_methods.include?(:__mk_set_display)
      $__mk_snap = {}
      (Events.onMapChange += proc { $__mk_snap.clear }) rescue nil
      class Game_Map
        def __mk_set_display(_vx, _vy)
          # [DISPXY v2 2026-07-05] early-return SO' se o mapa for o MESMO da
          # ultima chamada: ao cruzar fronteira de mapas CONECTADOS o mapa troca
          # de identidade com o MESMO display -> o return antigo saltava o
          # setMapsInRange e os vizinhos (e o seguidor, ancorado ao display do
          # mapa velho) dessincronizavam — o Gilbert "desaparecia na fronteira".
          if @display_x == _vx && @display_y == _vy && $__mk_disp_last == self.__id__
            return
          end
          $__mk_disp_last = self.__id__
          @display_x = _vx
          @display_y = _vy
          _sn = $__mk_snap[self.map_id]
          if _sn.nil?
            _sn = (GameData::MapMetadata.exists?(self.map_id) &&
                   GameData::MapMetadata.get(self.map_id).snap_edges) ? true : false
            $__mk_snap[self.map_id] = _sn
          end
          if _sn
            _max_x = (self.width  - Graphics.width * 1.0 / TILE_WIDTH)  * REAL_RES_X
            _max_y = (self.height - Graphics.height * 1.0 / TILE_HEIGHT) * REAL_RES_Y
            @display_x = [0, [@display_x, _max_x].min].max
            @display_y = [0, [@display_y, _max_y].min].max
          end
          $MapFactory.setMapsInRange if $MapFactory
        end
      end
      class Game_Player
        def update_screen_position(last_real_x, last_real_y)
          return if !(@moved_last_frame || @moved_this_frame) || self.map.scrolling?
          self.map.__mk_set_display(@real_x - SCREEN_CENTER_X, @real_y - SCREEN_CENTER_Y)
        end
      end
      MKXPDebug.log("[DISPXY] update_screen_position fundido (1x setMapsInRange/frame)") rescue nil
    end
  rescue => __edxy
    MKXPDebug.log("[DISPXY] falhou: #{__edxy.class}: #{__edxy.message}") rescue nil
  end
  # [PATIDLE] update_pattern early-out provado em characters parados
  begin
    if Object.const_defined?(:Game_Character) &&
       Game_Character.instance_methods.include?(:update_pattern) &&
       !Game_Character.instance_methods.include?(:__mk_up_orig)
      class Game_Character
        alias_method :__mk_up_orig, :update_pattern
        def update_pattern
          return if !@moved_this_frame && !@moved_last_frame && !@step_anime &&
                    (@anime_count || 0) == 0
          __mk_up_orig
        end
      end
      MKXPDebug.log("[PATIDLE] update_pattern early-out em characters parados") rescue nil
    end
  rescue => __epat
    MKXPDebug.log("[PATIDLE] falhou: #{__epat.class}: #{__epat.message}") rescue nil
  end
  # ---- FIM PLAYERPACK ------------------------------------------------------------------

  # ---- [PRIOSYNC] seam no CustomTilemap#refresh (sprites de priotiles) ------------
  # Fast-path: quando o rect de priotiles NAO muda e nada de tileset/autotile/
  # visibilidade mudou, os ~70 sprites so' precisam de x/y/z novos (funcao pura
  # de tx,ty,prio,ox,oy,bridge) -> 1 chamada nativa em vez de ~70x addTile
  # (~8-10 dispatches cada). tone/color: o update() do 075 ja' re-sincroniza
  # TODOS os @tiles quando @oldtone/@oldcolor mudam ANTES do refresh; bitmap/
  # src: rect igual => mesmos ids nas mesmas posicoes; frames de autotile:
  # @autotiles.changed forca o slow-path. Default VERIFY ($MKXP_PRIO_SYNC=2):
  # corre o vanilla E compara — liga-se (=1) com log limpo.
  begin
    _ps_ok = (Sprite.__mkxp_prio_sync([], [], 0, 0, 32, 32, false) == true) rescue false
    if Object.const_defined?(:CustomTilemap) && _ps_ok && !$__priosync_installed
      $__priosync_installed = true
      $MKXP_PRIO_SYNC = 2 if $MKXP_PRIO_SYNC.nil?
      class CustomTilemap
        # [2026-07-05c] SEM sonda por instance_method (envenenada pelo
        # method_missing universal — foi por isso que o seam nunca redefiniu o
        # metodo na 1a sessao). $__priosync_installed (fora) e' a idempotencia;
        # NameError do alias cai no rescue exterior.
        alias_method :__ps_orig_refresh, :refresh
          def __ps_capture
            @__ps_sprites = nil
            return if @usedsprites || !@priotilesfast || @priotilesfast.empty?
            _zin = (@__ps_zinfo ||= {})
            _spr = []; _data = []; _i = 0
            @priotilesfast.each do |_p|
              _s = @tiles[_i]; _i += 2
              return if !_s || (_s.disposed? rescue true)
              _id = _p[3]
              _zi = _zin[_id]
              if !_zi
                _t  = (GameData::TerrainTag.try_get(@terrain_tags[_id]) rescue nil)
                _br = ((_t && _t.bridge) ? 1 : 0)
                _rf = ((_t && _t.shows_reflections) ? 1 : 0)
                _zi = [_rf, _br, (@priorities[_id] || 0)]
                _zin[_id] = _zi
              end
              _spr.push(_s)
              _data.push(_p[0], _p[1], _zi[0], _zi[1], _zi[2])
            end
            @__ps_sprites = _spr
            @__ps_data = _data
            @__ps_rect = @priotilesrect
            @__ps_vis  = @visible
          end
          def __ps_fastable?
            _q = ($__ps_why ||= Hash.new(0))
            if !@__ps_sprites || !@layer0 || @usedsprites
              _q[:sem_capture] += 1
              return false
            end
            if @tilesetChanged || @autotiles.changed || @visible != @__ps_vis
              _q[:flags] += 1
              return false
            end
            if !@__ps_rect.equal?(@priotilesrect)
              _q[:rect_obj] += 1
              return false
            end
            _xs = @map_data.xsize; _ys = @map_data.ysize
            _minX = ((@ox / @tileWidth) - 1).clamp(0, _xs - 1)
            _maxX = (((@ox + @viewport.rect.width) / @tileWidth) + 1).clamp(0, _xs - 1)
            _minY = ((@oy / @tileHeight) - 1).clamp(0, _ys - 1)
            _maxY = (((@oy + @viewport.rect.height) / @tileHeight) + 1).clamp(0, _ys - 1)
            _r = @priotilesrect
            if _r[0] == _minX && _r[1] == _minY && _r[2] == _maxX && _r[3] == _maxY
              _q[:ok] += 1
              true
            else
              _q[:rect_calc] += 1
              false
            end
          end
          def refresh(autotiles = false)
            _m = $MKXP_PRIO_SYNC
            $__ps_n = ($__ps_n || 0) + 1
            if ($__ps_n % 300) == 1 && $__ps_why
              MKXPDebug.log("[PRIOSYNC|PORQUE] #{$__ps_why.inspect} capture=#{@__ps_sprites ? @__ps_sprites.size : 'nil'} fast=#{@priotilesfast ? @priotilesfast.size : 'nil'} used=#{@usedsprites.inspect}") rescue nil
            end
            if !autotiles && _m != false && _m != 2 && __ps_fastable?
              @layer0.visible = @visible
              if refreshLayer0(false)
                refreshFlashSprite
                _br = (($PokemonGlobal.bridge > 0) rescue false)
                if Sprite.__mkxp_prio_sync(@__ps_sprites, @__ps_data,
                                           @ox, @oy, @tileWidth, @tileHeight, _br)
                  @oldOx = @ox
                  @oldOy = @oy
                  return
                end
              end
            end
            _fast_was = (!autotiles && _m == 2 && __ps_fastable?)
            __ps_orig_refresh(autotiles)
            if _fast_was
              _br = (($PokemonGlobal.bridge > 0) rescue false)
              (Sprite.__mkxp_prio_sync(@__ps_sprites, @__ps_data,
                                       @ox, @oy, @tileWidth, @tileHeight, _br, 2) rescue nil)
            end
            (__ps_capture rescue nil)
          end
      end
      MKXPDebug.log("[PRIOSYNC] seam instalado (modo=#{$MKXP_PRIO_SYNC.inspect}; 1=on 2=verify false=off)") rescue nil
    else
      MKXPDebug.log("[PRIOSYNC] NAO instalado (nativo=#{_ps_ok})") rescue nil
    end
  rescue => _eps
    MKXPDebug.log("[PRIOSYNC] falhou: #{_eps.class}: #{_eps.message.to_s[0,80]}") rescue nil
  end
  # ---- FIM PRIOSYNC --------------------------------------------------------------------

  # ---- [TTGET] GameData::TerrainTag.try_get memoizado -----------------------------
  # O addTile do CustomTilemap chama try_get POR TILE POR FRAME (~70x) e cada
  # try_get faz validate (Hash+Array novos). O registry e' IMUTAVEL em runtime
  # -> cache por chave = mesmo objeto devolvido, zero validates repetidos.
  # Sonda pelo padrao provado: alias dentro de rescue + flag global.
  begin
    if !$__ttg_on && Object.const_defined?(:GameData)
      class << GameData::TerrainTag
        alias_method :__ttg_orig, :try_get
        def try_get(_k)
          _c = ($__ttg_cache ||= {})
          if _c.key?(_k)
            _c[_k]
          else
            _c[_k] = __ttg_orig(_k)
          end
        end
      end
      $__ttg_on = true
      MKXPDebug.log("[TTGET] TerrainTag.try_get memoizado (registry imutavel)") rescue nil
    end
  rescue Exception => _ett
    MKXPDebug.log("[TTGET] indisponivel: #{_ett.class}: #{_ett.message.to_s[0,60]}") rescue nil
  end
  # ---- FIM TTGET ---------------------------------------------------------------------

  # ---- [TTMAP] Game_Map#terrain_tag memoizado (nucleo) --------------------------------
  # Chamado dezenas de vezes por passo (passabilidade do player+NPCs, escadas do
  # plugin 0068 via pbFollowEventAcrossMaps, ledges, footsteps): cada chamada e'
  # loop de 3 camadas x Table#[] splat + try_get. data/@terrain_tags sao
  # imutaveis em gameplay; o resultado so' depende de (map,x,y,countBridge) e do
  # estado $PokemonGlobal.bridge==0 -> vai tudo na chave. Cache GLOBAL aninhada
  # por map_id (Game_Map e' marshalado — NUNCA ivars), limpa em onMapChange.
  # Chave interna aritmetica pequena (sem shifts largos: seguro mesmo em INT32).
  # Gate $MKXP_TTMAP=false reverte.
  begin
    if $MKXP_TTMAP != false && !$__ttmap_on && Object.const_defined?(:Game_Map)
      class Game_Map
        alias_method :__ttm_orig, :terrain_tag
        def terrain_tag(x, y, countBridge = false)
          if x.is_a?(Integer) && y.is_a?(Integer) &&
             x >= -16 && x < 4080 && y >= -16 && y < 4080
            _br = 0
            _br = 1 if $PokemonGlobal && ($PokemonGlobal.bridge rescue 0).to_i > 0
            _k = ((x + 16) * 4096 + (y + 16)) * 4 + (countBridge ? 2 : 0) + _br
            _c = (($__ttm_cache ||= {})[@map_id] ||= {})
            if _c.key?(_k)
              _c[_k]
            else
              _c[_k] = __ttm_orig(x, y, countBridge)
            end
          else
            __ttm_orig(x, y, countBridge)
          end
        end
      end
      $__ttm_cache = {}
      (Events.onMapChange += proc { $__ttm_cache.clear }) rescue nil
      $__ttmap_on = true
      MKXPDebug.log("[TTMAP] Game_Map#terrain_tag memoizado (global por mapa, chave inclui bridge)") rescue nil
    end
  rescue Exception => _etm
    MKXPDebug.log("[TTMAP] indisponivel: #{_etm.class}: #{_etm.message.to_s[0,60]}") rescue nil
  end
  # ---- FIM TTMAP ---------------------------------------------------------------------

  # ---- [BOOTGC] 1 GC.start no arranque do Scene_Map ----------------------------------
  # O boot deixa ~1.4M objetos vivos com o GC incremental atrasado -> full-GC
  # de EMERGENCIA (~centenas de ms) rebenta a meio do gameplay inicial. Um
  # GC.start unico no 1o Scene_Map#main (durante o fade-in, invisivel) arruma
  # o heap antes do gameplay. Gate $MKXP_BOOTGC=false reverte.
  begin
    if $MKXP_BOOTGC != false && !$__bootgc_on && Object.const_defined?(:Scene_Map)
      class Scene_Map
        alias_method :__bgc_orig_main, :main
        def main
          unless $__bootgc_done
            $__bootgc_done = true
            GC.start rescue nil
            MKXPDebug.log("[BOOTGC] GC.start no 1o Scene_Map#main (heap do boot arrumado)") rescue nil
          end
          __bgc_orig_main
        end
      end
      $__bootgc_on = true
      MKXPDebug.log("[BOOTGC] instalado") rescue nil
    end
  rescue Exception => _ebg
    MKXPDebug.log("[BOOTGC] indisponivel: #{_ebg.class}: #{_ebg.message.to_s[0,60]}") rescue nil
  end
  # ---- FIM BOOTGC --------------------------------------------------------------------

  # ---- [ADDTILE-FAST] slow-frame do refresh: addTile com metade dos dispatches ----
  # Nos 25% de frames em que o rect muda (fronteira de celula x2 tilemaps), o
  # refresh vanilla refaz addTile p/ ~75-125 sprites: ~8-10 dispatches cada
  # (visible=,x=,y=,z= + terrain/priority + tone=,color= + get*Tile) = 15-20ms
  # NUM frame — o serrilhado que resta na rua. Corpo EXATO do 075 com:
  # __mkxp_tile_set (4 setters -> 1 dispatch), zinfo [terrain_data,priority]
  # cacheado por id (dados imutaveis; TTGET ja' memoiza o try_get por baixo).
  # Gate $MKXP_ADDTILE_FAST=false reverte.
  begin
    if $MKXP_ADDTILE_FAST != false && !$__addtile_fast_on &&
       Object.const_defined?(:CustomTilemap) &&
       (Sprite.instance_methods.include?(:__mkxp_tile_set) rescue false)
      class CustomTilemap
        alias_method :__atf_orig_addTile, :addTile
        def addTile(tiles, count, xpos, ypos, id)
          # cache POR INSTANCIA (tilesets diferentes = terrains diferentes p/ o
          # mesmo id); tilemaps nao sao marshalados -> ivar seguro.
          # [v2] INVALIDA quando as TABELAS trocam de identidade (o TSC troca
          # @terrain_tags/@priorities DEPOIS de celulas ja' cacheadas -> zinfo
          # stale por id = z errado = casas atras do chao; raiz do bug 07-05).
          _tt_id = @terrain_tags.__id__
          _pr_id = @priorities.__id__
          if @__atf_tt != _tt_id || @__atf_pr != _pr_id
            @__atf_z = {}
            @__atf_tt = _tt_id
            @__atf_pr = _pr_id
          end
          _zc = @__atf_z
          _zi = _zc[id]
          if _zi.nil?
            _t = GameData::TerrainTag.try_get(@terrain_tags[id])
            _zc[id] = _zi = [_t, @priorities[id]]
          end
          terrain_tag_data = _zi[0]
          priority = _zi[1]
          if id >= 384   # Tileset tile
            if count >= tiles.length
              sprite = CustomTilemapSprite.new(@viewport)
              tiles.push(sprite, 0)
            else
              sprite = tiles[count]
              tiles[count + 1] = 0
            end
            sprite.tone  = @tone
            sprite.color = @color
            getRegularTile(sprite, id)
          else           # Autotile
            if count >= tiles.length
              sprite = CustomTilemapSprite.new(@viewport)
              tiles.push(sprite, 1)
            else
              sprite = tiles[count]
              tiles[count + 1] = 1
            end
            sprite.tone = @tone
            getAutotile(sprite, id)
          end
          if terrain_tag_data.shows_reflections
            _z2 = -100
          elsif $PokemonGlobal.bridge > 0 && terrain_tag_data.bridge
            _z2 = 1
          else
            _z2 = (priority == 0) ? 0 : ypos + priority * 32 + 32
          end
          sprite.__mkxp_tile_set(@visible, xpos, ypos, _z2)
          count += 2
          return count
        end
      end
      $__addtile_fast_on = true
      MKXPDebug.log("[ADDTILE-FAST] addTile com batch-setter + zinfo cache (slow-frames a metade)") rescue nil
    end
  rescue Exception => _eat
    MKXPDebug.log("[ADDTILE-FAST] indisponivel: #{_eat.class}: #{_eat.message.to_s[0,60]}") rescue nil
  end
  # ---- FIM ADDTILE-FAST ----------------------------------------------------------------

  # ---- [SAVE-CLEAN] quarentena automatica de saves 0-byte (anti-crash do menu) --
  # PROVADO no log 20:07: o ecra de gravar le TODOS os slots p/ mostrar a info
  # (pbSaveScreen -> getSaveInfoBoxContents -> read_from_file -> Marshal.load) e um
  # save de 0 BYTES (lixo da era do Marshal-stub) leva o load nativo a levantar
  # "dump format error (EOF)" -> menu de save rebenta. Aqui, no boot, qualquer
  # *.rxdata de tamanho 0 nos sitios de save e' RENOMEADO para .vazio (nao ha'
  # perda: 0 bytes = sem dados). O menu volta a abrir.
  begin
    _dirs = []
    _dirs.push((System.data_directory rescue nil))
    _dirs.push((SaveData::SAVE_DIR rescue nil)) if Object.const_defined?(:SaveData)
    _dirs.push("sdmc:")   # raiz do sdmc (onde o SAVE_DIR='.' antigo os deixou)
    _limpos = 0
    _dirs.compact.uniq.each do |_dd|
      ["File 01", "File 02", "File 03", "File 04", "File 05", "File 06",
       "File 07", "File 08", "File 09", "Game", "Backup"].each do |_slot|
        _fp = _dd + "/" + _slot + ".rxdata"
        next unless (File.file?(_fp) rescue false)
        _s = (File.size(_fp) rescue -1)
        # [SAVE-CLEAN v2 2026-07-03] limiar 0 -> 4096. Um save REAL do Essentials tem
        # ~50-200KB (player+mapa+bag+pokedex...); QUALQUER coisa < 4KB e' esqueletico
        # (era o bug do Game_SaveValues skippado: so' 2 valores de plugin = 101 bytes)
        # e ESSES CRASHAM o ecra de carregar no arranque (raise mal-formado na conversao
        # -> TypeError -> Scene_Intro morre -> jogo fecha). Pomos de lado; nao ha' perda
        # (um save de 101 bytes nao tem progresso jogavel).
        if _s >= 0 && _s < 4096
          if (File.rename(_fp, _fp + ".esqueleto") rescue false)
            _limpos += 1
            MKXPDebug.log("[SAVE-CLEAN] save esqueletico (#{_s}B < 4KB) posto de lado: #{_fp} -> .esqueleto") rescue nil
          end
        end
      end
    end
    MKXPDebug.log("[SAVE-CLEAN] varrimento: #{_limpos} save(s) 0-byte em quarentena") rescue nil
  rescue => __esc
    MKXPDebug.log("[SAVE-CLEAN] falhou: #{__esc.class}: #{__esc.message}") rescue nil
  end
  # ---- FIM SAVE-CLEAN --------------------------------------------------------------

  # ---- [SAVELOAD-TRACE] qual valor do load falha (map=0 no Continuar) -----------
  # PROVADO: o save e' perfeito (@maps=[map43], player@(29,25)) e o marshal le'-o
  # bem, MAS depois do load o $game_map e' o mapa 0. O SaveData::Value#load faz
  # validate_value ANTES do @load_proc -> se a validacao rebentar, o $MapFactory
  # NUNCA e' atribuido e fica no default (mapa 0). Este wrapper loga, por valor,
  # se o load correu OU rebentou (com a mensagem) -> nomeia o culpado exato.
  begin
    _svc = (defined?(SaveData::Value) ? SaveData::Value : nil)
    if _svc && _svc.instance_methods.include?(:load) && !_svc.instance_methods.include?(:__mkxp_orig_valload)
      _svc.class_eval do
        alias_method :__mkxp_orig_valload, :load
        def load(value)
          _id = (@id rescue '?')
          begin
            __mkxp_orig_valload(value)
            # [LOG-LEAN] "valor X OK" por-valor removido (18 linhas/load); so' os
            # que REBENTAM ficam (logados no rescue abaixo, que nomeiam o culpado)
          rescue => _e
            MKXPDebug.log("[SAVELOAD] valor '#{_id}' REBENTOU: #{_e.class}: #{_e.message.to_s[0,90]} (classe recebida=#{value.class}) -> NAO atribuido!") rescue nil
            raise
          end
        end
      end
      MKXPDebug.log("[SAVELOAD-TRACE] wrapper instalado em SaveData::Value#load (nomeia o valor que falha no Continuar)") rescue nil
    end
  rescue => _esl
    MKXPDebug.log("[SAVELOAD-TRACE] falhou: #{_esl.class}: #{_esl.message}") rescue nil
  end
  # ---- FIM SAVELOAD-TRACE ----------------------------------------------------------

  # ---- FIX menu Voltseon: menu_icon_width chama load_bitmap com 1 arg --------
  # O CALLMENU-DIAG capturou o backtrace do crash do menu de pausa:
  #   quicksave_update -> call_menu -> ... -> new -> initialize -> menu_icon_width
  #   -> load_bitmap  (ArgumentError "given 1, expected 2").
  # Causa REAL: o plugin (PokemonTemp#menu_icon_width) chama
  #   RPG::Cache.load_bitmap(MENU_FILE_PATH + "menuDebug")  -> 1 caminho combinado,
  # mas o load_bitmap do jogo e' load_bitmap(folder, filename, hue=0) (2 args:
  # faz path = folder + filename). Como menuDebug.png EXISTE, o safeExists? da'
  # true, o ramo corre e rebenta -> menu morto + picos ~50ms.
  # Re-implementamos menu_icon_width com a chamada CORRETA (2 args), protegido.
  # Idempotente. Cobre a UNICA chamada load_bitmap de 1 arg do jogo todo.
  begin
    if Object.const_defined?(:PokemonTemp) &&
       PokemonTemp.method_defined?(:menu_icon_width) &&
       !PokemonTemp.method_defined?(:__mkxp_orig_menu_icon_width)
      PokemonTemp.send(:alias_method, :__mkxp_orig_menu_icon_width, :menu_icon_width)
      PokemonTemp.send(:define_method, :menu_icon_width) do
        @menu_icon_width ||= begin
          __w = 48
          begin
            __mp = (defined?(MENU_FILE_PATH) ? MENU_FILE_PATH : "Graphics/Pictures/Voltseon's Pause Menu/")
            if (safeExists?(__mp + "menuDebug") rescue false)
              __bmp = RPG::Cache.load_bitmap(__mp, "menuDebug")   # 2 args (CORRETO)
              __w = __bmp.width
              __bmp.dispose
            end
          rescue
            __w = 48
          end
          __w
        end
      end
      MKXPDebug.log("[MENUFIX] PokemonTemp#menu_icon_width corrigido (load_bitmap com 2 args)") rescue nil
    end
  rescue => __emf
    MKXPDebug.log("[MENUFIX] falhou: #{__emf.class}: #{__emf.message}") rescue nil
  end
  # ---- FIM FIX menu Voltseon -------------------------------------------------

  # ---- BAGFIX: atalhos globais de saco rebentam quando $PokemonBag e' nil ----
  # CRASH capturado no teu log: menu de pausa -> call_menu -> ... -> pbHasItem?
  # -> SystemStackError (recursao infinita), bt[0..7] TODOS pbHasItem?.
  # Causa: os atalhos GLOBAIS (base_scripts.rb:82055-82071) fazem
  #   $PokemonBag.pbHasItem?(*args)   SEM guarda nenhuma. No quarto (logo apos
  # acordar, o "UP AND AT 'EM") o $PokemonBag ainda e' nil (saco criado mais
  # tarde). Como o mruby resolve nil.pbHasItem? para o PROPRIO atalho global
  # (Object#pbHasItem?), fica $PokemonBag.pbHasItem? == nil.pbHasItem? == atalho
  # -> recursao infinita -> SystemStackError. O menu Voltseon chama pbHasItem?/
  # pbQuantity SEM prefixo ao construir-se, com o saco nil -> crash.
  # NAO da' para guardar com respond_to? (respond_to_missing? universal devolve
  # true p/ tudo). Usamos is_a?(PokemonBag): cobre nil E tipo errado.
  # Sem saco: "tens item?" -> false, "quantos?" -> 0 (comportamento correto).
  begin
    if Object.const_defined?(:PokemonBag)
      Object.send(:define_method, :pbHasItem?) do |*args|
        $PokemonBag.is_a?(PokemonBag) ? $PokemonBag.pbHasItem?(*args) : false
      end
      Object.send(:define_method, :pbQuantity) do |*args|
        $PokemonBag.is_a?(PokemonBag) ? $PokemonBag.pbQuantity(*args) : 0
      end
      Object.send(:define_method, :pbCanStore?) do |*args|
        $PokemonBag.is_a?(PokemonBag) ? $PokemonBag.pbCanStore?(*args) : false
      end
      Object.send(:define_method, :pbStoreItem) do |*args|
        $PokemonBag.is_a?(PokemonBag) ? $PokemonBag.pbStoreItem(*args) : false
      end
      Object.send(:define_method, :pbStoreAllOrNone) do |*args|
        $PokemonBag.is_a?(PokemonBag) ? $PokemonBag.pbStoreAllOrNone(*args) : false
      end
      MKXPDebug.log("[BAGFIX] atalhos globais de saco guardados vs $PokemonBag nil (anti-recursao)") rescue nil
    end
  rescue => __ebag
    MKXPDebug.log("[BAGFIX] falhou: #{__ebag.class}: #{__ebag.message}") rescue nil
  end

  # ---- OUTFITDIAG: a estante deveria abrir o menu de outfit -----------------
  # Evento #15 "Closet" @tile(26,3) em Map003 e' Action Button (trigger 0), SEM
  # condicoes, e faz 117[34] -> Common Event 34 ("Change Clothes") -> CE173
  # (o menu de outfit, confirmado a funcionar pelo teu screenshot).
  # Como crashaste antes de testar a estante, ainda nao sabemos PORQUE nao abre.
  # Em vez de adivinhar, espiamos command_117 (chamar Common Event): se carregares
  # A virado p/ a estante e isto disparar p/ CE 34/173, o evento FUNCIONA e o
  # problema e' a jusante; se NAO disparar, estas virado p/ o objeto errado (a
  # comoda de madeira nao e' o Closet -- este esta' na parede de cima, tile x=26)
  # ou a trigger nao bate. command_117 e' no-arg e le @parameters[0]=ID do CE.
  begin
    if Object.const_defined?(:Interpreter) && Interpreter.method_defined?(:command_117) &&
       !Interpreter.method_defined?(:__mkxp_orig_command_117)
      Interpreter.send(:alias_method, :__mkxp_orig_command_117, :command_117)
      Interpreter.send(:define_method, :command_117) do
        begin
          __ceid = (@parameters[0] rescue nil)
          if __ceid.is_a?(Integer)
            $__mkxp_ce_seen ||= {}
            $__mkxp_ce_seen[__ceid] = ($__mkxp_ce_seen[__ceid] || 0) + 1
            # cadeia do outfit (34->212 picker,173; 20/21 closet) SEMPRE;
            # qualquer outro CE so' as 2 primeiras vezes (evita flood por-frame).
            if [34, 173, 212, 20, 21].include?(__ceid) || $__mkxp_ce_seen[__ceid] <= 2
              MKXPDebug.log("[OUTFITDIAG] command_117 -> CommonEvent ##{__ceid} (x#{$__mkxp_ce_seen[__ceid]})") rescue nil
            end
          end
        rescue
        end
        __mkxp_orig_command_117
      end
      MKXPDebug.log("[OUTFITDIAG] hook command_117 instalado") rescue nil
    end
  rescue => __eod
    MKXPDebug.log("[OUTFITDIAG] falhou: #{__eod.class}: #{__eod.message}") rescue nil
  end

  # [GILBERTDIAG] Diagnostico do seguidor (instalado no padrao que FUNCIONA, ao
  # contrario do ADDDEP do debug_probe que nunca instalava). O seguidor e' adicionado
  # por um comando de Script (code 355) 'pbAddDependency2(7,"Giltbert",3)' no evento do
  # mapa, mas nao havia instrumentacao nos Scripts de eventos de mapa. Logamos (a) os
  # Scripts que mencionam Dependency/Follow/addEvent, e (b) a entrada/saida de addEvent.
  # SEM regex (mruby do mkxp-3ds nao tem Regexp nativo) -> usa String#include?.
  begin
    if Object.const_defined?(:Interpreter) && Interpreter.method_defined?(:command_355) &&
       !Interpreter.method_defined?(:__mkxp_orig_command_355)
      Interpreter.send(:alias_method, :__mkxp_orig_command_355, :command_355)
      Interpreter.send(:define_method, :command_355) do
        begin
          # LAZY-INSTALL dos hooks que precisam de classes TARDIAS (DependentEvents so'
          # existe a t~17s; o bloco do binding corre a t~4s -> o hook one-shot nunca
          # instalava). command_355 so' dispara em runtime, por isso instalamos aqui.
          unless $__gilbert_late_done
            if Object.const_defined?(:DependentEvents) && (DependentEvents.method_defined?(:addEvent) rescue false) &&
               !(DependentEvents.method_defined?(:__g_orig_addEvent) rescue true)
              DependentEvents.send(:alias_method, :__g_orig_addEvent, :addEvent)
              DependentEvents.send(:define_method, :addEvent) do |*aa|
                __b = ($PokemonGlobal.dependentEvents.size rescue -1)
                (MKXPDebug.log("[GILBERTDIAG] addEvent CHAMADO args=#{aa.size} dep_antes=#{__b}") rescue nil)
                __r = (__g_orig_addEvent(*aa) rescue (MKXPDebug.log("[GILBERTDIAG] addEvent REBENTOU") rescue nil; nil))
                (MKXPDebug.log("[GILBERTDIAG] addEvent FIM dep_depois=#{$PokemonGlobal.dependentEvents.size rescue -1}") rescue nil)
                __r
              end
              (MKXPDebug.log("[GILBERTDIAG] hook addEvent instalado (lazy via command_355)") rescue nil)
              $__gilbert_late_done = true
            end
          end
          __sc = (@parameters[0].to_s rescue '')
          $__g355 = ($__g355 || 0) + 1
          # LOG TODOS os Script(355) (cap 60) -> ver o que o evento do Gilbert corre.
          # Dependency/Follow/addEvent SEMPRE (mesmo apos o cap).
          if $__g355 <= 60 || __sc.include?('Dependency') || __sc.include?('Follow') || __sc.include?('addEvent') || __sc.include?('Partner') || __sc.include?('Register')
            MKXPDebug.log("[GILBERTDIAG] Script(355) ##{$__g355} m=#{($game_map.map_id rescue '?')}: #{__sc[0,140]}") rescue nil
          end
        rescue; end
        # [GILBERTDIAG] CAPTURAR a excecao do Script (decisivo): se pbRegisterPartner
        # ou pbAddDependency2 rebentar, o evento do seguidor aborta antes de adicionar.
        # Logamos a causa EXATA + o script, e re-lancamos (sem mudar comportamento).
        begin
          __mkxp_orig_command_355
        rescue => __s355
          (MKXPDebug.log("[GILBERTDIAG] Script(355) REBENTOU: #{__s355.class}: #{__s355.message.to_s[0,140]} | sc='#{(@parameters[0].to_s[0,90] rescue '?')}'") rescue nil)
          raise
        end
      end
      MKXPDebug.log("[GILBERTDIAG] hook command_355 instalado") rescue nil
    end
    # command_655: linhas de CONTINUACAO de um Script multi-linha. O pbAddDependency
    # pode estar numa 655, nao na 355 -> sem isto perdiamos o texto.
    if Object.const_defined?(:Interpreter) && Interpreter.method_defined?(:command_655) &&
       !Interpreter.method_defined?(:__mkxp_orig_command_655)
      Interpreter.send(:alias_method, :__mkxp_orig_command_655, :command_655)
      Interpreter.send(:define_method, :command_655) do
        begin
          __sc6 = (@parameters[0].to_s rescue '')
          $__g655 = ($__g655 || 0) + 1
          if $__g655 <= 60 || __sc6.include?('Dependency') || __sc6.include?('Follow')
            MKXPDebug.log("[GILBERTDIAG] Script(655) ##{$__g655}: #{__sc6[0,140]}") rescue nil
          end
        rescue; end
        __mkxp_orig_command_655
      end
      MKXPDebug.log("[GILBERTDIAG] hook command_655 instalado") rescue nil
    end
  rescue => __egd
    MKXPDebug.log("[GILBERTDIAG] command_355 falhou: #{__egd.class}: #{__egd.message}") rescue nil
  end
  begin
    if Object.const_defined?(:DependentEvents) && DependentEvents.method_defined?(:addEvent) &&
       !DependentEvents.method_defined?(:__mkxp_orig_addEvent2)
      DependentEvents.send(:alias_method, :__mkxp_orig_addEvent2, :addEvent)
      DependentEvents.send(:define_method, :addEvent) do |*aa|
        __b = ($PokemonGlobal.dependentEvents.size rescue -1)
        (MKXPDebug.log("[GILBERTDIAG] addEvent CHAMADO args=#{aa.size} dep_antes=#{__b}") rescue nil)
        __rr = (__mkxp_orig_addEvent2(*aa) rescue (MKXPDebug.log("[GILBERTDIAG] addEvent REBENTOU") rescue nil; nil))
        __a = ($PokemonGlobal.dependentEvents.size rescue -1)
        (MKXPDebug.log("[GILBERTDIAG] addEvent FIM dep_depois=#{__a} (#{__a > __b ? '>>> SEGUIDOR ADICIONADO' : 'NAO adicionou'})") rescue nil)
        __rr
      end
      MKXPDebug.log("[GILBERTDIAG] hook addEvent instalado") rescue nil
    end
  rescue => __ead
    MKXPDebug.log("[GILBERTDIAG] addEvent falhou: #{__ead.class}: #{__ead.message}") rescue nil
  end

  def self.__mkxp_hook_scene(klass_name)
    return false unless (Object.const_defined?(klass_name) rescue false)
    klass = Object.const_get(klass_name)
    return false unless klass.is_a?(Class)
    return true if klass.method_defined?(:__diag_pbStartScene)
    return false unless klass.method_defined?(:pbStartScene)
    klass.send(:alias_method, :__diag_pbStartScene, :pbStartScene)
    klass.send(:define_method, :pbStartScene) do |*a, &b|
      MKXPDebug.log("[NAMEIN] #{klass_name}#pbStartScene ENTROU") rescue nil
      begin
        r = __diag_pbStartScene(*a, &b)
        MKXPDebug.log("[NAMEIN] #{klass_name}#pbStartScene OK -> #{r.inspect}") rescue nil
        r
      rescue => ex
        MKXPDebug.log("[NAMEIN] #{klass_name}#pbStartScene CRASH: #{ex.class}: #{ex.message}") rescue nil
        (ex.backtrace || [])[0,8].each_with_index{|l,i| MKXPDebug.log("[NAMEIN]   bt[#{i}]: #{l}") rescue nil}
        raise
      end
    end
    MKXPDebug.log("[NAMEIN] hook #{klass_name}#pbStartScene OK")
    true
  end
  ["PokemonEntryScene","PokemonEntryScene2","PokemonEntryScene3"].each do |kn|
    begin
      __mkxp_hook_scene(kn)
    rescue => e
      MKXPDebug.log("[NAMEIN] hook #{kn} falhou: #{e.class}: #{e.message}")
    end
  end
  # Hook GLOBAL ao proprio pbEnterText/pbEnterPlayerName: dizer se o teclado
  # chega a ser CHAMADO. Se nao houver "[NAMEIN] pbEnterText CHAMADO" quando o
  # jogo pede o nome, o script parte ANTES do teclado (ex: falha do sprite do
  # Trainer Card a montante). Se aparecer CHAMADO sem "->", fica preso dentro.
  def self.__mkxp_hook_global(meth)
    ok = Object.method_defined?(meth)
    ok ||= (Object.allocate.respond_to?(meth, true) rescue false) unless ok
    unless ok
      MKXPDebug.log("[NAMEIN] #{meth} ausente -- este jogo nao usa este metodo (ignorado, benigno)") rescue nil
      return false
    end
    diag = "__diag_g_#{meth}".to_sym
    return true if Object.method_defined?(diag)
    Object.send(:alias_method, diag, meth)
    Object.send(:define_method, meth) do |*a, &b|
      MKXPDebug.log("[NAMEIN] #{meth} CHAMADO args=#{a.inspect}") rescue nil
      begin
        r = send(diag, *a, &b)
        MKXPDebug.log("[NAMEIN] #{meth} -> #{r.inspect}") rescue nil
        r
      rescue => ex
        MKXPDebug.log("[NAMEIN] #{meth} CRASH: #{ex.class}: #{ex.message}") rescue nil
        (ex.backtrace || [])[0,8].each_with_index{|l,i| MKXPDebug.log("[NAMEIN]   bt[#{i}]: #{l}") rescue nil}
        raise
      end
    end
    # [LOG-LEAN] "hook GLOBAL X OK" por-metodo removido (6 linhas de boot; o hook
    # so' importa quando CHAMADO, e ai loga na mesma). Falhas de install idem:
    # pbChooseGender/pbNameEntry nao existem neste jogo = NameError esperado.
    true
  end
  [:pbEnterText, :pbEnterPlayerName, :pbTrainerName, :pbEnterPokemonName, :pbChooseGender, :pbNameEntry].each do |m|
    begin
      __mkxp_hook_global(m)
    rescue => e
      # [LOG-LEAN] silencioso (pbChooseGender/pbNameEntry ausentes = esperado)
    end
  end
  # Hook generico a um metodo de INSTANCIA de uma classe. Apanha caminhos
  # alternativos de entrada de nome que NAO passam por pbEnterText:
  #  - Essentials: PokemonEntry#pbStartScreen (camada acima de pbStartScene)
  #  - RMXP nativo (Name Input Processing / event command 105):
  #    Interpreter#command_105, Scene_Name#main, Window_NameInput/Edit#initialize
  # Se NENHUM destes nem os globais dispararem ao despachar a msg "write your
  # name", a entrada de nome nunca e' sequer tentada (bug a montante no evento).
  def self.__mkxp_hook_imethod(klass_name, meth)
    meth = meth.to_sym
    return false unless (Object.const_defined?(klass_name) rescue false)
    klass = Object.const_get(klass_name)
    return false unless klass.is_a?(Class)
    return false unless klass.method_defined?(meth)
    diag = "__diag_im_#{klass_name}_#{meth}".to_sym
    return true if klass.method_defined?(diag)
    klass.send(:alias_method, diag, meth)
    klass.send(:define_method, meth) do |*a, &b|
      MKXPDebug.log("[NAMEIN] #{klass_name}##{meth} ENTROU (args=#{a.size})") rescue nil
      begin
        r = send(diag, *a, &b)
        MKXPDebug.log("[NAMEIN] #{klass_name}##{meth} OK") rescue nil
        r
      rescue => ex
        MKXPDebug.log("[NAMEIN] #{klass_name}##{meth} CRASH: #{ex.class}: #{ex.message}") rescue nil
        (ex.backtrace || [])[0,10].each_with_index{|l,i| MKXPDebug.log("[NAMEIN]   bt[#{i}]: #{l}") rescue nil}
        raise
      end
    end
    MKXPDebug.log("[NAMEIN] hook #{klass_name}##{meth} OK")
    true
  end
  [
    ["PokemonEntry","pbStartScreen"], ["PokemonEntry2","pbStartScreen"],
    ["PokemonEntry3","pbStartScreen"],
    ["Interpreter","command_105"],
    ["Scene_Name","main"],
    ["Window_NameInput","initialize"], ["Window_NameEdit","initialize"],
  ].each do |kn, mn|
    begin
      __mkxp_hook_imethod(kn, mn)
    rescue => e
      MKXPDebug.log("[NAMEIN] hook #{kn}##{mn} falhou: #{e.class}: #{e.message}")
    end
  end
  # ---- FIM DIAGNOSTICO ----
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
#define MKXP_SKIP_BROKEN_PLUGINS 0   /* [2026-07-05] EBDX compila 100% OK (compile-only provou 485/485) -> DES-SALTADO p/ testar runtime (era saltado por suposto SyntaxError que nao existe) */
#endif
/* [EBDX-DIAG 2026-07-05] compila (compile-only, SEM executar) os scripts do plugin
 * saltado p/ CAPTURAR os SyntaxErrors exatos (sem cascata/crash), 1x no boot. Serve
 * para a proxima sessao saber que corrigir. Poe a 0 depois de capturados. */
#ifndef MKXP_DIAG_EBDX
#define MKXP_DIAG_EBDX 0   /* [QUIET 2026-07-12] 1->0: SyntaxErrors do EBDX ja' capturados/corrigidos. Repor 1 p/ recapturar. */
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
        g_plugin_exec_failed = true;   /* [PLUGIN-RETRY] p/ re-correr no 2o passo */
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
    if (mrb->exc) { show_error(mrb, script_name); mrb->exc = 0; g_plugin_exec_failed = true; }

    mrbc_context_free(mrb, c);
    mrb_gc_arena_restore(mrb, arena);
    return true;
}
#endif /* MKXP_CAN_DUMP_IREP */
#endif /* MKXP_PLUGIN_CACHE */


/* ============================================================================
 * [FILE-IO 2026-07-02] File/FileTest REAIS + System.data_directory (SAVE/LOAD)
 * ----------------------------------------------------------------------------
 * PRIORIDADE 1 (Save/Load). AUDITORIA PROVOU que gravar era um NO-OP silencioso:
 *   - rgss_stubs.rb `class File` e' FAKE: open{ yield nil }, read->"", write->0
 *     (por isso o motor ja' precisava do remendo eof?->true p/ file=nil).
 *   - SaveData.save_to_file faz File.open('wb'){|f| Marshal.dump(dados, f)} ->
 *     com o File fake nada e' escrito; o load nunca ve' um save.
 *   - System.data_directory nao existia -> FILE_PATH caia em './Game.rxdata'.
 * FIX motor: classe File REAL (fopen no sdmc), FileTest real, e
 * System.data_directory = sdmc:/mkxp/saves (mkdir na init). O rgss_stubs passa a
 * NAO sobrepor o File quando o nativo esta' presente (gate File.mkxp_native?).
 * API minima que o Essentials v19 usa: open/close/write/read/eof?/pos/pos=/seek/
 * size/gets + file?/exist?/directory?/delete/rename/move/mtime/binread/binwrite.
 * ============================================================================ */
struct MkxpFileH { FILE *fp; bool closed; size_t wrote; char path[256]; };
static void mkxp_fileh_free(mrb_state *mrb, void *p) {
    MkxpFileH *h = (MkxpFileH*)p;
    if (h) { if (h->fp && !h->closed) fclose(h->fp); mrb_free(mrb, h); }
}
static const mrb_data_type MkxpFileType = { "File", mkxp_fileh_free };

/* [SAVE-PROVA 2026-07-02] loga qualquer ESCRITA de ficheiro que terminou (>0 bytes)
 * -> prova irrefutavel no debug_binding.log de que o Save gravou, e onde. Barato
 * (1 linha por ficheiro fechado que foi escrito, nao por byte). */
static void mkxp_file_log_write(MkxpFileH *h) {
    if (!h || h->wrote == 0) return;
    extern FILE *g_dbglog;
    if (g_dbglog && g_log_verbose) {
        fprintf(g_dbglog, "[SAVE-PROVA] gravado '%s' (%zu bytes)\n", h->path, h->wrote);
        fflush(g_dbglog);
    }
}
static MkxpFileH *mkxp_file_get(mrb_state *mrb, mrb_value self, bool need_open) {
    MkxpFileH *h = (MkxpFileH*)mrb_data_get_ptr(mrb, self, &MkxpFileType);
    if (need_open && (!h || !h->fp || h->closed))
        mrb_raise(mrb, E_RUNTIME_ERROR, "ficheiro fechado");
    return h;
}
/* modo Ruby -> fopen: garante 'b' (o 3DS/newlib distingue; saves sao binarios) */
static void mkxp_file_mode(const char *in, char *out /*>=8*/) {
    size_t n = 0; bool has_b = false;
    for (const char *p = in; *p && n < 6; p++) { if (*p == 'b') has_b = true; out[n++] = *p; }
    if (!has_b) out[n++] = 'b';
    out[n] = '\0';
    if (n == 1) { out[0] = 'r'; out[1] = 'b'; out[2] = '\0'; }
}
/* [FILE-RESOLVE 2026-07-02] REGRESSAO CORRIGIDA: o jogo abre caminhos RELATIVOS
 * do diretorio do jogo e sem extensao (ex: getPlayTime2 -> File.open("Audio/BGM/
 * TITLE_MD")). No Windows resolve porque o CWD e' a pasta do jogo; aqui o fopen
 * cru falhava -> RuntimeError -> Scene_Intro morria -> o jogo FECHAVA sozinho.
 * Resolver como o resto do motor (mesmas raizes do bmp_load_file) + extensoes de
 * audio em LEITURA (o consumidor conhecido de caminhos sem extensao e' o audio).
 * Escrita relativa: determinista para a pasta do jogo (sdmc), nunca romfs (RO). */
static bool mkxp_file_is_abs(const char *p) {
    return p && (p[0] == '/' || strstr(p, ":/") != nullptr);
}
static bool mkxp_file_resolve(const char *path, bool for_read, bool want_dir,
                              char *out, size_t outsz) {
    static const char *roots[] = { "", "sdmc:/mkxp/game/", "sdmc:/mkxp/", "romfs:/", nullptr };
    static const char *exts[]  = { "", ".ogg", ".wav", ".mp3", nullptr };
    const bool is_abs = mkxp_file_is_abs(path);
    for (int ri = 0; roots[ri] != nullptr; ri++) {
        if (is_abs && roots[ri][0] != '\0') continue;   /* absoluto: so' como esta' */
        char base[512];
        snprintf(base, sizeof(base), "%s%s", roots[ri], path);
        for (int ei = 0; exts[ei] != nullptr; ei++) {
            if ((!for_read || want_dir) && ei > 0) break;   /* sem magia de ext p/ escrita/dirs */
            char cand[520];
            snprintf(cand, sizeof(cand), "%s%s", base, exts[ei]);
            struct stat st;
            if (::stat(cand, &st) == 0 &&
                (want_dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode))) {
                snprintf(out, outsz, "%s", cand);
                return true;
            }
        }
    }
    return false;
}
static mrb_value mkxp_file_s_open(mrb_state *mrb, mrb_value self) {
    const char *path; const char *mode = "r"; mrb_value blk = mrb_nil_value();
    mrb_get_args(mrb, "z|z&", &path, &mode, &blk);
    char m[8]; mkxp_file_mode(mode, m);
    const bool for_read = (m[0] == 'r');
    FILE *fp = fopen(path, m);
    if (!fp) {
        char full[520];
        if (for_read) {
            /* leitura: resolver raizes+extensoes como o resto do motor */
            if (mkxp_file_resolve(path, true, false, full, sizeof(full)))
                fp = fopen(full, m);
        } else if (!mkxp_file_is_abs(path)) {
            /* escrita relativa: determinista p/ a pasta do jogo no sdmc */
            snprintf(full, sizeof(full), "sdmc:/mkxp/game/%s", path);
            fp = fopen(full, m);
        }
    }
    if (!fp) mrb_raisef(mrb, E_RUNTIME_ERROR, "File.open falhou: %s (modo %s)", path, m);
    MkxpFileH *h = (MkxpFileH*)mrb_malloc(mrb, sizeof(MkxpFileH));
    h->fp = fp; h->closed = false; h->wrote = 0;
    snprintf(h->path, sizeof(h->path), "%s", path);
    mrb_value obj = mrb_obj_value(mrb_data_object_alloc(mrb, mrb_class_ptr(self), h, &MkxpFileType));
    if (!mrb_nil_p(blk)) {
        mrb_value res = mrb_yield(mrb, blk, obj);
        /* fecho garantido no fim do bloco (se o bloco levantar, o GC fecha via free) */
        if (!h->closed && h->fp) {
            fclose(h->fp); h->fp = nullptr; h->closed = true;
            mkxp_file_log_write(h);
        }
        return res;
    }
    return obj;
}
static mrb_value mkxp_file_write(mrb_state *mrb, mrb_value self) {
    MkxpFileH *h = mkxp_file_get(mrb, self, true);
    mrb_value s; mrb_get_args(mrb, "o", &s);
    if (!mrb_string_p(s)) s = mrb_funcall(mrb, s, "to_s", 0);
    size_t n = fwrite(RSTRING_PTR(s), 1, (size_t)RSTRING_LEN(s), h->fp);
    /* [SAVE-PROVA] contabiliza bytes escritos p/ o log de close saber se gravou */
    h->wrote += n;
    return mrb_int_value(mrb, (mrb_int)n);
}
static mrb_value mkxp_file_read(mrb_state *mrb, mrb_value self) {
    MkxpFileH *h = mkxp_file_get(mrb, self, true);
    mrb_int want = -1; mrb_get_args(mrb, "|i", &want);
    long pos = ftell(h->fp);
    if (want < 0) {                        /* ler ate' ao fim */
        fseek(h->fp, 0, SEEK_END);
        long end = ftell(h->fp);
        fseek(h->fp, pos, SEEK_SET);
        want = (mrb_int)(end - pos);
        if (want < 0) want = 0;
    }
    if (want == 0) return mrb_str_new_lit(mrb, "");
    std::vector<char> buf((size_t)want);
    size_t rd = fread(buf.data(), 1, (size_t)want, h->fp);
    if (rd == 0) return mrb_nil_value();   /* EOF com n pedido -> nil (CRuby) */
    return mrb_str_new(mrb, buf.data(), (mrb_int)rd);
}
static mrb_value mkxp_file_eof(mrb_state *mrb, mrb_value self) {
    MkxpFileH *h = mkxp_file_get(mrb, self, true);
    int c = fgetc(h->fp);
    if (c == EOF) return mrb_true_value();
    ungetc(c, h->fp);
    return mrb_false_value();
}
static mrb_value mkxp_file_close(mrb_state *mrb, mrb_value self) {
    MkxpFileH *h = mkxp_file_get(mrb, self, false);
    if (h && h->fp && !h->closed) {
        fclose(h->fp); h->fp = nullptr; h->closed = true;
        mkxp_file_log_write(h);
    }
    return mrb_nil_value();
}
static mrb_value mkxp_file_closed(mrb_state *mrb, mrb_value self) {
    MkxpFileH *h = mkxp_file_get(mrb, self, false);
    return mrb_bool_value(!h || h->closed || !h->fp);
}
static mrb_value mkxp_file_pos(mrb_state *mrb, mrb_value self) {
    MkxpFileH *h = mkxp_file_get(mrb, self, true);
    return mrb_int_value(mrb, (mrb_int)ftell(h->fp));
}
static mrb_value mkxp_file_pos_set(mrb_state *mrb, mrb_value self) {
    MkxpFileH *h = mkxp_file_get(mrb, self, true);
    mrb_int n; mrb_get_args(mrb, "i", &n);
    fseek(h->fp, (long)n, SEEK_SET);
    return mrb_int_value(mrb, n);
}
static mrb_value mkxp_file_seek(mrb_state *mrb, mrb_value self) {
    MkxpFileH *h = mkxp_file_get(mrb, self, true);
    mrb_int n; mrb_int whence = 0; mrb_get_args(mrb, "i|i", &n, &whence);
    int w = (whence == 1) ? SEEK_CUR : (whence == 2) ? SEEK_END : SEEK_SET;
    fseek(h->fp, (long)n, w);
    return mrb_int_value(mrb, 0);
}
static mrb_value mkxp_file_isize(mrb_state *mrb, mrb_value self) {
    MkxpFileH *h = mkxp_file_get(mrb, self, true);
    long pos = ftell(h->fp);
    fseek(h->fp, 0, SEEK_END);
    long end = ftell(h->fp);
    fseek(h->fp, pos, SEEK_SET);
    return mrb_int_value(mrb, (mrb_int)end);
}
static mrb_value mkxp_file_gets(mrb_state *mrb, mrb_value self) {
    MkxpFileH *h = mkxp_file_get(mrb, self, true);
    std::vector<char> line;
    int c;
    while ((c = fgetc(h->fp)) != EOF) {
        line.push_back((char)c);
        if (c == '\n') break;
    }
    if (line.empty()) return mrb_nil_value();
    return mrb_str_new(mrb, line.data(), (mrb_int)line.size());
}
/* ---- class-methods de caminho (partilhados com FileTest) ----
 * [FILE-RESOLVE] usam o MESMO resolver do open: um guard (safeExists?/file?) e o
 * open subsequente teem de CONCORDAR, senao o guard passa e o open rebenta. */
static mrb_value mkxp_file_s_file(mrb_state *mrb, mrb_value) {
    const char *p; mrb_get_args(mrb, "z", &p);
    char full[520];
    return mrb_bool_value(mkxp_file_resolve(p, true, false, full, sizeof(full)));
}
/* [DIR-OPENDIR 2026-07-02] stat()+S_ISDIR e' POUCO FIAVEL no 3DS/libctru (devolve
 * false para pastas reais) -> File.directory?("sdmc:/mkxp/saves") dava false ->
 * o Auto Multi Save punha SAVE_DIR='.' e gravava na RAIZ do sdmc (file "01.rxdata",
 * 0 bytes fora do sitio). mkxp_native_dir_exists usa opendir (fiavel). */
extern "C" int mkxp_native_dir_exists(const char *path);
static mrb_value mkxp_file_s_dir(mrb_state *mrb, mrb_value) {
    const char *p; mrb_get_args(mrb, "z", &p);
    return mrb_bool_value(mkxp_native_dir_exists(p) != 0);
}
static mrb_value mkxp_file_s_exist(mrb_state *mrb, mrb_value) {
    const char *p; mrb_get_args(mrb, "z", &p);
    char full[520];
    return mrb_bool_value(mkxp_file_resolve(p, true, false, full, sizeof(full)) ||
                          mkxp_native_dir_exists(p) != 0);
}
static mrb_value mkxp_file_s_size(mrb_state *mrb, mrb_value) {
    const char *p; mrb_get_args(mrb, "z", &p);
    char full[520];
    if (!mkxp_file_resolve(p, true, false, full, sizeof(full))) return mrb_int_value(mrb, 0);
    struct stat st;
    return mrb_int_value(mrb, (::stat(full, &st) == 0) ? (mrb_int)st.st_size : 0);
}
static mrb_value mkxp_file_s_delete(mrb_state *mrb, mrb_value) {
    /* CRuby levanta ENOENT; aqui NAO levantamos (SaveData.delete_file chama sem
     * verificar -> um raise partiria o fluxo; devolver a contagem e' suficiente). */
    const mrb_value *argv; mrb_int argc; mrb_get_args(mrb, "*", &argv, &argc);
    int n = 0;
    for (mrb_int i = 0; i < argc; i++)
        if (mrb_string_p(argv[i]) && ::remove(RSTRING_CSTR(mrb, argv[i])) == 0) n++;
    return mrb_int_value(mrb, n);
}
static mrb_value mkxp_file_s_rename(mrb_state *mrb, mrb_value) {
    const char *a, *b; mrb_get_args(mrb, "zz", &a, &b);
    if (::rename(a, b) != 0) {
        /* fallback copia+apaga (rename pode falhar entre dirs nalguns FS) */
        FILE *fi = fopen(a, "rb");
        if (!fi) return mrb_int_value(mrb, -1);
        FILE *fo = fopen(b, "wb");
        if (!fo) { fclose(fi); return mrb_int_value(mrb, -1); }
        char buf[4096]; size_t rd;
        while ((rd = fread(buf, 1, sizeof(buf), fi)) > 0) fwrite(buf, 1, rd, fo);
        fclose(fi); fclose(fo); ::remove(a);
    }
    return mrb_int_value(mrb, 0);
}
static mrb_value mkxp_file_s_mtime(mrb_state *mrb, mrb_value) {
    const char *p; mrb_get_args(mrb, "z", &p);
    char full[520];
    struct stat st;
    mrb_int secs = 0;
    if (mkxp_file_resolve(p, true, false, full, sizeof(full)) && ::stat(full, &st) == 0)
        secs = (mrb_int)st.st_mtime;
    /* Time.at se existir (o Essentials mostra a hora do save); senao Integer */
    struct RClass *tc = mrb_class_defined(mrb, "Time") ? mrb_class_get(mrb, "Time") : NULL;
    if (tc) {
        mrb_value t = mrb_funcall(mrb, mrb_obj_value(tc), "at", 1, mrb_int_value(mrb, secs));
        if (!mrb->exc) return t;
        mrb->exc = 0;
    }
    return mrb_int_value(mrb, secs);
}
static mrb_value mkxp_file_s_binread(mrb_state *mrb, mrb_value) {
    const char *p; mrb_get_args(mrb, "z", &p);
    FILE *fp = fopen(p, "rb");
    if (!fp) {
        char full[520];
        if (mkxp_file_resolve(p, true, false, full, sizeof(full)))
            fp = fopen(full, "rb");
    }
    if (!fp) mrb_raisef(mrb, E_RUNTIME_ERROR, "File.binread falhou: %s", p);
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    std::vector<char> buf((size_t)(sz > 0 ? sz : 0));
    size_t rd = (sz > 0) ? fread(buf.data(), 1, (size_t)sz, fp) : 0;
    fclose(fp);
    return mrb_str_new(mrb, buf.data(), (mrb_int)rd);
}
static mrb_value mkxp_file_s_binwrite(mrb_state *mrb, mrb_value) {
    const char *p; mrb_value s; mrb_get_args(mrb, "zS", &p, &s);
    FILE *fp = fopen(p, "wb");
    if (!fp) mrb_raisef(mrb, E_RUNTIME_ERROR, "File.binwrite falhou: %s", p);
    size_t n = fwrite(RSTRING_PTR(s), 1, (size_t)RSTRING_LEN(s), fp);
    fclose(fp);
    return mrb_int_value(mrb, (mrb_int)n);
}
static mrb_value mkxp_file_s_native(mrb_state*, mrb_value) { return mrb_true_value(); }

static const char *MKXP_SAVE_DIR = "sdmc:/mkxp/saves";
static mrb_value mkxp_system_data_dir(mrb_state *mrb, mrb_value) {
    return mrb_str_new_cstr(mrb, MKXP_SAVE_DIR);
}

static void fileBindingInit(mrb_state *mrb) {
    mkdir("sdmc:/mkxp", 0777);          /* idempotente */
    mkdir(MKXP_SAVE_DIR, 0777);
    /* [SAVE-DIR-FIX v4 2026-07-03, provado pelo SELFTEST em self-run] Os SCRIPTS
     * do jogo redefinem System.data_directory para "<game>/Save" (layout Windows)
     * DEPOIS deste init -> e' ESSA a pasta que o Auto Multi Save valida ao
     * carregar (t=4.2). Se nao existir, directory?=false -> SAVE_DIR='.' -> saves
     * na raiz do sdmc. Criamo-la ja' aqui (antes de scripts e plugins). */
    mkdir("sdmc:/mkxp/game/Save", 0777);
    /* [SAVE-DIR-FIX v2] AUTOTESTE no boot: o Auto Multi Save decide SAVE_DIR com
     * File.directory?(data_directory) no load dos plugins. Se isto der 0, os saves
     * vao parar a' RAIZ do sdmc (SAVE_DIR='.'). Loga o veredito de cada camada
     * para o proximo log dizer exatamente qual primitivo funciona neste VFS. */
    {
        extern int mkxp_native_dir_exists(const char *path);
        int ok = mkxp_native_dir_exists(MKXP_SAVE_DIR);
        DBGLOG("[FILE-IO] autoteste dir_exists('%s') = %d %s\n",
               MKXP_SAVE_DIR, ok, ok ? "(saves vao p/ a pasta certa)"
                                     : "(PROBLEMA: SAVE_DIR vai cair em '.')");
    }
    struct RClass *f = mrb_define_class(mrb, "File", mrb->object_class);
    MRB_SET_INSTANCE_TT(f, MRB_TT_DATA);
    mrb_define_class_method(mrb, f, "open",         mkxp_file_s_open,    MRB_ARGS_ARG(1,1)|MRB_ARGS_BLOCK());
    mrb_define_class_method(mrb, f, "file?",        mkxp_file_s_file,    MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, f, "directory?",   mkxp_file_s_dir,     MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, f, "exist?",       mkxp_file_s_exist,   MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, f, "exists?",      mkxp_file_s_exist,   MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, f, "size",         mkxp_file_s_size,    MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, f, "delete",       mkxp_file_s_delete,  MRB_ARGS_ANY());
    mrb_define_class_method(mrb, f, "rename",       mkxp_file_s_rename,  MRB_ARGS_REQ(2));
    mrb_define_class_method(mrb, f, "move",         mkxp_file_s_rename,  MRB_ARGS_REQ(2));
    mrb_define_class_method(mrb, f, "mtime",        mkxp_file_s_mtime,   MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, f, "binread",      mkxp_file_s_binread, MRB_ARGS_REQ(1));
    mrb_define_class_method(mrb, f, "binwrite",     mkxp_file_s_binwrite,MRB_ARGS_REQ(2));
    mrb_define_class_method(mrb, f, "mkxp_native?", mkxp_file_s_native,  MRB_ARGS_NONE());
    mrb_define_method(mrb, f, "write",   mkxp_file_write,   MRB_ARGS_REQ(1));
    mrb_define_method(mrb, f, "read",    mkxp_file_read,    MRB_ARGS_OPT(1));
    mrb_define_method(mrb, f, "eof?",    mkxp_file_eof,     MRB_ARGS_NONE());
    mrb_define_method(mrb, f, "close",   mkxp_file_close,   MRB_ARGS_NONE());
    mrb_define_method(mrb, f, "closed?", mkxp_file_closed,  MRB_ARGS_NONE());
    mrb_define_method(mrb, f, "pos",     mkxp_file_pos,     MRB_ARGS_NONE());
    mrb_define_method(mrb, f, "pos=",    mkxp_file_pos_set, MRB_ARGS_REQ(1));
    mrb_define_method(mrb, f, "seek",    mkxp_file_seek,    MRB_ARGS_ARG(1,1));
    mrb_define_method(mrb, f, "size",    mkxp_file_isize,   MRB_ARGS_NONE());
    mrb_define_method(mrb, f, "gets",    mkxp_file_gets,    MRB_ARGS_NONE());
    struct RClass *ft = mrb_define_module(mrb, "FileTest");
    mrb_define_module_function(mrb, ft, "file?",      mkxp_file_s_file,  MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, ft, "directory?", mkxp_file_s_dir,   MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, ft, "exist?",     mkxp_file_s_exist, MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, ft, "exists?",    mkxp_file_s_exist, MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, ft, "size",       mkxp_file_s_size,  MRB_ARGS_REQ(1));
    struct RClass *sys = mrb_define_module(mrb, "System");
    mrb_define_module_function(mrb, sys, "data_directory", mkxp_system_data_dir, MRB_ARGS_NONE());
    DBGLOG("[FILE-IO] File/FileTest REAIS (fopen) + System.data_directory=%s (Save/Load ativo)\n", MKXP_SAVE_DIR);
}

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

    /* [TRAINER-FULLNAME 2026-07-12] o plugin 'Scripted Battles' (e outros)
     * referencia Trainer#fullname no LOAD, mas o Trainer base (135_Trainer.rb)
     * so' tem #name -> NameError no boot ('undefined method fullname') e o
     * plugin fica meio-carregado (log: '4 ainda com erro'). O #fullname e'
     * normalmente adicionado por um script EBDX que carrega DEPOIS -> ordem
     * fragil. Aqui, ENTRE os base-scripts e os plugins (Trainer ja' existe,
     * nenhum plugin correu), definimos um #fullname robusto: "<tipo> <nome>"
     * quando ha' tipo, senao o nome cru. Se o EBDX definir o seu depois,
     * prevalece (carrega mais tarde). Generico p/ qualquer Essentials. */
    mrb_load_string(mrb,
        "begin;"
        " if Object.const_defined?(:Trainer);"
        "  class Trainer;"
        "   def fullname;"
        "    begin;"
        "     _tt=(self.trainer_type rescue nil);"
        "     _tn=(GameData::TrainerType.try_get(_tt).name rescue nil) if _tt;"
        "     (_tn && !_tn.to_s.empty?) ? (_tn.to_s+' '+self.name.to_s) : self.name.to_s;"
        "    rescue Exception; (self.name.to_s rescue ''); end;"
        "   end;"
        "  end;"
        " end;"
        "rescue Exception; end");
    if (mrb->exc) { mrb->exc = 0; }
    DBGLOG("[TRAINER-FULLNAME] Trainer#fullname garantido antes dos plugins\n");

    /* [TTF-FONTEXIST 2026-07-12] o pbTryFonts (080_MessageConfig) so' usa
     * "Power Green" se Font.exist?("Power Green") for true; o motor nao definia
     * Font.exist? -> caia sempre no fallback "arial" -> o [TTF] pedia 'arial'
     * (que nao existe) e voltava a' fonte do sistema. Definimos Font.exist? p/
     * devolver true a qualquer nome nao-vazio: o pbTryFonts passa a devolver o
     * nome REAL ("Power Green"), que chega ao @font.@name; o desenho TTF carrega
     * game/Fonts/<nome>.ttf e, se nao existir, cai no BCFNT (seguro). Escolhe
     * sempre o 1o da lista = a fonte principal pretendida, como no PC. */
    mrb_load_string(mrb,
        "begin;"
        " if Object.const_defined?(:Font);"
        "  class << Font;"
        "   def exist?(n); n.is_a?(String) && !n.empty?; end;"
        "   def exists?(n); n.is_a?(String) && !n.empty?; end;"
        "  end;"
        " end;"
        "rescue Exception; end");
    if (mrb->exc) { mrb->exc = 0; }

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
#if MKXP_DIAG_EBDX
                /* compile-only (no_exec) de cada script do plugin saltado -> captura
                 * os SyntaxErrors SEM executar (sem cascata/crash). show_error loga
                 * classe+mensagem+ficheiro. 'WARNING' no prefixo passa o filtro. */
                {
                    int _dok = 0, _derr = 0;
                    DBGLOG("[EBDX-DIAG] WARNING a compilar (compile-only, sem executar) '%s' p/ capturar erros...\n", pname);
                    for (int _si = 0; _si < nscr; _si++) {
                        mrb_value _scr = mrb_ary_entry(scripts_val, _si);
                        if (!mrb_array_p(_scr) || RARRAY_LEN(_scr) < 2) continue;
                        mrb_value _sp = mrb_ary_entry(_scr, 0);
                        mrb_value _cd = mrb_ary_entry(_scr, 1);
                        if (!mrb_string_p(_cd)) continue;
                        char _sn[128] = "ebdx_script";
                        if (mrb_string_p(_sp)) {
                            const char *_s = RSTRING_PTR(_sp); const char *_b = _s;
                            for (const char *_c = _s; *_c; _c++) if (*_c == '/' || *_c == '\\') _b = _c + 1;
                            snprintf(_sn, sizeof(_sn), "%s", _b);
                        }
                        uLongf _dl = ((int)RSTRING_LEN(_cd) > 0 ? (int)RSTRING_LEN(_cd) : 1) * 4;
                        std::vector<Bytef> _dc(_dl);
                        int _r = uncompress(_dc.data(), &_dl, (const Bytef*)RSTRING_PTR(_cd), (uLong)RSTRING_LEN(_cd));
                        while (_r == Z_BUF_ERROR) { _dl *= 2; _dc.resize(_dl); _r = uncompress(_dc.data(), &_dl, (const Bytef*)RSTRING_PTR(_cd), (uLong)RSTRING_LEN(_cd)); }
                        if (_r != Z_OK) { DBGLOG("[EBDX-DIAG] WARNING uncompress falhou '%s'\n", _sn); continue; }
                        _dc.resize(_dl);
                        mrbc_context *_cxt = mrbc_context_new(mrb);
                        mrbc_filename(mrb, _cxt, _sn);
                        _cxt->no_exec = TRUE;   /* compilar SEM executar */
                        mrb_load_nstring_cxt(mrb, (const char*)_dc.data(), _dc.size(), _cxt);
                        if (mrb->exc) {
                            char _ctx[220];
                            snprintf(_ctx, sizeof(_ctx), "EBDX-DIAG WARNING compile '%s'", _sn);
                            show_error(mrb, _ctx);
                            mrb->exc = 0;
                            _derr++;
                        } else _dok++;
                        mrbc_context_free(mrb, _cxt);
                    }
                    DBGLOG("[EBDX-DIAG] WARNING RESUMO '%s': %d compilam OK, %d com ERRO de compilacao\n", pname, _dok, _derr);
                }
#endif
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
            /* [EBDX-DEFINED-FIX 2026-07-05] RAIZ dos aliases partidos do EBDX: usa
             * o idioma `alias X_ebdx X unless defined?(X_ebdx)`, mas no mruby
             * `defined?(metodo_indefinido)` LEVANTA em vez de dar nil -> a linha
             * rebenta e o alias/override nunca instala (getFormattedText/
             * pbSpriteSetAnimFrame/pbStartOver/... partidos = batalha errada). O
             * motor ja' troca defined?(X)->nil nos scripts-base, mas NAO nos plugins
             * (o MODTS precisa de defined?). Aplicamos a troca APENAS ao EBDX. */
            if (strcmp(pname, "Elite Battle: DX") == 0 && !decomp.empty()) {
                /* [DEFINED-RESCUE] antes era replace_defined_keyword (defined?->nil),
                 * mas isso matava o guard `unless defined?(X_ebdx)` -> o PLUGIN-RETRY
                 * re-instalava o alias -> ataque/item a DUPLICAR. Agora envolve em
                 * `(defined?(X) rescue nil)`: instala na 1a passagem, mas o guard
                 * bloqueia a 2a (retry) -> idempotente, sem duplicacao. */
                wrap_defined_rescue(decomp);
            }
            patch_plugin_script(decomp, (uLong)decomp.size(), sname);
            double pl_patch_ms = prof_ticks_to_ms(prof_tick() - pl_t1);

            uint64_t pl_bytes = (uint64_t)decomp.size();
            int    pl_hit = 0;
            double pl_exec_ms = 0.0, pl_compile_ms = 0.0;
            uint64_t pl_t2 = prof_tick();   /* inicio da fase exec/compile (fora do #if p/ robustez) */
            bool executed = false;          /* fora do #if: o fallback abaixo precisa dela sempre */
            g_plugin_exec_failed = false;   /* [PLUGIN-RETRY] reset por-script */

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
                    g_plugin_exec_failed = true;   /* [PLUGIN-RETRY] */
                } else {
                    ok_scripts++;
                }
            }
            pl_compile_ms = prof_ticks_to_ms(prof_tick() - pl_t2);
            prof_script_record(sname, pl_bytes, pl_hit,
                               pl_decomp_ms, pl_patch_ms, pl_exec_ms, pl_compile_ms);
            /* [PLUGIN-RETRY] guarda o fonte dos scripts que falharam na execucao
             * (ordem-de-load: alias :M_ebdx :M com M ainda nao definido) p/ re-correr
             * no fim, quando M ja' existe. Evita duplicar Compiler (compile_all faz
             * compilacao PBS pesada) e scripts de modos opcionais. */
            if (g_plugin_exec_failed
                && !strstr(sname, "Compiler")
                && !strstr(sname, "Nuzlocke")
                && !strstr(sname, "Randomizer")) {
                g_plugin_retry.push_back(std::make_pair(std::string(sname),
                    std::string((const char*)decomp.data(), decomp.size())));
            }
        }
    }

    /* [PLUGIN-RETRY 2026-07-05] 2o PASSO: re-corre os scripts que falharam por
     * ordem-de-load, agora que TODOS os plugins carregaram (os metodos base ja'
     * existem -> os alias :M_ebdx :M passam a funcionar). Sao overrides/aliases
     * (idempotentes na maioria); os problematicos (Compiler/modos) ficam de fora. */
    if (!g_plugin_retry.empty()) {
        /* Trainer#fullname nao existe neste jogo (EBDX Scripted Battles espera-o e
         * faz alias :fullname_ebdx :fullname -> falha). Define-o = name p/ o alias
         * passar no retry (a override do EBDX corre a seguir e decora sobre este). */
        {
            int _ar = mrb_gc_arena_save(mrb);
            mrb_load_string(mrb,
                "unless (Trainer.method_defined?(:fullname) rescue true); "
                "class Trainer; def fullname; (self.name rescue '') end; end; end");
            if (mrb->exc) mrb->exc = 0;
            mrb_gc_arena_restore(mrb, _ar);
        }
        DBGLOG("[PLUGIN-RETRY] WARNING 2o passo: a re-correr %d scripts que falharam por ordem-de-load...\n",
               (int)g_plugin_retry.size());
        int _rok = 0, _rerr = 0;
        for (size_t _i = 0; _i < g_plugin_retry.size(); _i++) {
            const std::string &_nm = g_plugin_retry[_i].first;
            const std::string &_sc = g_plugin_retry[_i].second;
            int _ar = mrb_gc_arena_save(mrb);
            mrb_load_nstring(mrb, _sc.data(), _sc.size());
            if (mrb->exc) {
                /* [PLUGIN-FAIL] nomear os plugins que ficam meio-carregados p/ os
                 * corrigir um a um no proximo ciclo. So' o NOME (sem funcall na
                 * excecao — pode interferir); o show_error abaixo da' a mensagem. */
                DBGLOG("[PLUGIN-FAIL] RETRY '%s' ainda falha\n", _nm.c_str());
                char _cx[200]; snprintf(_cx, sizeof(_cx), "RETRY '%s'", _nm.c_str());
                show_error(mrb, _cx); mrb->exc = 0; _rerr++;
            } else _rok++;
            mrb_gc_arena_restore(mrb, _ar);
        }
        DBGLOG("[PLUGIN-RETRY] WARNING 2o passo concluido: %d OK, %d ainda com erro\n", _rok, _rerr);
        g_plugin_retry.clear();
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

	/* PROVA C-SIDE: qual compat_stubs.h o compilador realmente usou. A guarda
	 * #ifndef garante que, se o binding for compilado contra um compat_stubs.h
	 * ANTIGO (sem a tag), imprime 'DESCONHECIDO' em vez de partir o build. Como
	 * o binding_3ds.cpp recompila sempre, esta linha diz a verdade no log mesmo
	 * que o bloco Ruby do compat_stubs nem chegue a correr. */
#ifndef MKXP_COMPAT_STUBS_BUILD_TAG
#define MKXP_COMPAT_STUBS_BUILD_TAG "DESCONHECIDO (compat_stubs.h antigo, sem tag)"
#endif
	DBGLOG("[COMPAT_STUBS|C] header compilado tag=%s\n", MKXP_COMPAT_STUBS_BUILD_TAG);
	if (g_dbglog) fflush(g_dbglog);  /* flush imediato: esta prova nao se perde num crash */

	extern const char *COMPAT_STUBS_RUBY;  // declarado em compat_stubs.h
	mrb_load_string(mrb, COMPAT_STUBS_RUBY);
	if (mrb->exc) {
		show_error(mrb, "compat_stubs");
		mrb->exc = 0;
	}
	/* [OLDRAND-FIX] O motor ja' define oldRand (R maiusculo) mas SO' pos-scripts
	 * (check_entry_methods, tarde demais). Varios plugins do Essentials chamam
	 * oldrand (minusculo) JA' durante o load dos Scripts.rxdata -> NoMethodError
	 * no boot (t~3.2s, "Marin's Scripting Utilities"). Definimos AQUI, ANTES do
	 * loop de Scripts.rxdata, oldrand minusculo (Kernel module_function + global),
	 * delegando ao __native_rand__ (ja' registado em C neste ponto). Se um plugin
	 * fizer 'alias oldrand rand' por cima, sobrepoe-se sem problema. Tudo em
	 * begin/rescue p/ nunca partir o boot. */
	mrb_load_string(mrb,
	    "begin\n"
	    "  module Kernel\n"
	    "    def oldrand(*a)\n"
	    "      a.empty? ? __native_rand__ : __native_rand__(a[0])\n"
	    "    end\n"
	    "    module_function :oldrand\n"
	    "  end\n"
	    "  def oldrand(*a)\n"
	    "    a.empty? ? __native_rand__ : __native_rand__(a[0])\n"
	    "  end\n"
	    "rescue => e\n"
	    "end\n");
	if (mrb->exc) { mrb->exc = 0; }
	DBGLOG("[OLDRAND-FIX] oldrand (minusculo) definido antes dos Scripts.rxdata\n");
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
  # ---- Module#*_method_defined? (mruby 3.2.0 nao os implementa) -------------
  # CAUSA (teu log linha 117): durante o load do 'Voltseon's Pause Menu' um
  # plugin chama X.private_method_defined?(:m) p/ decidir se faz alias; mruby
  # nao tem esse metodo -> cai no method_missing universal -> NoMethodError
  # (nao fatal, mas suja o log e pode desligar uma feature do plugin).
  # Definimos com semantica aproximada via *_instance_methods (que mruby TEM).
  # Guardas por-metodo: so' definimos os que faltam.
  class ::Module
    unless method_defined?(:private_method_defined?)
      def private_method_defined?(name)
        s = (name.to_sym rescue name)
        a = (private_instance_methods(true) rescue nil)
        a.is_a?(Array) ? a.include?(s) : false
      end
    end
    unless method_defined?(:protected_method_defined?)
      def protected_method_defined?(name)
        s = (name.to_sym rescue name)
        a = (protected_instance_methods(true) rescue nil)
        a.is_a?(Array) ? a.include?(s) : false
      end
    end
    unless method_defined?(:public_method_defined?)
      def public_method_defined?(name)
        s = (name.to_sym rescue name)
        a = (public_instance_methods(true) rescue nil)
        if a.is_a?(Array) then a.include?(s) else (method_defined?(s) rescue false) end
      end
    end
  end
  # ---- Dir.delete CEDO (antes dos plugins) ----------------------------------
  # O PRE-FIX (t=0.05s) tenta definir Dir.delete, mas o Dir so' nasce no
  # compat_stubs (t=0.314s), logo o 'class << Dir' do PRE-FIX falha em silencio.
  # O bloco equivalente no literal RUBY carrega tarde (~t=3.8s) e usa respond_to?
  # (que o method_missing universal estraga). Resultado: o plugin do Marin chama
  # Dir.delete a t=3.251s e rebenta com NameError (nao fatal, mas suja o log).
  # Aqui (DEFEARLY, t=2.738s) o Dir JA' existe e ainda nao corremos plugins.
  # Usamos method_defined? (ve' metodos REAIS; nao dispara method_missing).
  if Object.const_defined?(:Dir)
    class << Dir
      def delete(*a); 0; end unless method_defined?(:delete)   # no-op seguro
      alias_method :rmdir,  :delete rescue nil
      alias_method :unlink, :delete rescue nil
    end
  end
rescue
end
)DEFEARLY";
        mrb_load_string(mrb, defined_early);
        if (mrb->exc) { show_error(mrb, "defined_early"); mrb->exc = 0; }
        else DBGLOG("[DEFINED-EARLY] defined?/const_missing instalados ANTES dos plugins\n");
    }

    /* [COMPILER-STUB 2026-07-10, GAP #4] o script base Compiler esta' SKIPADO (pesado,
     * dados ja' pre-compilados) mas os plugins 0001/0060 (EBDX) fazem
     * `alias compile_all_old compile_all` no LOAD -> NameError -> o plugin inteiro
     * falha. Modulo minimo ANTES dos plugins: compile_all/main no-op +
     * needCompiling?=false (nunca recompila; os .dat de 2026-06-12 sao a verdade). */
    {
        static const char *compiler_stub = R"CSTUB(
begin
  unless defined?(Compiler) && Compiler.respond_to?(:compile_all)
    module Compiler
      def self.compile_all(*a); end
      def self.main(*a); end
      def self.needCompiling?(*a); false; end
      def self.compile_pbs_files(*a); end
    end
  end
rescue; end
)CSTUB";
        mrb_load_string(mrb, compiler_stub);
        if (mrb->exc) mrb->exc = 0;
        DBGLOG("[COMPILER-STUB] Compiler minimo definido ANTES dos plugins (alias 0001/0060 passam)\n");
    }

    run_plugin_scripts(mrb, game_path);

    /* ── CALLMENU-DIAG: wrapper de backtrace INNER a' sonda ───────────────────
     * O menu de pausa rebenta com ArgumentError "given 1, expected 2" num metodo
     * fundo do Voltseon Pause Menu que NAO consegui identificar estaticamente
     * (call_menu/pbShowCommands/startComponent/initialize batem todos certo).
     * A sonda (MENUGUARD) apanha o erro mas a versao em uso nao loga backtrace.
     * Instalo este wrapper ANTES da sonda -> fica INNER -> apanha o erro primeiro,
     * loga bt[0..11] (bt[0] = a chamada exata com aridade errada) e re-lanca para
     * a sonda recuperar na mesma. Assim o proximo log da o ficheiro:linha exato.
     * Idempotente; nao altera comportamento (a sonda continua a tratar). */
    mrb_load_string(mrb, R"CMDIAG(
begin
  if defined?(Scene_Map) && Scene_Map.method_defined?(:call_menu) && !$__mkxp_callmenu_diag
    $__mkxp_callmenu_diag = true
    class Scene_Map
      alias_method :__mkxp_diag_orig_call_menu, :call_menu
      def call_menu(*a, &b)
        begin
          __mkxp_diag_orig_call_menu(*a, &b)
        rescue StandardError => __cme
          (MKXPDebug.log("[CALLMENU-DIAG] #{__cme.class}: #{__cme.message}") rescue nil)
          __bt = (__cme.backtrace || [])
          if __bt.empty?
            (MKXPDebug.log("[CALLMENU-DIAG]   (sem backtrace)") rescue nil)
          else
            __bt[0, 12].each_with_index { |__ln, __i| (MKXPDebug.log("[CALLMENU-DIAG]   bt[#{__i}] #{__ln}") rescue nil) }
          end
          raise
        end
      end
    end
    (MKXPDebug.log("[CALLMENU-DIAG] wrapper de backtrace instalado (inner a' sonda)") rescue nil)
  end
rescue => __cmei
  (MKXPDebug.log("[CALLMENU-DIAG] instalacao falhou: #{__cmei.class}: #{__cmei.message}") rescue nil)
end
)CMDIAG");
    if (mrb->exc) { mrb->exc = 0; }

    /* [TILEBMP-FIX] (2026-07-01) REMOVIDO: era um duplicado morto. O log 08:39 provou
     * que o dono FINAL de pbGetTileBitmap e' o bloco [TILEBMP] do proprio motor
     * (check_entry_methods, corre DEPOIS do probe) que sobrepunha esta injecao.
     * O fix real (usar o tileset residente via pbGetTileset) foi feito LA' ([TILEBMP v2],
     * ~linha 1920) em vez de manter dois sistemas paralelos. */

    /* ── [MENU-FLUSH] mata o auto-Bag: flush de flancos a' entrada de call_menu ──
     * RAIZ (provada: [USE-TRIG] f=5517 press=0 held=0 was_held=0 held_any=1 na
     * abertura do menu): o menu de pausa Voltseon abre IN-PLACE (sem Graphics.freeze),
     * por isso o flush automatico de flancos (que so' corre em grph_freeze) nao limpa
     * um held_any STALE de USE deixado por um toque anterior. A 1a leitura de
     * trigger?(USE) do menu apanha esse fantasma -> abre a Bag sozinha -> loop de
     * reabertura -> "menu congela". FIX: envolver Scene_Map#call_menu e chamar
     * Input.flush_edges (motor) a' ENTRADA -> o menu comeca sem flancos pendentes
     * (semantica RGSS correcta: e' preciso premir DE NOVO p/ selecionar). Injetado
     * pelo MOTOR (nao pela sonda); idempotente ($__mkxp_menuflush). */
    mrb_load_string(mrb, R"MENUFLUSH(
begin
  if defined?(Scene_Map) && Scene_Map.method_defined?(:call_menu) && !$__mkxp_menuflush
    $__mkxp_menuflush = true
    class Scene_Map
      alias_method :__mkxp_orig_call_menu_flush, :call_menu
      def call_menu(*a, &b)
        (Input.begin_menu_diag(120) rescue nil)   # [MENUDIAG] Bug 4: captura input CRU a' abertura (120 frames)
        (Input.flush_edges rescue nil) if $MKXP_MENU_FLUSH == true   # fix DESLIGADO por defeito (raiz nao provada)
        __mkxp_orig_call_menu_flush(*a, &b)
      end
    end
    (MKXPDebug.log("[MENU-FLUSH] call_menu envolvido: [MENUDIAG] arma sempre; flush_edges SO' se $MKXP_MENU_FLUSH==true (default OFF ate' raiz provada)") rescue nil)
  end
rescue => __mfe
  (MKXPDebug.log("[MENU-FLUSH] instalacao falhou: #{__mfe.class}: #{__mfe.message}") rescue nil)
end

# ── [BAGTRACE] Bug 4 (auto-Bag): identificar QUEM abre a Bag ──────────────────
# PROVADO no log 08:39: a Bag abre (PokemonBag_Scene#pbStartScene t=355.516) SEM
# NENHUMA leitura de Input.trigger? (o [MENUDIAG] nativo loga TODAS; a 1a leitura
# so' vem DEPOIS, f=7346) -> NAO e' input nem fantasma: alguem chama pbStartScreen
# programaticamente (e re-chama a cada ~0.87s p/ sempre = o "congelado"). O proprio
# probe nunca identificou o caller (FIX-BAGREOPEN bloqueia as CHAMADAS SEGUINTES no
# pbStartScreen "catch-all" sem saber a origem). Estes wrappers logam o BACKTRACE
# (tecnica raise/rescue ja' provada pelo CALLMENU-DIAG) na 1a-6a chamada de cada um
# -> o proximo log da' o ficheiro:linha exato do caller. So' diagnostico; zero
# mudanca de comportamento; caps baixos.
begin
  [[:PokemonBag_Scene, :pbStartScene, "SCENE"],
   [:PokemonBagScreen, :pbStartScreen, "SCREEN"],
   [:MenuEntryBag,     :selected,      "ENTRY"]].each do |klass_sym, meth, tag|
    next unless (Object.const_defined?(klass_sym) rescue false)
    _k = Object.const_get(klass_sym)
    next unless (_k.method_defined?(meth) rescue false)
    _orig = "__bagtrace_orig_#{meth}".to_sym
    next if (_k.method_defined?(_orig) rescue false)
    _k.class_eval do
      alias_method _orig, meth
      define_method(meth) do |*a, &b|
        begin
          if ($__bagtrace_n ||= 0) < 18
            $__bagtrace_n += 1
            _bt = []
            begin; raise RuntimeError, "bagtrace"; rescue => _bte; _bt = (_bte.backtrace || []); end
            (MKXPDebug.log("[BAGTRACE-#{tag}] #{klass_sym}##{meth} chamado (##{$__bagtrace_n})") rescue nil)
            _bt[1, 8].each_with_index { |_ln, _i| (MKXPDebug.log("[BAGTRACE-#{tag}]   bt[#{_i}] #{_ln}") rescue nil) }
          end
        rescue; end
        send(_orig, *a, &b)
      end
    end
  end
  (MKXPDebug.log("[BAGTRACE] wrappers instalados (Scene/Screen/Entry) -- proximo log da' o caller da auto-Bag") rescue nil)
rescue => __bte2
  (MKXPDebug.log("[BAGTRACE] instalacao falhou: #{__bte2.class}: #{__bte2.message}") rescue nil)
end
)MENUFLUSH");
    if (mrb->exc) { mrb->exc = 0; }

    /* ── VWrap (pokemon_tilemap_fix.rb): corrige o "QUARTO PRETO" ──────────────
     * CAUSA RAIZ (provada: banner VWRAP=false): tilesets de interiores 256x1280
     * (>1024 do PICA200) nunca eram embrulhados -> create_texture falha -> chao
     * preto. O VWrap reparte-os em colunas de 1024 (sem remapear ids, ao
     * contrario do TSC que so trata gigantes h>8192). O ficheiro existia em
     * source/ mas NUNCA era carregado por ninguem. Carregar AQUI: depois dos
     * scripts do jogo (CustomTilemap ja existe) e ANTES do debug_probe, para o
     * TSC encadear por cima (giant->TSC; resto->__tsc_orig_tileset_eq->VWrap),
     * sem partir os mapas gigantes que ja funcionavam. sdmc tem prioridade
     * (iteracao); romfs e' o baked-in. Confirmar no log: banner VWRAP=true e
     * [TMDBG] @tileset=512x1024 nos interiores. Desactivar: renomear o ficheiro. */
    {
        /* [MKXP-EARLY 2026-07-05] RAIZ-MÃE provada por sonda: o guard do fix
         * ("if $MKXP == true") era avaliado AQUI (t=4.31s) mas o $MKXP so' era
         * forcado no check_entry (t=4.54s) -> o patch da classe NUNCA aplicou
         * em sessao nenhuma (o banner de load saia na mesma). Forcar ANTES. */
        mrb_gv_set(mrb, mrb_intern_lit(mrb, "$MKXP"), mrb_true_value());
        DBGLOG("[MKXP-EARLY] $MKXP=true forcado ANTES do tilemap_fix (build 2026-07-05b)\n");
        FILE *vf = fopen("sdmc:/mkxp/pokemon_tilemap_fix.rb", "rb");
        if (!vf) vf = fopen("romfs:/pokemon_tilemap_fix.rb", "rb");
        if (vf) {
            fseek(vf, 0, SEEK_END); long vsz = ftell(vf); fseek(vf, 0, SEEK_SET);
            char *vbuf = (char*)malloc(vsz + 1);
            fread(vbuf, 1, vsz, vf); fclose(vf); vbuf[vsz] = 0;
            mrb_load_string(mrb, vbuf); free(vbuf);
            if (mrb->exc) { show_error(mrb, "pokemon_tilemap_fix"); mrb->exc = 0; }
            else DBGLOG("[VWRAP] pokemon_tilemap_fix.rb carregado (VWrap instalado)\n");
        } else {
            DBGLOG("[VWRAP] pokemon_tilemap_fix.rb nao encontrado -- quarto preto continua\n");
        }
    }

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
        /* Fixes do porting (render/tilemap, menu, input, perf, etc.) EMBEBIDOS no
         * binario via romfs -- mesmo mecanismo do rgss_stubs.rb / ios_compat_3ds.rb.
         * Assim os fixes nao dependem de um ficheiro externo em sdmc: ficam compilados
         * no .3dsx/.cia. O sdmc continua a ter prioridade (override para debug/iteracao);
         * se nao existir em sdmc, carrega o do romfs (a versao baked-in). */
        if (!sf) sf = fopen("romfs:/debug_probe.rb", "rb");
        if (sf) {
            fseek(sf, 0, SEEK_END); long ssz = ftell(sf); fseek(sf, 0, SEEK_SET);
            char *sbuf = (char*)malloc(ssz + 1);
            fread(sbuf, 1, ssz, sf); fclose(sf); sbuf[ssz] = 0;
            DBGLOG("[PROBE] WARNING a carregar debug_probe.rb (%ld bytes) primeiro_char=0x%02x\n",
                   ssz, (unsigned char)sbuf[0]);
            mrb_load_string(mrb, sbuf); free(sbuf);
            if (mrb->exc) { show_error(mrb, "debug_probe"); mrb->exc = 0; }
            else DBGLOG("[PROBE] WARNING debug_probe.rb carregado OK\n");
            if (g_dbglog) fflush(g_dbglog);
        } else {
            DBGLOG("[PROBE] WARNING debug_probe.rb NAO ENCONTRADO -- modo normal\n");
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

    /* ── MENUCOMP-GUARD: blindar refresh dos componentes do Voltseon Pause Menu ──
     * O log mostra call_menu a rebentar SEMPRE com
     *   TypeError: Object cannot be converted to Integer
     * dentro de pbRefresh -> @components.each{|c| c.refresh}. Um dos HUD
     * (SafariHud/BugContestHud/PokemonPartyHud/DateAndTimeHud/NewQuestHud) lanca
     * a excecao; o MENUGUARD (debug_probe) apanha mas ABORTA o menu inteiro ->
     * "nao aparece nada" + fundo laranja. Aqui envolvemos o refresh de cada
     * componente: se um falhar e' IGNORADO e LOGADO (com classe+backtrace, p/
     * sabermos exatamente qual e onde), e o resto do menu desenha na mesma.
     * Corre depois dos plugins+probe (classes e MKXPDebug ja' existem).
     * Idempotente (guarda __mkxp_orig_refresh_guard). */
    {
        static const char *menucomp_guard = R"MENUCOMP(
begin
  %w[SafariHud BugContestHud PokemonPartyHud DateAndTimeHud NewQuestHud].each do |__cn|
    next unless Object.const_defined?(__cn.to_sym)
    __k = Object.const_get(__cn.to_sym)
    next unless __k.is_a?(Class) && __k.method_defined?(:refresh)
    next if __k.method_defined?(:__mkxp_orig_refresh_guard)
    __k.send(:alias_method, :__mkxp_orig_refresh_guard, :refresh)
    __k.send(:define_method, :refresh) do |*__a, &__b|
      begin
        __mkxp_orig_refresh_guard(*__a, &__b)
      rescue => __re
        $__mkxp_menucomp_warned ||= {}
        unless $__mkxp_menucomp_warned[self.class.name]
          $__mkxp_menucomp_warned[self.class.name] = true
          (MKXPDebug.log("[MENUCOMP] #{self.class}#refresh rebentou: #{__re.class}: #{__re.message} -> componente ignorado, menu continua") rescue nil)
          (__re.backtrace || [])[0,8].each_with_index { |__l,__i| (MKXPDebug.log("[MENUCOMP]   bt[#{__i}] #{__l}") rescue nil) }
        end
      end
    end
  end
  (MKXPDebug.log("[MENUCOMP] guard de refresh instalado nos componentes do pause menu") rescue nil)
rescue => __mce
  (MKXPDebug.log("[MENUCOMP] instalacao falhou: #{__mce.class}: #{__mce.message}") rescue nil)
end
)MENUCOMP";
        mrb_load_string(mrb, menucomp_guard);
        if (mrb->exc) { mrb->exc = 0; }
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

extern "C" unsigned mkxp_strip_battle_giants(void);   /* [BATTLE-GIANTS] def em bitmap_binding_3ds.cpp */
extern "C" void mkxp_set_in_battle(int);              /* [BATTLE-ARENA] idem */

int binding_3ds_run(const char *game_path) {
    dbglog_open();
    DBGLOG("binding_3ds_run: %s\n", game_path);
    /* [MEMREGION 2026-07-12] Prova direta do ganho do CIA. Sob HBL/.3dsx o pool
     * APPLICATION e' ~64MB (teto artificial do Homebrew Launcher); sob CIA com
     * SystemMode:96MB + ResourceLimitCategory:Application passa a ~96MB (Old 3DS)
     * / ~124MB (New 3DS). fprintf direto p/ saltar o boot-lean (aparece SEMPRE). */
    if (g_dbglog) {
        u32 _app_tot  = osGetMemRegionSize(MEMREGION_APPLICATION);
        u32 _app_used = osGetMemRegionUsed(MEMREGION_APPLICATION);
        fprintf(g_dbglog, "[MEMREGION] APPLICATION total=%luKB (~%luMB) used=%luKB free=%luKB  <- 64MB=.3dsx/HBL, 96MB+=CIA\n",
                (unsigned long)(_app_tot/1024), (unsigned long)(_app_tot/1048576),
                (unsigned long)(_app_used/1024), (unsigned long)((_app_tot-_app_used)/1024));
        fflush(g_dbglog);
    }
    /* Resultado do ndspInit() (feito em main.cpp, antes deste log abrir). Se for
     * R_FAILED, NAO ha som -- o DSP nao processa (no log: "0 buffers reciclados"
     * + canais de SE sempre cheios). Causa quase sempre: falta o dspfirm.cdc. */
    {
        extern s32 g_ndsp_init_result;
        if (g_ndsp_init_result < 0)
            DBGLOG("[AUDIO][FALHA] ndspInit rc=0x%08lX -> SEM SOM (falta firmware do DSP / audio off no emulador)\n",
                   (unsigned long)g_ndsp_init_result);
        else
            DBGLOG("[AUDIO] ndspInit rc=0x%08lX OK (DSP disponivel -- se nao ha som, ver volume/output do emulador)\n",
                   (unsigned long)g_ndsp_init_result);
    }
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
    g_mkxp_mrb = mrb;   /* expor o mrb ao bitmap loader (GC-on-OOM dos tilesets) */

    /* O marshal tem um cache de resolucao de classe (nome -> RClass*) que acelera
     * o load de TODOS os ficheiros .rxdata/.dat em ~150x. Esses ponteiros so' sao
     * validos para ESTA sessao mruby; limpar o cache agora garante que uma nova
     * sessao (reiniciar/trocar de jogo) nunca reutiliza ponteiros antigos. */
    /* (marshal_class_cache_reset + $__mkxp_game_base movidos para DEPOIS do #endif,
     *  para correrem em AMBOS os caminhos -- pool on/off.) */
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
    g_mkxp_mrb = mrb;   /* garantir o global em ambos os caminhos (pool on/off) */

    /* ── SETUP COMUM A AMBOS OS CAMINHOS (pool on/off) ─────────────────────────
     * ANTES isto estava SO' no ramo da pool. Com USE_POOL_ALLOCATOR=0 (pool
     * desligada p/ Azahar -- liberta ~28MB) o $__mkxp_game_base ficava por
     * definir e NENHUMA imagem/windowskin resolvia. Agora corre sempre. */
    marshal_class_cache_reset();   /* cache de classes do marshal (load .rxdata ~150x) */
    {
        mrb_value gb = mrb_str_new_cstr(mrb, g_game_base);
        mrb_gv_set(mrb, mrb_intern_lit(mrb, "$__mkxp_game_base"), gb);
        DBGLOG("[WSKIN] $__mkxp_game_base = '%s' (p/ resolver imagens por caminho completo)\n", g_game_base);
    }


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
    fileBindingInit(mrb);    /* [FILE-IO] File real ANTES do rgss_stubs (que agora respeita o nativo) */
    /* [SAVE-0B-FIX v4 2026-07-03 — A RAIZ FINAL] marshalBindingInit estava definido
     * (marshal.cpp:1004) mas NUNCA era chamado: o Marshal.dump/load NATIVOS nunca
     * existiram no Ruby — so' os stubs (""/nil) dos compat/rgss_stubs. TODOS os
     * saves de 0 bytes vieram daqui. (load_data sempre funcionou porque vai direto
     * ao C por outra funcao — por isso nunca se notou.) */
    marshalBindingInit(mrb);
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
    /* __mkxp_ms: tempo real em ms (float) via relogio ARM11 (svcGetSystemTick).
     * Exposto ao Ruby para PROFILING GRANULAR do update: o nosso log mostra
     * update=157ms render=1.2ms (Ruby domina), mas nao QUAL parte do update.
     * Com isto, os patches Ruby podem medir cada seccao (player, eventos,
     * spritesets, tint) e isolar o gargalo real em vez de adivinhar. Custo: uma
     * leitura de registo do sistema (~ns). So' e' chamado pelos patches de
     * diagnostico, que logam periodicamente. */
    mrb_define_method(mrb, mrb->kernel_module,
        "__mkxp_ms",
        [](mrb_state *mrb2, mrb_value) -> mrb_value {
            (void)mrb2;
            return mrb_float_value(mrb2, tick_to_ms(svcGetSystemTick()));
        },
        MRB_ARGS_NONE());

    /* [GC-LIVE 2026-07-11] expõe mrb->gc.live ao Ruby p/ o probe tracer isolar a
     * FONTE do churn de 1.5M objetos por batalha (todas as hipoteses estruturais
     * — marshal, frames do battler — foram empiricamente refutadas). Um hook
     * mede o delta em volta de metodos-chave da batalha e nomeia o culpado. */
    mrb_define_method(mrb, mrb->kernel_module,
        "__mkxp_gc_live",
        [](mrb_state *mrb2, mrb_value) -> mrb_value {
            return mrb_fixnum_value((mrb_int)mrb2->gc.live);
        },
        MRB_ARGS_NONE());

    /* [BATTLE-GIANTS] o probe chama isto na entrada da batalha p/ libertar os
     * ~16MB dos tilesets gigantes do overworld (escondido) para a cena de
     * batalha. Recarregam a' saida. Devolve KB libertados. */
    mrb_define_method(mrb, mrb->kernel_module,
        "__mkxp_strip_battle_giants",
        [](mrb_state *mrb2, mrb_value) -> mrb_value {
            return mrb_fixnum_value((mrb_int)mkxp_strip_battle_giants());
        },
        MRB_ARGS_NONE());

    /* [BATTLE-ARENA] o probe marca ENTRADA (true, na batalha) e o tick do mapa
     * marca SAIDA (false). Enquanto true, a arena de 16MB livre pode servir o
     * battlebg/assets de batalha que senao ficavam magenta (overworld por tras). */
    mrb_define_method(mrb, mrb->kernel_module,
        "__mkxp_set_battle",
        [](mrb_state *mrb2, mrb_value) -> mrb_value {
            mrb_bool v = 0; mrb_get_args(mrb2, "b", &v);
            mkxp_set_in_battle(v ? 1 : 0);
            return mrb_nil_value();
        },
        MRB_ARGS_REQ(1));

    /* __mkxp_localtime: data/hora REAL do RTC do 3DS via time()/localtime().
     * CAUSA RAIZ (provada por nm libmruby.a -> 0 simbolos de Time): este build do
     * mruby NAO tem o gem mruby-time, logo a constante Time cai no const_missing
     * e devolve MKXP_UNDEF. O jogo faz Time.now.month.to_i -> MKXP_UNDEF e depois
     * pbGetAbbrevMonthName indexa um array com isso -> "Object cannot be converted
     * to Integer" (o crash do menu de pausa = DateAndTimeHud). Tambem parte o
     * ciclo dia/noite e tudo o que use data/hora.
     * Devolve [ano, mes(1-12), dia, hora, min, seg, wday, yday, epoch]. Aceita um
     * epoch opcional (p/ Time.at). A classe Time (Ruby) e' construida sobre isto. */
    mrb_define_method(mrb, mrb->kernel_module,
        "__mkxp_localtime",
        [](mrb_state *mrb2, mrb_value) -> mrb_value {
            mrb_int epoch_arg = -1;
            mrb_get_args(mrb2, "|i", &epoch_arg);
            time_t t = (epoch_arg >= 0) ? (time_t)epoch_arg : time(NULL);
            struct tm *lt = localtime(&t);
            mrb_value a = mrb_ary_new_capa(mrb2, 9);
            if (lt) {
                mrb_ary_push(mrb2, a, mrb_fixnum_value(lt->tm_year + 1900));
                mrb_ary_push(mrb2, a, mrb_fixnum_value(lt->tm_mon + 1));
                mrb_ary_push(mrb2, a, mrb_fixnum_value(lt->tm_mday));
                mrb_ary_push(mrb2, a, mrb_fixnum_value(lt->tm_hour));
                mrb_ary_push(mrb2, a, mrb_fixnum_value(lt->tm_min));
                mrb_ary_push(mrb2, a, mrb_fixnum_value(lt->tm_sec));
                mrb_ary_push(mrb2, a, mrb_fixnum_value(lt->tm_wday));
                mrb_ary_push(mrb2, a, mrb_fixnum_value(lt->tm_yday + 1));
                mrb_ary_push(mrb2, a, mrb_fixnum_value((mrb_int)t));
            } else {
                /* fallback (RTC indisponivel): epoch 0 = 1970-01-01, nao rebenta */
                mrb_ary_push(mrb2, a, mrb_fixnum_value(1970));
                for (int i = 0; i < 4; i++) mrb_ary_push(mrb2, a, mrb_fixnum_value(i == 0 ? 1 : (i == 1 ? 1 : 0)));
                mrb_ary_push(mrb2, a, mrb_fixnum_value(0));
                mrb_ary_push(mrb2, a, mrb_fixnum_value(4));
                mrb_ary_push(mrb2, a, mrb_fixnum_value(1));
                mrb_ary_push(mrb2, a, mrb_fixnum_value(0));
            }
            return a;
        },
        MRB_ARGS_OPT(1));

    /* Time (Ruby) construido sobre __mkxp_localtime. So' definido se a constante
     * Time faltar (este build nao tem mruby-time). const_defined? ve a tabela real
     * (NAO dispara o const_missing/MKXP_UNDEF), por isso o guard e' fiavel.
     * Corre antes de run_rmxp_scripts -> os scripts do jogo veem o Time. */
    {
        static const char *time_class_ruby = R"TIMECLS(
unless Object.const_defined?(:Time)
  class ::Time
    include Comparable
    attr_reader :year, :month, :day, :hour, :min, :sec, :wday, :yday
    def initialize(arr = nil)
      a = arr
      a = (__mkxp_localtime rescue nil) unless a.is_a?(Array)
      a = [1970, 1, 1, 0, 0, 0, 4, 1, 0] unless a.is_a?(Array) && a.size >= 9
      @year = a[0]; @month = a[1]; @day = a[2]; @hour = a[3]; @min = a[4]
      @sec = a[5]; @wday = a[6]; @yday = a[7]; @epoch = a[8]
    end
    def self.now; new; end
    def self.at(t); new((__mkxp_localtime(t.to_i) rescue nil)); end
    def to_i; @epoch.to_i; end
    def to_f; @epoch.to_f; end
    def mon; @month; end
    def mday; @day; end
    def <=>(o); to_i <=> (o.respond_to?(:to_i) ? o.to_i : o); end
    def -(o); to_i - (o.respond_to?(:to_i) ? o.to_i : o.to_i); end
    def +(o); Time.at(to_i + o.to_i); end
    def strftime(fmt)
      pad2 = lambda { |n| n = n.to_i; n < 10 ? "0#{n}" : n.to_s }
      h12 = @hour % 12; h12 = 12 if h12 == 0
      ampm = @hour < 12 ? "AM" : "PM"
      s = fmt.to_s.dup
      # [TIMEFIX-LOCALE 2026-07-11] %x/%X/%c saiam LITERAIS na caixa do save
      # slot (Auto_Multi_Save 0102:504). Expandir ANTES dos simples.
      s = s.gsub("%c", "%d/%m/%Y %H:%M:%S")
      s = s.gsub("%x", "%d/%m/%Y")
      s = s.gsub("%X", "%H:%M:%S")
      s = s.gsub("%Y", @year.to_s)
      s = s.gsub("%m", pad2.call(@month))
      s = s.gsub("%d", pad2.call(@day))
      s = s.gsub("%H", pad2.call(@hour))
      s = s.gsub("%M", pad2.call(@min))
      s = s.gsub("%S", pad2.call(@sec))
      s = s.gsub("%I", pad2.call(h12))
      s = s.gsub("%p", ampm)
      s
    end
    def to_s; strftime("%Y-%m-%d %H:%M:%S"); end
    def inspect; to_s; end
  end
  (MKXPDebug.log("[TIMEFIX] Time definido (mruby-time ausente) via RTC -> data/hora do menu + ciclo dia/noite") rescue nil)
end
)TIMECLS";
        mrb_load_string(mrb, time_class_ruby);
        if (mrb->exc) { DBGLOG("[TIMEFIX] ERRO ao definir Time\n"); mrb->exc = 0; }
        else { DBGLOG("[TIMEFIX] classe Time instalada (RTC do 3DS)\n"); }
    }

    /* DEBUG LOG CERTEIRO: contador CUMULATIVO de objetos/buffers NOVOS alocados
     * pelo allocator do mruby (= o "objs_criados" do PROFILE). Os patches Ruby
     * leem isto antes/depois de uma operacao para saber QUANTOS objetos ela
     * alocou -- e assim atribuir o pico de +125K do dialogo a' carga do mapa vs
     * ao render da mensagem (em vez de adivinhar). Float p/ nao haver overflow. */
    mrb_define_method(mrb, mrb->kernel_module,
        "__mkxp_objs",
        [](mrb_state *mrb2, mrb_value) -> mrb_value {
            /* g_prof_objs_created ja' esta' em scope (mesmo header que a linha do
             * g_prof_objs_created++ no allocator). Nao re-declarar -> sem risco de
             * conflito de tipo; o cast p/ mrb_float aceita qualquer tipo inteiro. */
            return mrb_float_value(mrb2, (mrb_float)g_prof_objs_created);
        },
        MRB_ARGS_NONE());

    /* DEBUG LOG CERTEIRO: contador CUMULATIVO de chamadas aos setters C++ de
     * Sprite (x=/y=/z=/bitmap=/src_rect=/opacity=/...). Decide o fix CEGO D
     * (batch-setter): se forem ~90/frame (15 x 6 sprites), um setter em lote
     * corta ~85 dispatches/frame; se forem poucas, o custo esta' no CORPO do
     * update Ruby, nao no dispatch, e o batch-setter nao ajudaria. */
    mrb_define_method(mrb, mrb->kernel_module,
        "__mkxp_setters",
        [](mrb_state *mrb2, mrb_value) -> mrb_value {
            extern unsigned long long g_spr_setter_calls;
            return mrb_float_value(mrb2, (mrb_float)g_spr_setter_calls);
        },
        MRB_ARGS_NONE());

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

            /* [LOG-LEAN] req= por-ficheiro removido (ruido de boot; flush SD) */

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
                        /* [LOG-LEAN] HIT silencioso (era 1 linha/ficheiro) */
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
                            /* [LOG-LEAN] load-OK silencioso */
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
            { long pz; fseek(fp,0,SEEK_END); pz=ftell(fp); fseek(fp,0,SEEK_SET); (void)pz; (void)src;
              /* [LOG-LEAN] "aberto" por-ficheiro removido */ }

            mrb_value v = mrb_nil_value();
            /* [BIGLOAD 2026-07-11] o log 22:33 mostrou 2x GC de ~1.5M objetos
             * orfaos POR BATALHA — churn catastrofico de fonte NAO confirmada.
             * Medir gc.live ANTES/DEPOIS de cada load_data isola definitivamente
             * qual ficheiro cria a explosao (hipotese: PkmnAnimations.rxdata).
             * So' loga quando o delta e' enorme (>100k) -> zero ruido normal. */
            int _bl_live0 = g_mkxp_mrb ? (int)g_mkxp_mrb->gc.live : 0;
            try {
                v = marshalLoadInt(mrb2, fp);
            } catch (...) {
                fclose(fp);
                DBGLOG("[load_data] ERRO: marshal falhou em '%s' (ver [marshal] acima)\n", path);
                mrb_raise(mrb2, mrb_class_get(mrb2, "RuntimeError"),
                          "load_data: marshal error");
            }
            fclose(fp);
            if (g_dbglog && g_mkxp_mrb) {
                int _bl_d = (int)g_mkxp_mrb->gc.live - _bl_live0;
                if (_bl_d > 100000) {
                    const char *_shape = mrb_array_p(v) ? "Array" : (mrb_hash_p(v) ? "Hash" :
                                        (mrb_nil_p(v) ? "nil" : "obj"));
                    long _alen = mrb_array_p(v) ? (long)RARRAY_LEN(v) : -1;
                    fprintf(g_dbglog, "[BIGLOAD] '%s' criou %d objetos (live %d->%d) -> %s(len=%ld) "
                            "== FONTE DO CHURN se >200k e repete\n",
                            fname ? fname : "?", _bl_d, _bl_live0, (int)g_mkxp_mrb->gc.live, _shape, _alen);
                    fflush(g_dbglog);
                }
            }

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

    /* ── PRE-FIX #4: round/Dir.delete ANTES dos scripts/plugins ──────────────
     * Os plugins chamam .round e Dir.delete no proprio LOAD (~t=3s). O bloco MFD
     * que os definia corre dentro de check_entry_methods a ~t=4s -> TARDE DEMAIS,
     * logo davam [ERROR] 'undefined method round/delete'. Definimo-los aqui, antes
     * de run_rmxp_scripts, para estarem prontos quando o 1o plugin carrega.
     * Idempotente (unless method_defined?). */
    mrb_load_string(mrb, R"PREFIX(
begin
  class Numeric
    unless method_defined?(:round)
      def round(ndigits = 0)
        if ndigits && ndigits > 0
          f = 10 ** ndigits
          (((self * f) + (self >= 0 ? 0.5 : -0.5)).to_i).to_f / f
        else
          ((self + (self >= 0 ? 0.5 : -0.5)).to_i)
        end
      end
    end
  end
rescue; end
begin
  class << Dir
    def delete(*a); 0; end unless respond_to?(:delete)
    alias_method :rmdir,  :delete rescue nil
    alias_method :unlink, :delete rescue nil
  end
rescue; end
begin
  # [DEPRECATE-STUB 2026-07-09] Module#deprecate_constant e' builtin do CRuby que o
  # mruby nao tem. O 253_Pokemon_Deprecated chama-o no corpo da classe (L102) ->
  # sem isto, o load do script abortava a meio (era 1 das razoes do skip). No-op:
  # a depreciacao e' so' um aviso de dev, sem efeito funcional.
  class Module
    def deprecate_constant(*a); nil; end unless method_defined?(:deprecate_constant)
  end
rescue; end
)PREFIX");
    if (mrb->exc) { mrb->exc = 0; }
    DBGLOG("[PRE-FIX] Numeric#round + Dir.delete definidos ANTES dos scripts\n");

    // Carregar scripts (ainda vazia, mas vamos preencher)
    DBGLOG("[BOOT] passo 3/4: run_rmxp_scripts (carregar+correr 376 scripts -- FASE MAIS LONGA)...\n");
    run_rmxp_scripts(mrb, game_path);
    DBGLOG("[BOOT] passo 3/4 OK (scripts carregados e executados).\n");
    /* NB: o #7 GPS-FIX (override de get_player) NAO fica aqui: GameData::Metadata
     * so' e' carregado em Game.initialize (~t=6.46s), bem depois de este ponto
     * (~t=3.72s). O override esta' instalado dentro de Game.initialize, logo a
     * seguir a GameData::Metadata.load -- ver bloco [GPS-FIX] mais acima. */

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