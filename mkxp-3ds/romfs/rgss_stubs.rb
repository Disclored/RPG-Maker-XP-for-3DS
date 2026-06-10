# rgss_stubs.rb -- stubs para mruby 3DS
# REGRA: Nunca definir initialize em classes que têm binding C (Bitmap, Sprite).
# O binding C (bmp_init / spr_init) é registado ANTES deste ficheiro carregar.
# Redefinir initialize aqui SOBREPÕE o C binding e causa
#   ArgumentError: wrong number of arguments (given 2, expected 1)
# quando o jogo faz BitmapWrapper.new(width, height).

class Viewport
  attr_accessor :visible, :z, :ox, :oy, :color, :tone, :rect
  def initialize(*a); @visible=true; @z=0; @ox=0; @oy=0; end
  def dispose; end
  def disposed?; false; end
  def flash(*a); end
  def update; end
end

class Color
  attr_accessor :red, :green, :blue, :alpha
  def initialize(r=255,g=255,b=255,a=255); @red=r;@green=g;@blue=b;@alpha=a; end
  def set(r,g,b,a=255); @red=r;@green=g;@blue=b;@alpha=a; end
  def to_s; "Color(#{@red},#{@green},#{@blue},#{@alpha})"; end
end

class Tone
  attr_accessor :red, :green, :blue, :gray
  def initialize(r=0,g=0,b=0,gr=0); @red=r;@green=g;@blue=b;@gray=gr; end
  def set(r,g,b,gr=0); @red=r;@green=g;@blue=b;@gray=gr; end
end

class Rect
  attr_accessor :x, :y, :width, :height
  def initialize(x=0,y=0,w=0,h=0); @x=x;@y=y;@width=w;@height=h; end
  def set(x,y,w,h); @x=x;@y=y;@width=w;@height=h; end
  def empty; @x=0;@y=0;@width=0;@height=0; end
end

# ── Bitmap ────────────────────────────────────────────────────────────────────
# NÃO definir initialize aqui! O binding C (bmp_init) já trata:
#   Bitmap.new(width, height)  -- cria bitmap em branco
#   Bitmap.new("path/file")    -- carrega imagem
# Redefinir initialize em Ruby sobrepõe o binding C e parte BitmapWrapper.new(w,h).
class Bitmap
  # Adicionar apenas métodos que o binding C não cobre.
  def clone
    b = Bitmap.new(width, height) rescue Bitmap.new(1, 1)
    b.blt(0, 0, self, rect) rescue nil
    b
  end
  def dup; clone; end
  # font ivar (o binding C já tem getter/setter, mas por segurança)
  unless method_defined?(:font)
    attr_accessor :font
  end
end

class Font
  attr_accessor :name, :size, :bold, :italic, :color, :shadow, :outline, :out_color
  @@default_name='Arial'; @@default_size=22; @@default_bold=false
  @@default_italic=false; @@default_color=Color.new(255,255,255)
  def self.default_name; @@default_name; end
  def self.default_name=(v); @@default_name=v; end
  def self.default_size; @@default_size; end
  def self.default_size=(v); @@default_size=v; end
  def self.default_bold; @@default_bold; end
  def self.default_bold=(v); @@default_bold=v; end
  def self.default_color; @@default_color; end
  def self.default_color=(v); @@default_color=v; end
  def self.exist?(n); true; end
  def initialize(name=@@default_name,size=@@default_size)
    @name=name; @size=size; @bold=false; @italic=false
    @color=Color.new(255,255,255); @shadow=false
    @outline=false; @out_color=Color.new
  end
end

# ── Sprite ────────────────────────────────────────────────────────────────────
# NÃO definir initialize aqui! O binding C (spr_init) regista o sprite na lista
# global g_sprites. Se o initialize Ruby sobrepuser o C, spr_init nunca é
# chamado → g_sprites fica vazio → 0 blits → ecrã preto.
# Adicionar apenas ivars e métodos auxiliares que o binding C não cobre.
class Sprite
  unless method_defined?(:flash)
    def flash(*a); end
  end
  unless method_defined?(:update)
    def update; end
  end
end

class Plane
  attr_accessor :bitmap,:visible,:z,:ox,:oy,:zoom_x,:zoom_y,:opacity,:blend_type,:color,:tone,:viewport
  def initialize(v=nil)
    @viewport=v;@visible=true;@z=0;@ox=0;@oy=0
    @zoom_x=1.0;@zoom_y=1.0;@opacity=255;@blend_type=0
    @color=Color.new(0,0,0,0);@tone=Tone.new
  end
  def dispose; end
  def disposed?; false; end
  def update; end
