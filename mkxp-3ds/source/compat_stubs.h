#pragma once
/* TAG DE BUILD (em C, FORA do raw-string Ruby). O binding_3ds.cpp imprime-a no
 * log mesmo que o Ruby nao corra -> prova QUAL compat_stubs.h o compilador usou.
 * Bumpa a cada alteracao. Se o log mostrar 'DESCONHECIDO' ou uma tag antiga, o
 * binding_3ds.cpp foi compilado contra OUTRO compat_stubs.h (pasta errada). */
#define MKXP_COMPAT_STUBS_BUILD_TAG "2026-06-16-F"
static const char *COMPAT_STUBS_RUBY = R"RUBY(
$VERBOSE = nil
$_INTL_defined = true

# ─── CARIMBO DE VERSAO (verificacao de build) ─────────────────────────────────
# Loga IMEDIATAMENTE qual compat_stubs.h esta' no binario. Se editas o header mas
# nao recompilas o binding_3ds.cpp (que e' o unico .cpp que embebe esta string),
# o Makefile mantem a versao ANTIGA -> e este carimbo prova-o no log. Bumpa a
# string a cada alteracao; o log passa a isolar "build velho" sem adivinhar.
$MKXP_COMPAT_STUBS_VERSION = "2026-06-16-F (libpng p/ intro pictures + profiler dos 232ms: game_map/interpreter/game_screen)"
(dbg "[COMPAT_STUBS] versao #{$MKXP_COMPAT_STUBS_VERSION} carregada" rescue nil)

# -- Settings ------------------------------------------------------------------
module Settings
  SCREEN_WIDTH  = 512
  SCREEN_HEIGHT = 384
  SCALE         = 1
end

# -- Encoding ------------------------------------------------------------------
module Encoding
  UTF_8      = "UTF-8"
  ASCII      = "ASCII"
  BINARY     = "BINARY"
  ASCII_8BIT = "ASCII-8BIT"
  UTF_16LE   = "UTF-16LE"
  UTF_16BE   = "UTF-16BE"
  @@di = UTF_8
  @@de = UTF_8
  def self.default_internal;     @@di; end
  def self.default_internal=(v); @@di = v; end
  def self.default_external;     @@de; end
  def self.default_external=(v); @@de = v; end
end

# -- RGSSError -----------------------------------------------------------------
class RGSSError < StandardError; end
class RGSSReset < Exception; end

# -- String extras -------------------------------------------------------------
unless "".respond_to?(:force_encoding)
  class String
    def force_encoding(e); self; end
    def encode(e, **o); self; end
    def encoding; "UTF-8"; end
    def valid_encoding?; true; end
    def b; self; end
  end
end
unless "".respond_to?(:bytesize)
  class String
    def bytesize; length; end
  end
end
unless "".respond_to?(:starts_with?)
  class String
    def starts_with?(s); start_with?(s.to_s); end
    def ends_with?(s); end_with?(s.to_s); end
  end
end
unless "".respond_to?(:gsub!)
  class String
    def gsub!(pat, rep); replace(gsub(pat, rep)); end
  end
end
unless "".respond_to?(:scan)
  class String
    def scan(pat)
      results = []
      if pat.is_a?(String) && !pat.empty?
        idx = 0
        while (pos = self.index(pat, idx))
          results << pat
          idx = pos + pat.length
        end
      end
      results
    end
  end
end
# FIX: String#scan com Regexp [x7 no log] -- o C binding do mruby lanca
# TypeError/ArgumentError quando o padrao e um Regexp stub (nao-nativo).
# Override incondicional: se o padrao for String usa logica manual segura;
# se for Regexp (stub) devolve [] sem crash.
# NOTA: nao usamos alias aqui porque o C binding de scan nao e um metodo Ruby
# normal -- alias_method pode nao funcionar sobre ele em mruby.
begin
  class String
    # [REGEXP-REAL] capturar o scan do onig ANTES de redefinir (onig carrega
    # antes do compat_stubs). Regexp -> onig real; String -> manual (o scan do
    # onig com argumento String rebenta neste build: 'string_scan' inexistente).
    alias_method :__onig_scan, :scan unless method_defined?(:__onig_scan)
    def scan(pat, &block)
      return [] if pat.nil?
      if pat.is_a?(String)
        results = []
        return results if pat.empty?
        idx = 0
        while (pos = self.index(pat, idx))
          results << pat
          idx = pos + pat.length
        end
        results
      else
        if $__strregex_n.to_i < 8
          $__strregex_n = $__strregex_n.to_i + 1
          (dbg "[STRREGEX] scan(#{pat.class})" rescue nil)
        end
        __onig_scan(pat, &block)
      end
    rescue => e
      (dbg "[STRREGEX] scan ERRO #{e.class}: #{e.message}" rescue nil)
      []
    end
  end
rescue; end

# String#split -- o C binding do mruby so aceita String como separador.
# Se o separador for um Regexp stub (ou qualquer nao-String), o C explode
# com TypeError: expected String.  Override completo em Ruby evita isso.
class String
  alias_method :__onig_split, :split unless method_defined?(:__onig_split)
  def split(sep=nil, limit=nil)
    if sep.nil?
      # Split em whitespace, omitindo tokens vazios (comportamento Ruby padrao)
      result = []
      cur    = ""
      each_char do |c|
        if c == " " || c == "\t" || c == "\n" || c == "\r"
          result << cur unless cur.empty?
          cur = ""
        else
          cur += c
        end
      end
      result << cur unless cur.empty?
      result
    elsif sep.is_a?(String)
      # Split numa string literal -- implementacao manual segura
      result = []
      rest   = self.dup
      slen   = sep.length
      if slen == 0
        # split("") -> cada caracter
        each_char { |c| result << c }
        return result
      end
      while (idx = rest.index(sep))
        result << rest[0...idx]
        rest = rest[idx + slen..-1] || ""
      end
      result << rest
      # Aplicar limit se fornecido
      if limit && limit.is_a?(Integer) && limit > 0 && result.length > limit
        tail   = result[limit-1..-1].join(sep)
        result = result[0...limit-1] << tail
      end
      result
    else
      # [REGEXP-REAL] Separador Regexp -> delegar ao onig (split real regex-aware)
      if $__strregex_n.to_i < 8
        $__strregex_n = $__strregex_n.to_i + 1
        (dbg "[STRREGEX] split(#{sep.class})" rescue nil)
      end
      begin
        limit ? __onig_split(sep, limit) : __onig_split(sep)
      rescue => e
        (dbg "[STRREGEX] split ERRO #{e.class}: #{e.message}" rescue nil)
        [self.dup]
      end
    end
  end
end

# String#[] -- mruby C binding lanca TypeError quando o indice e Regexp.
# Padrao comum em PE: str[/regex/, 1]  para extrair grupos de captura.
# Como o nosso Regexp e um stub sem match real, devolvemos nil nesses casos.
# NOTA: alias_method pode falhar silenciosamente em mruby -- usamos slice() que
# e o nome alternativo do C binding e nao e afectado pelo override de [].
begin
  class String
    # [REGEXP-REAL] capturar o [] do onig antes de redefinir; Regexp -> onig
    # (match real, incl. str[/re/,1] para grupos); resto -> slice nativo.
    alias_method :__onig_sqbr, :[] unless method_defined?(:__onig_sqbr)
    def [](idx, len=nil)
      if idx.is_a?(Integer) || idx.is_a?(String) ||
         (idx.respond_to?(:exclude_end?))  # Range
        begin
          len.nil? ? slice(idx) : slice(idx, len)
        rescue TypeError
          nil
        rescue => e
          nil
        end
      else
        # Regexp -> delegar ao onig
        if $__strregex_n.to_i < 8
          $__strregex_n = $__strregex_n.to_i + 1
          (dbg "[STRREGEX] [](#{idx.class},#{len.inspect})" rescue nil)
        end
        begin
          len.nil? ? __onig_sqbr(idx) : __onig_sqbr(idx, len)
        rescue => e
          (dbg "[STRREGEX] [] ERRO #{e.class}: #{e.message}" rescue nil)
          nil
        end
      end
    end
  end
rescue; end

# -- Numeric extras ------------------------------------------------------------
unless 0.respond_to?(:clamp)
  class Numeric
    def clamp(lo, hi)
      return lo if self < lo
      return hi if self > hi
      self
    end
  end
end

# -- Color ---------------------------------------------------------------------
begin
  class Color
    attr_accessor :red, :green, :blue, :alpha
    def initialize(r=255,g=255,b=255,a=255)
      @red=r.to_f; @green=g.to_f; @blue=b.to_f; @alpha=a.to_f
    end
    def set(r,g,b,a=255)
      @red=r.to_f; @green=g.to_f; @blue=b.to_f; @alpha=a.to_f; self
    end
    def dup; Color.new(@red,@green,@blue,@alpha); end
    def to_s; "Color(#{@red},#{@green},#{@blue},#{@alpha})"; end
  end
rescue; end

# -- Tone ----------------------------------------------------------------------
begin
  class Tone
    attr_accessor :red, :green, :blue, :gray
    def initialize(r=0,g=0,b=0,gr=0)
      @red=r.to_f; @green=g.to_f; @blue=b.to_f; @gray=gr.to_f
    end
    def set(r,g,b,gr=0)
      @red=r.to_f; @green=g.to_f; @blue=b.to_f; @gray=gr.to_f; self
    end
    def dup; Tone.new(@red,@green,@blue,@gray); end
    def to_s; "Tone(#{@red},#{@green},#{@blue},#{@gray})"; end
  end
rescue; end

# -- Rect ----------------------------------------------------------------------
begin
  class Rect
    attr_accessor :x, :y, :width, :height
    def initialize(x=0,y=0,w=0,h=0); @x=x; @y=y; @width=w; @height=h; end
    def set(x,y,w,h); @x=x; @y=y; @width=w; @height=h; self; end
    def empty; @x=0; @y=0; @width=0; @height=0; self; end
    def empty?; @width==0 && @height==0; end
    def dup; Rect.new(@x,@y,@width,@height); end
    def to_s; "Rect(#{@x},#{@y},#{@width},#{@height})"; end
  end
rescue; end

# -- Table ---------------------------------------------------------------------
begin
  class Table
    def initialize(x,y=1,z=1)
      # FIX: minimo 1 em cada dimensao -- clamp(0, xsize-1) com xsize=0 -> ArgumentError
      @x=[x.to_i,1].max; @y=[y.to_i,1].max; @z=[z.to_i,1].max
      @data=Array.new(@x*@y*@z, 0)
    end
    # FIX: usar splat -- mruby pode resolver [] para o C binding de Array
    # (expected 2) quando a assinatura tem argumentos opcionais.
    # Com splat, o dispatch Ruby prevalece e aceita 1, 2 ou 3 argumentos.
    def [](*args)
      x = args[0].to_i
      y = (args[1] || 0).to_i
      z = (args[2] || 0).to_i
      idx = z*@x*@y + y*@x + x
      (idx>=0 && idx<@data.length) ? (@data[idx]||0) : 0
    end
    def []=(*args)
      v = args.last.to_i
      x = args[0].to_i
      y = (args[1] || 0).to_i
      z = (args.length > 3 ? args[2] : 0).to_i
      idx = z*@x*@y + y*@x + x
      @data[idx] = v if idx>=0 && idx<@data.length
    end
    def xsize; @x; end
    def ysize; @y; end
    def zsize; @z; end
    # FIX: Array#data -- map_data e um Table mas pode ser acedido como Array
    # Sprite_AnimationSprite/CustomTilemap chamam map_data.data para aceder
    # ao array interno de tiles.
    def data; @data; end
    def resize(x,y=1,z=1)
      @x=[x.to_i,1].max; @y=[y.to_i,1].max; @z=[z.to_i,1].max
      @data=Array.new(@x*@y*@z, 0)
      self
    end
  end
rescue; end

# -- Font ----------------------------------------------------------------------
begin
  class Font
    @@dn="Arial"; @@ds=22; @@db=false; @@di=false
    @@dc=(Color.new(255,255,255,255) rescue nil); @@dsh=false; @@dol=false
    @@doc=(Color.new(0,0,0,255) rescue nil)
    def self.default_name;          @@dn;    end
    def self.default_name=(v);      @@dn=v;  end
    def self.default_size;          @@ds;    end
    def self.default_size=(v);      @@ds=v;  end
    def self.default_bold;          @@db;    end
    def self.default_bold=(v);      @@db=v;  end
    def self.default_italic;        @@di;    end
    def self.default_italic=(v);    @@di=v;  end
    def self.default_color;         @@dc ||= (Color.new(255,255,255,255) rescue nil); end
    def self.default_color=(v);     @@dc=v;  end
    def self.default_shadow;        @@dsh;   end
    def self.default_shadow=(v);    @@dsh=v; end
    def self.default_outline;       @@dol;   end
    def self.default_outline=(v);   @@dol=v; end
    def self.default_out_color;     @@doc ||= (Color.new(0,0,0,255) rescue nil); end
    def self.default_out_color=(v); @@doc=v; end
    def self.exist?(n); false; end
    attr_accessor :name,:size,:bold,:italic,:shadow,:outline,:out_color
    def initialize(name=@@dn,size=@@ds)
      @name=name; @size=size; @bold=@@db; @italic=@@di
      @color=(@@dc || (Color.new(255,255,255,255) rescue nil))
      @shadow=@@dsh; @outline=@@dol; @out_color=@@doc
    end
    # color nunca nil: cria a pedido (senao font.color.set rebenta no menu)
    def color
      @color ||= (Color.new(255,255,255,255) rescue nil)
    end
    def color=(v); @color=v; end
  end
rescue; end

# -- Bitmap extras -------------------------------------------------------------
# FIX: o GPU do 3DS suporta no máximo 1024x1024 por textura.
# O valor anterior (2048) fazia com que VWrap não dividisse tilesets entre
# 1024px e 2048px de altura, causando upload incompleto e ecrã preto.
begin
  unless Bitmap.respond_to?(:max_size)
    def Bitmap.max_size; 1024; end
  end
