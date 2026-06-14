/* ============================================================================
 * profile_3ds.cpp — definicoes dos contadores de profiling + relatorios.
 * Ver profile_3ds.h para a descricao do sistema.
 * ========================================================================== */
#include "profile_3ds.h"
#include <vector>
#include <string>
#include <algorithm>

/* O log global do binding (sdmc:/mkxp/debug_binding.log). Declarado extern para
 * o relatorio sair no mesmo ficheiro que o resto do diagnostico. */
extern FILE *g_dbglog;

/* ---- Instancias unicas dos contadores ------------------------------------ */
ProfZone g_prof_zones[PROF_ZONE_COUNT] = {{0,0,0}};
uint64_t g_prof_objs_created = 0;
uint64_t g_prof_gc_runs      = 0;
uint64_t g_prof_bytes_parsed = 0;
uint64_t g_prof_blits        = 0;
uint64_t g_prof_frees        = 0;

static const char *zone_name(int z) {
    switch (z) {
        case PROF_MARSHAL_TOTAL: return "marshal.total ";
        case PROF_MARSHAL_READ:  return "marshal.read  ";
        case PROF_MARSHAL_ALLOC: return "marshal.alloc ";
        case PROF_MARSHAL_IVSET: return "marshal.ivset ";
        case PROF_MARSHAL_CLASS: return "marshal.class ";
        case PROF_GC:            return "gc            ";
        case PROF_FRAME_UPDATE:  return "frame.update  ";
        case PROF_FRAME_RENDER:  return "frame.render  ";
        case PROF_RENDER_BEGIN:  return "render.begin  ";
        case PROF_RENDER_DRAW:   return "render.draw   ";
        case PROF_RENDER_END:    return "render.end    ";
        default:                 return "?             ";
    }
}

/* Imprime em g_dbglog E stdout (uma linha por destino). */
static void prof_emit(const char *line) {
    printf("%s", line);
    if (g_dbglog) fprintf(g_dbglog, "%s", line);
}

void prof_report(const char *tag) {
    char buf[256];

    snprintf(buf, sizeof(buf),
        "\n========== [PROFILE] %s ==========\n", tag ? tag : "");
    prof_emit(buf);

    snprintf(buf, sizeof(buf),
        "  %-14s | %12s | %10s | %12s\n",
        "zona", "total (ms)", "execucoes", "media (ms)");
    prof_emit(buf);
    prof_emit("  ---------------+--------------+------------+-------------\n");

    for (int z = 0; z < PROF_ZONE_COUNT; z++) {
        if (g_prof_zones[z].count == 0 && g_prof_zones[z].total_ticks == 0)
            continue;   /* zona nao usada -> nao polui o relatorio */
        double total_ms = prof_ticks_to_ms(g_prof_zones[z].total_ticks);
        double avg_ms   = g_prof_zones[z].count
                        ? total_ms / (double)g_prof_zones[z].count : 0.0;
        snprintf(buf, sizeof(buf),
            "  %-14s | %12.1f | %10llu | %12.3f\n",
            zone_name(z), total_ms,
            (unsigned long long)g_prof_zones[z].count, avg_ms);
        prof_emit(buf);
    }

    snprintf(buf, sizeof(buf),
        "  contadores: objs_criados=%llu  frees=%llu  bytes_parse=%llu  blits=%llu\n",
        (unsigned long long)g_prof_objs_created,
        (unsigned long long)g_prof_frees,
        (unsigned long long)g_prof_bytes_parsed,
        (unsigned long long)g_prof_blits);
    prof_emit(buf);
    prof_emit("==========================================\n\n");
    if (g_dbglog) fflush(g_dbglog);
}

