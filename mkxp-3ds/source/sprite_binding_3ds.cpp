#include "sprite_binding_3ds.h"
#include "debug_3ds.h"

#include "display_3ds.h"
/* FIX BUG 5: display_3ds_blit_ex aceita draw_w/draw_h separados de src_w/src_h */
extern void display_3ds_blit_ex(DS3Texture* t,
                                 float dx, float dy, float dw, float dh,
                                 float sx, float sy, float sw, float sh,
                                 float alpha);
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/value.h>
#include <mruby/variable.h>
#include <mruby/hash.h>     /* [GHOST-KILL] mrb_hash_get/mrb_hash_p p/ $game_map.@events */
#include <mruby/array.h>    /* [GHOST-KILL v3] RARRAY_LEN/mrb_ary_ref p/ @maps/@realEvents */
#include <mruby/string.h>   /* [ROUPEIRODIAG] mrb_str_to_cstr p/ os nomes dos sprites */
#include <stdlib.h>
#include <string.h>         /* [ZINVERT/STATUE-SPR] strcmp p/ comparar classe do sprite */
#include <string.h>
#include <math.h>   /* FIX-NSYNC: lround (arredondamento half-away-from-zero = .round do Ruby) */
#include <vector>
#include <algorithm>   /* std::stable_sort */

/* global sprite list for rendering */
static std::vector<Sprite3DS*> g_sprites;

/* [ZSORT-REGTIE] contador monotono de criacao de sprites. Atribuido a reg_seq
 * no spr_init; usado como desempate de ordenacao no topo extremo (chave>=99990).
 * unsigned: ~4 mil milhoes de criacoes antes de dar a volta (irrelevante numa
 * sessao). Nunca reutilizado -> ordem de criacao = ordem RGSS no empate. */
static unsigned int g_sprite_seq = 0;

/* mrb_state principal (definido em binding_3ds.cpp). Necessario para ler o
 * @src_rect Ruby LIVE no desenho -- o RGSS modifica-o via src_rect.set(...)
 * in-place, que nao passa pelo setter C++, por isso a cache s->src_w fica
 * obsoleta. Ler o Rect a cada frame resolve o "rasto" (sprite-sheet inteira). */
extern mrb_state *g_mkxp_mrb;

/* DUMP-DE-FRAME (botao Y): pedido pendente de despejo. O input poll poe true
 * quando o Y e' premido; o grph_update le, despeja e poe false. volatile
 * porque e' escrita/lida em pontos diferentes do loop. */
volatile bool g_dump_frame_requested = false;

static void sprite_free(mrb_state*, void *p) {
    Sprite3DS *s = (Sprite3DS*)p;
    if (!s) return;
    for (size_t i = 0; i < g_sprites.size(); i++)
        if (g_sprites[i] == s) { g_sprites.erase(g_sprites.begin() + i); break; }
    free(s);
}
const mrb_data_type Sprite3DSType = { "Sprite", sprite_free };

#define GET_SPR(s) ((Sprite3DS*)DATA_PTR(s))

/* P3: declarado adiante (definicao perto do spriteBindingInit). Le o z de um
 * Viewport Ruby de forma segura; 0 se nil/sem #z. Usado por spr_init e o
 * setter viewport= para preencher vp_z (chave primaria do sort de sprites). */
static int read_viewport_z(mrb_state *mrb, mrb_value vp);

#include "tilemap_binding_3ds.h"