rescue; end
begin
  class Bitmap
    # mega? = true faz o CustomTilemap do jogo correr TileWrap.wrapTileset, que
    # COPIA o tileset gigante (256xN) para um bitmap re-empacotado -> +RAM, +blts,
    # e em 4444 (Gen4 Interior/Outside) isso rebenta a memoria -> magenta. Para
    # tilesets 4444 NATIVOS devolvemos false: o jogo usa o tileset ORIGINAL direto
    # como fonte de blt (o C++ recorta cada tile 32x32 em qualquer altura, incl.
    # y>1024, sem problema). Para os restantes mantem-se height>1024 (wrap barato).
    def mega?
      return false if (respond_to?(:native_4444?) && native_4444?)
      (@height || 0) > 1024
    end
  end
rescue; end

# -- Viewport ------------------------------------------------------------------
# FIX: Viewport e uma classe Ruby pura (sem binding C++) -- definir normalmente.
begin
  class Viewport
    attr_accessor :rect,:visible,:z,:ox,:oy,:color,:tone
    def initialize(*args)
      if args.length==4
        @rect=Rect.new(*args)
      elsif args.length==1 && args[0].is_a?(Rect)
        @rect=args[0]
      else
        @rect=Rect.new(0,0,Settings::SCREEN_WIDTH,Settings::SCREEN_HEIGHT)
      end
      @visible=true; @z=0; @ox=0; @oy=0
      @color=Color.new(0,0,0,0); @tone=Tone.new; @disposed=false
    end
    def disposed?; @disposed; end
    def dispose; @disposed=true; end
    def update; end
    def flash(c,d); end
  end
rescue; end

# -- Sprite --------------------------------------------------------------------
# FIX CRÍTICO: Sprite tem binding C++ (sprite_binding_3ds.cpp) que regista
# cada instância em g_sprites para render. NÃO redefinir initialize aqui --
# isso substitui o initialize C++ e os sprites nunca chegam ao render.
# Apenas adicionar métodos em falta que o jogo usa mas o binding não define.
begin
  class Sprite
    # Atributos que o binding C++ não expõe mas os scripts do jogo acedem:
    attr_accessor :viewport, :bush_depth, :bush_opacity
    attr_accessor :wave_amp, :wave_length, :wave_speed, :wave_phase
    # update/flash são no-ops seguros (binding já define update como no-op,
    # mas redefinir aqui não quebra nada pois não toca no initialize)
    def update; end unless method_defined?(:update)
    def flash(c,d); end unless method_defined?(:flash)
  end
rescue; end

# -- Plane ---------------------------------------------------------------------
# FIX: Plane é uma classe Ruby pura (sem binding C++). O motor 3DS so' desenha
# Sprites (g_sprites), por isso um Plane sem Sprite interno NUNCA aparece -- o
# background de cenas Essentials que usam addBackgroundPlane (ecra de nome
# "Naming/bg_2", Trainer Card "Trainer Card/bg", creditos, etc.) ficava por
# desenhar e no seu lugar via-se lixo de VRAM (padrao xadrez amarelo). Damos ao
# Plane um Sprite interno que mostra o @bitmap, espelhando o que ja' se faz na
# classe Window (@__csprite). ox/oy (scroll do plano) -> deslocam o sprite;
# visible/z/opacity/zoom/blend sincronizados. Sem custo por-frame extra alem do
# que o Sprite ja' tem. Generico p/ qualquer cena RMXP/Essentials.
begin
  class Plane
    attr_accessor :zoom_x,:zoom_y,:opacity,:blend_type,:color,:tone,:viewport
    def initialize(viewport=nil)
      @visible=true; @z=0; @ox=0; @oy=0; @zoom_x=1.0; @zoom_y=1.0
      @opacity=255; @blend_type=0; @color=Color.new(0,0,0,0)
      @tone=Tone.new; @viewport=viewport; @disposed=false; @bitmap=nil
      begin
        @__psprite = Sprite.new(@viewport)
        # P3: passar o viewport pelo SETTER (spr_init ignora o arg do new), para
        # o vp_z da struct ser preenchido e o sort por (viewport.z, z) funcionar.
        # Sem isto, o background (Plane) ficava com vp_z=0 e caia atras do mapa.
        (@__psprite.viewport = @viewport) rescue nil
        @__psprite.z = @z
      rescue
        @__psprite = nil
      end
    end
    def bitmap; @bitmap; end
    def bitmap=(v)
      @bitmap = v
      begin
        if @__psprite && !@__psprite.disposed?
          @__psprite.bitmap = v
          __sync_psprite
        end
      rescue; end
      # DIAGNOSTICO (bg de cenas que nao aparece, ex: teclado "Naming/bg_2"):
      # logar o estado do psprite nas primeiras vezes p/ ver se recebe o bitmap.
      begin
        $__plane_diag ||= 0
        if $__plane_diag < 8
          $__plane_diag += 1
          bw = (v && !(v.disposed? rescue true)) ? "#{(v.width rescue '?')}x#{(v.height rescue '?')}" : "nil"
          ps = @__psprite ? "ok" : "NIL"
          psb = "?"
          if @__psprite && !(@__psprite.disposed? rescue true)
            pb = (@__psprite.bitmap rescue nil)
            psb = (pb && !(pb.disposed? rescue true)) ? "#{(pb.width rescue '?')}x#{(pb.height rescue '?')}" : "nil"
          end
          vpz = (@viewport && (@viewport.z rescue nil)) ? (@viewport.z rescue '?') : "sem-vp"
          (dbg "[PLANE-DIAG] bitmap= recebido=#{bw} psprite=#{ps} psprite.bitmap=#{psb} vis=#{@visible} vpz=#{vpz}") rescue nil
        end
      rescue; end
      v
    end
    def __sync_psprite
      return unless @__psprite && (@__psprite.disposed? rescue true)==false
      begin
        @__psprite.x = -(@ox || 0)
        @__psprite.y = -(@oy || 0)
        @__psprite.z = @z
        # P3: re-propagar o viewport re-le o seu z para o vp_z (o ecra de escolha
        # de personagem faz viewport.z=99999 e depois cria o Plane; mas se o z
        # mudasse depois, isto mantem o vp_z em dia). Custo: 1 leitura de #z.
        (@__psprite.viewport = @viewport) rescue nil
        @__psprite.visible = @visible
        @__psprite.opacity = @opacity
        (@__psprite.zoom_x = @zoom_x) rescue nil
        (@__psprite.zoom_y = @zoom_y) rescue nil
        (@__psprite.blend_type = @blend_type) rescue nil
      rescue; end
    end
    def visible; @visible; end
    def visible=(v); @visible=v; __sync_psprite; v; end
    def z; @z; end
    def z=(v); @z=v; __sync_psprite; v; end
    def ox; @ox; end
    def ox=(v); @ox=v; __sync_psprite; v; end
    def oy; @oy; end
    def oy=(v); @oy=v; __sync_psprite; v; end
    def disposed?; @disposed; end
    def dispose
      @disposed=true
      begin; @__psprite.dispose if @__psprite && !@__psprite.disposed?; rescue; end
    end
    def update; __sync_psprite; end
  end
rescue; end

# -- Tilemap -------------------------------------------------------------------
# FIX CRÍTICO: Tilemap tem binding C++ (tilemap_binding_3ds.cpp) que regista
# em g_tilemaps para render. NÃO redefinir initialize.
# Apenas adicionar métodos/atributos em falta.
begin
  class Tilemap
    # viewport não é exposto pelo binding mas os scripts acedem a ele
    attr_accessor :viewport
    # autotiles setter mais robusto (o binding devolve [] vazio em autotiles)
    def autotiles=(v)
      if v.is_a?(Array)
        @__autotiles_rb = v.map { |x| x.nil? ? "" : x.to_s }
        @__autotiles_rb += Array.new([0, 7 - @__autotiles_rb.length].max, "") if @__autotiles_rb.length < 7
      end
    end unless method_defined?(:autotiles=)
    def autotiles
      @__autotiles_rb ||= Array.new(7,"")
    end unless method_defined?(:autotiles)
    def update; end unless method_defined?(:update)
  end
rescue; end

# -- Window --------------------------------------------------------------------
# FIX: Window é uma classe Ruby pura (sem binding C++) -- definir normalmente.
begin
  class Window
    attr_accessor :windowskin,:stretch,:cursor_rect,:active
    attr_accessor :visible,:pause,:z,:ox,:oy
    attr_accessor :opacity,:back_opacity,:contents_opacity,:viewport
    def initialize(viewport=nil)
      @visible=true; @active=true; @pause=false; @stretch=true
      @x=0; @y=0; @z=100; @width=0; @height=0; @ox=0; @oy=0
      @opacity=255; @back_opacity=200; @contents_opacity=255
      @cursor_rect=Rect.new; @viewport=viewport; @disposed=false
      @contents=nil
      # SPRITE INTERNO: o sistema de render so' desenha Sprites. Para o conteudo
      # da janela (texto dos menus, desenhado em @contents via draw_text) ser
      # VISIVEL, criamos um Sprite que mostra @contents. Sem isto, as janelas
      # aparecem como caixas pretas (contents existe mas nunca vai ao ecra).
      begin
        @__csprite = Sprite.new(@viewport)
        @__csprite.z = @z + 1
      rescue
        @__csprite = nil
      end
      begin; self.contents = Bitmap.new(1,1); rescue; end
    end
    def contents; @contents; end
    def contents=(bmp)
      @contents = bmp
      # liga o bitmap do conteudo ao sprite interno -> passa a ser desenhado
      begin
        if @__csprite && !@__csprite.disposed?
          @__csprite.bitmap = bmp
          __sync_csprite
        end
      rescue; end
      bmp
    end
    def x; @x; end
    def x=(v); @x=v; __sync_csprite; v; end
    def y; @y; end
    def y=(v); @y=v; __sync_csprite; v; end
    def width; @width; end
    def width=(v); @width=v; v; end
    def height; @height; end
    def height=(v); @height=v; v; end
    # Sincroniza o sprite do conteudo com a janela. A moldura RGSS reserva 16px
    # de margem; o conteudo desenha-se deslocado (x+16, y+16) menos o scroll ox/oy.
    def __sync_csprite
      return unless @__csprite && (@__csprite.disposed? rescue true)==false
      begin
        @__csprite.x = @x + 16 - @ox
        @__csprite.y = @y + 16 - @oy
        @__csprite.z = @z + 1
        @__csprite.visible = @visible
        @__csprite.opacity = @contents_opacity
      rescue; end
    end
    def disposed?; @disposed; end
    def dispose
      @disposed=true
      begin; @__csprite.dispose if @__csprite && !@__csprite.disposed?; rescue; end
      begin; @contents.dispose if @contents && !@contents.disposed?; rescue; end
    end
    def update; __sync_csprite; end
    def refresh; __sync_csprite; end
  end
rescue; end

# -- Audio ---------------------------------------------------------------------
begin
  module Audio
    def self.bgm_play(f,v=100,p=100,pos=0); end
    def self.bgm_stop; end
    def self.bgm_fade(t); end
    def self.bgm_pos; 0; end
    def self.bgs_play(f,v=80,p=100,pos=0); end
    def self.bgs_stop; end
    def self.bgs_fade(t); end
    def self.me_play(f,v=100,p=100); end
    def self.me_stop; end
    def self.me_fade(t); end
    def self.se_play(f,v=80,p=100); end
    def self.se_stop; end
    def self.update; end
  end
rescue; end

# -- Graphics extras -----------------------------------------------------------
begin
  unless Graphics.respond_to?(:resize_screen)
    def Graphics.resize_screen(w,h); end
  end
  unless Graphics.respond_to?(:snap_to_bitmap)
    def Graphics.snap_to_bitmap
      Bitmap.new(Settings::SCREEN_WIDTH, Settings::SCREEN_HEIGHT)
    end
  end
  unless Graphics.respond_to?(:brightness)
    def Graphics.brightness; 255; end
    def Graphics.brightness=(v); end
  end
  unless Graphics.respond_to?(:scale)
    def Graphics.scale; 1.0; end
    def Graphics.scale=(v); end
  end
  unless Graphics.respond_to?(:center)
    def Graphics.center; end
  end
  unless Graphics.respond_to?(:fullscreen)
    def Graphics.fullscreen; false; end
    def Graphics.fullscreen=(v); end
  end
  unless Graphics.respond_to?(:delta)
    def Graphics.delta; 16666; end
  end
  unless Graphics.respond_to?(:delta_s)
    def Graphics.delta_s; (Graphics.delta || 16666) / 1_000_000.0; end
  end
rescue; end

# -- System (cria se nao existir como constante C) -----------------------------
begin
  System
rescue NameError
  module System; end
end
begin
  def System.data_directory; "sdmc:/mkxp/game/Save"; end
  unless System.respond_to?(:game_title)
    def System.game_title; "Game"; end
  end
  unless System.respond_to?(:set_window_title)
    def System.set_window_title(s); end
  end
  unless System.respond_to?(:uptime)
    def System.uptime; 0; end
  end
  unless System.respond_to?(:platform)
    def System.platform; "Nintendo 3DS"; end
  end
rescue; end