void prof_report_marshal(const char *fname, uint64_t bytes,
                         uint64_t objs, uint64_t frees,
                         double total_ms) {
    char buf[320];
    double ms_per_kb   = bytes ? (total_ms / ((double)bytes / 1024.0)) : 0.0;
    double ms_per_obj  = objs  ? (total_ms / (double)objs)             : 0.0;
    double us_per_byte = bytes ? (total_ms * 1000.0 / (double)bytes)   : 0.0;
    /* frees durante o parse = proxy da atividade do GC. Se houver MUITOS frees
     * (proximo do nº de objetos criados), o GC esteve a varrer/libertar durante
     * o parse -> e' o gargalo. Se ~0 frees, o GC nao correu -> o tempo esta'
     * noutro lado (leitura, criacao). */
    double free_ratio = objs ? ((double)frees / (double)objs) : 0.0;

    snprintf(buf, sizeof(buf),
        "[PROFILE|marshal] %s: %.0f ms | %llu bytes | %llu objs criados | %llu frees\n",
        fname ? fname : "?", total_ms,
        (unsigned long long)bytes, (unsigned long long)objs,
        (unsigned long long)frees);
    prof_emit(buf);

    snprintf(buf, sizeof(buf),
        "[PROFILE|marshal]   -> %.2f ms/KB | %.4f ms/obj | %.2f us/byte\n",
        ms_per_kb, ms_per_obj, us_per_byte);
    prof_emit(buf);

    snprintf(buf, sizeof(buf),
        "[PROFILE|marshal]   -> frees/objs=%.2f => GC %s\n",
        free_ratio,
        free_ratio > 0.5 ? "ESTEVE MUITO ATIVO no parse (GARGALO = garbage collector!)"
                         : (free_ratio > 0.05 ? "ativo (contribui p/ lentidao)"
                                              : "quase parado (tempo NAO e' GC; ver leitura/criacao)"));
    prof_emit(buf);

    /* BREAKDOWN interno: dos N ms totais, quantos em criar objetos (alloc),
     * setar variaveis de instancia (ivset), resolver classes (class), internar
     * simbolos (symbol) e criar strings (string). O resto (total - estes) e'
     * leitura do buffer + arrays/hashes. Isto localiza o gargalo SEM adivinhar. */
    double alloc_ms  = prof_ticks_to_ms(g_prof_zones[PROF_MARSHAL_ALLOC].total_ticks);
    double ivset_ms  = prof_ticks_to_ms(g_prof_zones[PROF_MARSHAL_IVSET].total_ticks);
    double class_ms  = prof_ticks_to_ms(g_prof_zones[PROF_MARSHAL_CLASS].total_ticks);
    double symbol_ms = prof_ticks_to_ms(g_prof_zones[PROF_MARSHAL_SYMBOL].total_ticks);
    double string_ms = prof_ticks_to_ms(g_prof_zones[PROF_MARSHAL_STRING].total_ticks);
    double resto_ms  = total_ms - alloc_ms - ivset_ms - class_ms - symbol_ms - string_ms;
    snprintf(buf, sizeof(buf),
        "[PROFILE|marshal]   -> BREAKDOWN: alloc=%.0f ivset=%.0f class=%.0f symbol=%.0f string=%.0f resto(arrays/leitura)=%.0f ms\n",
        alloc_ms, ivset_ms, class_ms, symbol_ms, string_ms, resto_ms);
    prof_emit(buf);
    snprintf(buf, sizeof(buf),
        "[PROFILE|marshal]      (alloc:%llu objs, ivset:%llu ivars, symbol:%llu, string:%llu) => maior custo: %s\n",
        (unsigned long long)g_prof_zones[PROF_MARSHAL_ALLOC].count,
        (unsigned long long)g_prof_zones[PROF_MARSHAL_IVSET].count,
        (unsigned long long)g_prof_zones[PROF_MARSHAL_SYMBOL].count,
        (unsigned long long)g_prof_zones[PROF_MARSHAL_STRING].count,
        (alloc_ms>=ivset_ms && alloc_ms>=class_ms && alloc_ms>=symbol_ms && alloc_ms>=string_ms && alloc_ms>=resto_ms) ? "ALLOC (criar objetos)" :
        (ivset_ms>=class_ms && ivset_ms>=symbol_ms && ivset_ms>=string_ms && ivset_ms>=resto_ms) ? "IVSET (setar ivars)" :
        (class_ms>=symbol_ms && class_ms>=string_ms && class_ms>=resto_ms) ? "CLASS (resolver classe)" :
        (symbol_ms>=string_ms && symbol_ms>=resto_ms) ? "SYMBOL (mrb_intern -- otimizavel)" :
        (string_ms>=resto_ms) ? "STRING (mrb_str_new)" : "ARRAYS/LEITURA (buffer/arrays/hashes)");
    prof_emit(buf);
    if (g_dbglog) fflush(g_dbglog);
}

/* ============================================================================
 * PROFILING DE SCRIPTS / PLUGINS — implementacao.
 * ========================================================================== */