void sprites_draw_all() {
    /* [BUILD-CHK-J20] Marcador INCONDICIONAL de compilacao (dispara 1x, sem
     * depender de estado do jogo). Se NAO aparecer no log, este
     * sprite_binding_3ds.cpp (com o FIX-GRELHA7) NAO esta' no binario. */
    {
        static bool s_once = false;
        if (!s_once) {
            s_once = true;
            extern FILE *g_dbglog;
            const char *m = "[BUILD-CHK-J21] sprite_binding_3ds ATIVO (FIX-GRELHA7 + ZSORT-REGTIE presente)\n";
            printf("%s", m);
            if (g_dbglog) { fprintf(g_dbglog, "%s", m); dbg_flush(); }
        }
    }
    /* ── DIAGNÓSTICO: dump completo nos primeiros 3 frames ── */
    {
        static int s_diag_frame = 0;
        s_diag_frame++;
        if (s_diag_frame <= 10) {
            extern FILE *g_dbglog;
            int total    = (int)g_sprites.size();
            int disposed = 0, invisible = 0, no_bitmap = 0, ready = 0;
            for (size_t i = 0; i < g_sprites.size(); i++) {
                Sprite3DS *s = g_sprites[i];
                if (!s || s->disposed)  { disposed++;   continue; }
                if (!s->visible)        { invisible++;  continue; }
                if (!s->bitmap)         { no_bitmap++;  continue; }
                ready++;
            }
            if (g_dbglog) {
                fprintf(g_dbglog,
                    "[SPR|DUMP] diag_frame=%d g_sprites=%d "
                    "(disposed=%d invisible=%d no_bitmap=%d ready=%d)\n",
                    s_diag_frame, total, disposed, invisible, no_bitmap, ready);
                /* Se g_sprites=0 nas primeiras frames, spr_init nunca foi chamado --
                 * confirmar com [SPR|INIT] acima. Se g_sprites>0 mas ready=0 e
                 * no_bitmap>0, o DATA_PTR ainda está nullptr (MRB_TT_DATA em falta). */
                if (total == 0 && s_diag_frame <= 3) {
                    fprintf(g_dbglog,
                        "[SPR|DUMP]   AVISO: g_sprites=0 -- spr_init C++ nao foi chamado!\n"
                        "[SPR|DUMP]   Sprite#initialize foi substituido por versao Ruby.\n"
                        "[SPR|DUMP]   Ver [SPRITE_CHECK] no MKXPDebug.log do probe.\n");
                } else if (total > 0 && no_bitmap == total && s_diag_frame <= 3) {
                    fprintf(g_dbglog,
                        "[SPR|DUMP]   AVISO: %d sprites mas todos sem bitmap!\n"
                        "[SPR|DUMP]   DATA_PTR=nullptr -> MRB_TT_DATA nao foi setado.\n"
                        "[SPR|DUMP]   Ver [FIX2] no log -- BitmapWrapper MRB_TT_DATA.\n",
                        total);
                }
                dbg_flush();
            }
        }
    }

    /* Ordenar como o RGSS: a chave primaria de cada sprite e' a sua posicao no
     * espaco Z GLOBAL do ecra -- que e' o viewport.z se tiver viewport, ou o
     * proprio z se NAO tiver. O desempate e' o z local.
     *   chave_primaria = has_vp ? vp_z : z
     *   chave_secundaria = z
     * Isto corrige DOIS casos:
     *  (1) background da escolha de personagem (Plane, viewport.z=99999) fica a'
     *      frente do mapa (viewport.z baixo) -- o que o P3 ja' resolvia;
     *  (2) janela de MENSAGEM nas intros: criada SEM viewport e com z=99999
     *      (pbCreateMessageWindow), tem de ficar a' frente das intro Pictures
     *      (que teem viewport.z>0). Antes, ao usar vp_z=0 para sem-viewport, a
     *      mensagem caia ATRAS das imagens -- as falas pareciam nao aparecer.
     * Sprites sem viewport mantem a ordem por z entre si (comportamento original).
     * Custo: leitura de 2 ints da struct, sem chamadas a Ruby. */
    /* [ROUPEIRO-ZFIX] vp_z VIVO (releitura por frame antes do sort). BUG do motor
     * (causa do roupeiro/menus com sprites do overworld por cima): o vp_z (z do
     * viewport, chave PRIMARIA do sort) era um SNAPSHOT -- so' atualizado em
     * spr_init/viewport=. Os overlays de menu (roupeiro, Voltseon) criam os seus
     * Sprites/Pictures e SO' DEPOIS sobem o viewport.z; o motor nunca o relia, por
     * isso o menu ordenava como vp_z=0 e os Sprite_Character always_on_top do
     * overworld (z~999) desenhavam-lhe por cima. Aqui relemos @viewport.@z de cada
     * sprite-com-viewport por frame (mesmo padrao do @src_rect vivo, linha ~214).
     * Generico: corrige QUALQUER menu/overlay Essentials que mude viewport.z em
     * runtime. Custo: 1-2 mrb_iv_get por sprite-com-viewport/frame (sem funcall no
     * caminho rapido). */
    if (g_mkxp_mrb) {
        mrb_state *mrb = g_mkxp_mrb;
        static mrb_sym sy_vp = 0, sy_z = 0;
        if (!sy_vp) { sy_vp = mrb_intern_lit(mrb, "@viewport"); sy_z = mrb_intern_lit(mrb, "@z"); }
        for (size_t i = 0; i < g_sprites.size(); i++) {
            Sprite3DS *s = g_sprites[i];
            if (!s || !s->has_vp || mrb_nil_p(s->rb_self)) continue;
            mrb_value vp = mrb_iv_get(mrb, s->rb_self, sy_vp);
            if (mrb_nil_p(vp)) continue;
            mrb_value zv = mrb_iv_get(mrb, vp, sy_z);   /* @z direto (Viewport e' attr_accessor) */
            if      (mrb_fixnum_p(zv)) s->vp_z = (int)mrb_fixnum(zv);
            else if (mrb_float_p(zv))  s->vp_z = (int)mrb_float(zv);
            else                       s->vp_z = read_viewport_z(mrb, vp);
        }
    }

    /* [ZSORT-REGTIE] Le o interruptor de runtime UMA vez por frame (fora do
     * comparador, que e' chamado O(n log n) vezes). $MKXP_ZSORT_REGTIE=false
     * reverte ao comportamento antigo (desempate por z local) SEM recompilar.
     * Default LIGADO (nil/true mantem). */
    bool z_regtie = true;
    if (g_mkxp_mrb) {
        mrb_value zv = mrb_gv_get(g_mkxp_mrb, mrb_intern_lit(g_mkxp_mrb, "$MKXP_ZSORT_REGTIE"));
        if (mrb_test(zv) == false && !mrb_nil_p(zv)) z_regtie = false; /* so' false explicito desliga */
    }
    std::stable_sort(g_sprites.begin(), g_sprites.end(),
        [z_regtie](const Sprite3DS *a, const Sprite3DS *b) {
            int ka = a->has_vp ? a->vp_z : a->z;
            int kb = b->has_vp ? b->vp_z : b->z;
            if (ka != kb) return ka < kb;
            /* [NOVP-OVERLAY-FIX] EMPATE na camada Z global. Regra RGSS: os sprites
             * SEM viewport (overlays de ecra criados DEPOIS do mapa: UI do roupeiro
             * CE#173, ecra de nome, janelas de mensagem) desenham-se POR CIMA do
             * conteudo dos viewports na MESMA camada. O motor ordenava por z LOCAL,
             * pondo a UI (z=0) por baixo dos Sprite_Character do quarto (z=48-400) ->
             * "quarto por cima do menu" + preview do outfit soterrado. Correcao: no
             * mesmo ka, os COM viewport primeiro (por baixo), os SEM viewport por cima.
             * Chave lexicografica (ka, has_vp?0:1, ...) -> strict-weak-ordering VALIDO
             * (sem UB). So' afecta empates de camada com no-viewport envolvido; o resto
             * do jogo (camadas != ou ambos com viewport) fica igual. */
            if (a->has_vp != b->has_vp) return a->has_vp;  /* com-vp (1) antes de sem-vp (0) => sem-vp por cima */
            /* [ZSORT-REGTIE] no TOPO EXTREMO (>=99990) desempata por ordem de criacao
             * (RGSS: criado mais tarde por cima). Abaixo, desempate por z local. */
            if (z_regtie && ka >= 99990) return a->reg_seq < b->reg_seq;
            return a->z < b->z;
        });

    /* ═══ [DIAG 2026-07-02] Instrumentacao dedicada para PROVAR 2 bugs nao confirmados.
     * g_sprites esta' AGORA em ordem de desenho (indice 0 = fundo, ultimo = topo).
     *   [ZINVERT]   Bug 5 (roupeiro): um sprite do MAPA (CustomTilemapSprite/
     *               Sprite_Character) desenhado ACIMA (indice maior) do sprite de UI de
     *               topo (ka>=9990) -> prova a inversao "mapa por cima do menu" no frame
     *               exato. So' corre quando existe UI de topo (menu/roupeiro ativo).
     *   [STATUE-SPR] Bug 3 (estatua): Sprite_Character cujo @character.@erased==true mas
     *               que AINDA tem bitmap valido + visible -> a estatua VISUAL (o
     *               STATUE-DIAG do probe le' so' o modelo Game_Event, nao o sprite).
     * Fiavel: e' o proprio sort do motor, no g_dbglog (mesmo caminho do USE-TRIG que ja'
     * funciona). Caps evitam flood.
     * [PERF 2026-07-04] Bugs FECHADOS (GHOST-KILL v3 + FOLLOWER-LEAK validados) ->
     * default agora OFF (varria todos os sprites por frame + iv_gets). Opt-in:
     * $MKXP_DIAG_ZSTAT=true reativa tudo tal-e-qual. */
    {
        extern FILE *g_dbglog;
        static bool zs_read = false; static bool zs_on = false;
        if (!zs_read && g_mkxp_mrb) {
            zs_read = true;
            mrb_value gv = mrb_gv_get(g_mkxp_mrb, mrb_intern_lit(g_mkxp_mrb, "$MKXP_DIAG_ZSTAT"));
            if (!mrb_nil_p(gv) && mrb_test(gv)) zs_on = true;   /* opt-in explicito */
        }
        if (zs_on && g_mkxp_mrb && g_dbglog && !g_sprites.empty()) {
            mrb_state *mrb = g_mkxp_mrb;
            static mrb_sym sy_ch3 = 0, sy_cn3 = 0, sy_er3 = 0;
            if (!sy_ch3) { sy_ch3 = mrb_intern_lit(mrb, "@character");
                           sy_cn3 = mrb_intern_lit(mrb, "@character_name");
                           sy_er3 = mrb_intern_lit(mrb, "@erased"); }
            const size_t nspr = g_sprites.size();

            /* --- [ZINVERT] --- indice do UI de topo (ultimo sprite com ka>=9990) --- */
            int ui_top = -1, ui_top_ka = 0;
            for (size_t i = 0; i < nspr; i++) {
                Sprite3DS *s = g_sprites[i]; if (!s) continue;
                int ka = s->has_vp ? s->vp_z : s->z;
                if (ka >= 9990) { ui_top = (int)i; ui_top_ka = ka; }
            }
            if (ui_top >= 0) {   /* ha' UI de topo -> menu/roupeiro ativo */
                static int zinv_n = 0;
                for (size_t i = (size_t)ui_top + 1; i < nspr && zinv_n < 40; i++) {
                    Sprite3DS *s = g_sprites[i]; if (!s || mrb_nil_p(s->rb_self)) continue;
                    const char *cls = mrb_obj_classname(mrb, s->rb_self);
                    if (cls && (strcmp(cls, "CustomTilemapSprite") == 0 || strcmp(cls, "Sprite_Character") == 0)) {
                        zinv_n++;
                        fprintf(g_dbglog, "[ZINVERT] f=%d MAPA '%s' idx=%u (z=%d vp_z=%d hvp=%d) DESENHADO ACIMA do UI-topo ka=%d (idx=%d) -> mapa sobre menu\n",
                                g_dbg_frame, cls, (unsigned)i, s->z, s->vp_z, s->has_vp ? 1 : 0, ui_top_ka, ui_top);
                        dbg_flush();
                        break;   /* um por frame chega */
                    }
                }
            }

            /* --- [GILSPR] 2026-07-02 --- censo DEFINITIVO dos "2 Gilberts": lista TODOS
             * os Sprite_Character visiveis cujo @character_name == "trainer_RIVAL1"
             * (erased OU nao, evento OU sprite orfao de rebuild). Se aparecerem 2 numa
             * amostra -> confirma o duplicado + as posicoes exatas p/ achar a origem.
             * Throttle 60f, cap 30 amostras. So' loga quando conta >=1. */
            {
                static unsigned gil_f = 0; static int gil_n = 0;
                if (gil_n < 30 && (++gil_f % 60) == 0) {
                    char line[512]; int off = 0; int cnt = 0;
                    off += snprintf(line + off, sizeof(line) - off, "[GILSPR] f=%d RIVAL1 visiveis:", g_dbg_frame);
                    for (size_t i = 0; i < nspr && off < (int)sizeof(line) - 80; i++) {
                        Sprite3DS *s = g_sprites[i];
                        if (!s || mrb_nil_p(s->rb_self) || !s->visible) continue;
                        mrb_value ch = mrb_iv_get(mrb, s->rb_self, sy_ch3);
                        if (mrb_nil_p(ch)) continue;
                        mrb_value cn = mrb_iv_get(mrb, ch, sy_cn3);
                        if (!mrb_string_p(cn)) continue;
                        const char *nm = mrb_str_to_cstr(mrb, cn);
                        if (!nm || strcmp(nm, "trainer_RIVAL1") != 0) continue;
                        int er = mrb_test(mrb_iv_get(mrb, ch, sy_er3)) ? 1 : 0;
                        int bw = s->bitmap ? s->bitmap->width : 0;
                        /* [GILSPR] id do evento + classe do @character -> distingue
                         * ev7 (evento real) de orfao/follower (@id igual=spriteset
                         * duplicado; classe DependentEvent=follower). */
                        static mrb_sym sy_id = 0; if (!sy_id) sy_id = mrb_intern_lit(mrb, "@id");
                        static mrb_sym sy_mid = 0; if (!sy_mid) sy_mid = mrb_intern_lit(mrb, "@map_id");
                        mrb_value idv = mrb_iv_get(mrb, ch, sy_id);
                        int evid = mrb_fixnum_p(idv) ? (int)mrb_fixnum(idv) : -1;
                        const char *ccls = mrb_obj_classname(mrb, ch);
                        /* [GILSPR v3 2026-07-03] object_id do @character + o @map_id DELE:
                         * distingue de vez a instancia-fantasma (2 object_ids p/ o mesmo
                         * ev7 = 2 Game_Event vivos; map_id diz de que instancia de mapa
                         * do factory veio a estatua). */
                        mrb_value oid = mrb_funcall(mrb, ch, "object_id", 0);
                        long oidn = mrb_fixnum_p(oid) ? (long)mrb_fixnum(oid) : -1;
                        mrb_value mi2 = mrb_iv_get(mrb, ch, sy_mid);
                        int chmap = mrb_fixnum_p(mi2) ? (int)mrb_fixnum(mi2) : -1;
                        off += snprintf(line + off, sizeof(line) - off,
                                        " #%u ev%d(%s,oid%ld,map%d)@(%d,%d)z%d er%d bmp%d", (unsigned)i, evid,
                                        ccls ? ccls : "?", oidn, chmap, s->x, s->y, s->z, er, bw);
                        cnt++;
                    }
                    if (cnt >= 1) {
                        gil_n++;
                        snprintf(line + off, sizeof(line) - off, " | TOTAL=%d%s", cnt, cnt >= 2 ? " <<< DUPLICADO" : "");
                        fprintf(g_dbglog, "%s\n", line);
                        dbg_flush();
                    }
                }
            }

            /* --- [STATUE-SPR] --- Sprite_Character erased mas ainda desenhado (throttle 30f, cap 40) --- */
            static unsigned stat_f = 0; static int stat_n = 0;
            if (stat_n < 40 && (++stat_f % 30) == 0) {
                int logged = 0;
                for (size_t i = 0; i < nspr && logged < 2 && stat_n < 40; i++) {
                    Sprite3DS *s = g_sprites[i];
                    if (!s || mrb_nil_p(s->rb_self) || !s->visible) continue;
                    if (!s->bitmap || s->bitmap->disposed || s->bitmap->width <= 1) continue;
                    mrb_value ch = mrb_iv_get(mrb, s->rb_self, sy_ch3);
                    if (mrb_nil_p(ch)) continue;                       /* nao e' Sprite_Character */
                    if (!mrb_test(mrb_iv_get(mrb, ch, sy_er3))) continue;/* @erased!=true -> ignora */
                    mrb_value cn = mrb_iv_get(mrb, ch, sy_cn3);
                    const char *nm = mrb_string_p(cn) ? mrb_str_to_cstr(mrb, cn) : "?";
                    stat_n++; logged++;
                    fprintf(g_dbglog, "[STATUE-SPR] f=%d Sprite_Character ERASED mas DESENHADO: char='%s' bmp=%dx%d vis=1 @(%d,%d) z=%d hvp=%d -> estatua visual (sprite nao limpou apos erase)\n",
                            g_dbg_frame, nm, s->bitmap->width, s->bitmap->height, s->x, s->y, s->z, s->has_vp ? 1 : 0);
                    dbg_flush();
                }
            }
        }
    }

    /* [ROUPEIRODIAG] topo da pilha de desenho (os ULTIMOS na lista ficam por cima),
     * 1x/120f. Com o roupeiro aberto, mostra se os Sprite_Character (always_on_top,
     * z~999) estao por cima das Pictures do menu -> confirma se o fix viewport2.z=200
     * resolveu. Cheap/seguro: so' mrb_obj_classname dos 8 do topo. */
    {
        extern FILE *g_dbglog;
        static unsigned s_rdg = 0; s_rdg++;
        /* [2026-07-02] Desligado por defeito (roupeiro/outfit ja' OK; o dump por-frame
         * era pesado + spam). So' corre com a mascara DBG_SUPER ligada. */
        if (DBGV(DBG_SUPER) && g_mkxp_mrb && g_dbglog && (s_rdg % 120) == 0 && !g_sprites.empty()) {
            mrb_state *mrb = g_mkxp_mrb;
            static mrb_sym sy_ch=0, sy_cn=0, sy_pic=0, sy_pn=0, sy_aot=0;
            if (!sy_ch) { sy_ch=mrb_intern_lit(mrb,"@character"); sy_cn=mrb_intern_lit(mrb,"@character_name");
                          sy_pic=mrb_intern_lit(mrb,"@picture"); sy_pn=mrb_intern_lit(mrb,"@name");
                          sy_aot=mrb_intern_lit(mrb,"@always_on_top"); }
            size_t n = g_sprites.size();
            size_t start = (n > 12) ? n - 12 : 0;   /* 12 do topo (os de cima) */
            for (size_t i = start; i < n; i++) {
                Sprite3DS *s = g_sprites[i];
                if (!s) continue;
                const char *cls = mrb_nil_p(s->rb_self) ? "nil" : mrb_obj_classname(mrb, s->rb_self);
                /* nome: @character.@character_name (Sprite_Character) ou @picture.@name (Picture) */
                char nm[48] = "-"; int aot = -1;
                if (!mrb_nil_p(s->rb_self)) {
                    mrb_value ch = mrb_iv_get(mrb, s->rb_self, sy_ch);
                    if (!mrb_nil_p(ch)) {
                        mrb_value cn = mrb_iv_get(mrb, ch, sy_cn);
                        if (mrb_string_p(cn)) snprintf(nm, sizeof(nm), "%s", mrb_str_to_cstr(mrb, cn));
                        aot = mrb_test(mrb_iv_get(mrb, ch, sy_aot)) ? 1 : 0;
                    } else {
                        mrb_value pc = mrb_iv_get(mrb, s->rb_self, sy_pic);
                        if (!mrb_nil_p(pc)) {
                            mrb_value pn = mrb_iv_get(mrb, pc, sy_pn);
                            if (mrb_string_p(pn)) snprintf(nm, sizeof(nm), "pic:%s", mrb_str_to_cstr(mrb, pn));
                        }
                    }
                }
                fprintf(g_dbglog, "[ROUPEIRODIAG] #%u/%u %s z=%d vp_z=%d hvp=%d aot=%d vis=%d name='%s'\n",
                        (unsigned)i, (unsigned)n, cls ? cls : "?", s->z, s->vp_z,
                        s->has_vp ? 1 : 0, aot, s->visible ? 1 : 0, nm);
            }
            dbg_flush();
        }
    }

    /* [ROUPEIROCOUNT] Prova o LEAK do roupeiro: total g_sprites + populacao a z>=99999
     * por estado. Se vis1 CRESCE a cada troca de roupa sem voltar a baixar => o gerador
     * de personagem (CommonEvent #212) cria camadas Sprite.new e NAO as dispoe. */
    {
        extern FILE *g_dbglog;
        static unsigned s_rc_f = 0;
        /* [2026-07-02] Desligado por defeito (ver ROUPEIRODIAG acima). DBG_SUPER liga. */
        if (DBGV(DBG_SUPER) && g_dbglog && (++s_rc_f % 60) == 0) {
            int tot=0, vis1=0, vis0=0, disp=0; size_t kb=0;
            for (size_t i=0;i<g_sprites.size();i++){
                Sprite3DS *s=g_sprites[i]; if(!s) continue; tot++;
                int key = s->has_vp ? s->vp_z : s->z;
                if (key < 99999) continue;
                if (s->disposed) { disp++; continue; }
                if (s->visible) {
                    vis1++;
                    if (s->bitmap && !s->bitmap->disposed)
                        kb += (size_t)s->bitmap->width * s->bitmap->height * 4 / 1024;
                } else vis0++;
            }
            if (tot > 120 || vis1 > 20)   /* so' loga quando ha' muitos (= menu/leak ativo) */
                fprintf(g_dbglog,
                    "[ROUPEIROCOUNT] total=%d | z>=99999: vis1=%d vis0=%d disposed=%d | bmp~%uKB\n",
                    tot, vis1, vis0, disp, (unsigned)kb);
        }
    }

    /* contador de frames p/ o RETRY periodico de texturas <=1024 que falharam por
     * memoria transitoria (FIX OUTFIT, ver dentro do loop). Incrementa 1x por frame. */
    static unsigned s_spr_frame = 0;
    s_spr_frame++;

    /* [BLACKDIAG 2026-07-02] Quando o ecra fica preto COM sprites vivos (ex: preto
     * persistente apos troca de mapa no fluxo do roupeiro -- provado no log: g_sprites
     * cheio mas blits=0 skipped=0), este pre-scan diz PORQUE nenhum sprite e'
     * desenhavel. So' loga quando ha' sprites (>10) mas ZERO desenhaveis, 1x/60f ->
     * custo ~0 no caso normal, e nomeia a causa raiz do preto sem adivinhar. */
    {
        extern FILE *g_dbglog;
        static unsigned s_bd_f = 0;
        if (g_dbglog && (++s_bd_f % 60) == 0 && g_sprites.size() > 10) {
            int total=0, ds=0, inv=0, nb=0, db=0, txf=0, draw=0;
            for (size_t i=0;i<g_sprites.size();i++){
                Sprite3DS *s=g_sprites[i]; if(!s){continue;} total++;
                if (s->disposed){ds++;continue;}
                if (!s->visible){inv++;continue;}
                if (!s->bitmap){nb++;continue;}
                if (s->bitmap->disposed){db++;continue;}
                if (s->bitmap->tex_failed && !s->bitmap->tex){txf++;continue;}
                draw++;
            }
            if (draw == 0) {
                fprintf(g_dbglog, "[BLACKDIAG] ECRA PRETO: %d sprites, 0 desenhaveis | disposed=%d invisivel=%d bmp_nulo=%d bmp_disposed=%d tex_falhou=%d\n",
                        total, ds, inv, nb, db, txf);
                dbg_flush();
            }
        }
    }

    /* [GHOST-KILL v3 2026-07-03] "2 Gilberts"/estatua — agora com o discriminador
     * COMPLETO (o v1 matava o seguidor porque so' conhecia $game_map.events).
     * Arquitetura provada nos scripts do jogo (050_Game_DependentEvents.rb):
     *   - seguidor = clone Game_Event guardado em $PokemonTemp.dependentEvents.@realEvents
     *   - sprites do seguidor pertencem a DependentEventSprites (usersprites)
     *   - estatuas = sprites orfaos (spriteset/DES descartado sem dispose) OU
     *     sprites de clones ANTIGOS substituidos por pbEnsureEvent.
     * WHITELIST de legitimidade para um Sprite_Character cujo @character e'
     * EXATAMENTE Game_Event:
     *   (a) e' o proprio $MapFactory.maps[*].events[@id] (evento vivo de qualquer
     *       mapa ligado)  OU
     *   (b) esta' (identidade) em @realEvents (clone vivo do seguidor).
     * Fora da whitelist -> fantasma -> nao desenhar.
     * DUPLICADO do MESMO clone (2 sprites, 1 char — orfao de DES descartado):
     * ambos passam (b); desempate: fica o sprite cuja posicao bate com
     * screen_x/screen_y ATUAL do personagem (o orfao nunca mais foi atualizado);
     * se ambos batem (parados), fica o de indice maior (mais recente).
     * Fail-open em TODAS as leituras: na duvida, desenha. Kill-switch:
     * $MKXP_GHOST_KILL=false desliga (default LIGADO). */
    bool ghost_on = true;
    static mrb_sym gk_sy_ch=0, gk_sy_id=0, gk_sy_cmid=0, gk_sy_mid=0, gk_sy_ev=0,
                   gk_sy_maps=0, gk_sy_re=0, gk_sy_dep=0, gk_sy_sx=0, gk_sy_sy=0;
    struct GKMap { int map_id; mrb_value events; };
    static std::vector<GKMap> gk_maps;           /* mapas do factory (1-3) */
    static std::vector<void*> gk_realevs;        /* ptrs dos clones vivos */
    static std::vector<uint8_t> gk_suppress;     /* flag por indice de sprite */
    gk_maps.clear(); gk_realevs.clear();
    bool gk_world_ok = false;                    /* so' suprimir com mundo lido */
    if (g_mkxp_mrb) {
        mrb_state *mrb = g_mkxp_mrb;
        if (!gk_sy_ch) {
            gk_sy_ch=mrb_intern_lit(mrb,"@character"); gk_sy_id=mrb_intern_lit(mrb,"@id");
            gk_sy_cmid=mrb_intern_lit(mrb,"@map_id");  gk_sy_mid=mrb_intern_lit(mrb,"@map_id");
            gk_sy_ev=mrb_intern_lit(mrb,"@events");    gk_sy_maps=mrb_intern_lit(mrb,"@maps");
            gk_sy_re=mrb_intern_lit(mrb,"@realEvents"); gk_sy_dep=mrb_intern_lit(mrb,"@dependentEvents");
            gk_sy_sx=mrb_intern_lit(mrb,"screen_x");   gk_sy_sy=mrb_intern_lit(mrb,"screen_y");
        }
        mrb_value fv = mrb_gv_get(mrb, mrb_intern_lit(mrb,"$MKXP_GHOST_KILL"));
        if (!mrb_nil_p(fv) && !mrb_test(fv)) ghost_on = false;   /* =false desliga */
        if (ghost_on) {
            /* mapas ligados do factory */
            mrb_value mfac = mrb_gv_get(mrb, mrb_intern_lit(mrb,"$MapFactory"));
            if (!mrb_nil_p(mfac)) {
                mrb_value maps = mrb_iv_get(mrb, mfac, gk_sy_maps);
                if (mrb_array_p(maps)) {
                    for (mrb_int mi = 0; mi < RARRAY_LEN(maps); mi++) {
                        mrb_value m = mrb_ary_ref(mrb, maps, mi);
                        if (mrb_nil_p(m)) continue;
                        mrb_value mid = mrb_iv_get(mrb, m, gk_sy_mid);
                        mrb_value ev  = mrb_iv_get(mrb, m, gk_sy_ev);
                        if (mrb_fixnum_p(mid) && mrb_hash_p(ev)) {
                            GKMap gm; gm.map_id=(int)mrb_fixnum(mid); gm.events=ev;
                            gk_maps.push_back(gm);
                        }
                    }
                    gk_world_ok = !gk_maps.empty();
                }
            }
            /* clones vivos dos seguidores (whitelist por identidade) */
            mrb_value ptemp = mrb_gv_get(mrb, mrb_intern_lit(mrb,"$PokemonTemp"));
            if (!mrb_nil_p(ptemp)) {
                mrb_value dep = mrb_iv_get(mrb, ptemp, gk_sy_dep);
                if (!mrb_nil_p(dep)) {
                    mrb_value re = mrb_iv_get(mrb, dep, gk_sy_re);
                    if (mrb_array_p(re)) {
                        for (mrb_int ri = 0; ri < RARRAY_LEN(re); ri++) {
                            mrb_value rv = mrb_ary_ref(mrb, re, ri);
                            if (!mrb_nil_p(rv)) gk_realevs.push_back(mrb_obj_ptr(rv));
                        }
                    } else {
                        gk_world_ok = false;   /* dependentEvents existe mas ilegivel -> fail-open */
                    }
                }
            }
        }
    }

    /* passada de analise: fantasmas + duplicados do mesmo clone */
    gk_suppress.assign(g_sprites.size(), 0);
    if (ghost_on && gk_world_ok && g_mkxp_mrb) {
        mrb_state *mrb = g_mkxp_mrb;
        /* [PERF 2026-07-04] classe exata por COMPARACAO DE PONTEIRO em vez de
         * mrb_obj_classname+strcmp por sprite por frame (~150 lookups de nome).
         * Resolvido 1x por frame (2 hash lookups) -> robusto a RGSS_RESET. */
        struct RClass *gk_ge_cls = mrb_class_defined(mrb, "Game_Event")
                                 ? mrb_class_get(mrb, "Game_Event") : NULL;
        /* candidatos: sprites visiveis com @character==Game_Event exato */
        struct GKCand { size_t idx; void *chp; mrb_value ch; };
        static std::vector<GKCand> cands; cands.clear();
        if (gk_ge_cls)
        for (size_t i = 0; i < g_sprites.size(); i++) {
            Sprite3DS *s = g_sprites[i];
            if (!s || s->disposed || !s->visible || !s->bitmap || s->bitmap->disposed) continue;
            if (mrb_nil_p(s->rb_self)) continue;
            mrb_value ch = mrb_iv_get(mrb, s->rb_self, gk_sy_ch);
            if (mrb_nil_p(ch) || mrb_type(ch) != MRB_TT_OBJECT) continue;
            if (mrb_obj_class(mrb, ch) != gk_ge_cls) continue;   /* classe EXATA (salta singleton), sem lookup de nome */
            GKCand c; c.idx=i; c.chp=mrb_obj_ptr(ch); c.ch=ch;
            cands.push_back(c);
        }
        static mrb_sym gk_sy_erased = 0;
        if (!gk_sy_erased) gk_sy_erased = mrb_intern_lit(mrb, "@erased");
        for (size_t ci = 0; ci < cands.size(); ci++) {
            mrb_value ch = cands[ci].ch;
            void *chp = cands[ci].chp;
            /* [STATUE-SPR fix 2026-07-04] evento ERASED nunca desenha (semantica
             * RGSS). PROVADO no log: sprite com char='' @erased=true continuava
             * a desenhar o bitmap antigo (32x32) = a ESTATUA visivel do user.
             * O clone do seguidor nunca esta' erased (so' o original e') -> seguro. */
            {
                mrb_value er = mrb_iv_get(mrb, ch, gk_sy_erased);
                if (mrb_test(er)) {
                    gk_suppress[cands[ci].idx] = 1;
                    static unsigned gk_elog = 0;
                    if ((++gk_elog % 300) == 1 && g_dbglog) {
                        fprintf(g_dbglog, "[GHOST-KILL v3] sprite de evento ERASED suprimido @(%d,%d) (estatua visual)\n",
                                g_sprites[cands[ci].idx]->x, g_sprites[cands[ci].idx]->y);
                        dbg_flush();
                    }
                    continue;
                }
            }
            /* (b) clone vivo do seguidor? */
            bool in_real = false;
            for (size_t k = 0; k < gk_realevs.size(); k++)
                if (gk_realevs[k] == chp) { in_real = true; break; }
            bool legit = in_real;
            if (!legit) {
                /* (a) evento vivo do seu proprio mapa (qualquer mapa do factory)? */
                mrb_value cid  = mrb_iv_get(mrb, ch, gk_sy_id);
                mrb_value cmid = mrb_iv_get(mrb, ch, gk_sy_cmid);
                if (mrb_fixnum_p(cid) && mrb_fixnum_p(cmid)) {
                    int cm = (int)mrb_fixnum(cmid);
                    bool map_seen = false;
                    for (size_t mi2 = 0; mi2 < gk_maps.size(); mi2++) {
                        if (gk_maps[mi2].map_id != cm) continue;
                        map_seen = true;
                        mrb_value live = mrb_hash_get(mrb, gk_maps[mi2].events, cid);
                        if (!mrb_nil_p(live) && mrb_obj_ptr(live) == chp) legit = true;
                        break;
                    }
                    /* mapa do evento nem esta' no factory: sprite de um mundo que ja'
                     * nao existe -> fantasma. (map_seen=false tambem e' fantasma) */
                    (void)map_seen;
                } else {
                    legit = true;   /* ivars ilegiveis -> fail-open */
                }
            }
            if (!legit) {
                gk_suppress[cands[ci].idx] = 1;
                static unsigned gk_log = 0;
                if ((++gk_log % 120) == 1 && g_dbglog) {
                    mrb_value cid = mrb_iv_get(mrb, ch, gk_sy_id);
                    fprintf(g_dbglog, "[GHOST-KILL v3] fantasma ev%d suprimido @(%d,%d) (nao e' evento vivo nem clone em realEvents)\n",
                            mrb_fixnum_p(cid)?(int)mrb_fixnum(cid):-1,
                            g_sprites[cands[ci].idx]->x, g_sprites[cands[ci].idx]->y);
                    dbg_flush();
                }
            }
        }
        /* duplicados do MESMO clone legitimo (orfao de DES descartado): manter o
         * sprite cuja posicao == screen_x/screen_y atual; empate -> indice maior */
        for (size_t ci = 0; ci < cands.size(); ci++) {
            if (gk_suppress[cands[ci].idx]) continue;
            for (size_t cj = ci + 1; cj < cands.size(); cj++) {
                if (cands[cj].chp != cands[ci].chp || gk_suppress[cands[cj].idx]) continue;
                /* par (ci,cj) do mesmo personagem — resolver 1x */
                int sx = 0, sy = 0; bool pos_ok = false;
                mrb_value rx = mrb_funcall_argv(mrb, cands[ci].ch, gk_sy_sx, 0, NULL);
                if (!mrb->exc && mrb_fixnum_p(rx)) {
                    mrb_value ry = mrb_funcall_argv(mrb, cands[ci].ch, gk_sy_sy, 0, NULL);
                    if (!mrb->exc && mrb_fixnum_p(ry)) {
                        sx=(int)mrb_fixnum(rx); sy=(int)mrb_fixnum(ry); pos_ok=true;
                    }
                }
                if (mrb->exc) mrb->exc = 0;   /* fail-open */
                size_t win, lose;
                if (pos_ok) {
                    Sprite3DS *si = g_sprites[cands[ci].idx];
                    Sprite3DS *sj = g_sprites[cands[cj].idx];
                    bool i_ok = (si && si->x >= sx-1 && si->x <= sx+1 && si->y >= sy-1 && si->y <= sy+1);
                    bool j_ok = (sj && sj->x >= sx-1 && sj->x <= sx+1 && sj->y >= sy-1 && sj->y <= sy+1);
                    if (i_ok && !j_ok)      { win=cands[ci].idx; lose=cands[cj].idx; }
                    else if (j_ok && !i_ok) { win=cands[cj].idx; lose=cands[ci].idx; }
                    else                    { win=cands[cj].idx; lose=cands[ci].idx; }  /* empate: mais recente */
                } else {
                    win=cands[cj].idx; lose=cands[ci].idx;
                }
                (void)win;
                gk_suppress[lose] = 1;
                static unsigned gk_dlog = 0;
                if ((++gk_dlog % 120) == 1 && g_dbglog) {
                    fprintf(g_dbglog, "[GHOST-KILL v3] duplicado do mesmo clone: sprite copia suprimido (fica o atualizado)\n");
                    dbg_flush();
                }
            }
        }
    }

    for (size_t i = 0; i < g_sprites.size(); i++) {
        Sprite3DS *s = g_sprites[i];
        if (!s || s->disposed || !s->visible || !s->bitmap) continue;
        if (s->bitmap->disposed) continue;

        /* [GHOST-KILL v3] veredito da passada de analise */
        if (ghost_on && i < gk_suppress.size() && gk_suppress[i]) continue;

        /* FIX BUG 3: fazer upload sempre que tex_dirty=true OU tex=nullptr.
         * tex pode ser nullptr mesmo com tex_dirty=false se o bitmap foi
         * recriado externamente (ex: bmp_clear seguido de draw sem flush). */
        /* FIX FPS (texturas >1024 / VRAM esgotada): se a textura JA' falhou e os
         * pixels NAO mudaram (tex_dirty=false), NAO re-tentar -- senao tentava-se
         * criar a mesma textura impossivel TODOS os frames, com EVICT-thrashing +
         * C3D_TexInit em loop (provado: tex_fail subia 100/frame, 2.2 fps em mapas
         * com tilesets grandes 1696x704). Quando os pixels mudam, tex_dirty volta
         * a true e limpamos o flag para dar nova oportunidade. */
        /* FIX OUTFIT (preview do gerador de roupa vazio a' 1a vez, aparece a' 2a):
         * um bitmap cuja textura <=1024 falhou (tex_failed) por MEMORIA transitoria
         * -- nao por tamanho -- deve RE-TENTAR. Sem isto, um sprite estatico
         * construido logo apos a intro usar RAM fica VAZIO ate' ser recriado (so'
         * reaparece se o jogador voltar a trocar de roupa). Re-tenta a cada 30 frames:
         * custo desprezavel e, ao contrario dos tilesets >1024 (que falham SEMPRE ->
         * NAO re-tentamos, p/ evitar o EVICT-thrash de 2.2fps), um <=1024 falha so'
         * transitoriamente -> resolve logo na 1a re-tentativa. Cobre tambem pictures
         * com downscale (dims efetivas da textura = tex_w/tex_h <=1024). */
        {
            int eff_w = (s->bitmap->tex_scale > 1 && s->bitmap->tex_w > 0) ? s->bitmap->tex_w : s->bitmap->width;
            int eff_h = (s->bitmap->tex_scale > 1 && s->bitmap->tex_h > 0) ? s->bitmap->tex_h : s->bitmap->height;
            if (s->bitmap->tex_failed && !s->bitmap->tex && s->bitmap->pixels
                && eff_w > 0 && eff_w <= 1024 && eff_h > 0 && eff_h <= 1024
                && (s_spr_frame % 30) == 0) {
                s->bitmap->tex_failed = false;   /* nova oportunidade ao bloco de criacao abaixo */
            }
        }
        if (s->bitmap->tex_dirty) s->bitmap->tex_failed = false;
        if (s->bitmap->tex_failed && !s->bitmap->tex) continue;
        if (s->bitmap->tex_dirty || !s->bitmap->tex) {
            /* [FPS] CAMINHO RAPIDO anti-40ms: se a textura JA' existe com as mesmas
             * dims, update IN-PLACE (sem free/create/evict, SEM fechar o frame GPU).
             * Critico p/ o @layer0 do tilemap e autotiles animados (agua/FX), que
             * marcam tex_dirty TODOS os frames mantendo o tamanho -> era re-upload de
             * 4MB/frame. So' 8888 (4444 e' so' fonte). Falha -> cai no create (else). */
            int _fstw = (s->bitmap->tex_scale > 1 && s->bitmap->tex_w > 0) ? s->bitmap->tex_w : s->bitmap->width;
            int _fsth = (s->bitmap->tex_scale > 1 && s->bitmap->tex_h > 0) ? s->bitmap->tex_h : s->bitmap->height;
            bool _inplace = (s->bitmap->tex && s->bitmap->pixels && !s->bitmap->pixels_4444 &&
                             display_3ds_update_texture(s->bitmap->tex, _fstw, _fsth, s->bitmap->pixels));
            if (!_inplace && s->bitmap->tex) {
                display_3ds_free_texture(s->bitmap->tex);
                s->bitmap->tex = nullptr;
            }
            if (!_inplace && s->bitmap->pixels && s->bitmap->width > 0 && s->bitmap->height > 0) {
                /* FIX-PICDOWN: dims da TEXTURA = tex_w/tex_h quando a picture foi
                 * REDUZIDA (tex_scale>1); senao = width/height (caso normal, intocado).
                 * Sem isto, uma textura evictada seria recriada com as dims ORIGINAIS
                 * (1696) a partir de um buffer REDUZIDO (848x352) -> leitura fora do
                 * buffer / >1024 -> magenta outra vez. */
                int stw = (s->bitmap->tex_scale > 1 && s->bitmap->tex_w > 0) ? s->bitmap->tex_w : s->bitmap->width;
                int sth = (s->bitmap->tex_scale > 1 && s->bitmap->tex_h > 0) ? s->bitmap->tex_h : s->bitmap->height;
                s->bitmap->tex = display_3ds_create_texture(
                    stw, sth, s->bitmap->pixels);
                /* se a criacao falhou (nullptr), marcar para nao re-tentar todos
                 * os frames; so' se re-tenta quando os pixels mudarem (acima). */
                if (!s->bitmap->tex) s->bitmap->tex_failed = true;
                /* [EVICT] registar o slot do dono (&s->bitmap->tex) para eviccao
                 * segura: se esta textura for libertada por falta de heap linear,
                 * o campo fica nullptr e e' recriada a partir de pixels (em RAM). */
                display_3ds_set_texture_owner(s->bitmap->tex, &s->bitmap->tex);
                /* FIX BORRAO: este caminho (sprites) cria a textura DIRETAMENTE,
                 * sem passar pelo bmp_flush -> o filtro LINEAR p/ texto NUNCA era
                 * aplicado aqui. Resultado: o conteudo de texto das caixas (que e'
                 * um sprite) ficava sempre em NEAREST e virava papa no downscale
                 * 0.625. Replicamos a logica do bmp_flush: LINEAR (custo GPU zero)
                 * SO quando o bitmap teve draw_text (has_text); todo o resto
                 * (tiles, personagens, fundos) fica em NEAREST por omissao, nitido. */
                if (s->bitmap->tex && s->bitmap->has_text) {
                    display_3ds_set_texture_filter(s->bitmap->tex, true);
                    s->bitmap->tex->has_text = true;
                }
            }
            s->bitmap->tex_dirty = false;
        }
        if (!s->bitmap->tex || !s->bitmap->tex->valid) continue;

        /* ── FIX RASTO (sprite-sheet inteira desenhada) ─────────────────────
         * O RGSS atualiza o recorte de frame via `self.src_rect.set(x,y,w,h)`,
         * que modifica o objecto Rect Ruby IN-PLACE. Isso NAO chama o setter
         * C++ `src_rect=` (spr_set_src_rect), por isso a cache s->src_w/h ficava
         * no valor inicial (= bitmap inteiro). Resultado: a sprite-sheet de
         * caminhada (4 frames x 4 direcoes) aparecia TODA -> jogador, NPCs e
         * Pokemon repetidos/"com rasto".
         * Solucao: ler o @src_rect LIVE do Ruby aqui (custo ~uns ns por sprite).
         * So' sobrescreve se o Rect tiver @width/@height validos (>0); senao
         * mantem o que ja' la' estava. */
        if (g_mkxp_mrb && !mrb_nil_p(s->rb_self)) {
            static mrb_sym sy_sr = 0, sy_w = 0, sy_h = 0, sy_x = 0, sy_y = 0;
            if (sy_sr == 0) {
                sy_sr = mrb_intern_lit(g_mkxp_mrb, "@src_rect");
                sy_w  = mrb_intern_lit(g_mkxp_mrb, "@width");
                sy_h  = mrb_intern_lit(g_mkxp_mrb, "@height");
                sy_x  = mrb_intern_lit(g_mkxp_mrb, "@x");
                sy_y  = mrb_intern_lit(g_mkxp_mrb, "@y");
            }
            mrb_value ro = mrb_iv_get(g_mkxp_mrb, s->rb_self, sy_sr);
            if (!mrb_nil_p(ro)) {
                mrb_value vw = mrb_iv_get(g_mkxp_mrb, ro, sy_w);
                mrb_value vh = mrb_iv_get(g_mkxp_mrb, ro, sy_h);
                /* Aceita Fixnum E Float. Alguns plugins/efeitos poem floats no
                 * Rect; a versao anterior so' aceitava Fixnum (mrb_fixnum_p) e
                 * por isso saltava-os, deixando a folha inteira. NAO usar
                 * mrb_to_int aqui -- pode levantar excepcao dentro do loop de
                 * desenho. So' sobrescreve se ambos forem numericos e > 0. */
                int rw = -1, rh = -1;
                if      (mrb_fixnum_p(vw)) rw = (int)mrb_fixnum(vw);
                else if (mrb_float_p(vw))  rw = (int)mrb_float(vw);
                if      (mrb_fixnum_p(vh)) rh = (int)mrb_fixnum(vh);
                else if (mrb_float_p(vh))  rh = (int)mrb_float(vh);
                if (rw > 0 && rh > 0) {
                    mrb_value vx = mrb_iv_get(g_mkxp_mrb, ro, sy_x);
                    mrb_value vy = mrb_iv_get(g_mkxp_mrb, ro, sy_y);
                    s->src_w = rw;
                    s->src_h = rh;
                    s->src_x = mrb_fixnum_p(vx) ? (int)mrb_fixnum(vx)
                             : (mrb_float_p(vx)  ? (int)mrb_float(vx)  : 0);
                    s->src_y = mrb_fixnum_p(vy) ? (int)mrb_fixnum(vy)
                             : (mrb_float_p(vy)  ? (int)mrb_float(vy)  : 0);
                }
            }
        }

        int src_w = s->src_w > 0 ? s->src_w : s->bitmap->width;
        int src_h = s->src_h > 0 ? s->src_h : s->bitmap->height;

        /* ── FIX BUG 7 (quarto: sprite do JOGADOR repetida em grelha 4x4) ─────
         * Algumas sprites de personagem (tipicamente o jogador, gerido por um
         * plugin FORA de @character_sprites -> o census nao o ve) desenham a
         * FOLHA INTEIRA porque o recorte de frame via @src_rect nunca se aplica
         * (fica = bitmap inteiro). Pokemon/NPCs normais nao sofrem porque o
         * @src_rect deles fica correcto (src_w=64 != 256, logo nao entram aqui).
         * Prova (FRAMEDUMP map=3): #067 bmp=256x256 src=[0,0,256,256] mas com
         * origem o=(32,64) = de UM frame 64x64 -> a folha 4x4 aparece como
         * ~8-16 copias (exactamente a imagem do quarto).
         * Fallback RGSS-correcto: se uma sprite COM @character desenha a folha
         * toda de um charset QUADRADO (w==h, %4==0, >=128 => 4 frames x 4
         * direcoes de 64x64), recortamos aqui o frame a partir de
         * @character.@pattern/@direction -- tal como Sprite_Character#update
         * faria. So' actua na CONDICAO DO BUG (src cobre tudo); sprites ja'
         * recortadas (pokemon, npcs) nao sao tocadas. */
        if (g_mkxp_mrb && !mrb_nil_p(s->rb_self)
            && s->bitmap->width == s->bitmap->height
            && s->bitmap->width >= 128 && (s->bitmap->width % 4) == 0
            && src_w >= s->bitmap->width && src_h >= s->bitmap->height) {
            static mrb_sym sy_ch = 0, sy_pat = 0, sy_dir = 0, sy_ev = 0;
            if (sy_ch == 0) {
                sy_ch  = mrb_intern_lit(g_mkxp_mrb, "@character");
                sy_pat = mrb_intern_lit(g_mkxp_mrb, "@pattern");
                sy_dir = mrb_intern_lit(g_mkxp_mrb, "@direction");
                sy_ev  = mrb_intern_lit(g_mkxp_mrb, "@event");
            }
            /* Fonte de pattern/direction: @character -> @event -> o proprio sprite.
             * Antes so' tratava @character; reflexos/sombras (Overworld Shadows EX,
             * Sprite_Reflection) usam o charset mas guardam a ref noutro ivar -> a
             * folha inteira ia para o void. Agora recortamos SEMPRE; sem fonte,
             * frame (0,0) -- um sprite estatico, nunca a grelha 4x4. */
            mrb_value src = mrb_iv_get(g_mkxp_mrb, s->rb_self, sy_ch);
            if (mrb_nil_p(src)) src = mrb_iv_get(g_mkxp_mrb, s->rb_self, sy_ev);
            mrb_value powner = mrb_nil_p(src) ? s->rb_self : src;
            mrb_value vp = mrb_iv_get(g_mkxp_mrb, powner, sy_pat);
            mrb_value vd = mrb_iv_get(g_mkxp_mrb, powner, sy_dir);
            int pat = mrb_fixnum_p(vp) ? (int)mrb_fixnum(vp) : 0;
            int dir = mrb_fixnum_p(vd) ? (int)mrb_fixnum(vd) : 2;
            if (pat < 0) pat = 0; else if (pat > 3) pat = pat & 3;
            int row = (dir >= 2 && dir <= 8) ? ((dir - 2) / 2) : 0;
            int cw = s->bitmap->width  / 4;
            int ch = s->bitmap->height / 4;
            s->src_x = pat * cw; s->src_y = row * ch;
            s->src_w = cw;       s->src_h = ch;
            src_w = cw;          src_h = ch;
            static int s_gp = 0;
            if (s_gp++ < 12) {
                const char *cls = "?";
                struct RClass *kc = mrb_class(g_mkxp_mrb, s->rb_self);
                if (kc) cls = mrb_class_name(g_mkxp_mrb, kc);
                extern FILE *g_dbglog;
                if (g_dbglog) {
                    fprintf(g_dbglog, "[FIX-GRELHA7] folha recortada: classe=%s "
                        "bmp=%dx%d pat=%d dir=%d fonte=%s -> src=(%d,%d,%d,%d)\n",
                        cls, s->bitmap->width, s->bitmap->height, pat, dir,
                        (mrb_nil_p(src) ? "self" : "char/event"),
                        s->src_x, s->src_y, cw, ch);
                    dbg_flush();
                }
            }
        }

        /* [DIAG SETAS] Generico: deteta uma TIRA de animacao (sprite-sheet
         * horizontal) desenhada SEM recorte de frame (src_rect por definir) ->
         * todos os frames aparecem de uma vez. E' a causa mais provavel das
         * "4 setas ▼" (downarrow ~224x40 com ~4 frames). NAO altera o desenho;
         * regista 1x por dimensao unica para identificar o bitmap no log e
         * confirmar se vem com src_rect ou nao. */
        if (s->src_w <= 0 && s->bitmap->height > 0 && s->bitmap->height <= 64
                          && s->bitmap->width >= 3 * s->bitmap->height) {
            static int seenW[24] = {0}, seenH[24] = {0}, seenN = 0;
            bool seen = false;
            for (int i = 0; i < seenN; i++)
                if (seenW[i] == s->bitmap->width && seenH[i] == s->bitmap->height) { seen = true; break; }
            if (!seen && seenN < 24) {
                seenW[seenN] = s->bitmap->width; seenH[seenN] = s->bitmap->height; seenN++;
                printf("[SHEET?] tira sem src_rect: bmp=%dx%d desenhada INTEIRA -> "
                       "dst=%.0fx%.0f @(%.0f,%.0f) vis=%d op=%d (provavel multi-frame)\n",
                       s->bitmap->width, s->bitmap->height,
                       (double)(s->bitmap->width  * s->zoom_x),
                       (double)(s->bitmap->height * s->zoom_y),
                       (double)s->x, (double)s->y, s->visible ? 1 : 0, s->opacity);
            }
        }

        /* FIX BUG 5: tamanho de destino tem em conta zoom.
         * zoom_x/zoom_y por omissão são 1.0, por isso não afecta o caso normal.
         * draw_w/draw_h são as dimensões no ecra (pixels destino). */
        float draw_w = src_w * s->zoom_x;
        float draw_h = src_h * s->zoom_y;
        float alpha  = s->opacity / 255.0f;

        /* ox/oy são o ponto de origem do sprite -- scaled pelo zoom */
        float dx = (float)s->x - s->ox * s->zoom_x;
        float dy = (float)s->y - s->oy * s->zoom_y;

        /* FIX-PICDOWN: se a textura foi REDUZIDA (picture sobredimensionada >1024 ou
         * OOM), as coords de ORIGEM tem de ser em espaco da textura REAL (divididas
         * pelo fator de downscale). O DESTINO (draw_w/h) ja' usa as dims ORIGINAIS
         * reportadas (src_w/h vem de bitmap->width), por isso a textura menor estica
         * de volta ao tamanho certo. Bitmaps normais: tex_scale<=1 -> /1 -> sem efeito. */
        int tscale = (s->bitmap->tex_scale > 1) ? s->bitmap->tex_scale : 1;
        /* [DRAWCHK 2026-07-03] Um sprite VISIVEL, com bitmap com conteudo, que chega
         * aqui com tex=NULL desenha NADA (blit_ex faz early-return em t=NULL). E' a
         * unica causa restante da copa da arvore/preview do roupeiro invisiveis
         * (tudo o resto -- vis/opacity/src/z -- ja' esta' certo). Loga classe+dims+
         * porque (tex_failed/4444/dims) p/ nomear a raiz. Throttle. */
        if (!s->bitmap->tex && g_mkxp_mrb && !mrb_nil_p(s->rb_self)) {
            static unsigned dc = 0;
            if ((++dc % 90) == 1 && g_dbglog) {
                const char *cls = mrb_obj_classname(g_mkxp_mrb, s->rb_self);
                int cont = 0;
                if (s->bitmap->pixels) {
                    int mid = (s->bitmap->width * s->bitmap->height / 2) * 4;
                    if (mid + 3 < s->bitmap->width * s->bitmap->height * 4) cont = s->bitmap->pixels[mid+3];
                }
                fprintf(g_dbglog, "[DRAWCHK] VISIVEL sem textura: cls=%s bmp=%dx%d tex_failed=%d tex_dirty=%d 4444=%d alpha_centro=%d -> INVISIVEL\n",
                        cls?cls:"?", s->bitmap->width, s->bitmap->height,
                        s->bitmap->tex_failed?1:0, s->bitmap->tex_dirty?1:0,
                        s->bitmap->pixels_4444?1:0, cont);
                dbg_flush();
            }
        }
        /* [TILEDRAW 2026-07-03] diagnostico da arvore cortada — RESOLVIDO (TSC-REVERT,
         * veredito 8/8). [PERF 2026-07-04] Custava um mrb_obj_classname POR SPRITE POR
         * FRAME (~150+/frame) + fprintf/flush 1x/180f. Agora OPT-IN: so' corre com
         * $MKXP_DIAG_TILEDRAW=true. Zero custo no caminho normal (1 branch estatico). */
        {
            static int td_on = -1;   /* -1 = por ler; le' a flag UMA vez */
            if (td_on < 0 && g_mkxp_mrb) {
                mrb_value tv = mrb_gv_get(g_mkxp_mrb, mrb_intern_lit(g_mkxp_mrb, "$MKXP_DIAG_TILEDRAW"));
                td_on = (!mrb_nil_p(tv) && mrb_test(tv)) ? 1 : 0;
            }
            if (td_on == 1 && g_mkxp_mrb && s->bitmap->tex && !mrb_nil_p(s->rb_self)) {
                static unsigned td_f = 0; static int td_ct = 0, td_zmin = 999999, td_zmax = -999999;
                const char *tcls = mrb_obj_classname(g_mkxp_mrb, s->rb_self);
                if (tcls && strcmp(tcls, "CustomTilemapSprite") == 0 && s->z > 0) {
                    td_ct++;
                    if (s->z < td_zmin) td_zmin = s->z;
                    if (s->z > td_zmax) td_zmax = s->z;
                }
                if ((++td_f % 180) == 0 && g_dbglog) {
                    fprintf(g_dbglog, "[TILEDRAW] CustomTilemapSprite (prioridade) desenhados c/ textura: %d | z=[%d..%d]\n",
                            td_ct, td_ct ? td_zmin : 0, td_ct ? td_zmax : 0);
                    dbg_flush();
                    td_ct = 0; td_zmin = 999999; td_zmax = -999999;
                }
            }
        }
        display_3ds_blit_ex(s->bitmap->tex,
            dx, dy, draw_w, draw_h,
            (float)s->src_x / tscale, (float)s->src_y / tscale,
            (float)src_w / tscale, (float)src_h / tscale,
            alpha);
    }
}