# -- File extras ---------------------------------------------------------------
begin
  class File
    SEPARATOR     = "/"
    ALT_SEPARATOR = "\\"
    PATH_SEPARATOR = ":"

    unless respond_to?(:basename)
      def self.basename(path, suffix=nil)
        return "" if !path || path.to_s.empty?
        p2 = path.to_s.gsub("\\", "/")
        p2 = p2[0...-1] while p2.length > 1 && p2[-1] == "/"
        idx  = p2.rindex("/")
        base = idx ? p2[idx+1..-1] : p2
        if suffix && !suffix.to_s.empty?
          if suffix == ".*"
            dot = base.rindex(".")
            base = base[0...dot] if dot && dot > 0
          elsif base.end_with?(suffix.to_s)
            base = base[0...base.length - suffix.to_s.length]
          end
        end
        base
      end
    end

    unless respond_to?(:dirname)
      def self.dirname(path)
        return "." if !path || path.to_s.empty?
        p2  = path.to_s.gsub("\\", "/")
        idx = p2.rindex("/")
        return "." if !idx || idx == 0
        p2[0...idx]
      end
    end

    unless respond_to?(:extname)
      def self.extname(path)
        base = self.basename(path.to_s)
        dot  = base.rindex(".")
        return "" if !dot || dot == 0
        base[dot..-1]
      end
    end

    unless respond_to?(:join)
      def self.join(*parts)
        parts.compact.map(&:to_s).reject(&:empty?).join("/").gsub(/\/\/+/, "/")
      end
    end

    unless respond_to?(:split)
      def self.split(path)
        [self.dirname(path.to_s), self.basename(path.to_s)]
      end
    end

    unless respond_to?(:file?)
      def self.file?(p)
        return false if p.nil?
        # Igual ao FileTest._try_open: em ambientes que nao lancam excecao para
        # ficheiros inexistentes (Azahar), exigir handle real + 1 byte legivel,
        # senao um save inexistente passa por "existe" e o get_newest_save_slot
        # rebenta (NullStub > Integer). Save inexistente -> false -> New Game.
        begin
          f = open(p.to_s, "rb")
          return false if f.nil?
          ok = false
          begin
            b = f.read(1)
            ok = !b.nil? && b.respond_to?(:bytesize) && b.bytesize > 0
          rescue
            ok = false
          end
          f.close rescue nil
          ok
        rescue
          false
        end
      end
    end

    def self.directory?(p); false; end

    unless respond_to?(:move)
      def self.move(src,dst)
        begin
          f=open(src,"rb"); d=f.read; f.close
          f=open(dst,"wb"); f.write(d); f.close
          File.delete(src)
        rescue; end
      end
    end
  end
rescue; end

# -- Dir (garante que e Class reabrivel pelo script do jogo) ------------------
begin
  class Dir; end
rescue TypeError
  Dir = Class.new
end
begin
  class Dir
    def self.mkdir(p,m=0777); 0; end
    def self.exist?(p); false; end
    def self.exists?(p); false; end
    def self.glob(pat,*a); []; end
    def self.[](pat); []; end
    def self.pwd; "sdmc:/mkxp/game"; end
    def self.getwd; pwd; end
    def self.chdir(p); nil; end
    def self.entries(p); []; end
    def self.home; "sdmc:/mkxp"; end
  end
rescue; end

# -- Marshal -------------------------------------------------------------------
begin
  module Marshal
    MAJOR_VERSION = 4
    MINOR_VERSION = 8
    def self.dump(obj,io=nil,limit=-1); ""; end
    def self.load(str,*a); nil; end
    def self.restore(str); nil; end
  end
rescue; end

# -- StringIO ------------------------------------------------------------------
begin
  class StringIO
    def initialize(s="",mode="r+"); @s=s.to_s; @pos=0; end
    def read(n=nil)
      if n
        r=@s[@pos,n.to_i]||""; @pos+=n.to_i; r
      else
        r=@s[@pos..-1].to_s; @pos=@s.length; r
      end
    end
    def write(s); @s+=s.to_s; s.to_s.length; end
    def puts(s=""); write(s.to_s+"\n"); end
    def string; @s; end
    def string=(v); @s=v.to_s; @pos=0; end
    def rewind; @pos=0; self; end
    def pos; @pos; end
    def pos=(n); @pos=n.to_i; end
    def eof?; @pos>=@s.length; end
    def close; nil; end
    def seek(n,w=0)
      @pos=(w==0 ? n : w==1 ? @pos+n : @s.length+n)
      @pos=0 if @pos<0
      self
    end
    def tell; @pos; end
    def size; @s.length; end
    def flush; self; end
    def binmode; self; end
  end
rescue; end

# -- Zlib ----------------------------------------------------------------------
begin
  module Zlib
    BEST_SPEED=1; BEST_COMPRESSION=9; DEFAULT_COMPRESSION=-1
    NO_COMPRESSION=0; SYNC_FLUSH=2; FINISH=4
    def self.deflate(str,level=-1); str.to_s; end
    def self.inflate(str); str.to_s; end
    class Deflate
      def initialize(l=-1,w=15,m=8,s=0); @buf=""; end
      def deflate(str,f=0); @buf+=str.to_s; ""; end
      def finish; @buf; end
      def close; nil; end
      def self.deflate(str,l=-1); str.to_s; end
    end
    class Inflate
      def initialize(w=15); @buf=""; end
      def inflate(str); @buf+=str.to_s; ""; end
      def finish; @buf; end
      def close; nil; end
      def self.inflate(str); str.to_s; end
    end
  end
rescue; end

# -- Process -------------------------------------------------------------------
begin
  module Process
    def self.exit(code=0); raise SystemExit; end
    def self.pid; 1; end
  end
rescue; end

# -- RPG -----------------------------------------------------------------------
begin
  module RPG
    class AudioFile
      attr_accessor :name, :volume, :pitch
      def initialize(name="", volume=100, pitch=100)
        @name=name; @volume=volume; @pitch=pitch
      end
      def dup; self.class.new(@name, @volume, @pitch); end
      def to_s; @name.to_s; end
    end

    class BGM
      @@last=nil
      attr_accessor :name,:volume,:pitch
      def initialize(name="",vol=100,pitch=100)
        @name=name; @volume=vol; @pitch=pitch
      end
      def play
        begin; Audio.bgm_play(@name,@volume,@pitch); rescue; end
        @@last=self
      end
      def self.stop;    begin; Audio.bgm_stop;    rescue; end; end
      def self.fade(t); begin; Audio.bgm_fade(t); rescue; end; end
      def self.last;    @@last || self.new; end
      def self.last=(v); @@last=v; end
    end
    class BGS
      @@last=nil
      attr_accessor :name,:volume,:pitch
      def initialize(name="",vol=80,pitch=100)
        @name=name; @volume=vol; @pitch=pitch
      end
      def play
        begin; Audio.bgs_play(@name,@volume,@pitch); rescue; end
        @@last=self
      end
      def self.stop;    begin; Audio.bgs_stop;    rescue; end; end
      def self.fade(t); begin; Audio.bgs_fade(t); rescue; end; end
      def self.last;    @@last || self.new; end
      def self.last=(v); @@last=v; end
    end
    class ME
      attr_accessor :name,:volume,:pitch
      def initialize(name="",vol=100,pitch=100)
        @name=name; @volume=vol; @pitch=pitch
      end
      def play;         begin; Audio.me_play(@name,@volume,@pitch); rescue; end; end
      def self.stop;    begin; Audio.me_stop;    rescue; end; end
      def self.fade(t); begin; Audio.me_fade(t); rescue; end; end
    end
    class SE
      attr_accessor :name,:volume,:pitch
      def initialize(name="",vol=80,pitch=100)
        @name=name; @volume=vol; @pitch=pitch
      end
      def play; begin; Audio.se_play(@name,@volume,@pitch); rescue; end; end
    end

    # ------------------------------------------------------------------
    # RPG::Event::Page::Condition -- necessario para Game_Event#setup
    # ------------------------------------------------------------------
    class Event
      attr_accessor :id, :name, :x, :y, :pages
      def initialize(id=0, x=0, y=0)
        @id=id; @name=""; @x=x; @y=y; @pages=[]
      end
      class Page
        attr_accessor :condition, :graphic, :move_type, :move_speed
        attr_accessor :move_frequency, :move_route, :walk_anime
        attr_accessor :step_anime, :direction_fix, :through, :always_on_top
        attr_accessor :trigger, :list
        def initialize
          @condition=Condition.new; @graphic=Graphic.new
          @move_type=0; @move_speed=3; @move_frequency=3
          @walk_anime=true; @step_anime=false; @direction_fix=false
          @through=false; @always_on_top=false; @trigger=0; @list=[]; @move_route=nil
        end
        class Condition
          attr_accessor :switch1_valid, :switch2_valid, :variable_valid
          attr_accessor :self_switch_valid, :switch1_id, :switch2_id
          attr_accessor :variable_id, :variable_value, :self_switch_ch
          def initialize
            @switch1_valid=false; @switch2_valid=false; @variable_valid=false
            @self_switch_valid=false; @switch1_id=1; @switch2_id=1
            @variable_id=1; @variable_value=0; @self_switch_ch="A"
          end
        end
        class Graphic
          attr_accessor :tile_id, :character_name, :character_hue
          attr_accessor :direction, :pattern, :opacity, :blend_type
          def initialize
            @tile_id=0; @character_name=""; @character_hue=0
            @direction=2; @pattern=0; @opacity=255; @blend_type=0
          end
        end
      end
    end

    # ------------------------------------------------------------------
    # RPG::EventCommand -- CRITICO: o interpretador le .code/.parameters/.indent
    # de CADA comando. Sem acessores, .code -> method_missing -> nil e NENHUM
    # comando de evento e' despachado (intro, mensagens, escolhas: tudo no-op).
    # O marshal ja' popula @code/@indent/@parameters; faltava expor os acessores.
    # ------------------------------------------------------------------
    class EventCommand
      attr_accessor :code, :indent, :parameters
      def initialize(code=0, indent=0, parameters=[])
        @code=code; @indent=indent; @parameters=parameters
      end
    end

    # RPG::MoveCommand / RPG::MoveRoute -- rotas de movimento de eventos.
    class MoveCommand
      attr_accessor :code, :parameters
      def initialize(code=0, parameters=[])
        @code=code; @parameters=parameters
      end
    end
    class MoveRoute
      attr_accessor :repeat, :skippable, :list
      def initialize
        @repeat=true; @skippable=false; @list=[MoveCommand.new(0)]
      end
    end

    # ------------------------------------------------------------------
    # RPG::MapInfo -- usado por $map_infos (carregado de MapInfos.rxdata)
    # ------------------------------------------------------------------
    class MapInfo
      attr_accessor :name, :parent_id, :order, :expanded, :scroll_x, :scroll_y
      def initialize
        @name=""; @parent_id=0; @order=0; @expanded=false
        @scroll_x=0; @scroll_y=0
      end
    end

    # ------------------------------------------------------------------
    # RPG::Tileset -- referenciado por Game_Map#setup via $data_tilesets
    # tileset_id, tileset_name, autotile_names, panorama_name, etc.
    # ------------------------------------------------------------------
    class Tileset
      attr_accessor :id, :name, :tileset_name, :autotile_names
      attr_accessor :panorama_name, :panorama_hue
      attr_accessor :fog_name, :fog_hue, :fog_opacity, :fog_blend_type
      attr_accessor :fog_zoom, :fog_sx, :fog_sy
      attr_accessor :battleback_name, :passages, :priorities, :terrain_tags
      def initialize
        @id=0; @name=""; @tileset_name=""
        # FIX: autotile_names DEVEM ser strings -- pbGetAutotile concatena
        @autotile_names=Array.new(7,"")
        @panorama_name=""; @panorama_hue=0
        @fog_name=""; @fog_hue=0; @fog_opacity=64; @fog_blend_type=0
        @fog_zoom=200; @fog_sx=0; @fog_sy=0
        @battleback_name=""
        @passages    = Table.new(1)
        @priorities  = Table.new(1)
        @terrain_tags= Table.new(1)
      end
    end

    # ------------------------------------------------------------------
    # RPG::Map -- referenciado por PokemonMapFactory/Game_Map#setup
    # width, height, tileset_id, events, autoplay_bgm, etc.
    # ------------------------------------------------------------------
    class Map
      attr_accessor :tileset_id, :width, :height, :autoplay_bgm, :autoplay_bgs
      attr_accessor :bgm, :bgs, :encounter_list, :encounter_step, :data, :events
      def initialize(width=20, height=15)
        @tileset_id=1; @width=width; @height=height
        @autoplay_bgm=false; @autoplay_bgs=false
        @bgm=RPG::BGM.new; @bgs=RPG::BGS.new
        @encounter_list=[]; @encounter_step=30
        @data=Table.new(@width,@height,3); @events={}
      end
    end

    class System
      attr_accessor :title_bgm, :battle_bgm, :battle_end_me, :gameover_me
      attr_accessor :title_name, :game_title, :magic_number
      attr_accessor :party_members, :elements, :switches, :variables
      attr_accessor :currency_unit, :battleback_name
      attr_accessor :battler_name, :battler_hue
      attr_accessor :start_map_id, :start_x, :start_y
      attr_accessor :test_troop_id, :edit_map_id
      # SE de interface (RMXP): sem estes campos, $data_system.decision_se etc.
      # caem no method_missing -> 0 -> pbSEPlay(0) nao toca som nenhum nos menus.
      # Adicionados com os nomes/volumes padrao do RPG Maker XP.
      attr_accessor :cursor_se, :decision_se, :cancel_se, :buzzer_se
      attr_accessor :equip_se, :shop_se, :save_se, :load_se
      attr_accessor :battle_start_se, :escape_se, :actor_collapse_se, :enemy_collapse_se
      attr_accessor :windowskin_name
      def initialize
        @title_bgm       = RPG::BGM.new
        @battle_bgm      = RPG::BGM.new
        @battle_end_me   = RPG::ME.new
        @gameover_me     = RPG::ME.new
        @title_name      = ""
        @game_title      = "Game"
        @magic_number    = 0
        @party_members   = []
        @elements        = []
        @switches        = [""]
        @variables       = [""]
        @currency_unit   = "$"
        @battleback_name = ""
        @battler_name    = ""
        @battler_hue     = 0
        @start_map_id    = 1
        @start_x         = 0
        @start_y         = 0
        @test_troop_id   = 1
        @edit_map_id     = 1
        @windowskin_name = ""
        # SE de UI com defaults do RMXP (estes ficheiros existem no Audio/SE/).
        @cursor_se          = RPG::SE.new("Cursor1", 80)
        @decision_se        = RPG::SE.new("Decision", 80)
        @cancel_se          = RPG::SE.new("Cancel1", 80)
        @buzzer_se          = RPG::SE.new("Buzzer1", 80)
        @equip_se           = RPG::SE.new("Equip", 80)
        @shop_se            = RPG::SE.new("Shop", 80)
        @save_se            = RPG::SE.new("Save", 80)
        @load_se            = RPG::SE.new("Load", 80)
        @battle_start_se    = RPG::SE.new("", 80)
        @escape_se          = RPG::SE.new("", 80)
        @actor_collapse_se  = RPG::SE.new("", 80)
        @enemy_collapse_se  = RPG::SE.new("", 80)
      end
      def windowskin_name
        @windowskin_name.is_a?(String) ? @windowskin_name : ""
      end
      def windowskin_name=(v)
        @windowskin_name = v.to_s rescue ""
      end
      def wordlist; @wordlist ||= Array.new(8, ""); end
    end
  end
