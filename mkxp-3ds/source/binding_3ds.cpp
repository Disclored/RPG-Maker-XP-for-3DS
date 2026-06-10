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

/* =========================================================
 * SUPER DEBUG HELPERS
 * ========================================================= */

/* Ficheiro de log global para dump detalhado */
FILE *g_dbglog = NULL;

static void dbglog_open() {
    g_dbglog = fopen("sdmc:/mkxp/debug_binding.log", "w");
    if (!g_dbglog) printf("[DBGLOG] AVISO: nao foi possivel criar sdmc:/mkxp/debug_binding.log\n");
    else           printf("[DBGLOG] log aberto em sdmc:/mkxp/debug_binding.log\n");
}
static void dbglog_close() { if (g_dbglog) { fclose(g_dbglog); g_dbglog = NULL; } }

/* Escreve para consola E para ficheiro */
#define DBGLOG(fmt, ...) \
    do { \
        printf(fmt, ##__VA_ARGS__); \
        if (g_dbglog) { fprintf(g_dbglog, fmt, ##__VA_ARGS__); fflush(g_dbglog); } \
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
            "  # Tentar carregar Tilesets.rxdata se ainda nao carregado\n"
            "  if !$data_tilesets.is_a?(Array) || $data_tilesets.length <= 1\n"
            "    begin\n"
            "      dbg '[TSET] a tentar load_data Tilesets.rxdata'\n"
            "      loaded = load_data('Data/Tilesets.rxdata')\n"
            "      dbg \"[TSET] carregado: #{loaded.class} len=#{loaded.length rescue '?'}\"\n"
            "      $data_tilesets = loaded if loaded.is_a?(Array) && loaded.length > 1\n"
            "      dbg \"[TSET] $data_tilesets=#{$data_tilesets.class} len=#{$data_tilesets.length rescue '?'}\"\n"
            "    rescue => e\n"
            "      dbg \"[TSET] ERRO load Tilesets.rxdata: #{e.class}: #{e.message}\"\n"
            "    end\n"
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
        if (mrb_string_p(str))
            DBGLOG("[MFD] %s\n", RSTRING_PTR(str));
        return mrb_nil_value();
    }, MRB_ARGS_REQ(1));

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

# FIX: Regexp#to_str -- [x41] no log -- conversao implicita quando codigo faz
# "string" + regexp_obj ou format() com regexp. Adicionar aqui (pos-scripts)
# garante override mesmo que algum script reabra Regexp.
begin
  class Regexp
    unless method_defined?(:to_str)
      def to_str; @src.to_s rescue ""; end
    end
    unless method_defined?(:length)
      def length; (@src.to_s rescue "").length; end
    end
  end
rescue; end

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
# TypeError: Regexp cannot be converted to Integer em _slice_c.
# PE usa str[/regex/, 1] para extrair grupos de captura.
# Redefinir aqui pos-scripts garante override mesmo que algum script reabra String.
begin
  class String
    def [](idx, len=nil)
      unless idx.is_a?(Integer) || idx.is_a?(String) ||
             (idx.respond_to?(:exclude_end?))
        return nil
      end
      begin
        len.nil? ? slice(idx) : slice(idx, len)
      rescue TypeError
        nil
      rescue
        nil
      end
    end

    alias_method :_native_sub,  :sub
    alias_method :_native_gsub, :gsub

    def sub(pat, rep=nil, &blk)
      if pat.is_a?(String)
        blk ? _native_sub(pat, &blk) : _native_sub(pat, rep.to_s)
      elsif pat.respond_to?(:to_str)
        blk ? _native_sub(pat.to_str, &blk) : _native_sub(pat.to_str, rep.to_s)
      else
        self.dup
      end
    rescue
      self.dup
    end

    def sub!(pat, rep=nil, &blk)
      r = sub(pat, rep, &blk)
      return nil if r == self
      replace(r)
      self
    rescue
      nil
    end

    def gsub(pat, rep=nil, &blk)
      if pat.is_a?(String)
        blk ? _native_gsub(pat, &blk) : _native_gsub(pat, rep.to_s)
      elsif pat.respond_to?(:to_str)
        blk ? _native_gsub(pat.to_str, &blk) : _native_gsub(pat.to_str, rep.to_s)
      else
        self.dup
      end
    rescue
      self.dup
    end

    def gsub!(pat, rep=nil, &blk)
      r = gsub(pat, rep, &blk)
      return nil if r == self
      replace(r)
      self
    rescue
      nil
    end
  end
rescue; end

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
  def self.initialize; end
  def self.set_up_system; end
end

class Regexp
  IGNORECASE = 1
  EXTENDED   = 2
  MULTILINE  = 4
  def initialize(src, flags=0); @src = src.to_s; end
  def ===(s); false; end
  def =~(s); nil; end
  def match(s); nil; end
  def source; @src; end
  def to_s; "/#{@src}/"; end
end

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
  return nil   if name.start_with?("update","draw","dispose","refresh",
                                   "clear","create","setup","start",
                                   "terminate","pbOn","pbOff","set",
                                   "load","save","init","reset","show","hide")
  return false if name.end_with?("?")
  return false if name.start_with?("is_","has_","can_","should_","will_")
  return []    if name.start_with?("all","list","each","get_all","find_all")
  return ""    if name.start_with?("name","title","filename","path",
                                   "character","charset","tileset","text")
  return 0
end

# -- method_missing universal em Object ---------------------------------------
# Apanha QUALQUER classe que nao tenha um metodo -- inclui classes nao listadas.
# As subclasses com method_missing proprio prevalecem (Ruby MRO).
begin
  class Object
    alias _original_method_missing method_missing rescue nil
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
  dbg "[PATCH] Object#method_missing universal OK"
rescue => e
  dbg "[PATCH] Object#method_missing falhou: #{e.message}"
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

class Scene_DebugIntro
  def main
    $__debug_intro_count = ($__debug_intro_count || 0) + 1
    dbg "[DebugIntro] chamada ##{$__debug_intro_count}"

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
        _obj.define_singleton_method(:pictures)       { {} }
        $game_screen = _obj
      end
    rescue => e
      dbg "[DebugIntro] game_screen stub failed: #{e.message}"
    end

    # --- $game_system fallback stub ---
    begin
      unless $game_system
        _obj = Object.new
        _obj.define_singleton_method(:bgm_play)       { |*a| }
        _obj.define_singleton_method(:bgs_play)       { |*a| }
        _obj.define_singleton_method(:update)         { }
        _obj.define_singleton_method(:map_interpreter){ nil }
        $game_system = _obj
      end
    rescue => e
      dbg "[DebugIntro] game_system stub failed: #{e.message}"
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

    DBGLOG("[binding] calling %s loop\n", entry_name);

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

    /* Injectar stub de Regexp ANTES do ios_compat_3ds.rb (que usa Regexp) */
    {
        static const char *regexp_stub =
            "begin\n"
            "  class Regexp\n"
            "    IGNORECASE = 1\n"
            "    EXTENDED   = 2\n"
            "    MULTILINE  = 4\n"
            "    def initialize(src, flags=0); @src = src.to_s; end\n"
            "    def ===(s); false; end\n"
            "    def =~(s); nil; end\n"
            "    def match(s); nil; end\n"
            "    def source; @src; end\n"
            "    def to_s; \"/#{@src}/\"; end\n"
            "    def inspect; \"/#{@src}/\"; end\n"
            "    def self.compile(src, flags=0); new(src, flags); end\n"
            "    def self.last_match; nil; end\n"
            "    def self.escape(str); str.to_s; end\n"
            "    def self.quote(str); str.to_s; end\n"
            "    def self.union(*args); new(args.map(&:to_s).join(\"|\")); end\n"
            "  end\n"
            "rescue; end\n";
        mrb_load_string(mrb, regexp_stub);
        if (mrb->exc) { show_error(mrb, "regexp_stub_early"); mrb->exc = 0; }
        else DBGLOG("[STUBS] Regexp stub pre-carregado OK\n");
    }

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

        // Aplicar patches (Main, etc.)
        patch_script(decomp, (uLong)decomp.size(), script_name);

        // Executar no mruby
        mrb_value result = mrb_load_nstring(mrb, (const char*)decomp.data(), decomp.size());
        (void)result;  /* valor de retorno não usado; erro verificado via mrb->exc */
        if (mrb->exc) {
            show_error(mrb, script_name);
            mrb->exc = 0;   // continuar mesmo com erro
        }
    }

    mrb_gc_unregister(mrb, scripts_array);
    DBGLOG("[SCRIPTS] Loaded %d scripts\n", num_scripts);

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
}

/* base path do jogo (ex.: "sdmc:/mkxp/game"), usado pelo load_data real */
static char g_game_base[300] = {0};

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

    mrb_state *mrb = mrb_open();
    if (!mrb) { DBGLOG("[ERROR] mrb_open failed\n"); dbglog_close(); return -1; }

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
     * ANTES de run_rmxp_scripts -> o stub do rgss_stubs.rb e ignorado. */
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

            DBGLOG("[load_data] req='%s' -> '%s'\n", fname ? fname : "(nil)", path);

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
    run_rmxp_scripts(mrb, game_path);

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