#pragma once
static const char *COMPAT_STUBS_RUBY = R"RUBY(
$VERBOSE = nil
$_INTL_defined = true

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
    def scan(pat)
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
        # Regexp ou outro tipo nao suportado -- devolver [] sem crash
        []
      end
    rescue
      []
    end
  end
rescue; end

# String#split -- o C binding do mruby so aceita String como separador.
# Se o separador for um Regexp stub (ou qualquer nao-String), o C explode
# com TypeError: expected String.  Override completo em Ruby evita isso.
class String
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
      # Separador nao-String (Regexp stub, Symbol, etc.)
      # Nao podemos passar ao C binding -- devolver [self] e conservador mas seguro.
      [self.dup]
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
    def [](idx, len=nil)
      # Regexp ou outro tipo nao suportado -> nil (evita TypeError no C binding)
      unless idx.is_a?(Integer) || idx.is_a?(String) ||
             (idx.respond_to?(:exclude_end?))  # Range
        return nil
      end
      # Usar slice() -- alias C nativo de [] que nao e afectado por este override
      begin
        len.nil? ? slice(idx) : slice(idx, len)
      rescue TypeError
        nil
      rescue => e
        nil
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
    @@dc=nil; @@dsh=false; @@dol=false; @@doc=nil
    def self.default_name;          @@dn;    end
    def self.default_name=(v);      @@dn=v;  end
    def self.default_size;          @@ds;    end
    def self.default_size=(v);      @@ds=v;  end
    def self.default_bold;          @@db;    end
    def self.default_bold=(v);      @@db=v;  end
    def self.default_italic;        @@di;    end
    def self.default_italic=(v);    @@di=v;  end
    def self.default_color;         @@dc;    end
    def self.default_color=(v);     @@dc=v;  end
    def self.default_shadow;        @@dsh;   end
    def self.default_shadow=(v);    @@dsh=v; end
    def self.default_outline;       @@dol;   end
    def self.default_outline=(v);   @@dol=v; end
    def self.default_out_color;     @@doc;   end
    def self.default_out_color=(v); @@doc=v; end
    def self.exist?(n); false; end
    attr_accessor :name,:size,:bold,:italic,:color,:shadow,:outline,:out_color
    def initialize(name=@@dn,size=@@ds)
      @name=name; @size=size; @bold=@@db; @italic=@@di
      @color=@@dc; @shadow=@@dsh; @outline=@@dol; @out_color=@@doc
    end
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
    def mega?; (@height||0) > 1024; end
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
# FIX: Plane é uma classe Ruby pura (sem binding C++) -- definir normalmente.
begin
  class Plane
    attr_accessor :bitmap,:visible,:z,:ox,:oy,:zoom_x,:zoom_y,:opacity
    attr_accessor :blend_type,:color,:tone,:viewport
    def initialize(viewport=nil)
      @visible=true; @z=0; @ox=0; @oy=0; @zoom_x=1.0; @zoom_y=1.0
      @opacity=255; @blend_type=0; @color=Color.new(0,0,0,0)
      @tone=Tone.new; @viewport=viewport; @disposed=false
    end
    def disposed?; @disposed; end
    def dispose; @disposed=true; end
    def update; end
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
    attr_accessor :windowskin,:contents,:stretch,:cursor_rect,:active
    attr_accessor :visible,:pause,:x,:y,:z,:width,:height,:ox,:oy
    attr_accessor :opacity,:back_opacity,:contents_opacity,:viewport
    def initialize(viewport=nil)
      @visible=true; @active=true; @pause=false; @stretch=true
      @x=0; @y=0; @z=100; @width=0; @height=0; @ox=0; @oy=0
      @opacity=255; @back_opacity=200; @contents_opacity=255
      @cursor_rect=Rect.new; @viewport=viewport; @disposed=false
      begin; @contents=Bitmap.new(1,1); rescue; end
    end
    def disposed?; @disposed; end
    def dispose
      @disposed=true
      begin; @contents.dispose if @contents && !@contents.disposed?; rescue; end
    end
    def update; end
    def refresh; end
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
        begin; f=open(p.to_s,"rb"); f.close; true; rescue; false; end
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
          fh.close
          return true
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
    def self.audio_exist?(p); _try_open(p); end
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

# -- Regexp (mruby 3.2 nao tem Regexp built-in) -------------------------------
begin
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
    def inspect; "/#{@src}/"; end
    def self.compile(src, flags=0); new(src, flags); end
    def self.last_match; nil; end
    def self.escape(str); str.to_s; end
    def self.quote(str); str.to_s; end
    def self.union(*args); new(args.map(&:to_s).join("|")); end
    # FIX: Regexp#to_str -- chamado [x41] quando codigo faz string + regexp
    # ex: Sprite_Timer usa format() com regexp como argumento.
    # to_str permite conversao implicita para String.
    def to_str; @src.to_s; end
  end
rescue; end

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
  unless defined?(RPG::Weather)
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
# Spriteset_Map usa AnimatedPlane para panorama e fog.
begin
  unless Object.const_defined?(:AnimatedPlane)
    class AnimatedPlane < Plane
      def initialize(viewport=nil); super; @bmp=nil; end
      def setPanorama(name,hue=0)
        if name.nil?
          @bitmap=nil
        else
          begin; @bitmap=AnimatedBitmap.new(name,hue).deanimate; rescue; @bitmap=nil; end
        end
      end
      def setFog(name,hue=0); setPanorama(name,hue); end
      def update; end
      def bitmap; @bitmap; end
      def bitmap=(v); @bitmap=v; end
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
)RUBY";