rescue; end

# -- Kernel helpers ------------------------------------------------------------
def pbGetBasicFont;     begin; Font.default_name; rescue; "Arial"; end; end
def pbGetOutlineFont;   begin; Font.default_name; rescue; "Arial"; end; end
def pbGetBasicFontSize; begin; Font.default_size; rescue; 22;      end; end
def isOneOf?(x,*vals);  vals.include?(x); end
def _INTL(s="",*args)
  r=s.to_s.dup
  args.each_with_index{|a,i| r=r.gsub("{#{i+1}}",a.to_s)}
  r
end
def _INTLR(s,*args); _INTL(s,*args); end
def _ISINTL(s,*args); _INTL(s,*args); end

# -- FileTest -- override total seguro para mruby ------------------------------
# PROBLEMA: em mruby, se FileTest existe como modulo C nativo, `module FileTest; end`
# reabre-o silenciosamente sem adicionar os metodos. O `rescue TypeError` nunca corre.
# O script PE FileTests (script 19) define `validate` internamente e chama
# FileTest.exists?(path) -- que cai no binding C nativo com assinatura incompativel
# -> ArgumentError: wrong number of arguments (given 1, expected 1).
# SOLUCAO: redefinir TODOS os metodos directamente com `def self.*` fora de qualquer
# bloco condicional, garantindo override mesmo sobre o binding C.
begin
  module FileTest
    # FIX: tentativa real via File.open antes de retornar false.
    # Em 3DS com sdmc: montado isto funciona para os assets do jogo.
    # Se File.open falhar (ficheiro inexistente) retornamos false sem crash.
    GAME_ROOTS_FT = [
      "",
      "sdmc:/mkxp/game/",
      "sdmc:/mkxp/",
      "romfs:/",
    ]
    def self._try_open(p)
      return false if p.nil? || p.to_s.empty?
      s = p.to_s
      GAME_ROOTS_FT.each do |root|
        full = root.empty? ? s : "#{root}#{s}"
        begin
          fh = open(full, "rb")
          # CRITICO (crash get_newest_save_slot sem save): alguns ambientes
          # (Azahar) nao lancam excecao para ficheiros inexistentes -- `open`
          # devolve nil ou um stub. Se aceitarmos isso como "existe", o
          # get_newest_save_slot tenta ler um save vazio e faz
          # `NullStub > Integer` -> ArgumentError -> jogo fecha. Por isso
          # exigimos um handle real E que consiga ler >=1 byte (ficheiro com
          # conteudo). Um save inexistente falha aqui e devolve false -> o
          # jogo trata como "sem save" e vai a New Game.
          if fh.nil?
            next
          end
          ok = false
          begin
            b = fh.read(1)
            ok = !b.nil? && b.respond_to?(:bytesize) && b.bytesize > 0
          rescue
            ok = false
          end
          fh.close rescue nil
          return true if ok
          next
        rescue
          next
        end
      end
      false
    end
    def self.exist?(p);       _try_open(p); end
    def self.exists?(p);      _try_open(p); end
    def self.file?(p);        _try_open(p); end
    def self.directory?(p);   false; end
    # CRITICO p/ o AUDIO funcionar: o jogo chama audio_exist?("Audio/SE/NOME")
    # SEM extensao. Os ficheiros reais sao NOME.ogg / .wav / .mp3. Se so'
    # tentassemos o caminho exacto, audio_exist? devolvia false e Audio.se_play
    # NUNCA seria chamado (som nenhum). Tentamos o caminho tal-qual E com as
    # extensoes de audio.
    def self.audio_exist?(p)
      return false if p.nil? || p.to_s.empty?
      return true if _try_open(p)
      base = p.to_s
      [".ogg", ".wav", ".mp3", ".OGG", ".WAV"].each do |ext|
        return true if _try_open(base + ext)
      end
      false
    end
    def self.size?(p);        nil;   end
    def self.zero?(p);        !_try_open(p); end
    def self.readable?(p);    _try_open(p); end
    def self.writable?(p);    false; end
    def self.executable?(p);  false; end
  end
rescue; end

# -- pbDayNightTint stub -------------------------------------------------------
# pbDayNightTint chama FileTest.exists? -> validate -> ArgumentError em mruby.
# Mesmo com o FileTest corrigido, a funcao precisa de Tone/Bitmap reais para funcionar.
# Neutralizar completamente ate ter rendering real.
begin
  def pbDayNightTint(sprite); end
  module Kernel
    def pbDayNightTint(sprite); end
    module_function :pbDayNightTint
  end
rescue; end

# -- Regexp -------------------------------------------------------------------
# [REGEXP-REAL] Stub no-op de Regexp REMOVIDO. O mruby-onig-regexp faz
# 'Regexp = OnigRegexp' no mrb_open (cedo). Como o COMPAT_STUBS_RUBY corre
# DEPOIS, este 'class Regexp ... end' reabria o PROPRIO OnigRegexp e injetava-lhe
# no-ops (===->false, =~->nil, match->nil) e um to_str->String. Esse to_str fazia
# o gsub nativo tentar converter o OnigRegexp para String -> "TypeError: OnigRegexp
# cannot be converted to String" em getFormattedTextForDims/resizeToFit, rebentando
# (em rescue silencioso) a criacao da janela de mensagem -> intro com caixa vazia.
# O onig ja fornece Regexp/OnigRegexp real e completo; nao mexer aqui.

# -- Kernel --------------------------------------------------------------------
# Metodos aqui ficam disponiveis em qualquer instancia (Kernel e mixado em Object)
# e tambem como funcoes de topo de nivel.
begin
  module Kernel
    # Deprecation helpers (fix 6)
    def self.deprecate_constant(*args); end
    def self.deprecated_method_alias(*args); end
    def self.deprecated_constant(*args); end

    # KGC ScreenCapture -- chamado sem receiver dentro de update() de sprites/scenes.
    # O stub em graphics_binding.cpp e module function e nao chega a instancias;
    # definir aqui garante que qualquer objecto o herda via mixin.
    def update_KGC_ScreenCapture; end
  end
  def deprecate_constant(*args); end
  def deprecated_method_alias(*args); end
  def deprecated_constant(*args); end
rescue; end


# -- Input (constantes RGSS standard + stubs) ---------------------------------
# Script 7 (Input, 658 bytes) redefine o modulo mas omite as teclas de funcao.
# Injectamos aqui ANTES dos scripts; se o script 7 reabrir Input sem apagar
# constantes, estas sobrevivem. Se apagar, adicionar tambem pos-load no binding.
begin
  module Input
    # Direccoes
    DOWN  = 2
    LEFT  = 4
    RIGHT = 6
    UP    = 8
    # Botoes RGSS
    A     = 11
    B     = 12
    C     = 13
    X     = 14
    Y     = 15
    Z     = 16
    L     = 17
    R     = 18
    # Modificadores
    SHIFT = 21
    CTRL  = 22
    ALT   = 23
    # Teclas de funcao
    F5    = 25
    F6    = 26
    F7    = 27
    F8    = 28
    F9    = 29
    # Metodos stub (caso o script 7 nao os defina)
    def self.press?(key);   false; end
    def self.trigger?(key); false; end
    def self.repeat?(key);  false; end
    def self.dir4;          0;     end
    def self.dir8;          0;     end
    def self.update;               end
  end
rescue; end
# -- Globals -------------------------------------------------------------------
$BTEST              = false
$DEBUG              = true
$game_system        ||= nil
$game_map           ||= nil
$game_player        ||= nil
$game_party         ||= nil
$game_switches      ||= nil
$game_variables     ||= nil
$game_self_switches ||= nil
$game_actors        ||= nil
$game_temp          ||= nil
$data_system        = RPG::System.new
$scene              ||= nil
$game_screen        ||= nil
$PokemonSystem      ||= nil
$PokemonMap         ||= nil
$PokemonGlobal      ||= nil
$PokemonStorage     ||= nil
$Trainer            ||= nil
$ResizeInitialized   = false
# Dados RGSS -- inicializar com tipos correctos para evitar missing methods em massa
# Game_Map#setup acede a $data_tilesets[id] e espera RPG::Tileset com tileset_name, etc.
# check_entry_methods() vai reforcar estes depois dos scripts, mas ter aqui garante
# que qualquer acesso antes disso nao crasha.
$data_tilesets      ||= [nil, RPG::Tileset.new]
$data_map_infos     ||= {}
$data_animations    ||= []
$data_common_events ||= [nil]

# -- rand / oldRand fix -------------------------------------------------------
# NAO fazer alias aqui: os scripts PE (RubyUtilities/MKXP_Compatibility) fazem:
#   alias oldRand rand
#   def rand(*args); oldRand(*args); end   # dentro de class<<Kernel
# Se fizermos alias antes dos scripts, depois do override oldRand aponta para
# o novo rand -> recursao infinita (SystemStackError).
# O fix real e feito em check_entry_methods() em binding_3ds.cpp, pos-scripts,
# redefinindo rand e oldRand para delegarem para __native_rand__ (C nativo).

# -- Object#method stub -------------------------------------------------------
# mruby sem o mrbgem 'mruby-method' nao tem Object#method.
# PE usa-o para guardar callbacks: @cb = method(:open_splash)
# Devolvemos um Proc que delega para send(), o que e suficiente
# para .call(*args) e para passar como bloco.
begin
  class Object
    unless method_defined?(:method) || respond_to?(:method)
      def method(sym)
        s   = sym.to_sym
        obj = self
        proc { |*args, &blk| obj.send(s, *args, &blk) }
      end
    end
  end
rescue
end

# -- method_nil stub -----------------------------------------------------------
# PE MKXP_Compatibility e RubyUtilities definem/usam method_nil como alias
# de nil? ou como fallback seguro de method_missing.
# mruby nao suporta alias de metodos built-in da mesma forma que MRI Ruby,
# por isso definimos o metodo directamente aqui antes de qualquer script.
begin
  class Object
    def method_nil(*a)
      nil
    end
  end
  class NilClass
    def method_nil(*a)
      nil
    end
  end
rescue
end

# -- NilClass safe stubs -------------------------------------------------------
# Portado de scripts/postload/nilclass_safe_stubs.rb do mkxp-z iOS.
# mruby nao aceita one-liners de operadores (def %(*)  0; end) -- usamos
# forma multi-linha para todos os metodos para garantir parse correcto.
begin
  class NilClass
    def %(other)
      0
    end
    def *(other)
      0
    end
    def **(other)
      0
    end
    def +(other)
      other
    end
    def -(other)
      0
    end
    def /(other)
      0
    end
    def <(other)
      true
    end
    def <<(other)
      0
    end
    def <=(other)
      false
    end
    def <=>(other)
      0
    end
    def >(other)
      false
    end
    def >=(other)
      false
    end
    def >>(other)
      0
    end
    def |(other)
      0
    end
    def abs
      0
    end
    def ceil(*a)
      0
    end
    def coerce(other)
      [other, 0]
    end
    def div(*a)
      0
    end
    def divmod(*a)
      []
    end
    def even?
      true
    end
    def fdiv(*a)
      0.0
    end
    def floor(*a)
      0
    end
    def finite?
      true
    end
    def hash
      0
    end
    def id
      nil
    end
    def infinite?
      true
    end
    def integer?
      false
    end
    def modulo(*a)
      0
    end
    def nan?
      true
    end
    def next
      0
    end
    def nonzero?
      false
    end
    def odd?
      false
    end
    def ord
      0
    end
    def quo(*a)
      0.0
    end
    def real
      0
    end
    def real?
      false
    end
    def remainder(*a)
      0
    end
    def round(*a)
      0
    end
    def succ
      0
    end
    def to_int
      0
    end
    def truncate(*a)
      0
    end
    def zero?
      true
    end
    # String methods
    def ascii_only?
      true
    end
    def bytesize
      0
    end
    def capitalize
      ''
    end
    def capitalize!
      nil
    end
    def casecmp(*a)
      -1
    end
    def center(*a)
      ''
    end
    def chomp(*a)
      ''
    end
    def chop
      ''
    end
    def chr
      ''
    end
    def clear
      ''
    end
    def concat(*a)
      ''
    end
    def count(*a)
      0
    end
    def delete(*a)
      ''
    end
    def downcase
      ''
    end
    def downcase!
      ''
    end
    def dump
      ''
    end
    def each(*a)
      self
    end
    def each_with_index(*a)
      self
    end
    def each_byte(*a)
      self
    end
    def each_char(*a)
      self
    end
    def each_line(*a)
      self
    end
    def empty?
      true
    end
    def encode(*a)
      ''
    end
    def end_with?(*a)
      false
    end
    def force_encoding(*a)
      ''
    end
    def getbyte(*a)
      0
    end
    def gsub(*a)
      ''
    end
    def gsub!(*a)
      ''
    end
    def hex
      0
    end
    def include?(*a)
      false
    end
    def index(*a)
      nil
    end
    def insert(*a)
      ''
    end
    def length
      0
    end
    def lines(*a)
      ''
    end
    def ljust(*a)
      ''
    end
    def lstrip
      ''
    end
    def match(*a)
      nil
    end
    def oct
      0
    end
    def partition(*a)
      []
    end
    def replace(*a)
      ''
    end
    def reverse
      ''
    end
    def rindex(*a)
      nil
    end
    def rjust(*a)
      ''
    end
    def rpartition(*a)
      []
    end
    def rstrip
      ''
    end
    def scan(*a)
      []
    end
    def size
      0
    end
    def slice(*a)
      ''
    end
    def split(*a)
      []
    end
    def squeeze(*a)
      ''
    end
    def start_with?(*a)
      false
    end
    def strip
      ''
    end
    def sub(*a)
      ''
    end
    def sum(*a)
      0
    end
    def swapcase
      ''
    end
    def tr(*a)
      ''
    end
    def unpack(*a)
      []
    end
    def upcase
      ''
    end
    def valid_encoding?
      true
    end
    def to_str
      ''
    end
    def to_ary
      []
    end
    # Pokemon Essentials globals chamados sem guard
    def defaultBGM
      nil
    end
    def defaultBGS
      nil
    end
    def title_bgm
      nil
    end
    def name
      ''
    end
    # FIX: Scene_Map update loop -- $PokemonGlobal nil
    def batterywarning; false; end
    def cueFrames; -1; end
    def cueFrames=(v); v; end
    def cueBGM; nil; end
    def bugContestState; nil; end
    # FIX: $PokemonSystem / $PokemonBag nil
    def keyItemCalling; false; end
    def keyItemCalling=(v); v; end
    def registeredItems; []; end
    # FIX: $game_temp nil antes de DebugIntro criar stub
    def common_event_id=(v); v; end
    # FIX: $Trainer nil -- pokemon_party deve devolver [] e nao crashar
    def pokemon_party; []; end
    # FIX: $PokemonBag nil
    def pockets; []; end
    def last_pocket; 1; end
    def last_item; nil; end
  end
