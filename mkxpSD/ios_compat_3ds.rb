# ios_compat_3ds.rb -- mruby-safe compat layer for 3DS port
# Junho 2026 -- correcoes:
#   - pbRgssExists? / pbRgssOpen / pbLoadBattleAnimations stub adicionados
#   - nil_or_empty? movido para top-level e corrigido (era Kernel-only)
#   - String#starts_with_vowel? garantida antes dos scripts
#   - pbGetBasicFont / isOneOf? / _INTL garantidos aqui tambem

$joiplay = true
$DEBUG   = true
$BTEST   = false
$MKXP    = true

# -- Thread stub ---------------------------------------------------------------
begin
  Thread
rescue NameError
  class Thread
    def self.new(*_a); end
    def self.critical; false; end
    def self.critical=(v); v; end
    def join; end
    def kill; end
  end
end

begin
  unless Thread.respond_to?(:critical)
    def Thread.critical; false; end
    def Thread.critical=(v); v; end
  end
rescue; end

# -- NDS::NullStub -------------------------------------------------------------
module NDS
  class NullStub
    def self.method_missing(_n, *_a); self; end
    def self.respond_to_missing?(_n, _i=false); true; end
    def self.const_missing(_n); self; end
    def self.new(*_a); self; end
    def self.to_s; ''; end
    def self.to_str; ''; end
    def self.inspect; '#<NDS::NullStub>'; end
    def method_missing(_n, *_a); nil; end
    def respond_to_missing?(_n, _i=false); true; end
  end
  ErrorStubs = {}
  ERROR_SUFFIXES = ["Error", "Err", "Exception", "Failure"].freeze
end

class Module
  def const_missing(name)
    nds_ok = false
    begin
      nds_ok = Object.const_defined?(:NDS) && ::NDS.const_defined?(:NullStub)
    rescue; end
    return super unless nds_ok
    begin
      n = name.to_s
      is_error = ::NDS::ERROR_SUFFIXES.any? { |s| n.end_with?(s) }
      if is_error
        key = [self, name]
        ::NDS::ErrorStubs[key] ||= begin
          klass = Class.new(StandardError)
          const_set(name, klass) rescue nil
          klass
        end
      else
        ::NDS::NullStub
      end
    rescue
      super rescue nil
    end
  end
end

# -- Process / Kernel sandboxing -----------------------------------------------
module Kernel
  def exit!(status=false); exit(status) rescue nil; end
  def system(*_a); nil; end
  def exec(*_a); nil; end
  def fork(*_a); nil; end
  def spawn(*_a); nil; end
  module_function :exit!, :system, :exec, :fork, :spawn
end
module Process
  def self.exit!(status=false); Kernel.exit(status) rescue nil; end
end

# -- USEKEYBOARDTEXTENTRY ------------------------------------------------------
begin
  unless Object.const_defined?(:USEKEYBOARDTEXTENTRY)
    USEKEYBOARDTEXTENTRY = false
  end
rescue; end