/* DUMP-DE-FRAME (botao Y): despeja TODOS os sprites registados no frame actual,
 * na MESMA ordem em que sao desenhados (ja' ordenados por z acima). Para cada um:
 * classe Ruby, x/y/z/ox/oy, zoom, opacity, visible, has_vp/vp_z, dims do bitmap,
 * e a COR CENTRAL do bitmap (lida directamente dos pixels em RAM). Isto da' um
 * retrato completo do que esta' empilhado em cada pixel -- p/ identificar a
 * origem de artefactos (ex. axadrezado dourado do teclado) sem adivinhar.
 * Custo: so' corre quando o utilizador carrega no Y (nunca por-frame). */
void sprites_dump_frame(mrb_state *mrb, const char *motivo) {
    extern FILE *g_dbglog;
    if (!g_dbglog) return;
    fprintf(g_dbglog, "[FRAMEDUMP] ===== inicio (motivo=%s) total_sprites=%d =====\n",
            motivo ? motivo : "?", (int)g_sprites.size());
    int idx = 0;
    for (size_t i = 0; i < g_sprites.size(); i++) {
        Sprite3DS *s = g_sprites[i];
        if (!s) continue;
        /* classe Ruby */
        const char *cls = "?";
        if (!mrb_nil_p(s->rb_self)) {
            cls = mrb_obj_classname(mrb, s->rb_self);
            if (!cls) cls = "?";
        }
        /* dims do bitmap */
        int bw = 0, bh = 0;
        bool has_bmp = (s->bitmap && !s->bitmap->disposed && s->bitmap->pixels);
        if (has_bmp) { bw = s->bitmap->width; bh = s->bitmap->height; }
        /* cor central do bitmap (RAM) */
        int cr = -1, cg = -1, cb = -1, ca = -1;
        if (has_bmp && bw > 0 && bh > 0) {
            int cx = bw / 2, cy = bh / 2;
            long pi = ((long)cy * bw + cx) * 4;
            cr = s->bitmap->pixels[pi];
            cg = s->bitmap->pixels[pi+1];
            cb = s->bitmap->pixels[pi+2];
            ca = s->bitmap->pixels[pi+3];
        }
        /* destino real no ecra (mesma formula do blit) */
        int src_w = s->src_w > 0 ? s->src_w : bw;
        int src_h = s->src_h > 0 ? s->src_h : bh;
        float dx = (float)s->x - s->ox * s->zoom_x;
        float dy = (float)s->y - s->oy * s->zoom_y;
        fprintf(g_dbglog,
            "[FRAMEDUMP] #%03d %s bmp=%dx%d src=[%d,%d,%d,%d] xy=(%d,%d) o=(%d,%d) "
            "z=%d vpz=%d hvp=%d zoom=(%.3f,%.3f) op=%d vis=%d disp=%d "
            "dst=(%.0f,%.0f %0.fx%.0f) cxc=(%d,%d,%d,a%d)\n",
            idx++, cls, bw, bh,
            s->src_x, s->src_y, s->src_w, s->src_h,
            s->x, s->y, s->ox, s->oy,
            s->z, s->vp_z, s->has_vp ? 1 : 0,
            (double)s->zoom_x, (double)s->zoom_y,
            s->opacity, s->visible ? 1 : 0, s->disposed ? 1 : 0,
            (double)dx, (double)dy, (double)(src_w * s->zoom_x), (double)(src_h * s->zoom_y),
            cr, cg, cb, ca);
    }
    fprintf(g_dbglog, "[FRAMEDUMP] ===== fim =====\n");
    dbg_flush();
}