rescue
end

# -- Game_Map#bridge patch -----------------------------------------------------
# NOTA: O patch de Game_Map#bridge NAO pode ser feito aqui porque compat_stubs.h
# carrega ANTES dos scripts do jogo -- a classe Game_Map real ainda nao existe.
# O patch real esta em check_entry_methods() em binding_3ds.cpp, que corre
# depois de todos os 376 scripts terem carregado.

# -- Sprite_DynamicShadows stub ------------------------------------------------
# Script blankeado (demasiado complexo para mruby 3DS). Qualquer referencia
# residual a esta classe nos scripts do jogo (ex: shadow_initialize) nao deve
# crashar -- devolvemos um sprite inerte.
begin
  unless Object.const_defined?(:Sprite_DynamicShadows)
    class Sprite_DynamicShadows < Sprite
      def initialize(*a); super(a[0]); end
      def update(*a); end
      def dispose(*a); super; end
    end
  end
rescue; end

# -- Game_System: metodos em falta --------------------------------------------
begin
  if Object.const_defined?(:Game_System)
    class Game_System
      def timer_working; @timer_working ||= false; end unless method_defined?(:timer_working)
      def timer_working=(v); @timer_working = v; end unless method_defined?(:timer_working=)
      def getPlayingBGM; @playing_bgm; end unless method_defined?(:getPlayingBGM)
      def playing_bgm; @playing_bgm; end unless method_defined?(:playing_bgm)
      def playing_bgs; @playing_bgs; end unless method_defined?(:playing_bgs)
    end
  end
rescue; end

# -- Game_Temp: metodos em falta -----------------------------------------------
begin
  if Object.const_defined?(:Game_Temp)
    class Game_Temp
      def to_title; @to_title ||= false; end unless method_defined?(:to_title)
      def to_title=(v); @to_title = v; end unless method_defined?(:to_title=)
      def transition_processing; @transition_processing ||= false; end unless method_defined?(:transition_processing)
      def transition_processing=(v); @transition_processing = v; end unless method_defined?(:transition_processing=)
      def menu_calling; @menu_calling ||= false; end unless method_defined?(:menu_calling)
      def menu_calling=(v); @menu_calling = v; end unless method_defined?(:menu_calling=)
      def debug_calling; @debug_calling ||= false; end unless method_defined?(:debug_calling)
      def debug_calling=(v); @debug_calling = v; end unless method_defined?(:debug_calling=)
      def player_transferring; @player_transferring ||= false; end unless method_defined?(:player_transferring)
      def player_transferring=(v); @player_transferring = v; end unless method_defined?(:player_transferring=)
      def common_event_id; @common_event_id ||= 0; end unless method_defined?(:common_event_id)
      def common_event_id=(v); @common_event_id = v; end unless method_defined?(:common_event_id=)
      def in_menu; @in_menu ||= false; end unless method_defined?(:in_menu)
      def in_menu=(v); @in_menu = v; end unless method_defined?(:in_menu=)
    end
  end
rescue; end

# -- Game_Player: metodos em falta ---------------------------------------------
begin
  if Object.const_defined?(:Game_Player)
    class Game_Player
      def tile_id; @tile_id ||= 0; end unless method_defined?(:tile_id)
      def sprite_size=(v); @sprite_size = v; end unless method_defined?(:sprite_size=)
      def sprite_size; @sprite_size ||= [8, 8]; end unless method_defined?(:sprite_size)
      def screen_x; @x ? (@x * 32) : 0; end unless method_defined?(:screen_x)
      def screen_y; @y ? (@y * 32) : 0; end unless method_defined?(:screen_y)
      def screen_z(h=0); 0; end unless method_defined?(:screen_z)
      def animation_id; @animation_id ||= 0; end unless method_defined?(:animation_id)
      def animation_id=(v); @animation_id = v; end unless method_defined?(:animation_id=)
      def moving?; @moving ||= false; end unless method_defined?(:moving?)
      def straighten; end unless method_defined?(:straighten)
    end
  end
rescue; end

# -- Integer extras ------------------------------------------------------------
begin
  class Integer
    def name; self.to_s; end unless method_defined?(:name)
    def each(*_a); yield self if block_given?; self; end unless method_defined?(:each)
    def each_with_index(*_a)
      yield self, 0 if block_given?
      self
    end unless method_defined?(:each_with_index)
    def expired?; false; end unless method_defined?(:expired?)
    # FIX: Integer#length -- chamado quando $data_common_events[id] e Integer
    # (antes dos dados reais carregarem). Evita [MFD] Integer#length [x47].
    def length; 0; end unless method_defined?(:length)
    # FIX: Integer#keys -- chamado quando $data_map_infos e nil/Integer
    def keys; []; end unless method_defined?(:keys)
    # FIX: Integer#pages -- chamado em Game_Event quando events[id] e Integer
    def pages; []; end unless method_defined?(:pages)
    # FIX: Integer#reverse -- chamado em iteracao sobre events
    def reverse; []; end unless method_defined?(:reverse)
    # FIX: Integer#condition -- chamado em Game_Event#setup por pages[].condition
    def condition; nil; end unless method_defined?(:condition)
    # FIX pbCanUseHiddenMove? itera pokemon_party -- se items forem Integer IDs
    def egg?; false; end unless method_defined?(:egg?)
    def compatible_with_move?(_move); false; end unless method_defined?(:compatible_with_move?)
    # FIX: Integer#inProgress? / pbEnd -- chamado em safariState/bugContestState
    def inProgress?; false; end unless method_defined?(:inProgress?)
    def pbEnd; end unless method_defined?(:pbEnd)
  end
rescue; end

# -- validate stub (substitui Validation script que e SKIP-PATCH) -------------
# Kernel#validate recebe {value => Class/Array/Symbol} e valida tipos.
# Em mruby o dispatch de private conflitua com binding C -- neutralizar.
begin
  module Kernel
    def validate(pairs=nil); end  # no-op: aceita qualquer argumento
    module_function :validate
  end
  def validate(pairs=nil); end
rescue; end

# -- GameData stub minimo para boot --------------------------------------------
# GameData::Item.get(id) e chamado em pbUseKeyItem antes de qualquer dado estar carregado.
# DATA e nil/vazio no boot -- get/exists? crasham com "Unknown ID".
# Stub retorna um objeto com os metodos minimos usados pelo loop de Scene_Map.
begin
  module GameData
    # NullEntry -- objeto retornado por .get quando DATA esta vazio
    class NullEntry
      def id;        :NONE; end
      def id_number; 0;     end
      def name;      "";    end
      def real_name; "";    end
      def is_a?(klass); klass == NullEntry || super; end
      def method_missing(m, *a); nil; end
      def respond_to_missing?(*a); true; end
    end

    # Patch ClassMethods para tornar get/exists? seguros com DATA vazio
    module ClassMethods
      def get(other)
        return NullEntry.new if other.nil?
        return NullEntry.new unless Object.const_defined?(:GameData)
        begin
          return other if other.is_a?(self)
          key = other.is_a?(String) ? other.to_sym : other
          return (self::DATA && self::DATA[key]) ? self::DATA[key] : NullEntry.new
        rescue
          NullEntry.new
        end
      end
      def exists?(other)
        return false if other.nil?
        begin
          key = other.is_a?(String) ? other.to_sym : other
          return self::DATA && self::DATA.has_key?(key)
        rescue
          false
        end
      end
      def try_get(other)
        return nil if other.nil?
        begin
          key = other.is_a?(String) ? other.to_sym : other
          return (self::DATA && self::DATA[key]) ? self::DATA[key] : nil
        rescue
          nil
        end
      end
    end
    module ClassMethodsSymbols
      def get(other)
        return NullEntry.new if other.nil?
        begin
          key = other.is_a?(String) ? other.to_sym : other
          return (self::DATA && self::DATA[key]) ? self::DATA[key] : NullEntry.new
        rescue; NullEntry.new; end
      end
      def exists?(other)
        return false if other.nil?
        begin
          key = other.is_a?(String) ? other.to_sym : other
          self::DATA && self::DATA.has_key?(key)
        rescue; false; end
      end
    end
  end
rescue; end

# -- String#starts_with_vowel? (usado em pbUseKeyItem) ------------------------
begin
  class String
    def starts_with_vowel?
      return false if empty?
      "AEIOUaeiou".include?(self[0].to_s)
    end unless method_defined?(:starts_with_vowel?)
  end
rescue; end

# -- Pokemon stubs para pbUseKeyItem (egg?, compatible_with_move?) -------------
# $Trainer.pokemon_party retorna [] (NilClass stub), mas se $Trainer real existir
# as instancias de Pokemon precisam destes metodos.
begin
  if Object.const_defined?(:Pokemon)
    class Pokemon
      def egg?; @egg ||= false; end unless method_defined?(:egg?)
      def compatible_with_move?(move); false; end unless method_defined?(:compatible_with_move?)
    end
  end
rescue; end

# -- $PokemonBag stub ----------------------------------------------------------
# $PokemonBag.registeredItems e chamado em pbUseKeyItem (Scene_Map update).
# Sem save file $PokemonBag e nil -- o NilClass stub ja cobre registeredItems,
# mas se a classe real PokemonBag existir sem esse metodo, precisa de patch.
begin
  if Object.const_defined?(:PokemonBag)
    class PokemonBag
      def registeredItems; @registeredItems ||= []; end unless method_defined?(:registeredItems)
      def registeredItems=(v); @registeredItems = v; end unless method_defined?(:registeredItems=)
      def last_pocket; @last_pocket ||= 1; end unless method_defined?(:last_pocket)
      def last_item; @last_item; end unless method_defined?(:last_item)
    end
  end
rescue; end

# -- Proc#arity stub -----------------------------------------------------------
# mruby 3.2 sem o gem mruby-method pode nao ter Proc#arity, ou devolver valores
# errados. PE usa arity() em varias callbacks de eventos para decidir quantos
# argumentos passar (ex: trigger handlers, event lists).
# Crash silencioso aqui bloqueia o loop de eventos no Scene_Map update.
# Retornar -1 (variadic / aceita qualquer numero de args) e o comportamento
# mais conservador e compativel com MRI Ruby.
begin
  class Proc
    def arity
      -1
    end unless method_defined?(:arity)
  end
rescue; end

# -- Method#arity stub ---------------------------------------------------------
# Object#method devolve um Proc-like -- se o codigo chamar .arity nesse objecto,
# deve tambem devolver -1 para evitar crash.
begin
  class Method
    def arity
      -1
    end unless method_defined?(:arity)
  end
rescue; end

# -- PokemonReadyMenu stubs -----------------------------------------------------
# pbUseKeyItem tenta criar PokemonReadyMenu_Scene/PokemonReadyMenu no Scene_Map
# update. Estas classes existem nos scripts mas requerem Bitmap/Viewport reais.
# Stub minimo: new() retorna um objeto que aceita qualquer mensagem.
begin
  unless Object.const_defined?(:PokemonReadyMenu_Scene)
    class PokemonReadyMenu_Scene
      def initialize(*a); end
      def update; end
      def dispose; end
      def method_missing(m, *a); nil; end
      def respond_to_missing?(*a); true; end
    end
  end
rescue; end
begin
  unless Object.const_defined?(:PokemonReadyMenu)
    class PokemonReadyMenu
      def initialize(*a); end
      def update; end
      def dispose; end
      def method_missing(m, *a); nil; end
      def respond_to_missing?(*a); true; end
    end
  end
rescue; end

# -- pbCanUseHiddenMove? stub ---------------------------------------------------
# Chamado em pbUseKeyItem -- verifica HMs disponiveis. Sem GameData real,
# retornar false (nao crashar, apenas nao mostrar opcoes de HM).
begin
  unless Kernel.respond_to?(:pbCanUseHiddenMove?)
    module Kernel
      def pbCanUseHiddenMove?(move); false; end
      module_function :pbCanUseHiddenMove?
    end
  end
rescue; end