# -- NilClass safe stubs -------------------------------------------------------
# NOTE: operator defs (*, **, <, <=, <=>, >, >=, <<, >>, /, %) omitted --
# mruby parser rejects them on NilClass. Covered by coerce + method_missing.
class NilClass
  def abs; 0; end
  def ceil(*_a); 0; end
  def coerce(o); [o, 0]; end
  def div(*_a); 0; end
  def divmod(*_a); []; end
  def even?; true; end
  def fdiv(*_a); 0.0; end
  def floor(*_a); 0; end
  def finite?; true; end
  def hash; 0; end
  def id; nil; end
  def infinite?; true; end
  def integer?; false; end
  def modulo(*_a); 0; end
  def nan?; true; end
  def next; 0; end
  def nonzero?; false; end
  def odd?; false; end
  def ord; 0; end
  def quo(*_a); 0.0; end
  def real; 0; end
  def real?; false; end
  def remainder(*_a); 0; end
  def round(*_a); 0; end
  def succ; 0; end
  def to_int; 0; end
  def truncate(*_a); 0; end
  def zero?; true; end
  def bytesize; 0; end
  def capitalize; ''; end
  def chomp(*_a); ''; end
  def chr; ''; end
  def count(*_a); 0; end
  def delete(*_a); ''; end
  def downcase; ''; end
  def dump; ''; end
  def each(*_a); self; end
  def each_char(*_a); self; end
  def each_line(*_a); self; end
  def each_with_index(*_a); self; end
  def empty?; true; end
  def encode(*_a); ''; end
  def end_with?(*_a); false; end
  def force_encoding(*_a); ''; end
  def getbyte(*_a); 0; end
  def gsub(*_a); ''; end
  def gsub!(*_a); ''; end
  def hex; 0; end
  def include?(*_a); false; end
  def index(*_a); nil; end
  def insert(*_a); ''; end
  def length; 0; end
  def lines(*_a); []; end
  def ljust(*_a); ''; end
  def lstrip; ''; end
  def match(*_a); nil; end
  def oct; 0; end
  def partition(*_a); []; end
  def replace(*_a); ''; end
  def reverse; ''; end
  def rindex(*_a); nil; end
  def rjust(*_a); ''; end
  def rpartition(*_a); []; end
  def rstrip; ''; end
  def scan(*_a); []; end
  def size; 0; end
  def slice(*_a); nil; end
  def split(*_a); []; end
  def squeeze(*_a); ''; end
  def start_with?(*_a); false; end
  def strip; ''; end
  def sub(*_a); ''; end
  def sum(*_a); 0; end
  def swapcase; ''; end
  def tr(*_a); ''; end
  def unpack(*_a); []; end
  def upcase; ''; end
  def valid_encoding?; true; end
  def to_str; ''; end
  def to_ary; []; end
  def name; ''; end
  def defaultBGM; nil; end
  def defaultBGS; nil; end
  def title_bgm; nil; end
  def batterywarning; false; end
  def cueFrames; -1; end
  def cueFrames=(v); v; end
  def cueBGM; nil; end
  def bugContestState; nil; end
  def bugContestState=(v); v; end
  def hiddenMoveEventCalling; false; end
  def hiddenMoveEventCalling=(v); v; end
  def can_dive; false; end
  def can_surf_freely; false; end
  def waterfall; false; end
  def get_pokemon_with_move(_m); nil; end
  def keyItemCalling; false; end
  def keyItemCalling=(v); v; end
  def pokemon_party; []; end
  def registeredItems; []; end
  def common_event_id=(v); v; end
  def any?(*_a); false; end
  def all?(*_a); true; end
  def none?(*_a); true; end
  def first(*_a); nil; end
  def last(*_a); nil; end
  def flatten(*_a); []; end
  def compact; []; end
  def uniq; []; end
  def sort; []; end
  def sort_by; []; end
  def min; nil; end
  def max; nil; end
  def reduce(*_a); _a[0]; end
  def inject(*_a); _a[0]; end
  def zip(*_a); []; end
  def push(*_a); self; end
  def pop; nil; end
  def shift; nil; end
  def concat(*_a); self; end
  def keys; []; end
  def values; []; end
  def has_key?(_k); false; end
  def key?(_k); false; end
  def fetch(_k, *d); d[0]; end
  def merge(*_a); {}; end
  def to_a; []; end
  def to_h; {}; end
  def to_i; 0; end
  def to_f; 0.0; end
  def member?(_v); false; end
  def method_nil(*_a); nil; end
  def disposed?; true; end
  def dispose; nil; end
  def update; nil; end
  def visible; false; end
  def visible=(v); nil; end
  # pbRgssExists? / pbRgssOpen chamados em nil -- retorno seguro
  def pbRgssExists?(_f); false; end
  def pbRgssOpen(_f, _m="rb"); nil; end
  # battle animations
  def pbLoadBattleAnimations; nil; end
  # FIX: NilClass#visitedMaps / roamPokemon / roamPosition / roamedAlready= --
  # PokemonMapFactory acede a $PokemonGlobal.visitedMaps, roamPokemon, etc.
  # quando $PokemonGlobal e nil.
  def visitedMaps; {}; end
  def roamPokemon; []; end
  def roamPosition; {}; end
  def roamedAlready; false; end
  def roamedAlready=(_v); _v; end
  # FIX: NilClass#safariState / bugContestState -- inProgress? chamado em nil
  def safariState; self; end
  def safariState=(_v); _v; end
  def pbClearIfEnded; end
  # FIX: NilClass#inProgress? / pbEnd -- safariState devolve self (nil)
  # e depois chama .inProgress? sobre esse nil.
  def inProgress?; false; end
  def pbEnd; end
  # FIX: NilClass#encounter_version -- PokemonEncounters#setup acede a isto
  def encounter_version; 0; end
  # FIX: NilClass#autoplay_bgm -- Game_Map#autoplay acede a $game_map.map_data.autoplay_bgm
  # quando map_data e nil (antes dos dados do mapa carregarem)
  def autoplay_bgm; false; end
  def autoplay_bgs; false; end
  def bgm; nil; end
  def bgs; nil; end
  # FIX: NilClass#moving? / #x / #y / #in_menu -- Scene_Map update loop
  def moving?; false; end
  def x; 0; end
  def y; 0; end
  def in_menu; false; end