static mrb_value spr_init(mrb_state *mrb, mrb_value self) {
    /* P3: aceita viewport opcional. Antes era ignorado; agora guardamo-lo como
     * ivar @viewport E lemos o seu z para vp_z (chave primaria de ordenacao),
     * para que QUALQUER Sprite.new(viewport) -- nao so' via o setter -- fique
     * com a ordem correta (viewport.z, z), como no RGSS. */
    mrb_value vp = mrb_nil_value();
    mrb_get_args(mrb, "|o", &vp);
    Sprite3DS *s = (Sprite3DS*)calloc(1, sizeof(Sprite3DS));
    s->opacity = 255;
    s->zoom_x  = 1.0f;
    s->zoom_y  = 1.0f;
    s->visible = true;
    s->reg_seq = ++g_sprite_seq; /* [ZSORT-REGTIE] ordem de criacao p/ desempate no topo */
    s->rb_self = self; /* DUMP: guardar o objecto Ruby p/ ler classe/ivars no dump-Y */
    DATA_PTR(self)  = s;
    DATA_TYPE(self) = &Sprite3DSType;
    g_sprites.push_back(s);
    if (!mrb_nil_p(vp)) {
        mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@viewport"), vp);
        s->vp_z = read_viewport_z(mrb, vp);
        s->has_vp = true;
    }

    /* DIAGNÓSTICO: log nas primeiras 10 chamadas para confirmar que spr_init C++ está activo */
    {
        static int s_spr_init_count = 0;
        s_spr_init_count++;
        if (s_spr_init_count <= 10) {
            extern FILE *g_dbglog;
            if (g_dbglog) {
                fprintf(g_dbglog,
                    "[SPR|INIT] #%d spr_init C++ chamado: sprite=%p g_sprites.size=%d\n",
                    s_spr_init_count, (void*)s, (int)g_sprites.size());
                dbg_flush();
            }
        }
    }
    return self;
}

static mrb_value spr_dispose(mrb_state*, mrb_value self) {
    Sprite3DS *s = GET_SPR(self);
    if (s) s->disposed = true;
    return mrb_nil_value();
}
static mrb_value spr_disposed(mrb_state*, mrb_value self) {
    Sprite3DS *s = GET_SPR(self);
    return mrb_bool_value(!s || s->disposed);
}
static mrb_value spr_update(mrb_state*, mrb_value) { return mrb_nil_value(); }

/* ═══ [CHARBATCH 2026-07-04] Sprite_Character#update NATIVO (caso comum) ═══════
 * Substitui o corpo vanilla (061, ~45-55 dispatches + 10-12 setters C++ por
 * sprite por frame) por UMA chamada com ~30 mrb_iv_get. QUALQUER desvio do caso
 * comum devolve 0 -> o seam Ruby corre o corpo vanilla ORIGINAL via alias (zero
 * replica, zero drift). Semantica replicada linha-a-linha de 061 L94-169.
 * Retornos: 0=fallback vanilla | 1=tratado (seam faz SpriteAnimation.clear) |
 *           2=return imediato (gate should_update?, ANTES do super — como o vanilla)
 * modo: 1=aplicar | 2=verify (compara com o estado que o vanilla ACABOU de
 *       escrever, loga [CHARBATCH|DIFF], NAO altera nada).
 * Formulas x/y/z = as de essentials_accel_3ds.cpp (validadas ao pixel). */
extern "C" void *mkxp_etc_rect_ptr(mrb_state *mrb, mrb_value v);
extern "C" void *mkxp_etc_tone_ptr(mrb_state *mrb, mrb_value v);
static Bitmap3DS *resolve_bitmap(mrb_state *mrb, mrb_value bobj);   /* fwd (definido junto ao spr_set_bitmap) */
struct CBRect { int x, y, w, h; };
struct CBTone { float r, g, b, gray; };