end

class Window
  attr_accessor :windowskin,:contents,:stretch,:cursor_rect,:viewport
  attr_accessor :active,:visible,:arrows_visible,:pause,:x,:y,:z
  attr_accessor :width,:height,:ox,:oy,:opacity,:back_opacity,:contents_opacity
  def initialize(x=0,y=0,w=0,h=0)
    x=x.to_i rescue 0; y=y.to_i rescue 0
    w=w.to_i rescue 0; h=h.to_i rescue 0
    @x=x;@y=y;@width=w;@height=h;@visible=true;@active=true
    @z=100;@ox=0;@oy=0;@opacity=255;@back_opacity=200
    @contents_opacity=255;@pause=false;@stretch=true
    @arrows_visible=true;@cursor_rect=Rect.new
    @contents=Bitmap.new([w-32,1].max,[h-32,1].max)
  end
  def dispose; end
  def disposed?; false; end
  def update; end
end

module Audio
  def self.bgm_play(*a); end
  def self.bgm_stop; end
  def self.bgm_fade(*a); end
  def self.bgs_play(*a); end
  def self.bgs_stop; end
  def self.bgs_fade(*a); end
  def self.me_play(*a); end
  def self.me_stop; end
  def self.me_fade(*a); end
  def self.se_play(*a); end
  def self.se_stop; end
  def self.bgm_pos; 0; end
end

module RPG
  class AudioFile
    attr_accessor :name, :volume, :pitch
    def initialize(n='',v=100,p=100); @name=n;@volume=v;@pitch=p; end
  end
end

=begin
module Graphics
  def self.update; end
  def self.freeze; end
  def self.transition(*a); end
  def self.frame_rate; 40; end
  def self.frame_count; 0; end
  def self.frame_count=(v); end
  def self.width; 400; end
  def self.height; 240; end
  def self.snap_to_bitmap; Bitmap.new(400,240); end
  def self.brightness; 255; end
  def self.brightness=(v); end
  def self.resize_screen(*a); end
  def self.play_movie(*a); end
end
=end

module Dir
  def self.glob(p,*a); []; end
  def self.[](p); []; end
  def self.exist?(p); false; end
  def self.exists?(p); false; end
  def self.mkdir(*a); end
  def self.pwd; "/"; end
  def self.chdir(d=nil)
    return yield if block_given?
    nil
  end
  def self.safe?(d); false; end
  def self.get(*a); []; end
  def self.all(*a); []; end
end

class File
  def self.open(path, mode="r", *a)
    return nil unless block_given?
    begin
      yield nil
    rescue
    end
    nil
  end
  def self.exist?(p); false; end
  def self.exists?(p); false; end
  def self.read(p); ""; end
  def self.write(*a); 0; end
  def self.join(*parts); parts.join("/"); end
  def self.basename(p, *a)
	  s = p.to_s.split("/").last || ""
	  a[0] ? s.sub(a[0].to_s, "") : s
	end
  def self.dirname(p)
    parts = p.to_s.split("/")
    parts.length > 1 ? parts[0..-2].join("/") : "."
  end
  def self.extname(p)
    b = self.basename(p.to_s)
    i = b.rindex(".")
    i ? b[i..-1] : ""
  end
  def self.expand_path(p, *a); p.to_s; end
  def self.delete(*a); end
  def self.rename(*a); end
  def self.size(p); 0; end
  def self.foreach(p, *a); end
  def self.readlines(p); []; end
  def self.safe?(f); false; end
end

module FileTest
  def self.exist?(p); false; end
  def self.exists?(p); false; end
  def self.directory?(p); false; end
  def self.file?(p); false; end
  def self.size(p); 0; end
end

# Só definir se o binding C (marshal.cpp) ainda não registou
unless respond_to?(:load_data)
  def load_data(path); []; end
end
unless respond_to?(:save_data)
  def save_data(obj, path); nil; end
end
def safeExists?(f); false; end
def safeIsDirectory?(f); false; end

module PluginManager
  def self.runPlugins; end
  def self.register(*a); end
  def self.load(*a); end
  def self.mustActivate?(*a); false; end
  def self.installed?(*a); false; end
  def self.versions; {}; end
  def self.require(*a); end
end

module Compiler
  def self.main; end
  def self.compile(*a); end
  def self.checkForChanges; false; end
end

module Game
  def self.initialize; end
  def self.set_up_system; end
end

def pbCallTitle
  printf("[stub] pbCallTitle\n")
  begin; Scene_Title.new; rescue; nil; end
end