end rescue nil

# -- RPG::System stub ----------------------------------------------------------
begin
  module RPG
    class System
      def windowskin_name
        @windowskin_name.is_a?(String) ? @windowskin_name : ""
      end
      def windowskin_name=(v)
        @windowskin_name = v.to_s rescue ""
      end
      def magic_number; @magic_number ||= 0; end
      def magic_number=(v); @magic_number = v; end
      def start_map_id; @start_map_id ||= 1; end
      def start_x; @start_x ||= 0; end
      def start_y; @start_y ||= 0; end
      def title_bgm; @title_bgm; end
      def battle_bgm; @battle_bgm; end
      def battle_end_me; @battle_end_me; end
    end
  end
rescue; end

# -- Integer extras ------------------------------------------------------------
begin
  class Integer
    def name; to_s; end unless method_defined?(:name)
    def each(*_a); yield self if block_given?; self; end unless method_defined?(:each)
    def each_with_index(*_a)
      yield self, 0 if block_given?
      self
    end unless method_defined?(:each_with_index)
    def expired?; false; end unless method_defined?(:expired?)
    def egg?; false; end unless method_defined?(:egg?)
    def compatible_with_move?(_m); false; end unless method_defined?(:compatible_with_move?)
    def last(*_a); self; end unless method_defined?(:last)
    def split(*_a); []; end unless method_defined?(:split)
    def pop; self; end unless method_defined?(:pop)
    def join(*_a); to_s; end unless method_defined?(:join)

    # FIX: [x47] Integer#length -- $data_common_events[id] devolve Integer
    # antes dos dados reais carregarem; Sprite_Timer chama .length no timer.
    def length; 0; end unless method_defined?(:length)
    # FIX: Integer#keys / #[] acessos quando $data_map_infos nao carregou
    def keys; []; end unless method_defined?(:keys)
    # FIX: Integer#pages -- Game_Event#setup chama pages em events[id] quando
    # o marshal devolve um Integer em vez de RPG::Event
    def pages; []; end unless method_defined?(:pages)
    # FIX: Integer#reverse -- iteracao sobre events reversed
    def reverse; []; end unless method_defined?(:reverse)
    # FIX: Integer#condition -- acesso a pages[0].condition quando page e Integer
    def condition; nil; end unless method_defined?(:condition)
    # FIX: Integer#switch1_valid / switch1_id -- Page::Condition fields
    def switch1_valid; false; end unless method_defined?(:switch1_valid)
    def switch1_id; 1; end unless method_defined?(:switch1_id)
    # FIX: Integer#inProgress? / pbEnd -- safariState/bugContestState e Integer
    def inProgress?; false; end unless method_defined?(:inProgress?)
    def pbEnd; end unless method_defined?(:pbEnd)
    # FIX: Integer#id / #x / #y -- RPG::Event fields quando marshal falha
    def id; self; end unless method_defined?(:id)
    def x; 0; end unless method_defined?(:x)
    def y; 0; end unless method_defined?(:y)
    # FIX: Integer#xsize/ysize/zsize [x7/x3/x2 no log] -- map_data pode ser
    # Integer se marshal falhar. CustomTilemap chama map_data.xsize para dims.
    # CRITICO: devolver 1, nunca 0 -- clamp(0, xsize-1) com 0 -> ArgumentError.
    def xsize; 1; end unless method_defined?(:xsize)
    def ysize; 1; end unless method_defined?(:ysize)
    def zsize; 1; end unless method_defined?(:zsize)
    def data; [0]; end unless method_defined?(:data)
  end