static mrb_value spr_char_batch(mrb_state *mrb, mrb_value self) {
    mrb_int mode = 1;
    mrb_get_args(mrb, "|i", &mode);
    /* [FUNIL 2026-07-04] histograma de desqualificacao por __LINE__ (verify):
     * cobertura=0 sem isto era indiagnosticavel. Log 1x/900 entradas. */
    static unsigned long cbq_line[16], cbq_cnt[16]; static unsigned long cbq_in = 0;
    if ((++cbq_in % 900) == 0) {     /* [2026-07-04e] funil tambem em modo 1 (a rua desqualifica muito) */
        extern FILE *g_dbglog;
        if (g_dbglog) {
            fprintf(g_dbglog, "[CHARBATCH|FUNIL] entradas=%lu |", cbq_in);
            for (int qi = 0; qi < 16 && cbq_line[qi]; qi++)
                fprintf(g_dbglog, " L%lu:%lu", cbq_line[qi], cbq_cnt[qi]);
            fprintf(g_dbglog, "\n");
            dbg_flush();
        }
    }
#define CB_FAIL0() do { { unsigned long __l = (unsigned long)__LINE__; \
        for (int __i = 0; __i < 16; __i++) { \
            if (cbq_line[__i] == __l) { cbq_cnt[__i]++; break; } \
            if (cbq_line[__i] == 0) { cbq_line[__i] = __l; cbq_cnt[__i] = 1; break; } } } \
        return mrb_fixnum_value(0); } while (0)
    Sprite3DS *s = GET_SPR(self);
    if (!s || s->disposed) CB_FAIL0();

    static mrb_sym sy_char=0, sy_toupd=0, sy_refl=0, sy_surf=0, sy_tile=0, sy_cname=0,
        sy_chue=0, sy_obush=0, sy_bushd=0, sy_animid=0, sy_jc=0, sy_jdl=0, sy_transp=0,
        sy_pat=0, sy_dir=0, sy_cw=0, sy_chh=0, sy_soff=0, sy_bob=0, sy_op=0, sy_bt=0,
        sy_cbmp=0, sy_cban=0, sy_ibmp=0, sy_frames=0, sy_curf=0, sy_wdur=0, sy_adur=0,
        sy_edur=0, sy_cdur=0, sy_ddur=0, sy_blink=0, sy_anims=0, sy_lanims=0,
        sy_tone=0, sy_srcr=0, sy_realx=0, sy_realy=0, sy_dispx=0, sy_dispy=0,
        sy_mapiv=0, sy_w=0, sy_aot=0, sy_event=0, sy_name=0, sy_rtone=0, sy_ctile=0;
    if (!sy_char) {
        sy_char=mrb_intern_lit(mrb,"@character"); sy_toupd=mrb_intern_lit(mrb,"@to_update");
        sy_refl=mrb_intern_lit(mrb,"@reflection"); sy_surf=mrb_intern_lit(mrb,"@surfbase");
        sy_tile=mrb_intern_lit(mrb,"@tile_id"); sy_cname=mrb_intern_lit(mrb,"@character_name");
        sy_chue=mrb_intern_lit(mrb,"@character_hue"); sy_obush=mrb_intern_lit(mrb,"@oldbushdepth");
        sy_bushd=mrb_intern_lit(mrb,"@bush_depth"); sy_animid=mrb_intern_lit(mrb,"@animation_id");
        sy_jc=mrb_intern_lit(mrb,"@jump_count"); sy_jdl=mrb_intern_lit(mrb,"@jump_distance_left");
        sy_transp=mrb_intern_lit(mrb,"@transparent"); sy_pat=mrb_intern_lit(mrb,"@pattern");
        sy_dir=mrb_intern_lit(mrb,"@direction"); sy_cw=mrb_intern_lit(mrb,"@cw");
        sy_chh=mrb_intern_lit(mrb,"@ch"); sy_soff=mrb_intern_lit(mrb,"@spriteoffset");
        sy_bob=mrb_intern_lit(mrb,"@bob_height"); sy_op=mrb_intern_lit(mrb,"@opacity");
        sy_bt=mrb_intern_lit(mrb,"@blend_type"); sy_cbmp=mrb_intern_lit(mrb,"@charbitmap");
        sy_cban=mrb_intern_lit(mrb,"@charbitmapAnimated"); sy_ibmp=mrb_intern_lit(mrb,"@bitmap");
        sy_frames=mrb_intern_lit(mrb,"@frames"); sy_curf=mrb_intern_lit(mrb,"@currentFrame");
        sy_wdur=mrb_intern_lit(mrb,"@_whiten_duration"); sy_adur=mrb_intern_lit(mrb,"@_appear_duration");
        sy_edur=mrb_intern_lit(mrb,"@_escape_duration"); sy_cdur=mrb_intern_lit(mrb,"@_collapse_duration");
        sy_ddur=mrb_intern_lit(mrb,"@_damage_duration"); sy_blink=mrb_intern_lit(mrb,"@_blink");
        sy_anims=mrb_intern_lit(mrb,"@animations"); sy_lanims=mrb_intern_lit(mrb,"@loopAnimations");
        sy_tone=mrb_intern_lit(mrb,"@tone"); sy_srcr=mrb_intern_lit(mrb,"@src_rect");
        sy_realx=mrb_intern_lit(mrb,"@real_x"); sy_realy=mrb_intern_lit(mrb,"@real_y");
        sy_dispx=mrb_intern_lit(mrb,"@display_x"); sy_dispy=mrb_intern_lit(mrb,"@display_y");
        sy_mapiv=mrb_intern_lit(mrb,"@map"); sy_w=mrb_intern_lit(mrb,"@width");
        sy_aot=mrb_intern_lit(mrb,"@always_on_top"); sy_event=mrb_intern_lit(mrb,"@event");
        sy_name=mrb_intern_lit(mrb,"@name"); sy_rtone=mrb_intern_lit(mrb,"@__cb_rtone");
        sy_ctile=mrb_intern_lit(mrb,"@tile_id");
    }

    mrb_value ch = mrb_iv_get(mrb, self, sy_char);
    if (mrb_nil_p(ch) || mrb_type(ch) != MRB_TT_OBJECT) CB_FAIL0();

    /* gate L95 (ANTES do super, como o vanilla): Game_Event (is_a?, inclui
     * subclasses) com @to_update falso -> return imediato */
    struct RClass *ge_cls = mrb_class_defined(mrb, "Game_Event")
                          ? mrb_class_get(mrb, "Game_Event") : NULL;
    bool is_event = ge_cls && mrb_obj_is_kind_of(mrb, ch, ge_cls);
    if (is_event && !mrb_test(mrb_iv_get(mrb, ch, sy_toupd)))
        return mrb_fixnum_value(2);

    /* ── DESQUALIFICADORES (qualquer um -> 0 = corpo vanilla) ─────────────── */
    /* [REFL 2026-07-04] @reflection existe em ~92% dos sprites deste jogo (plugin
     * de reflexos) — desqualificar matava a cobertura. Semantica exata do vanilla:
     * o corpo corre e no FIM chama @reflection.update/@surfbase.update (funcall,
     * so' no APPLY; no verify o vanilla ja' os chamou). */
    mrb_value cb_refl = mrb_iv_get(mrb, self, sy_refl);
    mrb_value cb_surf = mrb_iv_get(mrb, self, sy_surf);
    /* estado RPG::Sprite ativo (super teria trabalho a fazer) */
    {
        static const int NDUR = 5;
        mrb_sym durs[NDUR] = { sy_wdur, sy_adur, sy_edur, sy_cdur, sy_ddur };
        for (int i = 0; i < NDUR; i++) {
            mrb_value d = mrb_iv_get(mrb, self, durs[i]);
            if (mrb_fixnum_p(d) && mrb_fixnum(d) > 0) CB_FAIL0();
        }
        if (mrb_test(mrb_iv_get(mrb, self, sy_blink))) CB_FAIL0();
        mrb_value an = mrb_iv_get(mrb, self, sy_anims);
        if (mrb_array_p(an) && RARRAY_LEN(an) > 0) CB_FAIL0();
        mrb_value ln = mrb_iv_get(mrb, self, sy_lanims);
        if (mrb_array_p(ln) && RARRAY_LEN(ln) > 0) CB_FAIL0();
    }
    /* change-check (L97-100): qualquer mudanca -> vanilla (rebuild de bitmap) */
    bool cb_is_tile = false;
    {
        mrb_value st = mrb_iv_get(mrb, self, sy_tile);
        mrb_value ct = mrb_iv_get(mrb, ch,   sy_ctile);
        if (!mrb_fixnum_p(st) || !mrb_fixnum_p(ct) ||
            mrb_fixnum(st) != mrb_fixnum(ct)) CB_FAIL0();
        cb_is_tile = (mrb_fixnum(st) != 0);
        /* [TILESPR 2026-07-04] tiles (sinais/objetos de cenario) sao 40% das
         * entradas na rua — steady-state coberto: sem src/oy (fixos no setup),
         * bitmap=@charbitmap direto, z=ground+prio*32. */
        /* (is_tile decidido abaixo; mudanca de tile_id ja' caiu no check acima) */
        mrb_value sn = mrb_iv_get(mrb, self, sy_cname);
        mrb_value cn = mrb_iv_get(mrb, ch,   sy_cname);
        if (!mrb_string_p(sn) || !mrb_string_p(cn)) CB_FAIL0();
        if (RSTRING_LEN(sn) != RSTRING_LEN(cn) ||
            memcmp(RSTRING_PTR(sn), RSTRING_PTR(cn), RSTRING_LEN(sn)) != 0)
            CB_FAIL0();
        mrb_value sh = mrb_iv_get(mrb, self, sy_chue);
        mrb_value chv = mrb_iv_get(mrb, ch,  sy_chue);
        if (!mrb_obj_eq(mrb, sh, chv)) CB_FAIL0();
        /* bush: character.bush_depth = @bush_depth||0; sprite @oldbushdepth */
        mrb_value cb = mrb_iv_get(mrb, ch, sy_bushd);
        long cbush = mrb_fixnum_p(cb) ? (long)mrb_fixnum(cb) : 0;
        mrb_value ob = mrb_iv_get(mrb, self, sy_obush);
        long obush = mrb_fixnum_p(ob) ? (long)mrb_fixnum(ob) : 0;
        if (cbush != obush) CB_FAIL0();
        if (cbush != 0) CB_FAIL0();            /* bush -> vanilla */
    }
    /* salto / animation_id / bob != 0 -> vanilla */
    {
        mrb_value jc = mrb_iv_get(mrb, ch, sy_jc);
        if (mrb_fixnum_p(jc) && mrb_fixnum(jc) > 0) CB_FAIL0();
        mrb_value jdl = mrb_iv_get(mrb, ch, sy_jdl);
        if ((mrb_fixnum_p(jdl) && mrb_fixnum(jdl) > 0) ||
            (mrb_float_p(jdl) && mrb_float(jdl) > 0.0)) CB_FAIL0();
        mrb_value ai = mrb_iv_get(mrb, ch, sy_animid);
        if (mrb_fixnum_p(ai) && mrb_fixnum(ai) != 0) CB_FAIL0();
        mrb_value bb = mrb_iv_get(mrb, ch, sy_bob);
        if (!mrb_nil_p(bb) && !(mrb_fixnum_p(bb) && mrb_fixnum(bb) == 0))
            CB_FAIL0();
    }
    /* aot coberto: z=999 fixo (formula exata 045) */
    bool cb_aot = mrb_test(mrb_iv_get(mrb, ch, sy_aot));
    /* visivel: so' o caso SEM mudanca (mudanca tem de correr a cadeia visible=
     * dos plugins: bolhas/sombras) */
    bool newvis;
    {
        mrb_value tr = mrb_iv_get(mrb, ch, sy_transp);
        newvis = !mrb_test(tr);
        if (s->visible != newvis) CB_FAIL0();
    }
    /* bitmap: tiles usam @charbitmap DIRETO (nao-animado); charsets vao por
     * AnimatedBitmap -> PngAnimatedBitmap/GifBitmap */
    Bitmap3DS *frame_bmp; mrb_value frame_val;
    if (cb_is_tile) {
        frame_val = mrb_iv_get(mrb, self, sy_cbmp);
        if (mrb_nil_p(frame_val)) CB_FAIL0();
        frame_bmp = (mrb_type(frame_val) == MRB_TT_DATA) ? (Bitmap3DS*)DATA_PTR(frame_val) : NULL;
        if (!frame_bmp) frame_bmp = resolve_bitmap(mrb, frame_val);
        if (!frame_bmp || frame_bmp->disposed) CB_FAIL0();
    } else {
        mrb_value cban = mrb_iv_get(mrb, self, sy_cban);
        if (!mrb_test(cban)) CB_FAIL0();
        mrb_value ab = mrb_iv_get(mrb, self, sy_cbmp);
        if (mrb_nil_p(ab) || mrb_type(ab) != MRB_TT_OBJECT) CB_FAIL0();
        mrb_value pb = mrb_iv_get(mrb, ab, sy_ibmp);
        if (mrb_nil_p(pb) || mrb_type(pb) != MRB_TT_OBJECT) CB_FAIL0();
        static struct RClass *png_cls = NULL, *gif_cls = NULL;
        if (!png_cls && mrb_class_defined(mrb, "PngAnimatedBitmap"))
            png_cls = mrb_class_get(mrb, "PngAnimatedBitmap");
        if (!gif_cls && mrb_class_defined(mrb, "GifBitmap"))
            gif_cls = mrb_class_get(mrb, "GifBitmap");
        struct RClass *pbc = mrb_obj_class(mrb, pb);
        if (png_cls && pbc == png_cls) {
            mrb_value fr = mrb_iv_get(mrb, pb, sy_frames);
            if (!mrb_array_p(fr) || RARRAY_LEN(fr) < 1) CB_FAIL0();
            mrb_int fr_len = RARRAY_LEN(fr);
            mrb_int fr_cur = 0;
            if (fr_len > 1) {
                /* [MULTIFRAME] replica 086 PngAnimatedBitmap#update em ivars.
                 * VERIFY (mode 2): o vanilla JA' avancou -> so' LER @currentFrame.
                 * APPLY (mode 1): somos o dono do update -> mutar exatamente igual. */
                mrb_value fc = mrb_iv_get(mrb, pb, sy_curf);
                mrb_value fn = mrb_iv_get(mrb, pb, mrb_intern_lit(mrb, "@framecount"));
                mrb_value fd = mrb_iv_get(mrb, pb, mrb_intern_lit(mrb, "@frameDelay"));
                if (!mrb_fixnum_p(fc) || !mrb_fixnum_p(fn) || !mrb_fixnum_p(fd)) CB_FAIL0();
                fr_cur = mrb_fixnum(fc);
                if (mode != 2) {
                    mrb_int nfn = mrb_fixnum(fn) + 1;
                    if (nfn >= mrb_fixnum(fd)) {
                        nfn = 0;
                        fr_cur = (fr_cur + 1) % fr_len;
                        mrb_iv_set(mrb, pb, sy_curf, mrb_fixnum_value(fr_cur));
                    }
                    mrb_iv_set(mrb, pb, mrb_intern_lit(mrb, "@framecount"), mrb_fixnum_value(nfn));
                }
                if (fr_cur < 0 || fr_cur >= fr_len) CB_FAIL0();
            }
            frame_val = mrb_ary_ref(mrb, fr, fr_cur);
        } else if (gif_cls && pbc == gif_cls) {
            /* [GIFBMP 2026-07-04] o caso COMUM dos charsets ('trchar000' etc, sem
             * [N] no nome): GifBitmap wrapa 1 bitmap estavel e o seu #update e'
             * NO-OP explicito ("Gifs are animated automatically... does nothing")
             * -> frame = @bitmap, sem replica nenhuma. */
            frame_val = mrb_iv_get(mrb, pb, sy_ibmp);
            if (mrb_nil_p(frame_val)) CB_FAIL0();
        } else {
            CB_FAIL0();
        }
        frame_bmp = (mrb_type(frame_val) == MRB_TT_DATA) ? (Bitmap3DS*)DATA_PTR(frame_val) : NULL;
        if (!frame_bmp) frame_bmp = resolve_bitmap(mrb, frame_val);   /* wrappers */
        if (!frame_bmp || frame_bmp->disposed) CB_FAIL0();
        (void)sy_curf;
    }
    /* cena: pbDayNightTint so' age em Scene_Map -> fora dela, vanilla */
    {
        mrb_value scn = mrb_gv_get(mrb, mrb_intern_lit(mrb, "$scene"));
        static struct RClass *sm_cls = NULL;
        if (!sm_cls && mrb_class_defined(mrb, "Scene_Map"))
            sm_cls = mrb_class_get(mrb, "Scene_Map");
        if (!sm_cls || mrb_nil_p(scn) || !mrb_obj_is_kind_of(mrb, scn, sm_cls))
            CB_FAIL0();
    }
    /* rect/tone: neste runtime os Rect/Tone vivos sao RUBY-PUROS (o initialize
     * do compat_stubs sobrepoe o nativo -> instancias com ivars @x/@width e
     * @red/@gray, sem struct C). Provado por cobertura=0 quando so' aceitavamos
     * TT_DATA. Suporte DUPLO: struct C se existir, senao ivars (o mesmo caminho
     * que o extract_rect do motor usa). @tone nil -> vanilla (getter cria-o). */
    static mrb_sym sy_rx=0, sy_ry=0, sy_rw=0, sy_rh=0, sy_tr=0, sy_tg=0, sy_tb=0, sy_tgr=0;
    if (!sy_rx) {
        sy_rx=mrb_intern_lit(mrb,"@x"); sy_ry=mrb_intern_lit(mrb,"@y");
        sy_rw=mrb_intern_lit(mrb,"@width"); sy_rh=mrb_intern_lit(mrb,"@height");
        sy_tr=mrb_intern_lit(mrb,"@red"); sy_tg=mrb_intern_lit(mrb,"@green");
        sy_tb=mrb_intern_lit(mrb,"@blue"); sy_tgr=mrb_intern_lit(mrb,"@gray");
    }
    CBRect *rc; CBTone *tn;
    mrb_value ro, to;
    bool rc_iv = false, tn_iv = false;
    {
        ro = mrb_nil_value();
        if (!cb_is_tile) {
        ro = mrb_iv_get(mrb, self, sy_srcr);
        if (mrb_nil_p(ro)) CB_FAIL0();
        /* IVARS PRIMEIRO (a mesma fonte de verdade do live-read/extract_rect do
         * motor): ha' Rects de classe TT_DATA cujo struct C existe mas esta'
         * DESSINCRONIZADO (o set do jogo e' o Ruby, escreve ivars). O struct C
         * so' e' fonte quando NAO ha' ivars. No apply escrevemos AMBOS. */
        rc = (CBRect*)mkxp_etc_rect_ptr(mrb, ro);
        if (mrb_fixnum_p(mrb_iv_get(mrb, ro, sy_rx))) {
            rc_iv = true;             /* ivars presentes -> mandam */
        } else if (!rc) {
            CB_FAIL0();
        }
        }   /* fim !cb_is_tile (rect) */
        /* tone: o vanilla so' tinge `if self.visible` -> para sprites INVISIVEIS
         * o @tone pode nem existir e NAO deve ser tocado (43% das entradas no
         * funil eram @tone-nil de invisiveis). So' exigimos/escrevemos com
         * newvis==true. */
        to = mrb_nil_value(); tn = NULL;
        if (newvis) {
            to = mrb_iv_get(mrb, self, sy_tone);
            if (mrb_nil_p(to)) {
                /* @tone lazy: criar como o vanilla cria (getter .tone) — 1 funcall
                 * so' na 1a vez por sprite; depois o iv_get acerta sempre. */
                to = mrb_funcall(mrb, self, "tone", 0);
                if (mrb->exc) { mrb->exc = 0; CB_FAIL0(); }
                if (mrb_nil_p(to)) CB_FAIL0();
            }
            tn = (CBTone*)mkxp_etc_tone_ptr(mrb, to);
            {
                mrb_value tv0 = mrb_iv_get(mrb, to, sy_tr);
                if (mrb_float_p(tv0) || mrb_fixnum_p(tv0)) tn_iv = true;  /* ivars mandam */
                else if (!tn) CB_FAIL0();
            }
        }
    }

    /* ── CALCULOS (semantica exata do vanilla) ─────────────────────────────── */
    mrb_value cw_v = mrb_iv_get(mrb, self, sy_cw);
    mrb_value ch_v = mrb_iv_get(mrb, self, sy_chh);
    if (!mrb_fixnum_p(cw_v) || !mrb_fixnum_p(ch_v)) CB_FAIL0();
    const long cw = (long)mrb_fixnum(cw_v), chh = (long)mrb_fixnum(ch_v);
    long sx = 0, sy = 0, oy = 0, cb_pat = 0, cb_dir = 0;
    if (!cb_is_tile) {
        mrb_value pat_v = mrb_iv_get(mrb, ch, sy_pat);
        mrb_value dir_v = mrb_iv_get(mrb, ch, sy_dir);
        if (!mrb_fixnum_p(pat_v) || !mrb_fixnum_p(dir_v)) CB_FAIL0();
        cb_pat = (long)mrb_fixnum(pat_v); cb_dir = (long)mrb_fixnum(dir_v);
        sx = cb_pat * cw;
        sy = ((cb_dir - 2) / 2) * chh;
        oy = mrb_test(mrb_iv_get(mrb, self, sy_soff)) ? (chh - 16) : chh;
    }

    /* x/y/z (formulas do accel, validadas ao pixel) */
    long scr_x, scr_y, scr_z;
    {
        mrb_value map = mrb_iv_get(mrb, ch, sy_mapiv);
        if (mrb_nil_p(map)) map = mrb_gv_get(mrb, mrb_intern_lit(mrb, "$game_map"));
        if (mrb_nil_p(map)) CB_FAIL0();
        mrb_value rx = mrb_iv_get(mrb, ch, sy_realx);
        mrb_value ry = mrb_iv_get(mrb, ch, sy_realy);
        mrb_value dx = mrb_iv_get(mrb, map, sy_dispx);
        mrb_value dy = mrb_iv_get(mrb, map, sy_dispy);
        /* div_sub replica ((real-disp)/4).round: int/int -> floor; float -> round */
        long gx, gy;
        if (mrb_fixnum_p(rx) && mrb_fixnum_p(dx)) {
            long d = (long)mrb_fixnum(rx) - (long)mrb_fixnum(dx);
            gx = (long)floor((double)d / 4.0);
        } else {
            double d = (mrb_fixnum_p(rx)?(double)mrb_fixnum(rx):mrb_float_p(rx)?(double)mrb_float(rx):0.0)
                     - (mrb_fixnum_p(dx)?(double)mrb_fixnum(dx):mrb_float_p(dx)?(double)mrb_float(dx):0.0);
            gx = (long)round(d / 4.0);
        }
        if (mrb_fixnum_p(ry) && mrb_fixnum_p(dy)) {
            long d = (long)mrb_fixnum(ry) - (long)mrb_fixnum(dy);
            gy = (long)floor((double)d / 4.0);
        } else {
            double d = (mrb_fixnum_p(ry)?(double)mrb_fixnum(ry):mrb_float_p(ry)?(double)mrb_float(ry):0.0)
                     - (mrb_fixnum_p(dy)?(double)mrb_fixnum(dy):mrb_float_p(dy)?(double)mrb_float(dy):0.0);
            gy = (long)round(d / 4.0);
        }
        mrb_value wv = mrb_iv_get(mrb, ch, sy_w);
        long w = mrb_fixnum_p(wv) ? (long)mrb_fixnum(wv) : 1;
        scr_x = gx + w * 32 / 2;
        scr_y = gy + 32;
        if (cb_aot) {
            scr_z = 999;                       /* 045: return 999 if @always_on_top */
        } else if (cb_is_tile) {
            /* 045: z = ground + map.priorities[@tile_id]*32 (raise se nil -> fallback) */
            mrb_value prt = mrb_iv_get(mrb, map, mrb_intern_lit(mrb, "@priorities"));
            if (mrb_nil_p(prt)) CB_FAIL0();
            mrb_value prd = mrb_iv_get(mrb, prt, mrb_intern_lit(mrb, "@data"));
            mrb_value ctv = mrb_iv_get(mrb, ch, sy_ctile);
            long ctid = mrb_fixnum_p(ctv) ? (long)mrb_fixnum(ctv) : -1;
            if (!mrb_array_p(prd) || ctid < 0 || ctid >= RARRAY_LEN(prd)) CB_FAIL0();
            mrb_value pv = mrb_ary_ref(mrb, prd, ctid);
            if (!mrb_fixnum_p(pv)) CB_FAIL0();  /* nil -> vanilla raise (identico) */
            scr_z = scr_y + (long)mrb_fixnum(pv) * 32;
        } else {
            scr_z = scr_y + ((chh > 32) ? 31 : 0);
        }
    }
    /* opacity/blend */
    mrb_value opv = mrb_iv_get(mrb, ch, sy_op);
    mrb_value btv = mrb_iv_get(mrb, ch, sy_bt);
    if (!mrb_fixnum_p(opv) || !mrb_fixnum_p(btv)) CB_FAIL0();
    const long opc = (long)mrb_fixnum(opv), blt2 = (long)mrb_fixnum(btv);

    /* tone: regulartone (memoizado por sprite) OU cache do pbDayNightTint —
     * SO' quando visivel (o vanilla salta o tint por completo se invisivel) */
    float tr_ = 0, tg_ = 0, tb_ = 0, tgray_ = 0;
    if (newvis) {
        mrb_value rt = mrb_iv_get(mrb, self, sy_rtone);
        int is_rt;
        if (mrb_nil_p(rt)) {
            is_rt = 0;
            if (is_event) {
                mrb_value ev = mrb_iv_get(mrb, ch, sy_event);
                if (!mrb_nil_p(ev)) {
                    mrb_value nm = mrb_iv_get(mrb, ev, sy_name);
                    if (mrb_string_p(nm)) {
                        /* /regulartone/i = substring case-insensitive */
                        const char *p = RSTRING_PTR(nm); mrb_int L = RSTRING_LEN(nm);
                        static const char pat[] = "regulartone";
                        for (mrb_int i = 0; i + 11 <= L; i++) {
                            int hit = 1;
                            for (int k = 0; k < 11; k++) {
                                char c = p[i+k]; if (c >= 'A' && c <= 'Z') c += 32;
                                if (c != pat[k]) { hit = 0; break; }
                            }
                            if (hit) { is_rt = 1; break; }
                        }
                    }
                }
            }
            mrb_iv_set(mrb, self, sy_rtone, mrb_fixnum_value(is_rt));
        } else {
            is_rt = (int)mrb_fixnum(rt);
        }
        if (!is_rt) {
            mrb_value ap = mrb_gv_get(mrb, mrb_intern_lit(mrb, "$__dnt_apply"));
            mrb_value tv = mrb_gv_get(mrb, mrb_intern_lit(mrb, "$__dnt_tone"));
            if (mrb_test(ap) && mrb_array_p(tv) && RARRAY_LEN(tv) >= 4) {
                mrb_value e0=mrb_ary_ref(mrb,tv,0), e1=mrb_ary_ref(mrb,tv,1),
                          e2=mrb_ary_ref(mrb,tv,2), e3=mrb_ary_ref(mrb,tv,3);
                tr_   = mrb_float_p(e0)?(float)mrb_float(e0):mrb_fixnum_p(e0)?(float)mrb_fixnum(e0):0;
                tg_   = mrb_float_p(e1)?(float)mrb_float(e1):mrb_fixnum_p(e1)?(float)mrb_fixnum(e1):0;
                tb_   = mrb_float_p(e2)?(float)mrb_float(e2):mrb_fixnum_p(e2)?(float)mrb_fixnum(e2):0;
                tgray_= mrb_float_p(e3)?(float)mrb_float(e3):mrb_fixnum_p(e3)?(float)mrb_fixnum(e3):0;
            }
        }
    }

    if (mode == 2) {
        /* VERIFY: comparar com o que o vanilla ACABOU de escrever (nao altera nada).
         * [COBERTURA 2026-07-04] contadores: sem eles, DIFF=0 podia so' significar
         * "tudo desqualificado antes de comparar". Log 1x/900 comparacoes. */
        static unsigned diff_log = 0;
        static unsigned long cb_cmp = 0, cb_ok = 0, cb_diff = 0;
        /* NOTA: s->src_* NAO entra na igualdade — apos o vanilla fica o valor
         * INTERMEDIO do bitmap= (folha inteira); o recorte real vive nos ivars
         * do Rect e o live-read do draw aplica-o. Comparamos o Rect vivo abaixo. */
        bool ok = (s->x == (int)scr_x) && (s->y == (int)scr_y) && (s->z == (int)scr_z) &&
                  (s->opacity == (int)opc) && (s->blend_type == (int)blt2) &&
                  (s->bitmap == frame_bmp) && (s->visible == newvis);
        if (!cb_is_tile) ok = ok && (s->oy == (int)oy);
        /* rect: comparar via struct C OU ivars (conforme o tipo do objeto vivo) */
        if (cb_is_tile) { /* tiles nao tocam o rect */ }
        else if (rc_iv) {
            mrb_value vx = mrb_iv_get(mrb, ro, sy_rx), vy = mrb_iv_get(mrb, ro, sy_ry);
            mrb_value vw = mrb_iv_get(mrb, ro, sy_rw), vh = mrb_iv_get(mrb, ro, sy_rh);
            bool r_ok = mrb_fixnum_p(vx) && mrb_fixnum(vx) == (mrb_int)sx &&
                        mrb_fixnum_p(vy) && mrb_fixnum(vy) == (mrb_int)sy &&
                        mrb_fixnum_p(vw) && mrb_fixnum(vw) == (mrb_int)cw &&
                        mrb_fixnum_p(vh) && mrb_fixnum(vh) == (mrb_int)chh;
            ok = ok && r_ok;
        } else {
            ok = ok && (rc->x == (int)sx && rc->y == (int)sy && rc->w == (int)cw && rc->h == (int)chh);
        }
        cb_cmp++; if (ok) cb_ok++; else cb_diff++;
        if ((cb_cmp % 900) == 1 && g_dbglog) {
            fprintf(g_dbglog, "[CHARBATCH|COBERTURA] comparacoes=%lu ok=%lu diff=%lu (rect_iv=%d tone_iv=%d)\n",
                    cb_cmp, cb_ok, cb_diff, rc_iv ? 1 : 0, tn_iv ? 1 : 0);
            dbg_flush();
        }
        if (!ok && (++diff_log % 30) == 1 && g_dbglog) {
            fprintf(g_dbglog, "[CHARBATCH|RACIO] cmp=%lu ok=%lu diff=%lu\n", cb_cmp, cb_ok, cb_diff);
            if (rc_iv) {
                mrb_value dvx = mrb_iv_get(mrb, ro, sy_rx), dvy = mrb_iv_get(mrb, ro, sy_ry);
                mrb_value dvw = mrb_iv_get(mrb, ro, sy_rw), dvh = mrb_iv_get(mrb, ro, sy_rh);
                mrb_value dcn = mrb_iv_get(mrb, ch, sy_cname);
                fprintf(g_dbglog, "[CHARBATCH|DIFF-RECT] vivo=(%d,%d,%d,%d) esperado=(%ld,%ld,%ld,%ld) char='%s' pat=%ld dir=%ld\n",
                        mrb_fixnum_p(dvx)?(int)mrb_fixnum(dvx):-999, mrb_fixnum_p(dvy)?(int)mrb_fixnum(dvy):-999,
                        mrb_fixnum_p(dvw)?(int)mrb_fixnum(dvw):-999, mrb_fixnum_p(dvh)?(int)mrb_fixnum(dvh):-999,
                        sx, sy, cw, chh,
                        mrb_string_p(dcn)?mrb_str_to_cstr(mrb,dcn):"?",
                        cb_pat, cb_dir);
            }
            fprintf(g_dbglog, "[CHARBATCH|DIFF] batch=(x%ld y%ld z%ld oy%ld op%ld src %ld,%ld,%ld,%ld) real=(x%d y%d z%d oy%d op%d src %d,%d,%d,%d bmp%s)\n",
                    scr_x, scr_y, scr_z, oy, opc, sx, sy, cw, chh,
                    s->x, s->y, s->z, s->oy, s->opacity, s->src_x, s->src_y, s->src_w, s->src_h,
                    (s->bitmap == frame_bmp) ? "OK" : "DIFF");
            dbg_flush();
        }
        return mrb_fixnum_value(ok ? 1 : 0);
    }

    /* ── APLICAR (uma escrita por campo; struct C ou ivars conforme o objeto) ── */
    if (s->bitmap != frame_bmp) {
        s->bitmap = frame_bmp;
        mrb_iv_set(mrb, self, sy_ibmp, frame_val);   /* getter Ruby coerente */
    }
    if (!cb_is_tile) {
        if (rc_iv) {
            mrb_iv_set(mrb, ro, sy_rx, mrb_fixnum_value((mrb_int)sx));
            mrb_iv_set(mrb, ro, sy_ry, mrb_fixnum_value((mrb_int)sy));
            mrb_iv_set(mrb, ro, sy_rw, mrb_fixnum_value((mrb_int)cw));
            mrb_iv_set(mrb, ro, sy_rh, mrb_fixnum_value((mrb_int)chh));
        }
        if (rc) { rc->x = (int)sx; rc->y = (int)sy; rc->w = (int)cw; rc->h = (int)chh; }
        s->src_x = (int)sx; s->src_y = (int)sy; s->src_w = (int)cw; s->src_h = (int)chh;
        s->oy = (int)oy;
    }
    s->x = (int)scr_x; s->y = (int)scr_y; s->z = (int)scr_z;
    s->opacity = (int)opc; s->blend_type = (int)blt2;
    if (newvis) {
        if (tn_iv) {
            mrb_iv_set(mrb, to, sy_tr,  mrb_float_value(mrb, (mrb_float)tr_));
            mrb_iv_set(mrb, to, sy_tg,  mrb_float_value(mrb, (mrb_float)tg_));
            mrb_iv_set(mrb, to, sy_tb,  mrb_float_value(mrb, (mrb_float)tb_));
            mrb_iv_set(mrb, to, sy_tgr, mrb_float_value(mrb, (mrb_float)tgray_));
        }
        if (tn) { tn->r = tr_; tn->g = tg_; tn->b = tb_; tn->gray = tgray_; }
    }
    /* fim do vanilla: @reflection.update / @surfbase.update (ordem exata 061) */
    if (!mrb_nil_p(cb_refl)) {
        mrb_funcall(mrb, cb_refl, "update", 0);
        if (mrb->exc) return mrb_fixnum_value(1);   /* exc propaga ao voltar ao Ruby (como o vanilla) */
    }
    if (!mrb_nil_p(cb_surf)) {
        mrb_funcall(mrb, cb_surf, "update", 0);
    }
    return mrb_fixnum_value(1);
