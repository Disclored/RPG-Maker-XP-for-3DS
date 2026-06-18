#include "sprite_binding_3ds.h"

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
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>   /* std::stable_sort */

/* global sprite list for rendering */
static std::vector<Sprite3DS*> g_sprites;

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
                fflush(g_dbglog);
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
    std::stable_sort(g_sprites.begin(), g_sprites.end(),
        [](const Sprite3DS *a, const Sprite3DS *b) {
            int ka = a->has_vp ? a->vp_z : a->z;
            int kb = b->has_vp ? b->vp_z : b->z;
            if (ka != kb) return ka < kb;
            return a->z < b->z;
        });

    for (size_t i = 0; i < g_sprites.size(); i++) {
        Sprite3DS *s = g_sprites[i];
        if (!s || s->disposed || !s->visible || !s->bitmap) continue;
        if (s->bitmap->disposed) continue;

        /* FIX BUG 3: fazer upload sempre que tex_dirty=true OU tex=nullptr.
         * tex pode ser nullptr mesmo com tex_dirty=false se o bitmap foi
         * recriado externamente (ex: bmp_clear seguido de draw sem flush). */
        /* FIX FPS (texturas >1024 / VRAM esgotada): se a textura JA' falhou e os
         * pixels NAO mudaram (tex_dirty=false), NAO re-tentar -- senao tentava-se
         * criar a mesma textura impossivel TODOS os frames, com EVICT-thrashing +
         * C3D_TexInit em loop (provado: tex_fail subia 100/frame, 2.2 fps em mapas
         * com tilesets grandes 1696x704). Quando os pixels mudam, tex_dirty volta
         * a true e limpamos o flag para dar nova oportunidade. */
        if (s->bitmap->tex_dirty) s->bitmap->tex_failed = false;
        if (s->bitmap->tex_failed && !s->bitmap->tex) continue;
        if (s->bitmap->tex_dirty || !s->bitmap->tex) {
            if (s->bitmap->tex) {
                display_3ds_free_texture(s->bitmap->tex);
                s->bitmap->tex = nullptr;
            }
            if (s->bitmap->pixels && s->bitmap->width > 0 && s->bitmap->height > 0) {
                s->bitmap->tex = display_3ds_create_texture(
                    s->bitmap->width, s->bitmap->height, s->bitmap->pixels);
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
                if (mrb_fixnum_p(vw) && mrb_fixnum_p(vh) &&
                    mrb_fixnum(vw) > 0 && mrb_fixnum(vh) > 0) {
                    mrb_value vx = mrb_iv_get(g_mkxp_mrb, ro, sy_x);
                    mrb_value vy = mrb_iv_get(g_mkxp_mrb, ro, sy_y);
                    s->src_w = (int)mrb_fixnum(vw);
                    s->src_h = (int)mrb_fixnum(vh);
                    s->src_x = mrb_fixnum_p(vx) ? (int)mrb_fixnum(vx) : 0;
                    s->src_y = mrb_fixnum_p(vy) ? (int)mrb_fixnum(vy) : 0;
                }
            }
        }

        int src_w = s->src_w > 0 ? s->src_w : s->bitmap->width;
        int src_h = s->src_h > 0 ? s->src_h : s->bitmap->height;

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

        display_3ds_blit_ex(s->bitmap->tex,
            dx, dy, draw_w, draw_h,
            (float)s->src_x, (float)s->src_y,
            (float)src_w, (float)src_h,
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
    fflush(g_dbglog);
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
                fflush(g_dbglog);
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
                fflush(g_dbglog); }
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
                    fflush(g_dbglog); }
                return bmp;
            }
        }
    }

    if (g_dbglog) {
        fprintf(g_dbglog,
            "[BMP|UNWRAP] FALHOU: DATA_PTR=NULL e nenhum fallback resultou "
            "(type=%d) -- BitmapWrapper.initialize nao chama super?\n",
            (int)mrb_type(bobj));
        fflush(g_dbglog);
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
                fflush(g_dbglog);
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
                    fflush(g_dbglog);
                }
            }
        }
    }
    return v;
}

void spriteBindingInit(mrb_state *mrb) {
    RClass *k = mrb_define_class(mrb, "Sprite", mrb->object_class);
    MRB_SET_INSTANCE_TT(k, MRB_TT_DATA);
    mrb_define_method(mrb, k, "initialize", spr_init,         MRB_ARGS_OPT(1));
    mrb_define_method(mrb, k, "dispose",    spr_dispose,      MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "disposed?",  spr_disposed,     MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "update",     spr_update,       MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "flash",      spr_flash,        MRB_ARGS_REQ(2));
    mrb_define_method(mrb, k, "bitmap",     spr_get_bitmap,   MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "bitmap=",    spr_set_bitmap,   MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "src_rect",   spr_get_src_rect, MRB_ARGS_NONE());
    mrb_define_method(mrb, k, "src_rect=",  spr_set_src_rect, MRB_ARGS_REQ(1));
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
            fflush(g_dbglog);
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
        fflush(g_dbglog);
    }
}