rescue; end

# -- String extras -------------------------------------------------------------
begin
  class String
    def starts_with_vowel?
      return false if empty?
      "AEIOUaeiou".include?(self[0].to_s)
    end unless method_defined?(:starts_with_vowel?)

    def nil_or_empty?
      false
    end unless method_defined?(:nil_or_empty?)
  end
rescue; end

# -- Regexp extras -------------------------------------------------------------
# FIX: Regexp#to_str [x41] -- conversao implicita quando codigo faz
# "string" + regexp ou passa regexp a format(). to_str permite coercao implicita.
# FIX: Regexp#length -- chamado raramente mas pode acontecer no mesmo contexto.
begin
  class Regexp
    def to_str; @src.to_s rescue ""; end unless method_defined?(:to_str)
    def length; (@src.to_s rescue "").length; end unless method_defined?(:length)
  end
rescue; end

# -- Array extras: stubs para campos RPG::Map / RPG::Tileset ------------------
# O marshal do 3DS pode devolver Arrays em vez de objectos RPG:: quando o
# formato rxdata nao e completamente suportado. Estes stubs evitam o crash
# em massa de [MFD] Array#tileset_id, Array#events, Array#width, etc.
begin
  class Array
    # RPG::Map fields
    def tileset_id; 1; end unless method_defined?(:tileset_id)
    def events; {}; end unless method_defined?(:events)
    def width; self[0].is_a?(Integer) ? self[0] : 20; end unless method_defined?(:width)
    def height; self[1].is_a?(Integer) ? self[1] : 15; end unless method_defined?(:height)
    def autoplay_bgm; false; end unless method_defined?(:autoplay_bgm)
    def autoplay_bgs; false; end unless method_defined?(:autoplay_bgs)
    def bgm; nil; end unless method_defined?(:bgm)
    def bgs; nil; end unless method_defined?(:bgs)
    def encounter_list; []; end unless method_defined?(:encounter_list)
    def encounter_step; 30; end unless method_defined?(:encounter_step)
    # FIX: Array#data -- CustomTilemap/Sprite_AnimationSprite acede a map_data.data
    # quando map_data e um Array em vez de Table. Devolve self (o proprio array).
    def data; self; end unless method_defined?(:data)
    # FIX: Array#xsize/ysize/zsize -- mesmo contexto que Integer acima.
    # CRITICO: nunca devolver 0 -- clamp(0, xsize-1) com 0 -> ArgumentError.
    def xsize; [self[0].is_a?(Integer) ? self[0] : length, 1].max; end unless method_defined?(:xsize)
    def ysize; [self[1].is_a?(Integer) ? self[1] : 1, 1].max; end unless method_defined?(:ysize)
    def zsize; 1; end unless method_defined?(:zsize)
    # RPG::Tileset fields
    def tileset_name; ""; end unless method_defined?(:tileset_name)
    def autotile_names; Array.new(7, ""); end unless method_defined?(:autotile_names)
    def panorama_name; ""; end unless method_defined?(:panorama_name)
    def panorama_hue; 0; end unless method_defined?(:panorama_hue)
    def fog_name; ""; end unless method_defined?(:fog_name)
    def fog_hue; 0; end unless method_defined?(:fog_hue)
    def fog_opacity; 64; end unless method_defined?(:fog_opacity)
    def fog_blend_type; 0; end unless method_defined?(:fog_blend_type)
    def fog_zoom; 200; end unless method_defined?(:fog_zoom)
    def fog_sx; 0; end unless method_defined?(:fog_sx)
    def fog_sy; 0; end unless method_defined?(:fog_sy)
    def battleback_name; ""; end unless method_defined?(:battleback_name)
    def passages; nil; end unless method_defined?(:passages)
    def priorities; nil; end unless method_defined?(:priorities)
    def terrain_tags; nil; end unless method_defined?(:terrain_tags)
  end
rescue; end

# -- nil_or_empty? top-level ---------------------------------------------------
begin
  unless respond_to?(:nil_or_empty?)
    def nil_or_empty?(string)
      string.nil? || !string.is_a?(String) || string.size == 0
    end
  end
rescue; end