#undef CB_FAIL0
}
static mrb_value spr_flash (mrb_state *mrb, mrb_value) { (void)mrb; return mrb_nil_value(); }

/* bitmap get/set */
static mrb_value spr_get_bitmap(mrb_state *mrb, mrb_value self) {
    return mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@bitmap"));
}
/* Resolve um objecto Ruby Bitmap ou BitmapWrapper para o Bitmap3DS* C++ subjacente.
 *
 * BitmapWrapper é criado pelos scripts com initialize Ruby puro que não chama
 * super, por isso DATA_PTR da instância wrapper fica nullptr mesmo com
 * MRB_TT_DATA na classe.  Estratégia de fallback:
 *   1. DATA_PTR directo (Bitmap puro criado por bmp_init C++)
 *   2. ivar @bitmap  (BitmapWrapper guarda o Bitmap base em @bitmap)
 *   3. ivar @__bmp__ (alternativa usada nalguns wrappers)
 *   4. método #bitmap (chamada Ruby -- mais lenta mas robusta)
 */
static Bitmap3DS *resolve_bitmap(mrb_state *mrb, mrb_value bobj) {
    if (mrb_nil_p(bobj)) return nullptr;

    /* Tentativa 1: DATA_PTR directo */
    Bitmap3DS *bmp = (Bitmap3DS*)DATA_PTR(bobj);
    if (bmp) return bmp;

    extern FILE *g_dbglog;

    /* Tentativa 2: ivar @bitmap */
    mrb_value iv = mrb_iv_get(mrb, bobj, mrb_intern_lit(mrb, "@bitmap"));
    if (!mrb_nil_p(iv) && !mrb_equal(mrb, iv, bobj)) {
        bmp = (Bitmap3DS*)DATA_PTR(iv);
        if (bmp) {
            if (g_dbglog) { fprintf(g_dbglog,
                "[BMP|UNWRAP] resolvido via @bitmap: %dx%d\n", bmp->width, bmp->height);
                dbg_flush(); }
            return bmp;
        }
        /* @bitmap pode ser outro wrapper -- recursão protegida */
        bmp = resolve_bitmap(mrb, iv);
        if (bmp) return bmp;
    }

    /* Tentativa 3: ivar @__bmp__ */
    iv = mrb_iv_get(mrb, bobj, mrb_intern_lit(mrb, "@__bmp__"));
    if (!mrb_nil_p(iv) && !mrb_equal(mrb, iv, bobj)) {
        bmp = (Bitmap3DS*)DATA_PTR(iv);
        if (bmp) return bmp;
    }

    /* Tentativa 4: chamar método #bitmap em Ruby */
    if (!mrb->exc && mrb_respond_to(mrb, bobj, mrb_intern_lit(mrb, "bitmap"))) {
        iv = mrb_funcall(mrb, bobj, "bitmap", 0);
        if (mrb->exc) { mrb->exc = 0; }
        else if (!mrb_nil_p(iv) && !mrb_equal(mrb, iv, bobj)) {
            bmp = (Bitmap3DS*)DATA_PTR(iv);
            if (bmp) {
                if (g_dbglog) { fprintf(g_dbglog,
                    "[BMP|UNWRAP] resolvido via #bitmap(): %dx%d\n", bmp->width, bmp->height);
                    dbg_flush(); }
                return bmp;
            }
        }
    }

    /* [FIX-UNWRAP] (migrado p/ o motor) Ultimo recurso: o objeto e' um Bitmap/
     * BitmapWrapper cuja initialize nao chamou super -> ficou sem DATA_PTR. Em vez
     * de devolver NULL (sprite sem bitmap + falha [BMP|UNWRAP] repetida), anexa um
     * Bitmap3DS 1x1 VALIDO. O sprite fica vazio (como ja' ficava), mas SEM NULL e
     * SEM falha -> 0 falhas UNWRAP no log, robustez total. */
    if (mrb_type(bobj) == MRB_TT_DATA && !DATA_PTR(bobj)) {
        extern Bitmap3DS *bmp_alloc_public(int, int);
        extern const mrb_data_type Bitmap3DSType;
        Bitmap3DS *nb = bmp_alloc_public(1, 1);
        if (nb) {
            DATA_PTR(bobj)  = nb;
            DATA_TYPE(bobj) = &Bitmap3DSType;
            if (g_dbglog) { fprintf(g_dbglog, "[FIX-UNWRAP] DATA_PTR nulo (initialize sem super) -> anexado Bitmap 1x1 valido (sem NULL, sem falha)\n"); dbg_flush(); }
            return nb;
        }
    }

    if (g_dbglog) {
        /* [UNWRAP-ID] O motor JA' forca instance_tt=MRB_TT_DATA em Bitmap/BitmapWrapper
         * (bitmap_binding L2531/2550), por isso type=6 (MRB_TT_OBJECT) NAO e' um
         * BitmapWrapper-sem-super -> e' OUTRO objeto passado como bitmap. Logar a CLASSE
         * e se responde a #bitmap identifica-o a 100% no proximo run (Bug 5-UNWRAP). */
        const char *cls = mrb_obj_classname(mrb, bobj);
        int has_bmp_m = mrb_respond_to(mrb, bobj, mrb_intern_lit(mrb, "bitmap")) ? 1 : 0;
        fprintf(g_dbglog,
            "[BMP|UNWRAP] FALHOU: nao-bitmap passado como bitmap -- classe='%s' type=%d responde_a_#bitmap=%d\n",
            cls ? cls : "?", (int)mrb_type(bobj), has_bmp_m);
        dbg_flush();
    }
    return nullptr;
}