# -- Game_DependentEvents stub -------------------------------------------------
# Script blankeado. Referencias residuais nao devem crashar.
begin
  unless Object.const_defined?(:Game_DependentEvents)
    class Game_DependentEvents
      def initialize(*a); end
      def update(*a); end
      def method_missing(m, *a); nil; end
    end
  end
rescue; end

# -- pbRgssExists? / pbRgssOpen / pbLoadBattleAnimations -----------------------
# pbRgssExists? e chamado por pbLoadBattleAnimations (bt[0]) que e chamado em
# initialize() de algum manager durante o boot -- causa NoMethodError imediato.
# No 3DS todos os assets estao em sdmc:/mkxp/game/.
# safeExists? ainda nao existe aqui (e injectado pos-scripts pelo binding C),
# por isso usamos File.open rescue como mecanismo de verificacao seguro.
begin
  GAME_ROOT_3DS = "sdmc:/mkxp/game" unless Object.const_defined?(:GAME_ROOT_3DS)
rescue; end

begin
  unless respond_to?(:pbRgssExists?)
    def pbRgssExists?(filename)
      return false if filename.nil? || filename.to_s.empty?
      f = filename.to_s
      root = (Object.const_defined?(:GAME_ROOT_3DS) ? GAME_ROOT_3DS : "sdmc:/mkxp/game")
      [f, "#{root}/#{f}"].any? do |p|
        begin
          fh = File.open(p, "rb")
          fh.close
          true
        rescue
          false
        end
      end
    end
    module Kernel
      def pbRgssExists?(filename)
        return false if filename.nil? || filename.to_s.empty?
        f = filename.to_s
        root = (Object.const_defined?(:GAME_ROOT_3DS) ? GAME_ROOT_3DS : "sdmc:/mkxp/game")
        [f, "#{root}/#{f}"].any? do |p|
          begin
            fh = File.open(p, "rb")
            fh.close
            true
          rescue
            false
          end
        end
      end
      module_function :pbRgssExists?
    end
  end
rescue; end

begin
  unless respond_to?(:pbRgssOpen)
    def pbRgssOpen(filename, mode="rb")
      return nil if filename.nil? || filename.to_s.empty?
      f = filename.to_s
      root = (Object.const_defined?(:GAME_ROOT_3DS) ? GAME_ROOT_3DS : "sdmc:/mkxp/game")
      [f, "#{root}/#{f}"].each do |p|
        begin
          fh = File.open(p, mode)
          return fh
        rescue
          next
        end
      end
      nil
    end
    module Kernel
      def pbRgssOpen(filename, mode="rb")
        return nil if filename.nil? || filename.to_s.empty?
        f = filename.to_s
        root = (Object.const_defined?(:GAME_ROOT_3DS) ? GAME_ROOT_3DS : "sdmc:/mkxp/game")
        [f, "#{root}/#{f}"].each do |p|
          begin
            fh = File.open(p, mode)
            return fh
          rescue
            next
          end
        end
        nil
      end
      module_function :pbRgssOpen
    end
  end
rescue; end

begin
  unless respond_to?(:pbLoadBattleAnimations)
    def pbLoadBattleAnimations
      # Sem marshal real, retornar hash vazio -- nao crasha e nao bloqueia boot.
      {}
    end
    module Kernel
      def pbLoadBattleAnimations
        {}
      end
      module_function :pbLoadBattleAnimations
    end
  end
rescue; end

# -- pbGetBasicFont / pbGetOutlineFont / isOneOf? / _INTL (garantir aqui tambem) -
# Estes sao definidos na seccao Kernel helpers acima, mas se por alguma razao
# falhou (ex: Font nao existia ainda), redefinir aqui com rescue em cada um.
begin
  unless respond_to?(:pbRgssExists?)
    def pbRgssExists?(_f); false; end
  end
rescue; end

# -- AnimatedBitmap#initialize patch -------------------------------------------
# AnimatedBitmap faz file.split(/[\\/]/) para separar dir/filename.
# Com o nosso Regexp stub, split(Regexp) devolve [self] -- logo filename fica
# vazio e dir fica com o path completo, partindo o load.
# Patch: substituir a logica de split por String#split com "/" (string literal).
begin
  if Object.const_defined?(:AnimatedBitmap)
    class AnimatedBitmap
      unless method_defined?(:__orig_init_3ds)
        alias __orig_init_3ds initialize
        def initialize(file, hue = 0)
          raise "Filename is nil (missing graphic)." if file.nil?
          path     = file.to_s
          filename = ""
          # Usar split com string literal em vez de Regexp (nao suportado em mruby)
          if path[-1] != "/"
            parts    = path.split("/")
            filename = parts.pop || ""
            path     = parts.empty? ? "" : parts.join("/") + "/"
          end
          # filename[/regex/] com stub Regexp e sempre nil -- tratar como nao-animado
          # PngAnimatedBitmap so e necessario para ficheiros com [N] no nome
          if filename.include?("[") && filename.include?("]")
            begin
              @bitmap = PngAnimatedBitmap.new(path, filename, hue)
            rescue => e
              dbg "[AnimatedBitmap] PngAnimatedBitmap falhou: #{e.message}" rescue nil
              @bitmap = GifBitmap.new(path, filename, hue)
            end
          else
            # FIX: GifBitmap pode nao existir ou falhar -- usar Bitmap.new directo
            begin
              @bitmap = GifBitmap.new(path, filename, hue)
            rescue => e
              full_path = path.to_s + filename.to_s
              begin
                @bitmap = Bitmap.new(full_path)
              rescue
                @bitmap = Bitmap.new(1, 1)
              end
            end
          end
        end
      end
    end
  end
rescue; end

# -- RPG::Weather stub ---------------------------------------------------------
# Spriteset_Map cria RPG::Weather -- nao existe no compat por default.
begin
  unless (RPG.const_defined?(:Weather) rescue false)
    module RPG
      class Weather
        def initialize(viewport=nil); @type=:None; @max=0; @ox=0; @oy=0; end
        def fade_in(type, max, dur); @type=type; @max=max; end
        def update; end
        def dispose; end
        def ox=(v); @ox=v; end
        def oy=(v); @oy=v; end
        def type; @type; end
        def max; @max; end
      end
    end
  end
rescue; end

# -- AnimatedPlane stub --------------------------------------------------------
# Spriteset_Map usa AnimatedPlane para panorama e fog; addBackgroundPlane usa-o
# para fundos de cena. Herda o render do Plane (sprite interno): todos os
# caminhos que definem o bitmap (setBitmap/setPanorama/setFog/bitmap=) passam
# pelo setter bitmap= do pai (super) para o sprite ser atualizado e DESENHADO.
begin
  unless Object.const_defined?(:AnimatedPlane)
    class AnimatedPlane < Plane
      def initialize(viewport=nil); super; end
      def setBitmap(name,hue=0)
        if name.nil? || name.to_s.empty?
          self.bitmap = nil
          (dbg "[PLANE-DIAG] AnimatedPlane.setBitmap nome VAZIO -> bitmap=nil") rescue nil
        else
          begin
            bmp = AnimatedBitmap.new(name,hue).deanimate
            self.bitmap = bmp
            begin
              $__aplane_diag ||= 0
              if $__aplane_diag < 8
                $__aplane_diag += 1
                bsz = (bmp && !(bmp.disposed? rescue true)) ? "#{(bmp.width rescue '?')}x#{(bmp.height rescue '?')}" : "nil/disposed"
                (dbg "[PLANE-DIAG] AnimatedPlane.setBitmap '#{name}' -> #{bsz}") rescue nil
              end
            rescue; end
          rescue => _pe
            self.bitmap = nil
            (dbg "[PLANE-DIAG] AnimatedPlane.setBitmap '#{name}' EXCECAO #{_pe.class}: #{_pe.message}") rescue nil
          end
        end
      end
      def setPanorama(name,hue=0); setBitmap(name,hue); end
      def setFog(name,hue=0); setBitmap(name,hue); end
      def update; super; end
    end
  end
rescue; end

# -- Sprite_Picture stub -------------------------------------------------------
begin
  unless Object.const_defined?(:Sprite_Picture)
    class Sprite_Picture < Sprite
      def initialize(viewport=nil, picture=nil); super(viewport); end
      def update; end
    end
  end
rescue; end

# -- Sprite_Timer stub ---------------------------------------------------------
begin
  unless Object.const_defined?(:Sprite_Timer)
    class Sprite_Timer < Sprite
      def initialize(viewport=nil); super(viewport); end
      def update; end
    end
  end
rescue; end


# -- SM#update loop debug helper -----------------------------------------------
# Conta iterações do loop principal para detectar loop infinito.
# Quando SM#update correr em loop sem avançar, este contador permite
# saber exactamente onde o loop está preso.
$__sm_update_count  = 0
$__sm_scene_stalls  = 0
$__sm_last_scene    = nil

# Hook injectado no início de cada tick do loop principal (binding_3ds.cpp
# deve chamar esta função antes de $scene.update):
def __sm_tick_debug__
  $__sm_update_count += 1
  cur = $scene.class.name rescue "nil"
  if cur == $__sm_last_scene
    $__sm_scene_stalls += 1
    if ($__sm_scene_stalls % 500) == 0
      puts "[SM_DEBUG] tick=#{$__sm_update_count} stall=#{$__sm_scene_stalls} scene=#{cur} player_transferring=#{$game_temp.player_transferring rescue '?'} menu_calling=#{$game_temp.menu_calling rescue '?'} common_event_id=#{$game_temp.common_event_id rescue '?'}"
    end
  else
    $__sm_scene_stalls = 0
    $__sm_last_scene   = cur
    puts "[SM_DEBUG] SCENE CHANGED -> #{cur} (tick=#{$__sm_update_count})"
  end
end

# ═══════════════════════════════════════════════════════════════════════════════
# FIX RAIZ CEDO: defined? de METODO + method_missing (corrige Waterfall/plugins)
# -------------------------------------------------------------------------------
# CAUSA (PROVADO em mruby 3.2 host): `defined?(X)` e' compilado como CHAMADA de
# metodo com X avaliado ANTES. Para `defined?(Constante)` o [DEFINED-FIX] do bloco
# MFD resolve via const_missing->MKXP_UNDEF. MAS para `defined?(__metodo__)` (um
# METODO indefinido) NAO ha' method_missing global -> mruby LANCA NoMethodError na
# propria linha do defined?. Os plugins (Following Pokemon EX) usam exatamente:
#   alias __followingpkmn__X Y unless defined?(__followingpkmn__X)
# => a linha REBENTA, o alias nao nasce, o ficheiro do plugin ABORTA, e o golpe de
# campo (Waterfall: pbHiddenMoveAnimation/pbEndSurf) chama um alias inexistente ->
# mensagem "used 0!" com nome de especie/golpe vazios. O [DEFINED-FIX] do bloco MFD
# corre a t=8.7s, DEPOIS dos plugins (t=6-7s) -> TARDE DEMAIS. Por isso instalamos
# AQUI (t=0.5s, ANTES dos scripts e plugins), no FIM do compat_stubs (depois das
# definicoes acima, para nao mudar como ELAS carregam).
#
# SEGURANCA: o method_missing global so' devolve MKXP_UNDEF para nomes __*
# (convencao de alias-backup). Metodos normais indefinidos AINDA lancam
# NoMethodError -> bugs reais e duck-typing NAO sao mascarados. Provado no host:
#   pbHiddenMoveAnimation('x') -> "FOLLOW_ANIM(BASE_ANIM)" (cadeia intacta)
#   pbEndSurf() -> "FOLLOW_SURF(BASE_SURF)"
#   metodo_normal_indefinido() -> ainda NoMethodError
#   defined?(ConstInexistente) -> nil ; defined?(String) -> "expression"
# Desligar (se regredir): por $MKXP_DEFINED_FIX_EARLY = false e recompilar.
# Log via MKXPDebug.log (registado em C antes desta carga -> funciona a t=0.5s).
# ═══════════════════════════════════════════════════════════════════════════════
$MKXP_DEFINED_FIX_EARLY = true if $MKXP_DEFINED_FIX_EARLY.nil?
if $MKXP_DEFINED_FIX_EARLY
  begin
    unless Object.const_defined?(:MKXP_UNDEF)
      __mkxp_u = Object.new
      class << __mkxp_u
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
      Object.const_set(:MKXP_UNDEF, __mkxp_u)
    end

    # Kernel#defined?(v): recebe o valor JA' avaliado. MKXP_UNDEF -> nil (nao
    # definido); senao -> "expression" (definido). Da' a resposta certa a
    # defined?(Constante) E a defined?(__metodo__) (via method_missing abaixo).
    module ::Kernel
      def defined?(*args)
        return nil if args.empty?
        args[0].equal?(MKXP_UNDEF) ? nil : "expression"
      end
    end

    # method_missing GLOBAL apenas para nomes __* (alias-backups). Resto -> super
    # (NoMethodError normal, nao mascara bugs). const_missing -> MKXP_UNDEF (como
    # o [DEFINED-FIX] do MFD, mas a comecar a t=0.5s em vez de t=8.7s).
    class ::Object
      def method_missing(m, *a, &b)
        nm = m.to_s
        return MKXP_UNDEF if nm.start_with?("__")
        super
      end
      def self.const_missing(name)
        MKXP_UNDEF
      end
    end
    (MKXPDebug.log("[DEFINED-FIX-EARLY] instalado t=0.5s (defined?/method_missing __*/const_missing) -- corrige Waterfall/plugins") rescue nil)
  rescue => __mkxp_dfe
    (MKXPDebug.log("[DEFINED-FIX-EARLY] FALHOU: #{__mkxp_dfe.class}: #{__mkxp_dfe.message}") rescue nil)
  end
end