# -- Module#pbResolveBitmap ----------------------------------------------------
begin
  class Module
    unless method_defined?(:pbResolveBitmap)
      def pbResolveBitmap(_x); nil; end
    end
  end
rescue; end

# -- PokemonSystem stub --------------------------------------------------------
begin
  unless Object.const_defined?(:PokemonSystem)
    class PokemonSystem
      attr_accessor :textspeed, :battlescene, :battlestyle, :frame, :textskin
      attr_accessor :screensize, :language, :runstyle, :bgmvolume, :bgsvolume
      attr_accessor :sevolume, :daytone, :performance
      def initialize
        @textspeed = 2; @battlescene = 0; @battlestyle = 0
        @frame = 0; @textskin = 0; @screensize = 1; @language = 0
        @runstyle = 1; @bgmvolume = 100; @bgsvolume = 100
        @sevolume = 100; @daytone = 0; @performance = 0
      end
    end
  end
rescue; end

$PokemonSystem ||= (PokemonSystem.new rescue nil)

# -- Win32API stub -------------------------------------------------------------
begin
  unless Object.const_defined?(:Win32API)
    class Win32API
      def initialize(_dll, _func, *_a); end
      def call(*_a); 0; end
    end
  end
rescue; end

# -- ENV -----------------------------------------------------------------------
begin
  tmp = '/tmp'
  userdata = "#{tmp}/UserData"
  ENV['TEMP']         ||= tmp
  ENV['TMP']          ||= tmp
  ENV['APPDATA']      ||= "#{userdata}/AppData"
  ENV['USERPROFILE']  ||= userdata
  ENV['COMPUTERNAME'] ||= '3DS'
  ENV['USERNAME']     ||= 'Player'
  ENV['OS']           ||= 'Windows_NT'
  ENV['PATH']         ||= ''
rescue; end

# -- Online stubs --------------------------------------------------------------
module GameJolt
  def self.method_missing(*_a); false; end
  def self.respond_to_missing?(*_a); true; end
  def self.is_logged_in; false; end
  def self.login; true; end
end rescue nil

module Downloader
  def self.downloading?; false; end
  def self.update; end
  def self.progress?; 100; end
  def self.download(*_a); end
end rescue nil

module FontInstaller
  def self.install; end
end rescue nil

begin
  unless respond_to?(:pbGetTextFromInternet)
    def pbGetTextFromInternet(_url); ''; end
  end
rescue; end

# -- Kernel extras -------------------------------------------------------------
begin
  module Kernel
    def set_loop_points(*_a); end
    def load_module(*_a); nil; end
    def autosave(*_a); end
    module_function :set_loop_points, :load_module, :autosave
  end
rescue; end

# -- Graphics extras -----------------------------------------------------------
begin
  module Graphics
    begin
      PlaneSpeedUp = false unless const_defined?(:PlaneSpeedUp)
    rescue; end
    def self.poke_width; width; end unless respond_to?(:poke_width)
    def self.poke_height; height; end unless respond_to?(:poke_height)
    def self.poke_snap_to_bitmap; snap_to_bitmap; end unless respond_to?(:poke_snap_to_bitmap)
    def self.poke_resize_screen(w, h); resize_screen(w, h); end unless respond_to?(:poke_resize_screen)
    def self.haveresizescreen; true; end unless respond_to?(:haveresizescreen)
  end
rescue; end

# -- Game_Map#bridge -----------------------------------------------------------
begin
  if Object.const_defined?(:Game_Map)
    class Game_Map
      unless method_defined?(:bridge)
        def bridge; @bridge ||= 0; end
        def bridge=(v); @bridge = v; end
      end
    end
  end
rescue; end

# -- Game_System extras --------------------------------------------------------
begin
  if Object.const_defined?(:Game_System)
    class Game_System
      def timer_working; @timer_working ||= false; end unless method_defined?(:timer_working)
      def getPlayingBGM; @playing_bgm; end unless method_defined?(:getPlayingBGM)
      def playing_bgm; @playing_bgm; end unless method_defined?(:playing_bgm)
      def playing_bgs; @playing_bgs; end unless method_defined?(:playing_bgs)
    end
  end
rescue; end