static mrb_value spr_set_bitmap(mrb_state *mrb, mrb_value self) {
    mrb_value bobj; mrb_get_args(mrb, "o", &bobj);
    extern unsigned long long g_spr_setter_calls; g_spr_setter_calls++;
    mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@bitmap"), bobj);
    Sprite3DS *s = GET_SPR(self);
    if (s) {
        Bitmap3DS *bmp = resolve_bitmap(mrb, bobj);
        s->bitmap = bmp;

        /* ── FIX GRELHA/RASTO (CAUSA RAIZ, provado pelo FRAMEDUMP) ───────────
         * No RGSS verdadeiro, `sprite.bitmap = bmp` define AUTOMATICAMENTE
         * src_rect = (0,0,bmp.width,bmp.height). Aqui isso FALTAVA: o @src_rect
         * ficava NIL. O Sprite_Character (all_core 12093) faz
         *     self.src_rect.set(pattern*cw, dir*ch, cw, ch)
         * mas `self.src_rect` devolvia NIL e `nil.set(...)` cai no patch do
         * NilClass (no-op) -> o frame NUNCA era recortado -> a folha 4x4
         * inteira (256x256) aparecia -> jogador e Pokemon repetidos em grelha.
         * O FRAMEDUMP (botao Y) provou-o: TODO o Sprite_Character tinha
         * src=[0,0,0,0] (s->src_w=0 -> o blit usa bitmap->width = folha toda).
         *
         * Solucao RGSS-correcta: inicializar/repor @src_rect para o bitmap
         * INTEIRO sempre que bitmap= e' chamado. O `.set(...)` do jogo, logo a
         * seguir (mesma passagem de Sprite_Character#update), recorta este Rect
         * para 1 frame; o live-read em sprites_draw_all le' entao o Rect ja'
         * recortado e poe s->src_w/h = cw/ch. Reutiliza o Rect existente (sem
         * churn de GC -- Sprite_Character chama bitmap= todos os frames p/
         * charsets animados) ou cria um se ainda for nil. */
        if (bmp) {
            const int bw = bmp->width, bh = bmp->height;
            mrb_value ro = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@src_rect"));
            if (mrb_nil_p(ro)) {
                RClass *rk = mrb_class_get(mrb, "Rect");
                mrb_value argv[4] = { mrb_fixnum_value(0), mrb_fixnum_value(0),
                                      mrb_fixnum_value(bw), mrb_fixnum_value(bh) };
                ro = mrb_obj_new(mrb, rk, 4, argv);
                mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@src_rect"), ro);
            } else {
                /* [SRCRECT-FIX 2026-07-03] repor dims no Rect existente — MAS o Rect
                 * e' MRB_TT_DATA (C++): escrever @ivars era ESCREVER FANTASMAS (os
                 * getters nativos leem a struct C, nao ivars) -> o Rect REAL ficava
                 * velho/0x0 e o live-read por frame punha src=0x0 -> sprite MUDO com
                 * bitmap/z/opacity todos certos (provado: preview do roupeiro
                 * src=0x0; suspeito nos tiles esparsos de pool do tilemap). Usar o
                 * set NATIVO, que atualiza a struct C. */
                mrb_value argv2[4] = { mrb_fixnum_value(0), mrb_fixnum_value(0),
                                       mrb_fixnum_value(bw), mrb_fixnum_value(bh) };
                mrb_funcall_argv(mrb, ro, mrb_intern_lit(mrb, "set"), 4, argv2);
                if (mrb->exc) mrb->exc = 0;   /* Rect sem #set (impossivel) -> nao partir o frame */
            }
            s->src_x = 0; s->src_y = 0; s->src_w = bw; s->src_h = bh;
        }

        /* DIAGNÓSTICO: log nas primeiras 20 chamadas */
        static int s_set_bmp_count = 0;
        s_set_bmp_count++;
        if (s_set_bmp_count <= 20) {
            extern FILE *g_dbglog;
            if (g_dbglog) {
                int tt = mrb_nil_p(bobj) ? -1 : (int)mrb_type(bobj);
                void *dp = mrb_nil_p(bobj) ? nullptr : DATA_PTR(bobj);
                fprintf(g_dbglog,
                    "[SPR|SET_BMP] #%d sprite=%p bobj_type=%d DATA_PTR=%p bmp=%p\n",
                    s_set_bmp_count, (void*)s, tt, dp, (void*)bmp);
                if (bmp) {
                    fprintf(g_dbglog,
                        "[SPR|SET_BMP]   bmp OK via resolve: %dx%d tex_dirty=%d\n",
                        bmp->width, bmp->height, bmp->tex_dirty);
                } else if (!mrb_nil_p(bobj)) {
                    fprintf(g_dbglog,
                        "[SPR|SET_BMP]   resolve_bitmap FALHOU (type=%d)"
                        " -- ver [BMP|UNWRAP] acima\n", tt);
                }
                dbg_flush();
            }
        }
    }
    return bobj;
}

/* src_rect get/set */
static mrb_value spr_get_src_rect(mrb_state *mrb, mrb_value self) {
    return mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@src_rect"));
}

/* Extrai Rect de objecto Ruby puro (ivars @x,@y,@width,@height) ou C-struct.
 * DATA_PTR sobre Rect Ruby puro devolve NULL/lixo -- nunca fazer cast directo. */
static bool spr_extract_rect(mrb_state *mrb, mrb_value ro,
                              int *x, int *y, int *w, int *h) {
    if (mrb_nil_p(ro)) return false;
    /* Tentar ivars Ruby primeiro (Rect definido em compat_stubs.h) */
    mrb_value vx = mrb_iv_get(mrb, ro, mrb_intern_lit(mrb, "@x"));
    mrb_value vy = mrb_iv_get(mrb, ro, mrb_intern_lit(mrb, "@y"));
    mrb_value vw = mrb_iv_get(mrb, ro, mrb_intern_lit(mrb, "@width"));
    mrb_value vh = mrb_iv_get(mrb, ro, mrb_intern_lit(mrb, "@height"));
    if (!mrb_nil_p(vx)) {
        *x = (int)mrb_integer(mrb_to_int(mrb, vx));
        *y = (int)mrb_integer(mrb_to_int(mrb, vy));
        *w = (int)mrb_integer(mrb_to_int(mrb, vw));
        *h = (int)mrb_integer(mrb_to_int(mrb, vh));
        return true;
    }
    return false;
}

static mrb_value spr_set_src_rect(mrb_state *mrb, mrb_value self) {
    mrb_value ro; mrb_get_args(mrb, "o", &ro);
    extern unsigned long long g_spr_setter_calls; g_spr_setter_calls++;
    mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@src_rect"), ro);
    Sprite3DS *s = GET_SPR(self);
    if (s) {
        int x = 0, y = 0, w = 0, h = 0;
        if (spr_extract_rect(mrb, ro, &x, &y, &w, &h)) {
            s->src_x = x; s->src_y = y; s->src_w = w; s->src_h = h;
        }
    }
    return ro;
}

/* DEBUG LOG CERTEIRO (fix CEGO D): contador CUMULATIVO de chamadas aos setters
 * C++ de Sprite. Exposto ao Ruby via Kernel#__mkxp_setters (binding_3ds.cpp).
 * Diz-nos QUANTOS dispatches mruby->C++ por frame os sprites fazem -> se um
 * batch-setter (uma chamada que poe x/y/z/bitmap/src de uma vez) valeria a pena.
 * Incremento trivial (~1ns); sempre ligado, so' se LOGA periodicamente. */
unsigned long long g_spr_setter_calls = 0;

/* int properties */
#define INT_PROP(name, field) \
static mrb_value spr_get_##name(mrb_state *mrb, mrb_value s) { \
    (void)mrb; Sprite3DS *p = GET_SPR(s); return mrb_fixnum_value(p ? p->field : 0); } \
static mrb_value spr_set_##name(mrb_state *mrb, mrb_value s) { \
    mrb_int v; mrb_get_args(mrb, "i", &v); g_spr_setter_calls++; \
    Sprite3DS *p = GET_SPR(s); if (p) p->field = (int)v; \
    return mrb_fixnum_value(v); }

INT_PROP(x, x) INT_PROP(y, y) INT_PROP(ox, ox) INT_PROP(oy, oy)
INT_PROP(z, z) INT_PROP(opacity, opacity) INT_PROP(blend_type, blend_type)

/* float properties */
#define FLOAT_PROP(name, field) \
static mrb_value spr_get_##name(mrb_state *mrb, mrb_value s) { \
    Sprite3DS *p = GET_SPR(s); return mrb_float_value(mrb, p ? p->field : 0.0f); } \
static mrb_value spr_set_##name(mrb_state *mrb, mrb_value s) { \
    mrb_float v; mrb_get_args(mrb, "f", &v); g_spr_setter_calls++; \
    Sprite3DS *p = GET_SPR(s); if (p) p->field = (float)v; \
    return mrb_float_value(mrb, v); }

FLOAT_PROP(zoom_x, zoom_x) FLOAT_PROP(zoom_y, zoom_y) FLOAT_PROP(angle, angle)

/* bool properties */
#define BOOL_PROP(name, field) \
static mrb_value spr_get_##name(mrb_state *mrb, mrb_value s) { \
    (void)mrb; Sprite3DS *p = GET_SPR(s); return mrb_bool_value(p && p->field); } \
static mrb_value spr_set_##name(mrb_state *mrb, mrb_value s) { \
    mrb_bool v; mrb_get_args(mrb, "b", &v); g_spr_setter_calls++; \
    Sprite3DS *p = GET_SPR(s); if (p) p->field = (bool)v; \
    return mrb_bool_value(v); }

BOOL_PROP(visible, visible) BOOL_PROP(mirror, mirror)

/* width/height from bitmap */
static mrb_value spr_width(mrb_state *mrb, mrb_value s) {
    (void)mrb;
    Sprite3DS *p = GET_SPR(s);
    return mrb_fixnum_value(p && p->bitmap ? (p->src_w > 0 ? p->src_w : p->bitmap->width) : 0);
}
static mrb_value spr_height(mrb_state *mrb, mrb_value s) {
    (void)mrb;
    Sprite3DS *p = GET_SPR(s);
    return mrb_fixnum_value(p && p->bitmap ? (p->src_h > 0 ? p->src_h : p->bitmap->height) : 0);
}

/* color/tone — guardados como ivars.
 * CRITICO p/ performance do menu: o codigo RGSS faz sprite.color.set(...),
 * sprite.color.alpha=, sprite.color.red, etc. a CADA frame. Se @color for nil,
 * cada um desses cai no method_missing universal (NilClass#alpha/red/green/blue/
 * set/x=...) CENTENAS de vezes por frame -> menu a 4 FPS. Por isso, em vez de
 * devolver nil, criamos uma Color/Tone real a pedido (lazy) e guardamo-la. */
static mrb_value spr_get_color(mrb_state *mrb, mrb_value s) {
    mrb_value v = mrb_iv_get(mrb, s, mrb_intern_lit(mrb, "@color"));
    if (mrb_nil_p(v) && mrb_class_defined(mrb, "Color")) {
        struct RClass *cc = mrb_class_get(mrb, "Color");
        if (cc) {
            mrb_value args[4] = { mrb_fixnum_value(0), mrb_fixnum_value(0),
                                  mrb_fixnum_value(0), mrb_fixnum_value(0) };
            v = mrb_obj_new(mrb, cc, 4, args);
            mrb_iv_set(mrb, s, mrb_intern_lit(mrb, "@color"), v);
        }
    }
    return v; }
static mrb_value spr_set_color(mrb_state *mrb, mrb_value s) {
    mrb_value v; mrb_get_args(mrb, "o", &v);
    extern unsigned long long g_spr_setter_calls; g_spr_setter_calls++;
    mrb_iv_set(mrb, s, mrb_intern_lit(mrb, "@color"), v); return v; }
static mrb_value spr_get_tone(mrb_state *mrb, mrb_value s) {
    mrb_value v = mrb_iv_get(mrb, s, mrb_intern_lit(mrb, "@tone"));
    if (mrb_nil_p(v) && mrb_class_defined(mrb, "Tone")) {
        struct RClass *tc = mrb_class_get(mrb, "Tone");
        if (tc) {
            mrb_value args[4] = { mrb_fixnum_value(0), mrb_fixnum_value(0),
                                  mrb_fixnum_value(0), mrb_fixnum_value(0) };
            v = mrb_obj_new(mrb, tc, 4, args);
            mrb_iv_set(mrb, s, mrb_intern_lit(mrb, "@tone"), v);
        }
    }
    return v; }
static mrb_value spr_set_tone(mrb_state *mrb, mrb_value s) {
    mrb_value v; mrb_get_args(mrb, "o", &v);
    extern unsigned long long g_spr_setter_calls; g_spr_setter_calls++;
    mrb_iv_set(mrb, s, mrb_intern_lit(mrb, "@tone"), v); return v; }

/* P3: ler o z de um objecto Viewport Ruby de forma segura (sem rebentar se for
 * nil ou nao responder a #z). Devolve 0 quando nao ha viewport. */
static int read_viewport_z(mrb_state *mrb, mrb_value vp) {
    if (mrb_nil_p(vp)) return 0;
    if (!mrb_respond_to(mrb, vp, mrb_intern_lit(mrb, "z"))) return 0;
    mrb_value zv = mrb_funcall(mrb, vp, "z", 0);
    if (mrb_fixnum_p(zv)) return (int)mrb_fixnum(zv);
    if (mrb_float_p(zv))  return (int)mrb_float(zv);
    return 0;
}

static mrb_value spr_get_viewport(mrb_state *mrb, mrb_value self) {
    return mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@viewport"));
}
/* Guarda o viewport como ivar Ruby (como antes) E copia o seu z para vp_z na
 * struct, para o sort de sprites_draw_all ordenar por (viewport.z, z) -- igual
 * ao RGSS. Resolve o background (Plane) que caia atras do mapa por o motor so'
 * ordenar por z local. Sprites sem viewport ficam com vp_z=0 (ordem inalterada). */
static mrb_value spr_set_viewport(mrb_state *mrb, mrb_value self) {
    mrb_value v; mrb_get_args(mrb, "o", &v);
    mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@viewport"), v);
    Sprite3DS *s = GET_SPR(self);
    if (s) {
        if (mrb_nil_p(v)) {
            s->has_vp = false;
            s->vp_z = 0;
        } else {
            s->has_vp = true;
            s->vp_z = read_viewport_z(mrb, v);
        }
        /* DIAGNOSTICO P3: logar as primeiras vezes que um viewport de z alto e'
         * aplicado (cenas tipo escolha de personagem, vp.z>=99999). Confirma no
         * log que o vp_z chega a' struct e o sort por (viewport.z, z) actua. */
        if (s->has_vp && s->vp_z >= 1000) {
            static int s_vpz_log = 0;
            if (s_vpz_log < 8) {
                s_vpz_log++;
                extern FILE *g_dbglog;
                if (g_dbglog) {
                    fprintf(g_dbglog,
                        "[SPR|VPZ] viewport.z=%d aplicado a sprite z=%d (ordena por vp_z,z)\n",
                        s->vp_z, s->z);
                    dbg_flush();
                }
            }
        }
    }
    return v;
}