# ─── INSTALL DEFERIDO (RUBYPROF + tint + J3) ──────────────────────────────────
# IMPORTANTE: o COMPAT_STUBS_RUBY corre a ~t=0.5s, ANTES dos scripts do jogo.
# Nessa altura NAO existem Spriteset_Map / Sprite_Character / pbDayNightTint, nem
# o 'dbg'. Por isso NAO instalamos aqui (falhava em silencio). Em vez disso,
# REGISTAMOS o install numa lambda global; o bloco MFD do binding_3ds.cpp (que
# corre a ~t=8.7s, DEPOIS dos scripts, com 'dbg' e classes ja' definidas) chama
# $__mkxp_perf_install.call. Ruby resolve 'dbg'/classes no momento da CHAMADA,
# nao da definicao -> tudo existe quando corre.
$__mkxp_perf_install = lambda do

# ─── PROFILER GRANULAR DO UPDATE (isolar o gargalo de ~157ms/frame) ───────────
# O log mostra update=157ms render=1.2ms (Ruby domina) mas nao QUAL parte. Em vez
# de adivinhar, MEDIMOS cada peca pesada do loop de mapa com __mkxp_ms (relogio
# ARM11 real). Acumula tempo por categoria e despeja a media a cada N frames.
# Tudo protegido: se algo falhar, o jogo continua. Custo: 2 leituras de relogio
# por seccao (~ns) -- desprezavel face aos 157ms que estamos a investigar.
# DETECAO de __mkxp_ms por CHAMADA DIRETA (nao por respond_to? nem defined?):
#  - 'defined?(m)' LANCA no mruby 3.2 se m nao existe -> rebentava o bloco.
#  - 'respond_to?(:m)' devolve FALSE para metodos PRIVADOS. __mkxp_ms e' definido
#    em Kernel via mrb_define_method => e' PRIVADO (chama-se 'bare', sem receiver,
#    mas respond_to? nao o ve). Foi ISTO que impediu o profiler de instalar.
#  - A chamada bare '__mkxp_ms' funciona p/ metodo privado de Kernel a partir de
#    qualquer contexto. Se nao existir -> NoMethodError, que apanhamos. Robusto.
__rp_has_ms = false
__rp_t = nil
begin
  __rp_t = __mkxp_ms
  __rp_has_ms = true
rescue => __rp_e
  __rp_has_ms = false
end
if __rp_has_ms
  (dbg sprintf("[RUBYPROF] __mkxp_ms PRESENTE (teste=%.1fms) -> a instalar profiler", __rp_t)) rescue (puts "[RUBYPROF] __mkxp_ms presente")
  $__prof = Hash.new(0.0)
  $__prof_n = 0
  $__prof_calls = Hash.new(0)

  # ─── SUB-PROFILER LEAVES: DESLIGADAS ────────────────────────────────────────
  # As folhas (sc_tint/sc_refl/sc_animupd/sc_bmpnew/sc_rpgsuper) JA' CUMPRIRAM:
  # provaram que o custo do sprite e' o sc_inline (atribuicoes self.x=/bitmap=...)
  # e que super/tint/reflexo/animbitmap sao desprezaveis (<0.5ms). Mante-las
  # custava ~8 wrappers Ruby + leituras __mkxp_ms por sprite/frame -- overhead que
  # eu proprio introduzia. Ficam DESLIGADAS por defeito (codigo preservado, so'
  # nao instala). O topo (sprite_char + spriteset_map) FICA, para monitorizacao.
  # Por em true se algum dia quiseres re-repartir o custo do sprite.
  $__mkxp_subprof_leaves = false

  def __prof_acc(key, t0)
    $__prof[key] += (__mkxp_ms - t0)
    $__prof_calls[key] += 1
  rescue; end

  def __prof_dump_maybe
    $__prof_n += 1
    return unless ($__prof_n % 120) == 0   # ~a cada 120 frames
    line = "[RUBYPROF] f=#{$__prof_n}"
    # TOTAL_medido = so' chaves de TOPO (sprite_char + spriteset_map). As chaves
    # sc_* sao SUB-partes de sprite_char (senao duplicavam o total).
    total = 0.0
    # TOTAL = partes DISJUNTAS de topo: spriteset_map (ja' inclui sprite_char) +
    # game_map + interpreter + game_screen. Excluimos sc_* (folhas) E sprite_char
    # (aninhado no spriteset_map) p/ nao duplicar. Compara com PROFILE|frame.
    $__prof.each { |k, v| total += v unless k.to_s.start_with?("sc_") || k == :sprite_char }
    # ordenar por tempo desc
    $__prof.sort_by { |k, v| -v }.each do |k, v|
      avg = v / 120.0
      calls = $__prof_calls[k] / 120.0
      line += sprintf(" | %s=%.1fms(x%.0f)", k, avg, calls)
    end
    # RESIDUAL: super (RPG::Sprite#update + ::Sprite#update C++) = sprite_char -
    # (sc_tint+sc_refl+sc_animupd). sc_bmpnew NAO se subtrai (acontece DENTRO do
    # bloco de troca, que ja' faz parte do super-path inline; conta-se a' parte).
    sc_total = ($__prof[:sc_tint] + $__prof[:sc_refl] + $__prof[:sc_animupd]) rescue 0.0
    # sc_inline = atribuicoes self.x=/bitmap=/src_rect= (= sprite_char - super - leaves)
    sc_inline = ($__prof[:sprite_char] - $__prof[:sc_rpgsuper] - sc_total) rescue 0.0
    line += sprintf(" | sc_inline~=%.1fms/frame", sc_inline / 120.0) if $__prof[:sprite_char] > 0
    line += sprintf(" | TOTAL_medido=%.1fms/frame", total / 120.0)
    # ─── DEBUG CERTEIRO: churn (objs/frame) + dispatches de setter/frame ────────
    # objs/frame MAX expoe o PICO de alocacao (o +125K do dialogo): se o MAX for
    # enorme num frame isolado e a media baixa, e' carga pontual (mapa); se a media
    # for alta, e' custo SUSTENTADO por-frame (render da mensagem). setters/frame
    # decide o batch-setter (fix D): ~90 = vale a pena; poucas = custo no corpo.
    if ($__objs_fn || 0) > 0
      line += sprintf(" | objs/frame avg=%.0f MAX=%.0f", ($__objs_fsum / $__objs_fn), ($__objs_fmax || 0))
      $__objs_fmax = 0.0; $__objs_fsum = 0.0; $__objs_fn = 0
    end
    if $__set_prev0
      line += sprintf(" | setters/frame=%.0f", (((__mkxp_setters rescue 0.0) - $__set_prev0) / 120.0))
    end
    $__set_prev0 = (__mkxp_setters rescue 0.0)
    (dbg line) rescue (puts line)
    $__prof = Hash.new(0.0)
    $__prof_calls = Hash.new(0)
  end

  # Instrumentar Sprite_Character#update (o suspeito #1 segundo XPAL/seniores)
  if Object.const_defined?(:Sprite_Character) &&
     Sprite_Character.method_defined?(:update) &&
     !Sprite_Character.method_defined?(:__update_prof_orig)
    class Sprite_Character
      alias __update_prof_orig update
      def update
        t0 = __mkxp_ms
        $__in_sc_update = true       # marca: o super que correr agora e' de char
        __update_prof_orig
        $__in_sc_update = false
        __prof_acc(:sprite_char, t0)
      end
    end
    (dbg "[RUBYPROF] Sprite_Character#update instrumentado") rescue nil
  end

  # ─── SUB-PROFILER: REPARTIR o custo de Sprite_Character#update ───────────────
  # O sprite_char custa ~3.7ms/sprite mas NAO sabemos QUAL parte. Em vez de
  # adivinhar (ja' descartamos getTone -- ja' tem cache interna), MEDIMOS as
  # pecas folha que o update chama, todas no MESMO $__prof (chaves sc_*):
  #   sc_tint    = pbDayNightTint(self)        (tom dia/noite por sprite)
  #   sc_refl    = Sprite_Reflection#update    (reflexo na agua -- so' jogador)
  #   sc_animupd = AnimatedBitmap#update       (@charbitmap.update -- frames anim)
  #   sc_bmpnew  = AnimatedBitmap.new          (RECARGA de bitmap; conta chamadas)
  # O RESIDUAL (super = RPG::Sprite#update + ::Sprite#update C++) = sprite_char -
  # (sc_tint+sc_refl+sc_animupd) e' calculado no dump. Wrapping por reabertura
  # EXPLICITA de classe (provado robusto em mruby dentro da lambda). Risco baixo:
  # sao metodos-folha + begin/rescue; nao reescrevemos o update quente.
  begin
    # pbDayNightTint (metodo de topo/Object) -- so' instrumenta, nao altera logica
    if $__mkxp_subprof_leaves && (Object.method_defined?(:pbDayNightTint) || Object.private_method_defined?(:pbDayNightTint) rescue false)
      unless (Object.method_defined?(:__pbdnt_prof_orig) || Object.private_method_defined?(:__pbdnt_prof_orig) rescue false)
        class Object
          alias __pbdnt_prof_orig pbDayNightTint
          def pbDayNightTint(o)
            __pbdnt_t0 = __mkxp_ms
            __pbdnt_prof_orig(o)
            __prof_acc(:sc_tint, __pbdnt_t0)
          end
        end
        (dbg "[SUBPROF] pbDayNightTint instrumentado (sc_tint)") rescue nil
      end
    end
  rescue => __sp_e
    (dbg "[SUBPROF] pbDayNightTint NAO instrumentado: #{__sp_e.class}") rescue nil
  end

  if $__mkxp_subprof_leaves && Object.const_defined?(:Sprite_Reflection) &&
     Sprite_Reflection.method_defined?(:update) &&
     !Sprite_Reflection.method_defined?(:__refl_prof_orig)
    class Sprite_Reflection
      alias __refl_prof_orig update
      def update(*a)
        __refl_t0 = __mkxp_ms
        __refl_prof_orig(*a)
        __prof_acc(:sc_refl, __refl_t0)
      end
    end
    (dbg "[SUBPROF] Sprite_Reflection#update instrumentado (sc_refl)") rescue nil
  end

  if $__mkxp_subprof_leaves && Object.const_defined?(:AnimatedBitmap) &&
     AnimatedBitmap.method_defined?(:update) &&
     !AnimatedBitmap.method_defined?(:__aupd_prof_orig)
    class AnimatedBitmap
      alias __aupd_prof_orig update
      def update(*a)
        __aupd_t0 = __mkxp_ms
        __aupd_r = __aupd_prof_orig(*a)
        __prof_acc(:sc_animupd, __aupd_t0)
        __aupd_r
      end
    end
    (dbg "[SUBPROF] AnimatedBitmap#update instrumentado (sc_animupd)") rescue nil
  end

  # AnimatedBitmap.new -- detecta RECARGAS de bitmap por frame (cada uma e' cara:
  # ler PNG do disco + decode). Se sc_bmpnew tiver muitas chamadas/frame, e' o
  # bloco de troca de bitmap (char_name/hue/tile_id a mudar) que esta' a custar.
  begin
    if $__mkxp_subprof_leaves && Object.const_defined?(:AnimatedBitmap) &&
       !(class << AnimatedBitmap; method_defined?(:__anew_prof_orig); end)
      class << AnimatedBitmap
        alias __anew_prof_orig new
        def new(*a, &b)
          __anew_t0 = __mkxp_ms
          __anew_r = __anew_prof_orig(*a, &b)
          (($__prof[:sc_bmpnew] += (__mkxp_ms - __anew_t0)); ($__prof_calls[:sc_bmpnew] += 1)) rescue nil
          __anew_r
        end
      end
      (dbg "[SUBPROF] AnimatedBitmap.new instrumentado (sc_bmpnew)") rescue nil
    end
  rescue => __sp_e2
    (dbg "[SUBPROF] AnimatedBitmap.new NAO instrumentado: #{__sp_e2.class}") rescue nil
  end

  # SPLIT FINAL do sc_super: o Sprite#update C++ e' no-op, logo o "super" e' o
  # RPG::Sprite#update (Ruby) + as atribuicoes inline. Aqui medimos o RPG::Sprite#
  # update ISOLADO (so' quando chamado de dentro de Sprite_Character#update, via
  # flag $__in_sc_update -> nao conta battlers/pictures). No dump:
  #   sc_rpgsuper = RPG::Sprite#update (durations/animations/SpriteAnimation.clear)
  #   sc_inline   = sprite_char - sc_rpgsuper - (tint+refl+animupd) = as ~15
  #                 atribuicoes self.x=/bitmap=/src_rect= (dispatch mruby no ARM11)
  # Assim sabemos se otimizar o super (dificil, e' base) ou as atribuicoes (possivel
  # via setter C++ em lote).
  begin
    if $__mkxp_subprof_leaves && Object.const_defined?(:RPG) && RPG.const_defined?(:Sprite) &&
       RPG::Sprite.method_defined?(:update) &&
       !RPG::Sprite.method_defined?(:__rpgupd_prof_orig)
      module RPG
        class Sprite
          alias __rpgupd_prof_orig update
          def update(*a)
            if $__in_sc_update
              __rpg_t0 = __mkxp_ms
              __rpgupd_prof_orig(*a)
              __prof_acc(:sc_rpgsuper, __rpg_t0)
            else
              __rpgupd_prof_orig(*a)
            end
          end
        end
      end
      (dbg "[SUBPROF] RPG::Sprite#update instrumentado (sc_rpgsuper)") rescue nil
    end
  rescue => __sp_e3
    (dbg "[SUBPROF] RPG::Sprite#update NAO instrumentado: #{__sp_e3.class}") rescue nil
  end

  # ─── ACELERADOR OPCIONAL (Essentials): screen_x/y/z NATIVOS ─────────────────
  # Modulo opt-in e ISOLADO (essentials_accel_3ds.cpp). Substitui os 3 getters de
  # posicao quentes do Game_Character por C++ (validado no host: IDENTICO ao Ruby
  # ao pixel, inclui floor-inteiro vs round-float). E' especifico do Essentials,
  # por isso fica atras da flag $__mkxp_native_spritechar. MUDA AQUI para ligar:
  #   false   -> Ruby original. Nucleo 100% intacto. (DEFEITO)
  #   :verify -> corre NATIVO e Ruby, compara, regista [ACCEL|DIFF] e conta
  #              $__accel_diffs; mas USA o resultado Ruby (ZERO risco). Serve para
  #              PROVAR no 3DS real que o nativo bate certo antes de confiar nele.
  #   true    -> usa o nativo. Casos raros/complexos (salto, tile-sprites,
  #              always_on_top) o C devolve nil e cai no Ruby automaticamente.
  # As funcoes C sao registadas por essentialsAccelInit (de spriteBindingInit). Se
  # o modulo nao estiver compilado, o guard falha e fica tudo em Ruby (nada parte).
  # ⇩⇩⇩ PARA LIGAR O ACELERADOR, MUDA SO' ESTA LINHA: false | :verify | true ⇩⇩⇩
  $__mkxp_native_spritechar = true
  begin
    if Object.const_defined?(:Game_Character) &&
       Game_Character.method_defined?(:screen_x) &&
       Game_Character.method_defined?(:__gc_native_screen_x) &&
       !Game_Character.method_defined?(:__scrx_accel_orig)
      class Game_Character
        alias __scrx_accel_orig screen_x
        alias __scry_accel_orig screen_y
        alias __scrz_accel_orig screen_z
        def screen_x
          m = $__mkxp_native_spritechar
          return __scrx_accel_orig unless m
          nat = __gc_native_screen_x(self)
          return __scrx_accel_orig if nat.nil?
          if m == :verify
            rb = __scrx_accel_orig
            if rb != nat
              $__accel_diffs = ($__accel_diffs || 0) + 1
              (dbg "[ACCEL|DIFF] screen_x rb=#{rb} nat=#{nat} cls=#{self.class} tid=#{@tile_id}") rescue nil
            end
            return rb
          end
          nat
        end
        def screen_y
          m = $__mkxp_native_spritechar
          return __scry_accel_orig unless m
          nat = __gc_native_screen_y(self)
          return __scry_accel_orig if nat.nil?
          if m == :verify
            rb = __scry_accel_orig
            if rb != nat
              $__accel_diffs = ($__accel_diffs || 0) + 1
              (dbg "[ACCEL|DIFF] screen_y rb=#{rb} nat=#{nat} cls=#{self.class}") rescue nil
            end
            return rb
          end
          nat
        end
        def screen_z(height = 0)
          m = $__mkxp_native_spritechar
          return __scrz_accel_orig(height) unless m
          nat = __gc_native_screen_z(self, height)
          return __scrz_accel_orig(height) if nat.nil?
          if m == :verify
            rb = __scrz_accel_orig(height)
            if rb != nat
              $__accel_diffs = ($__accel_diffs || 0) + 1
              (dbg "[ACCEL|DIFF] screen_z rb=#{rb} nat=#{nat} cls=#{self.class} tid=#{@tile_id}") rescue nil
            end
            return rb
          end
          nat
        end
      end
      (dbg "[ACCEL] screen_x/y/z NATIVO instalado (modo=#{$__mkxp_native_spritechar.inspect})") rescue nil
    else
      (dbg "[ACCEL] nativo NAO instalado (Game_Character ou __gc_native_screen_x em falta -- fica em Ruby)") rescue nil
    end
  rescue => __acc_e
    (dbg "[ACCEL] nativo NAO instalado: #{__acc_e.class}: #{__acc_e.message rescue ''}") rescue nil
  end

  # Instrumentar Spriteset_Map#update (o contentor: sprites + tilemap + panorama)
  if Object.const_defined?(:Spriteset_Map) &&
     Spriteset_Map.method_defined?(:update) &&
     !Spriteset_Map.method_defined?(:__update_prof_orig)
    class Spriteset_Map
      alias __update_prof_orig update
      def update
        # DEBUG CERTEIRO: objs alocados no FRAME INTEIRO anterior = delta de
        # __mkxp_objs entre dois updates de spriteset (corre 1x/frame). Acumula
        # avg e MAX p/ o dump distinguir pico pontual de custo sustentado.
        __o = (__mkxp_objs rescue nil)
        if __o && $__objs_lastf
          __fd = __o - $__objs_lastf
          $__objs_fmax = __fd if __fd > ($__objs_fmax || 0)
          $__objs_fsum = ($__objs_fsum || 0.0) + __fd
          $__objs_fn   = ($__objs_fn || 0) + 1
        end
        $__objs_lastf = __o if __o
        t0 = __mkxp_ms
        __update_prof_orig
        __prof_acc(:spriteset_map, t0)
        __prof_dump_maybe   # despeja periodicamente (uma vez por spriteset/frame)
      end
    end
    (dbg "[RUBYPROF] Spriteset_Map#update instrumentado") rescue nil
  end

  # DEBUG CERTEIRO: atribuir a carga do MAPA. Spriteset_Map.new cria todos os
  # sprites de tiles/eventos/panorama de uma vez -- prime suspeito do pico de
  # +125K objs quando entras num mapa. Loga quantos objs alocou (1x/carga).
  if Object.const_defined?(:Spriteset_Map) &&
     Spriteset_Map.method_defined?(:initialize) &&
     !Spriteset_Map.method_defined?(:__init_churn_orig)
    class Spriteset_Map
      alias __init_churn_orig initialize
      def initialize(*a, &b)
        __o0 = (__mkxp_objs rescue nil)
        __r = __init_churn_orig(*a, &b)
        (dbg sprintf("[CHURN] Spriteset_Map.new alocou %.0f objs (carga de mapa)", ((__mkxp_objs rescue __o0) - __o0))) rescue nil if __o0
        __r
      end
    end
    (dbg "[CHURN] Spriteset_Map#initialize instrumentado") rescue nil
  end

  # ─── INSTRUMENTAR OS ~232ms DE "OUTRA" LOGICA (o gargalo real da catedral) ────
  # O frame de 287ms tem so' ~55ms em sprites/tilemap; os outros ~232ms sao a
  # logica de jogo chamada por Scene_Map#update: o loop de eventos do mapa
  # (Game_Map#update), o interpretador do cutscene (Interpreter#update) e os
  # efeitos de ecra'/pictures (Game_Screen#update). Medimo-los para saber QUAL
  # domina -- e' isto que decide se o ganho vem de saltar updates de eventos
  # dormentes, otimizar o interpretador, ou as pictures. Top-level (nao aninhados
  # no spriteset), por isso somam-se ao total.
  if Object.const_defined?(:Game_Map) && Game_Map.method_defined?(:update) &&
     !Game_Map.method_defined?(:__gm_prof_orig)
    class Game_Map
      alias __gm_prof_orig update
      def update(*a)
        __gm_t0 = __mkxp_ms
        __gm_r = __gm_prof_orig(*a)
        __prof_acc(:game_map, __gm_t0)
        __gm_r
      end
    end
    (dbg "[RUBYPROF] Game_Map#update instrumentado") rescue nil
  end

  if Object.const_defined?(:Interpreter) && Interpreter.method_defined?(:update) &&
     !Interpreter.method_defined?(:__interp_prof_orig)
    class Interpreter
      alias __interp_prof_orig update
      def update(*a)
        __ip_t0 = __mkxp_ms
        __ip_r = __interp_prof_orig(*a)
        __prof_acc(:interpreter, __ip_t0)
        __ip_r
      end
    end
    (dbg "[RUBYPROF] Interpreter#update instrumentado") rescue nil
  end

  if Object.const_defined?(:Game_Screen) && Game_Screen.method_defined?(:update) &&
     !Game_Screen.method_defined?(:__gs_prof_orig)
    class Game_Screen
      alias __gs_prof_orig update
      def update(*a)
        __gs_t0 = __mkxp_ms
        __gs_r = __gs_prof_orig(*a)
        __prof_acc(:game_screen, __gs_t0)
        __gs_r
      end
    end
    (dbg "[RUBYPROF] Game_Screen#update instrumentado") rescue nil
  end