# -- Game_Temp extras ----------------------------------------------------------
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
      def transition_name; @transition_name ||= ""; end unless method_defined?(:transition_name)
      def transition_name=(v); @transition_name = v; end unless method_defined?(:transition_name=)
      def message_window_showing; @message_window_showing ||= false; end unless method_defined?(:message_window_showing)
      def message_window_showing=(v); @message_window_showing = v; end unless method_defined?(:message_window_showing=)
    end
  end
rescue; end

# -- Game_Player extras --------------------------------------------------------
begin
  if Object.const_defined?(:Game_Player)
    class Game_Player
      def tile_id; @tile_id ||= 0; end unless method_defined?(:tile_id)
      def sprite_size=(v); @sprite_size = v; end unless method_defined?(:sprite_size=)
      def sprite_size; @sprite_size ||= [8, 8]; end unless method_defined?(:sprite_size)
      def screen_x; @x ? (@x * 32) : 0; end unless method_defined?(:screen_x)
      def screen_y; @y ? (@y * 32) : 0; end unless method_defined?(:screen_y)
      def screen_z(_h=0); 0; end unless method_defined?(:screen_z)
      def animation_id; @animation_id ||= 0; end unless method_defined?(:animation_id)
      def animation_id=(v); @animation_id = v; end unless method_defined?(:animation_id=)
      def moving?; @moving ||= false; end unless method_defined?(:moving?)
      def straighten; end unless method_defined?(:straighten)
    end
  end
rescue; end

# -- PokemonBag extras ---------------------------------------------------------
begin
  if Object.const_defined?(:PokemonBag)
    class PokemonBag
      def registeredItems; @registeredItems ||= []; end unless method_defined?(:registeredItems)
      def registeredItems=(v); @registeredItems = v; end unless method_defined?(:registeredItems=)
    end
  end
rescue; end

# -- pbCanUseHiddenMove? -------------------------------------------------------
begin
  unless Kernel.respond_to?(:pbCanUseHiddenMove?)
    module Kernel
      def pbCanUseHiddenMove?(_move); false; end
      module_function :pbCanUseHiddenMove?
    end
  end
rescue; end

# -- pbRgssExists? / pbRgssOpen ------------------------------------------------
# Chamados por pbLoadBattleAnimations e outros loaders de assets.
# No 3DS todos os assets estao em sdmc:/mkxp/game/ -- verificamos la.
# safeExists? e definido mais tarde pelo binding C (check_entry_methods),
# por isso usamos File.open rescue como fallback seguro.
begin
  unless respond_to?(:pbRgssExists?)
    GAME_ROOT_3DS = "sdmc:/mkxp/game"

    def pbRgssExists?(filename)
      return false if filename.nil? || filename.to_s.empty?
      f = filename.to_s
      root = (Object.const_defined?(:GAME_ROOT_3DS) ? GAME_ROOT_3DS : "sdmc:/mkxp/game")
      [f, "#{root}/#{f}"].any? do |p|
        begin
          fh = File.open(p, "rb")
          fh.close if fh && fh.respond_to?(:close)
          !fh.nil?
        rescue
          false
        end
      end
    end
    begin
      module Kernel
        def pbRgssExists?(filename)
          return false if filename.nil? || filename.to_s.empty?
          f = filename.to_s
          root = (Object.const_defined?(:GAME_ROOT_3DS) ? GAME_ROOT_3DS : "sdmc:/mkxp/game")
          [f, "#{root}/#{f}"].any? do |p|
            begin
              fh = File.open(p, "rb")
              fh.close if fh && fh.respond_to?(:close)
              !fh.nil?
            rescue
              false
            end
          end
        end
        module_function :pbRgssExists?
      end
    rescue; end
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
    begin
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
    rescue; end
  end
rescue; end

# -- pbLoadBattleAnimations stub -----------------------------------------------
# Chamado em initialize() de algum scene/manager durante o boot.
# Sem marshal real nem assets PBS descodificados, retorna hash vazio.
begin
  unless respond_to?(:pbLoadBattleAnimations)
    def pbLoadBattleAnimations
      {}
    end
    begin
      module Kernel
        def pbLoadBattleAnimations
          {}
        end
        module_function :pbLoadBattleAnimations
      end
    rescue; end
  end
rescue; end

printf("[IOS_COMPAT] ios_compat_3ds.rb carregado OK\n")

# Garantir que o contador de DebugIntro existe e começa a 0
$__debug_intro_count = 0