/* ============================================================================
 * FIX-NSYNC — sync NATIVO de posicao para sprites de personagem ESTATICOS.
 *
 * Motivo: para um NPC parado os plugins que embrulham Sprite_Character#update
 * (Water bubbles, Marin's Footprints) sao NO-OP (nao surfa -> sem bolha; nao se
 * move -> sem pegada). Logo, quando a assinatura do @character nao mudou, podemos
 * saltar TODA a cadeia Ruby do update e recalcular SO' a posicao (x/y/z) aqui em
 * C++, com a formula EXATA da v19.1. Ataca a CAMINHADA: o scroll da camara move
 * todos os NPCs parados no ecra' sem ~10 dispatches mruby por sprite.
 *
 * Devolve: true  -> tratou (estatico): escreveu s->x/y/z, o Ruby salta a cadeia.
 *          false -> precisa do update Ruby completo (movimento/animacao/salto/
 *                   escadas/tile/especial/refresh periodico/1a chamada).
 *
 * Formula v19.1 (007_Game_Character.rb; X_SUBPIXELS=4, TILE=32, REAL_RES=128):
 *   screen_x        = round((real_x - map.display_x)/4) + width*32/2
 *   screen_y_ground = round((real_y - map.display_y)/4) + 32
 *   screen_z        = 999 se @always_on_top; senao screen_y_ground + (ch>32?31:0)
 * (jumping/tile/escadas/animacao tratados pelo fallback -> nunca chegam ao calculo)
 *
 * Seguranca: flag Ruby $MKXP_NATIVE_STATIC_SYNC desliga tudo; ler ivar inexistente
 * devolve nil -> tratado como "mudou" -> fallback Ruby (nunca crasha); refresh
 * forcado a cada NS_REFRESH_FRAMES apanha o tom dia/noite (cache de 30s) e qualquer
 * edge case dentro de <=1s. Sprites com @reflection/@surfbase (player, reflexos,
 * follower) sao excluidos -> precisam do update Ruby para os sub-sprites.
 * ========================================================================== */
#define NS_REFRESH_FRAMES 30   /* ~1s a 30fps: refresh Ruby periodico de seguranca */

static inline double ns_num2dbl(mrb_value v){
    if (mrb_float_p(v))  return mrb_float(v);
    if (mrb_fixnum_p(v)) return (double)mrb_fixnum(v);
    return 0.0;
}
static inline int ns_ivint(mrb_state *mrb, mrb_value o, mrb_sym k, int dflt){
    mrb_value v = mrb_iv_get(mrb, o, k);
    if (mrb_fixnum_p(v)) return (int)mrb_fixnum(v);
    if (mrb_float_p(v))  return (int)mrb_float(v);
    return dflt;
}

static mrb_value spr_static_sync(mrb_state *mrb, mrb_value self){
    Sprite3DS *s = GET_SPR(self);
    if (!s || s->disposed) return mrb_false_value();

    static mrb_sym k_char=0,k_refl,k_surf,k_map,k_rx,k_ry,k_dx,k_dy,
                   k_dir,k_pat,k_op,k_blend,k_transp,k_tile,k_bob,
                   k_offx,k_offy,k_bush,k_anim,k_jc,k_jdl,k_aot,k_width;
    if (!k_char){
        k_char  =mrb_intern_lit(mrb,"@character");        k_refl =mrb_intern_lit(mrb,"@reflection");
        k_surf  =mrb_intern_lit(mrb,"@surfbase");         k_map  =mrb_intern_lit(mrb,"@map");
        k_rx    =mrb_intern_lit(mrb,"@real_x");           k_ry   =mrb_intern_lit(mrb,"@real_y");
        k_dx    =mrb_intern_lit(mrb,"@display_x");        k_dy   =mrb_intern_lit(mrb,"@display_y");
        k_dir   =mrb_intern_lit(mrb,"@direction");        k_pat  =mrb_intern_lit(mrb,"@pattern");
        k_op    =mrb_intern_lit(mrb,"@opacity");          k_blend=mrb_intern_lit(mrb,"@blend_type");
        k_transp=mrb_intern_lit(mrb,"@transparent");      k_tile =mrb_intern_lit(mrb,"@tile_id");
        k_bob   =mrb_intern_lit(mrb,"@bob_height");       k_offx =mrb_intern_lit(mrb,"@real_offset_x");
        k_offy  =mrb_intern_lit(mrb,"@real_offset_y");    k_bush =mrb_intern_lit(mrb,"@bush_depth");
        k_anim  =mrb_intern_lit(mrb,"@animation_id");     k_jc   =mrb_intern_lit(mrb,"@jump_count");
        k_jdl   =mrb_intern_lit(mrb,"@jump_distance_left");k_aot =mrb_intern_lit(mrb,"@always_on_top");
        k_width =mrb_intern_lit(mrb,"@width");
    }

    /* [NSYNC-STAGGER 2026-07-02] intervalo de refresh TUNAVEL + fase ESCALONADA.
     * PROBLEMA medido (log): update=18ms steady mas com PICOS periodicos (23->15fps
     * ripple). Causa: os sprites estaticos sao criados JUNTOS no load do mapa, por
     * isso os contadores ns_static_frames atingem NS_REFRESH_FRAMES no MESMO frame ->
     * 1 frame/segundo faz ~31x update Ruby completo de uma vez = hitch periodico.
     *  - FASE: semeando ns_static_frames com (reg_seq % refresh) na 1a chamada, cada
     *    sprite refresca numa fase diferente -> custo CONSTANTE em vez de PICO. O TETO
     *    (<= refresh frames entre refreshes) mantem-se -> ZERO mudanca de comportamento.
     *  - TUNAVEL: $MKXP_STATIC_REFRESH_FRAMES (default 30 = ~1s). Lido 1x/120 chamadas
     *    (cache estatica partilhada) p/ nao pagar mrb_gv_get por sprite/frame. Subir
     *    (ex: 60/90) troca latencia-de-edge-case (tom dia/noite tem cache de 30s, logo
     *    ha' folga) por FPS em mapas com muitos NPCs parados. >=8 aceite; default e'
     *    identico ao atual. So' afeta o fast-path nativo -> nunca crasha. */
    static int s_ns_refresh = NS_REFRESH_FRAMES, s_ns_refresh_ttl = 0;
    if (--s_ns_refresh_ttl <= 0) {
        s_ns_refresh_ttl = 120;
        mrb_value rv = mrb_gv_get(mrb, mrb_intern_lit(mrb,"$MKXP_STATIC_REFRESH_FRAMES"));
        s_ns_refresh = (mrb_fixnum_p(rv) && mrb_fixnum(rv) >= 8) ? (int)mrb_fixnum(rv) : NS_REFRESH_FRAMES;
    }

    mrb_value ch = mrb_iv_get(mrb, self, k_char);
    if (mrb_nil_p(ch)) return mrb_false_value();   /* sem @character -> Ruby */

    /* Especiais -> SEMPRE update Ruby (precisam de atualizar sub-sprites): sprites
     * com @reflection (player, reflexos, e o FOLLOWER via setReflection) ou
     * @surfbase (player a surfar). Verificado a CADA chamada de proposito -- o
     * follower so' recebe @reflection DEPOIS do initialize (em
     * DependentEventSprites#refresh), logo cachar a elegibilidade na 1a chamada
     * deixaria o reflexo dele por atualizar. 2 leituras de ivar = barato. */
    if (!mrb_nil_p(mrb_iv_get(mrb,self,k_refl)) || !mrb_nil_p(mrb_iv_get(mrb,self,k_surf)))
        return mrb_false_value();

    /* 1a chamada: guardar a assinatura inicial e forcar o update Ruby neste frame. */
    if (!s->ns_init){
        s->ns_rx =ns_num2dbl(mrb_iv_get(mrb,ch,k_rx));  s->ns_ry =ns_num2dbl(mrb_iv_get(mrb,ch,k_ry));
        s->ns_dir=ns_ivint(mrb,ch,k_dir,2);  s->ns_pat  =ns_ivint(mrb,ch,k_pat,0);
        s->ns_op =ns_ivint(mrb,ch,k_op,255); s->ns_blend=ns_ivint(mrb,ch,k_blend,0);
        s->ns_tile=ns_ivint(mrb,ch,k_tile,0);s->ns_bob  =ns_ivint(mrb,ch,k_bob,0);
        s->ns_offx=ns_ivint(mrb,ch,k_offx,0);s->ns_offy =ns_ivint(mrb,ch,k_offy,0);
        s->ns_bush=ns_ivint(mrb,ch,k_bush,0);s->ns_transp=mrb_test(mrb_iv_get(mrb,ch,k_transp));
        /* [NSYNC-STAGGER] fase inicial escalonada por sprite (reg_seq % refresh):
         * desincroniza os refreshes periodicos -> sem hitch de 31x-update-no-mesmo-frame. */
        s->ns_init=true; s->ns_static_frames = (int)(s->reg_seq % (unsigned)s_ns_refresh);
        return mrb_false_value();
    }

    /* ler assinatura atual do @character */
    double rx=ns_num2dbl(mrb_iv_get(mrb,ch,k_rx)), ry=ns_num2dbl(mrb_iv_get(mrb,ch,k_ry));
    int dir=ns_ivint(mrb,ch,k_dir,2),  pat=ns_ivint(mrb,ch,k_pat,0);
    int op =ns_ivint(mrb,ch,k_op,255), blend=ns_ivint(mrb,ch,k_blend,0);
    int tile=ns_ivint(mrb,ch,k_tile,0),bob=ns_ivint(mrb,ch,k_bob,0);
    int offx=ns_ivint(mrb,ch,k_offx,0),offy=ns_ivint(mrb,ch,k_offy,0);
    int bush=ns_ivint(mrb,ch,k_bush,0),anim=ns_ivint(mrb,ch,k_anim,0);
    int jc =ns_ivint(mrb,ch,k_jc,0),   jdl=ns_ivint(mrb,ch,k_jdl,0);
    bool transp=mrb_test(mrb_iv_get(mrb,ch,k_transp));

    /* desqualificadores -> via Ruby (trata salto/animacao/tile-z/escadas/bitmap):
     * tile-based, animacao a tocar, a saltar, ou em escadas (offset != 0). */
    bool disq = (tile!=0 || anim!=0 || jc!=0 || jdl!=0 || offx!=0 || offy!=0);

    bool same = !disq &&
        s->ns_rx==rx && s->ns_ry==ry && s->ns_dir==dir && s->ns_pat==pat &&
        s->ns_op==op && s->ns_blend==blend && s->ns_tile==tile && s->ns_bob==bob &&
        s->ns_offx==offx && s->ns_offy==offy && s->ns_bush==bush && s->ns_transp==transp;

    if (!same || s->ns_static_frames >= s_ns_refresh){
        /* mudou OU refresh periodico -> guardar nova assinatura e ir ao Ruby completo */
        s->ns_rx=rx; s->ns_ry=ry; s->ns_dir=dir; s->ns_pat=pat; s->ns_op=op;
        s->ns_blend=blend; s->ns_tile=tile; s->ns_bob=bob; s->ns_offx=offx;
        s->ns_offy=offy; s->ns_bush=bush; s->ns_transp=transp;
        s->ns_static_frames=0;
        return mrb_false_value();
    }

    /* ── ESTATICO: so' a camara pode ter mudado -> recalcular posicao em C++ ──── */
    mrb_value map = mrb_iv_get(mrb,ch,k_map);
    if (mrb_nil_p(map)){                                 /* @map nil -> $game_map */
        map = mrb_gv_get(mrb, mrb_intern_lit(mrb,"$game_map"));
        if (mrb_nil_p(map)) return mrb_false_value();    /* sem mapa -> Ruby */
    }
    double dx=ns_num2dbl(mrb_iv_get(mrb,map,k_dx)), dy=ns_num2dbl(mrb_iv_get(mrb,map,k_dy));
    int width=ns_ivint(mrb,ch,k_width,1);
    int chh = (s->src_h>0) ? s->src_h : 32;              /* @ch == altura do frame == src_h */

    int sx  = (int)lround((rx-dx)/4.0) + (width*32)/2;   /* offx==0 garantido (disq) */
    int syg = (int)lround((ry-dy)/4.0) + 32;             /* offy==0 garantido */
    int sz  = mrb_test(mrb_iv_get(mrb,ch,k_aot)) ? 999 : syg + ((chh>32)?31:0);

    s->x=sx; s->y=syg; s->z=sz;   /* ox/oy/src_rect/opacity/blend/visible/tone inalterados */
    s->ns_static_frames++;
    return mrb_true_value();
}

void spriteBindingInit(mrb_state *mrb) {
    RClass *k = mrb_define_class(mrb, "Sprite", mrb->object_class);
    MRB_SET_INSTANCE_TT(k, MRB_TT_DATA);
    mrb_define_method(mrb, k, "initialize", spr_init,         MRB_ARGS_OPT(1));
    mrb_define_method(mrb, k, "dispose",    spr_dispose,      MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "disposed?",  spr_disposed,     MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "update",     spr_update,       MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "mkxp_static_sync", spr_static_sync, MRB_ARGS_NONE()); /* FIX-NSYNC */
    mrb_define_method(mrb, k, "flash",      spr_flash,        MRB_ARGS_REQ(2));
    mrb_define_method(mrb, k, "bitmap",     spr_get_bitmap,   MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "bitmap=",    spr_set_bitmap,   MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "src_rect",   spr_get_src_rect, MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "src_rect=",  spr_set_src_rect, MRB_ARGS_REQ(1));
    /* [CHARBATCH] corpo nativo do Sprite_Character#update (caso comum); o seam
     * Ruby (injecao pos-scripts) decide off/on/verify via $MKXP_NATIVE_CHAR_BATCH */
    mrb_define_method(mrb, k, "__mkxp_char_batch", spr_char_batch, MRB_ARGS_OPT(1));
#define MP(n) \
    mrb_define_method(mrb, k, #n,    spr_get_##n, MRB_ARGS_NONE()); \
    mrb_define_method(mrb, k, #n"=", spr_set_##n, MRB_ARGS_REQ(1))
    MP(x); MP(y); MP(ox); MP(oy); MP(z);
    MP(opacity); MP(blend_type);
    MP(zoom_x); MP(zoom_y); MP(angle);
    MP(visible); MP(mirror);
    MP(color); MP(tone);
#undef MP
    mrb_define_method(mrb, k, "width",  spr_width,  MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "height", spr_height, MRB_ARGS_NONE());
    // viewport, bush_depth, bush_opacity, wave_* -- guardados como ivars Ruby
    // Os scripts acedem mas o render 3DS não usa (simplificação aceitável)
    #define IVAR_PROP(nm) \
    mrb_define_method(mrb, k, nm, \
        [](mrb_state *mrb, mrb_value self) -> mrb_value { \
            return mrb_iv_get(mrb, self, mrb_intern_cstr(mrb, "@" nm)); }, \
        MRB_ARGS_NONE()); \
    mrb_define_method(mrb, k, nm "=", \
        [](mrb_state *mrb, mrb_value self) -> mrb_value { \
            mrb_value v; mrb_get_args(mrb, "o", &v); \
            mrb_iv_set(mrb, self, mrb_intern_cstr(mrb, "@" nm), v); return v; }, \
        MRB_ARGS_REQ(1));
    // viewport: setter dedicado (P3) que propaga viewport.z -> vp_z na struct
    // para o sort por (viewport.z, z). bush_depth/wave_* continuam ivars puros.
    mrb_define_method(mrb, k, "viewport",  spr_get_viewport, MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "viewport=", spr_set_viewport, MRB_ARGS_REQ(1));
    IVAR_PROP("bush_depth")
    IVAR_PROP("bush_opacity")
    IVAR_PROP("wave_amp")
    IVAR_PROP("wave_length")
    IVAR_PROP("wave_speed")
    IVAR_PROP("wave_phase")
    #undef IVAR_PROP

    // ── Acelerador OPCIONAL do Essentials (modulo isolado) ──────────────────
    // Regista __gc_native_screen_x/y/z. So' tem efeito quando o Ruby liga a flag
    // $__mkxp_native_spritechar (ver compat_stubs.h). Registar aqui e' seguro:
    // corre no arranque, muito antes dos scripts do jogo definirem Game_Character.
    extern void essentialsAccelInit(mrb_state *mrb);
    essentialsAccelInit(mrb);
}

/* FIX3: Re-bind Sprite#initialize para spr_init C++ após os scripts Ruby terem corrido.
 * Alguns scripts fazem "class Sprite; def initialize..." sobrescrevendo o binding C++.
 * Chamar esta função em check_entry_methods, antes do loop principal. */
void spriteBindingReinit(mrb_state *mrb) {
    extern FILE *g_dbglog;

    struct RClass *spr_cls = mrb_class_get(mrb, "Sprite");
    if (!spr_cls) {
        if (g_dbglog) {
            fprintf(g_dbglog, "[FIX3] ERRO: classe Sprite nao encontrada\n");
            dbg_flush();
        }
        return;
    }

    /* Garantir MRB_TT_DATA */
    MRB_SET_INSTANCE_TT(spr_cls, MRB_TT_DATA);

    /* Re-bind initialize para spr_init C++ -- sobrescreve qualquer def Ruby */
    mrb_define_method(mrb, spr_cls, "initialize", spr_init, MRB_ARGS_OPT(1));

    /* Re-bind bitmap= também porque alguns scripts podem tê-lo sobrescrito */
    mrb_define_method(mrb, spr_cls, "bitmap=", spr_set_bitmap, MRB_ARGS_REQ(1));

    if (g_dbglog) {
        fprintf(g_dbglog,
            "[FIX3] Sprite#initialize re-bound para spr_init C++ OK "
            "(MRB_TT_DATA garantido)\n");
        dbg_flush();
    }
}