namespace {
    struct ScriptRec {
        std::string name;
        uint64_t bytes;
        int      cache_hit;
        double   decomp_ms, patch_ms, exec_ms, compile_ms;
        double   total_ms;   /* soma das fases, para ordenar */
    };
    std::vector<ScriptRec> g_script_recs;
}

void prof_scripts_reset(void) {
    g_script_recs.clear();
}

void prof_script_record(const char *name, uint64_t bytes, int cache_hit,
                        double decomp_ms, double patch_ms,
                        double exec_ms, double compile_ms) {
    ScriptRec r;
    r.name       = name ? name : "?";
    r.bytes      = bytes;
    r.cache_hit  = cache_hit;
    r.decomp_ms  = decomp_ms;
    r.patch_ms   = patch_ms;
    r.exec_ms    = exec_ms;
    r.compile_ms = compile_ms;
    r.total_ms   = decomp_ms + patch_ms + exec_ms + compile_ms;
    g_script_recs.push_back(r);
}

void prof_report_scripts(const char *tag) {
    char buf[320];
    if (g_script_recs.empty()) return;

    /* Agregados por fase. */
    int    hits = 0, miss = 0;
    double t_decomp = 0, t_patch = 0, t_exec = 0, t_compile = 0;
    uint64_t total_bytes = 0;
    for (size_t i = 0; i < g_script_recs.size(); i++) {
        const ScriptRec &r = g_script_recs[i];
        if (r.cache_hit) hits++; else miss++;
        t_decomp  += r.decomp_ms;
        t_patch   += r.patch_ms;
        t_exec    += r.exec_ms;
        t_compile += r.compile_ms;
        total_bytes += r.bytes;
    }
    double t_total = t_decomp + t_patch + t_exec + t_compile;

    snprintf(buf, sizeof(buf),
        "\n========== [PROFILE|%s] %d scripts, %llu KB ==========\n",
        tag ? tag : "scripts", (int)g_script_recs.size(),
        (unsigned long long)(total_bytes / 1024));
    prof_emit(buf);

    snprintf(buf, sizeof(buf),
        "  cache: %d HIT (bytecode) / %d MISS (compilou) => %s\n",
        hits, miss,
        hits > miss ? "cache A FUNCIONAR (maioria hit)"
                    : (miss > 0 && hits == 0 ? "cache NAO FUNCIONA (tudo miss -- compila sempre!)"
                                             : "cache parcial"));
    prof_emit(buf);

    snprintf(buf, sizeof(buf),
        "  tempo por fase: descomprimir=%.0fms patch=%.0fms compilar=%.0fms executar=%.0fms (total=%.0fms)\n",
        t_decomp, t_patch, t_compile, t_exec, t_total);
    prof_emit(buf);

    /* Diagnostico: onde esta' o tempo? */
    const char *culpado =
        (t_exec >= t_compile && t_exec >= t_patch && t_exec >= t_decomp) ?
            "EXECUCAO (definir classes -- custo real, so' reduz cortando scripts)" :
        (t_compile >= t_patch && t_compile >= t_decomp) ?
            "COMPILACAO (cache miss -- se a cache gravasse, poupava isto)" :
        (t_patch >= t_decomp) ? "PATCH (patch_script -- otimizavel)" :
            "DESCOMPRIMIR (zlib)";
    snprintf(buf, sizeof(buf), "  => maior custo: %s\n", culpado);
    prof_emit(buf);

    /* Top 10 scripts mais lentos -- para saber QUAIS cortar. */
    std::vector<ScriptRec> sorted = g_script_recs;
    std::sort(sorted.begin(), sorted.end(),
              [](const ScriptRec &a, const ScriptRec &b){ return a.total_ms > b.total_ms; });
    prof_emit("  --- 10 mais lentos (nome: total | exec | compile | hit?) ---\n");
    int n = (int)sorted.size(); if (n > 10) n = 10;
    for (int i = 0; i < n; i++) {
        const ScriptRec &r = sorted[i];
        snprintf(buf, sizeof(buf),
            "  %2d. %-32s %.0fms | exec=%.0f comp=%.0f | %s\n",
            i+1, r.name.c_str(), r.total_ms, r.exec_ms, r.compile_ms,
            r.cache_hit ? "HIT" : "MISS");
        prof_emit(buf);
    }
    prof_emit("==========================================\n\n");
    if (g_dbglog) fflush(g_dbglog);
}