else
  (dbg "[RUBYPROF] SKIP: __mkxp_ms AUSENTE -> binding_3ds.cpp NAO recompilado com o metodo nativo. Forcar rebuild: 'touch source/binding_3ds.cpp && make' (ou apagar binding_3ds.o). Sem isto o profiler nao mede nada.") rescue (puts "[RUBYPROF] SKIP __mkxp_ms ausente")
end

# ─── OTIMIZACAO 1: cache de pbDayNightTint por-FRAME ──────────────────────────
# pbDayNightTint(object) e' chamado por CADA sprite de personagem, CADA frame
# (54 eventos => 54x/frame). Faz lookups que sao IDENTICOS para todos os sprites
# no mesmo frame: GameData::MapMetadata.exists?(map_id), .get(map_id).outdoor_map,
# e PBDayNight.getTone. Em vez de repetir 54x, calculamos o TOM uma vez por frame
# (chaveado por Graphics.frame_count) e so' aplicamos object.tone.set(...) a cada
# sprite (que e' barato e necessario). SEGURO: o resultado e' identico -- todos os
# sprites do mesmo mapa/frame recebem o mesmo tom; so' eliminamos lookups
# redundantes. Reversivel: e' um alias do metodo original, que fica como fallback.
# GENERICO: beneficia qualquer mapa com varios eventos.
# NOTA mruby: respond_to? em vez de defined? (ver acima -- defined? lanca erro).
if respond_to?(:pbDayNightTint) && !respond_to?(:__pbDayNightTint_cache_orig)
  $__dnt_frame = -1            # frame em que o tom foi calculado
  $__dnt_apply = false         # true = aplicar tom; false = limpar (set 0,0,0,0)
  $__dnt_tone  = nil           # [r,g,b,gray] a aplicar quando $__dnt_apply

  alias __pbDayNightTint_cache_orig pbDayNightTint
  def pbDayNightTint(object)
    return if !$scene.is_a?(Scene_Map)
    fc = (Graphics.frame_count rescue 0)
    if fc != $__dnt_frame
      # recalcular UMA vez por frame
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
        # em caso de erro, cair no comportamento original (sem cache)
        return __pbDayNightTint_cache_orig(object)
      end
    end
    # aplicar o resultado cacheado (barato: so' um set por sprite)
    if $__dnt_apply && $__dnt_tone
      object.tone.set($__dnt_tone[0], $__dnt_tone[1], $__dnt_tone[2], $__dnt_tone[3])
    else
      object.tone.set(0, 0, 0, 0)
    end
  rescue
    (__pbDayNightTint_cache_orig(object) rescue nil)
  end
  (dbg "[OPT] pbDayNightTint com cache por-frame instalado") rescue nil
end

# ─── [J3] THROTTLE do SPRITESET VIZINHO ───────────────────────────────────────
# Mapas ligados (PokemonMapFactory) fazem o Scene_Map manter UM Spriteset_Map por
# mapa. Em Gardenia o log mostra @spritesets=Hash(2): DOIS mapas a fazer update
# completo por frame. O grosso do custo e' o loop:
#     for sprite in @character_sprites; sprite.update; end
# So' o mapa ATIVO (@map == $game_map) precisa dos NPCs a 30fps; o(s) vizinho(s)
# (so' visiveis na costura) podem animar a 1 em cada N frames.
#
# COMO (seguro, SEM tearing): num frame a saltar corremos o update ORIGINAL na
# mesma, mas com @character_sprites temporariamente VAZIO. Assim o scroll do
# tilemap, panorama, fog e weather atualizam CERTO (a juncao nao rasga); so' o
# loop dos NPCs do vizinho e' que e' saltado. Em mensagem ativa, o vizinho fica
# parado de todo. Reversivel; controlado por $MKXP_PERF_J3 / $MKXP_PERF_J3_EVERY.
$MKXP_PERF_J3       = true if $MKXP_PERF_J3.nil?
$MKXP_PERF_J3_EVERY = 3    if $MKXP_PERF_J3_EVERY.nil?
$__j3_skipped = 0
$__j3_ran     = 0
if (Object.const_defined?(:Spriteset_Map) rescue false) &&
   (Spriteset_Map.method_defined?(:update) rescue false) &&
   !(Spriteset_Map.method_defined?(:__j3_update_orig) rescue false)
  class Spriteset_Map
    J3_EMPTY = [].freeze
    alias __j3_update_orig update
    def update(*a)
      throttle = false
      if $MKXP_PERF_J3
        begin
          neighbor = (@map && $game_map && @map != $game_map)
          if neighbor
            if $game_temp && $game_temp.message_window_showing
              throttle = true                     # vizinho parado durante dialogo
            else
              @__j3_n = (@__j3_n || 0) + 1
              throttle = ((@__j3_n % $MKXP_PERF_J3_EVERY) != 0)
            end
          end
        rescue
          throttle = false
        end
      end
      if throttle
        saved = @character_sprites
        begin
          @character_sprites = J3_EMPTY          # loop de NPCs do vizinho nao corre
          __j3_update_orig(*a)
        ensure
          @character_sprites = saved             # restaura SEMPRE (mesmo com erro)
        end
        $__j3_skipped += 1
      else
        __j3_update_orig(*a)
        $__j3_ran += 1
      end
      if (($__j3_skipped + $__j3_ran) % 600) == 0
        (dbg "[J3] vizinho: #{$__j3_skipped} updates de NPC saltados / #{$__j3_ran} completos (every=#{$MKXP_PERF_J3_EVERY})") rescue nil
      end
    end
  end
  (dbg "[J3] throttle de spriteset vizinho instalado (every=#{$MKXP_PERF_J3_EVERY}, flag $MKXP_PERF_J3)") rescue nil
else
  (dbg "[J3] NAO instalado (Spriteset_Map ausente ou ja' aliasado)") rescue nil
end

end  # fim da lambda $__mkxp_perf_install
(dbg "[PERF] install deferido REGISTADO (sera' chamado pelo bloco MFD apos os scripts)" rescue nil)
)RUBY";