# ── Thread stub ──────────────────────────────────────────────────────────────
class Thread
  def self.new(*a); end
  def self.current; nil; end
  def self.critical; false; end
  def self.critical=(v); v; end
  def self.pass; end
  def self.list; []; end
  def self.kill(*a); end
  def join; self; end
  def kill; self; end
  def alive?; false; end
  def status; nil; end
end

class Mutex
  def lock; self; end
  def unlock; self; end
  def locked?; false; end
  def try_lock; true; end
  def synchronize; yield; end
end

def pbCriticalCode
  yield
rescue Exception => e
  printf("[pbCriticalCode] %s\n", e.message)
end

def pbPrintException(e)
  printf("[EXCEPTION] %s\n", e.message) rescue nil
end

def pbEmergencySave; end

module Marshal
  def self.dump(obj, *a); ""; end
  def self.load(str); nil; end
end

class Object
  def method_nil(*a); nil; end
end

# ── NilClass stubs ───────────────────────────────────────────────────────────
class NilClass
  def length; 0; end
  def size; 0; end
  def count(*a); 0; end
  def empty?; true; end
  def events; {}; end          # FIX: Game_Map#events chamado em nil → crash
  def [](i); nil; end
  def []=(i,v); nil; end
  def each; self; end
  def each_with_index; self; end
  def map; []; end
  def select; []; end
  def reject; []; end
  def find; nil; end
  def any?(*a); false; end
  def all?(*a); true; end
  def none?(*a); true; end
  def include?(v); false; end
  def member?(v); false; end
  def first(*a); nil; end
  def last(*a); nil; end
  def flatten(*a); []; end
  def compact; []; end
  def uniq; []; end
  def sort; []; end
  def sort_by; []; end
  def reverse; []; end
  def min; nil; end
  def max; nil; end
  def sum(*a); 0; end
  def reduce(*a); a[0]; end
  def inject(*a); a[0]; end
  def zip(*a); []; end
  def push(*a); self; end
  def pop; nil; end
  def shift; nil; end
  def concat(*a); self; end
  def keys; []; end
  def values; []; end
  def has_key?(k); false; end
  def key?(k); false; end
  def fetch(k,*d); d[0]; end
  def merge(*a); {}; end
  def to_a; []; end
  def to_h; {}; end
  def to_i; 0; end
  def to_f; 0.0; end
  def split(*a); []; end
  def strip; ""; end
  def chomp(*a); ""; end
  def upcase; ""; end
  def downcase; ""; end
  def gsub(*a); ""; end
  def sub(*a); ""; end
  def start_with?(*a); false; end
  def end_with?(*a); false; end
  def match(*a); nil; end
  def scan(*a); []; end
  def chars; []; end
  def lines; []; end
  def encode(*a); ""; end
  def center(*a); ""; end
  def ljust(*a); ""; end
  def rjust(*a); ""; end
  def zero?; true; end
  def abs; 0; end
  def ceil(*a); 0; end
  def floor(*a); 0; end
  def round(*a); 0; end
  def clamp(*a); 0; end
  def method_nil(*a); nil; end
  def disposed?; true; end
  def dispose; nil; end
  def update; nil; end
  def visible; false; end
  def visible=(v); nil; end
end

# ── Integer stubs extra ───────────────────────────────────────────────────────
# FIX: Integer#size, #arity, #owner -- usados em diagnóstico de binding
class Integer
  def size; 4; end       # sizeof(int) em ARM
  def arity; -1; end     # como C native method
  def owner; 0; end
end

# ── PokemonSystem stub ────────────────────────────────────────────────────────
class PokemonSystem
  attr_accessor :textspeed, :battlescene, :battlestyle, :frame, :textskin
  attr_accessor :screensize, :language, :runstyle, :bgmvolume, :bgsvolume
  attr_accessor :sevolume, :textinput, :measurements, :lowhp, :sendtoboxes
  attr_accessor :nameprompt, :battletext, :autosave, :effectiveness
  attr_accessor :bikemusic, :surfmusic, :hmprompt, :daytone, :performance
  attr_accessor :autoheal, :controlinput, :weather, :shadows, :grassanim
  def initialize
    @textspeed    = 2
    @battlescene  = 0
    @battlestyle  = 0
    @frame        = 0
    @textskin     = 0
    @screensize   = 1
    @language     = 0
    @runstyle     = 1
    @bgmvolume    = 100
    @bgsvolume    = 100
    @sevolume     = 100
    @textinput    = 1
    @measurements = 0
    @lowhp        = 1
    @sendtoboxes  = 1
    @nameprompt   = 0
    @battletext   = 1
    @autosave     = 0
    @effectiveness= 0
    @bikemusic    = 0
    @surfmusic    = 0
    @hmprompt     = 1
    @daytone      = 0
    @performance  = 0
    @autoheal     = 0
    @controlinput = 0
    @weather      = 0
    @shadows      = 0
    @grassanim    = 0
  end
  def method_missing(m, *a); nil; end
end

# ── Game_Temp stub ────────────────────────────────────────────────────────────
class Game_Temp
  attr_accessor :common_event_id
  attr_accessor :in_battle
  attr_accessor :in_menu
  attr_accessor :message_window_showing
  attr_accessor :player_transferring
  attr_accessor :player_new_map_id
  attr_accessor :player_new_x
  attr_accessor :player_new_y
  attr_accessor :player_new_direction
  attr_accessor :transition_processing
  attr_accessor :transition_name
  attr_accessor :gameover
  attr_accessor :to_title
  attr_accessor :last_file_loaded
  attr_accessor :debug_top_row
  attr_accessor :debug_index
  def initialize
    @common_event_id = 0
    @in_battle       = false
    @in_menu         = false
    @message_window_showing = false
    @player_transferring    = false
    @gameover               = false
    @to_title               = false
  end
  def method_missing(m, *a); nil; end
end

module System
  def self.user_language; "en_US"; end
  def self.platform; "3DS"; end
  def self.delta_t; 1; end
  def self.uptime; 0; end
  def self.power_state; {}; end
end

module Settings
  LANGUAGES = []
  def self.const_missing(name); nil; end
end

module Game
  def self.start_new
    $PokemonTemp.begunNewGame = true rescue nil
    $game_temp.common_event_id = 0   rescue nil
    $scene = Scene_Map.new           rescue nil
    SaveData.load_new_game_values    rescue nil
  end
end

module SaveData
  FILE_PATH = "Save.rxdata"
  def self.exists?; false; end
  def self.exist?;  false; end
  def self.read_from_file(path); {}; end
  def self.valid?(data); true; end
  def self.delete_file; end
  def self.move_old_windows_save; end
  def self.initialize_bootup_values
    $PokemonSystem = PokemonSystem.new rescue nil
  end
  def self.load_bootup_values(data)
    $PokemonSystem = PokemonSystem.new rescue nil
  end
  def self.load_new_game_values; end
  def self.load_all_values(data); end
end

# =============================================================================
# FIX CRÍTICO: BitmapWrapper DATA_PTR + initialize
#
# O PE define BitmapWrapper com initialize(bitmap) -- 1 argumento (um Bitmap).
# O nosso bmp_init C aceita tanto (width,height) como (bitmap) correctamente.
# Mas scripts do jogo podem ter redefinido initialize por cima.
#
# Esta secção corre DEPOIS de todos os scripts e:
#  1. Remove o initialize Ruby se ainda existir
#  2. Garante que BitmapWrapper.new(w, h) e BitmapWrapper.new(bitmap) funcionam
# =============================================================================
begin
  if Object.const_defined?(:BitmapWrapper)
    # Verificar se o initialize actual é Ruby (tem source_location) ou C (nil)
    src = nil
    begin
      m = BitmapWrapper.instance_method(:initialize)
      src = m.source_location rescue nil
    rescue; end

    if src
      # Initialize Ruby encontrado -- tentar remover para restaurar C-native
      removed = false
      begin
        BitmapWrapper.send(:remove_method, :initialize)
        removed = true
        printf("[BW_FIX] BitmapWrapper#initialize Ruby removido -- C-native OK\n")
      rescue => e
        printf("[BW_FIX] remove_method falhou: %s\n", e.message)
      end

      unless removed
        # Fallback: substituir por delegação explícita a bmp_init via super
        begin
          BitmapWrapper.class_eval do
            def initialize(arg1 = nil, arg2 = nil)
              if arg1.is_a?(Integer) && !arg2.nil?
                super(arg1, arg2.to_i)
              elsif arg1.is_a?(Integer)
                super(arg1, arg1)
              elsif arg1.is_a?(String)
                super(arg1)
              elsif arg1.nil?
                super(1, 1)
              else
                # Bitmap/BitmapWrapper object -- copiar via super
                super(arg1)
              end
            end
          end
          printf("[BW_FIX] fallback delegate super aplicado\n")
        rescue => e2
          printf("[BW_FIX] fallback tambem falhou: %s\n", e2.message)
        end
      end
    else
      printf("[BW_FIX] BitmapWrapper#initialize ja e C-native (OK)\n")
    end
  end
rescue => e
  printf("[BW_FIX] erro geral: %s\n", e.message)
end
