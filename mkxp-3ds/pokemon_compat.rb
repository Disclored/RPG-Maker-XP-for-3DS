# pokemon_compat.rb  —  mruby 3.2 / 3DS port
#
# Portado de mkxp-z-apple-mobile (iOS) para mruby 3.2 sem stdlib,
# sem Regexp nativo, sem define_method com blocos MRI-style,
# sem alias_method sobre C bindings (mruby não suporta),
# sem Module#const_missing (não existe em mruby 3.2),
# sem require 'zlib' / Thread.critical / Process.exit! (não existe).
#
# Carrega DEPOIS dos scripts do jogo (postload / check_entry_methods).
# O binding_3ds.cpp já define $MKXP=true e $joiplay=true.

# ---------------------------------------------------------------------------
# 1. Sinalizar JoiPlay-compat (já feito em binding_3ds mas reforçar aqui)
# ---------------------------------------------------------------------------
$joiplay = true unless defined?($joiplay)
$MKXP    = true unless defined?($MKXP)

# ---------------------------------------------------------------------------
# 2. $game_exists — Uranium hard-reset prevention
# ---------------------------------------------------------------------------
$game_exists = nil

# ---------------------------------------------------------------------------
# 3. PokemonSystem#screensize backstop
#    Forks antigos de PE acedem a $PokemonSystem.screensize antes do setter
# ---------------------------------------------------------------------------
begin
  if Object.const_defined?(:PokemonSystem)
    class PokemonSystem
      unless method_defined?(:screensize)
        def screensize
          @screensize ||= 1.0
        end
        def screensize=(v)
          @screensize = v
        end
      end
    end
  end
rescue
end

# ---------------------------------------------------------------------------
# 4. USEKEYBOARDTEXTENTRY — forçar teclado virtual (sem teclado físico no 3DS)
# ---------------------------------------------------------------------------
begin
  # Em mruby não há Object.const_defined? com frozen string —
  # usar defined?() que é safe em qualquer contexto
  unless defined?(USEKEYBOARDTEXTENTRY)
    USEKEYBOARDTEXTENTRY = false
  end
rescue
end

# ---------------------------------------------------------------------------
# 5. Thread.critical / Thread.critical= — PE usa para guardar estado
#    mruby não tem threads reais, não-ops são correctos
# ---------------------------------------------------------------------------
begin
  unless Thread.respond_to?(:critical)
    def Thread.critical
      false
    end
    def Thread.critical=(v)
      v
    end
  end
rescue
end

# ---------------------------------------------------------------------------
# 6. Kernel#system / exec / fork / spawn — sem processos no 3DS
# ---------------------------------------------------------------------------
begin
  module Kernel
    def system(*_a); nil; end
    def exec(*_a);   nil; end
    def fork(*_a);   nil; end
    def spawn(*_a);  nil; end
    module_function :system, :exec, :fork, :spawn
  end
rescue
end

# ---------------------------------------------------------------------------
# 7. Kernel#exit! → redirigir para exit normal
#    mruby não tem Process.exit! mas pode ter Kernel.exit!
# ---------------------------------------------------------------------------
begin
  module Kernel
    def exit!(status = false)
      exit(status)
    end
    module_function :exit!
  end
rescue
end

# ---------------------------------------------------------------------------
# 8. Float operadores bitwise — scripts PE fazem x^2 querendo dizer x**2
# ---------------------------------------------------------------------------
begin
  class Float
    def ^(other)
      self ** other
    end unless method_defined?(:^)

    def <<(n)
      self * (2 ** n)
    end unless method_defined?(:<<)

    def >>(n)
      self / (2 ** n)
    end unless method_defined?(:>>)
  end
rescue
end

# ---------------------------------------------------------------------------
# 9. set_loop_points — plugins de áudio BGM chamam isto como função global
# ---------------------------------------------------------------------------
begin
  module Kernel
    def set_loop_points(*_a); end
    module_function :set_loop_points
  end
rescue
end

# ---------------------------------------------------------------------------
# 10. ENV — variáveis de ambiente Windows esperadas por PE
#     mruby tem ENV mas pode estar vazio; preencher valores seguros
# ---------------------------------------------------------------------------
begin
  tmp = "sdmc:/tmp"
  userdata = "sdmc:/mkxp/userdata"
  {
    "TEMP"                  => tmp,
    "TMP"                   => tmp,
    "APPDATA"               => "#{userdata}/AppData",
    "LOCALAPPDATA"          => "#{userdata}/AppData",
    "ALLUSERSPROFILE"       => userdata,
    "USERPROFILE"           => userdata,
    "HOMEDRIVE"             => "",
    "HOMEPATH"              => userdata,
    "SystemRoot"            => userdata,
    "windir"                => userdata,
    "COMPUTERNAME"          => "3DS",
    "USERNAME"              => "Player",
    "USERDOMAIN"            => "3DS",
    "SESSIONNAME"           => "3DS",
    "OS"                    => "Windows_NT",
    "PATH"                  => "",
    "PATHEXT"               => "",
    "NUMBER_OF_PROCESSORS"  => "1",
    "PROCESSOR_ARCHITECTURE"=> "x86",
  }.each do |k, v|
    ENV[k] ||= v
  end
rescue
end

# ---------------------------------------------------------------------------
# 11. Disposed-safe wrappers para Sprite / Window / Viewport / Plane / Tilemap
#     PE acede a propriedades em objectos já disposed entre frames.
#     mruby não tem alias_method sobre C bindings — usar method_defined?
#     e define_method que em mruby funciona com bloco simples.
#
#     ATENÇÃO: em mruby define_method com bloco que captura variáveis
#     locais funciona mas tem limitações. Usar forma mais simples possível.
# ---------------------------------------------------------------------------
begin
  # Propriedades que devem devolver 0 se disposed
  safe_zero_props  = [:x, :y, :z, :ox, :oy, :width, :height,
                      :opacity, :back_opacity, :contents_opacity]
  # Propriedades que devem devolver false se disposed
  safe_false_props = [:visible]

  [Sprite, Window, Viewport, Plane, Tilemap].each do |klass|
    next unless klass.is_a?(Class)

    safe_zero_props.each do |meth|
      next unless klass.method_defined?(meth)
      next if klass.method_defined?(:"_safe_orig_#{meth}")
      begin
        # Em mruby não podemos alias C methods directamente.
        # Verificar se disposed? antes de chamar o método nativo.
        # Como não podemos alias, definir um wrapper que usa respond_to?
        # e disposed? para retornar safe default.
        #
        # Técnica: guardar referência ao método original via UnboundMethod
        # (disponível em mruby 3.x com mrbgem mruby-method).
        # Se não disponível, simplesmente adicionar um rescue.
        klass.class_eval do
          orig_name = :"_safe_orig_#{meth}"
          # Tentar alias — se falhar (C binding), ignorar
          begin
            alias_method orig_name, meth
            define_method(meth) do
              return 0 if disposed? rescue false
              begin
                send(orig_name)
              rescue
                0
              end
            end
          rescue
            # alias_method falhou (C binding sem suporte) — definir
            # override que faz rescue do RGSSError / RuntimeError
            define_method(meth) do
              begin
                # Chamar super equivalente — em mruby sem alias
                # não temos acesso ao original. Retornar 0 se disposed.
                d = begin; disposed?; rescue; false; end
                return 0 if d
                # Tentar chamar — se RGSSError (objecto C freed), devolver 0
                super()
              rescue
                0
              end
            end rescue nil
          end
        end
      rescue
      end
    end

    safe_false_props.each do |meth|
      next unless klass.method_defined?(meth)
      next if klass.method_defined?(:"_safe_orig_#{meth}")
      begin
        klass.class_eval do
          orig_name = :"_safe_orig_#{meth}"
          begin
            alias_method orig_name, meth
            define_method(meth) do
              return false if disposed? rescue false
              begin
                send(orig_name)
              rescue
                false
              end
            end
          rescue
            define_method(meth) do
              begin
                d = begin; disposed?; rescue; false; end
                return false if d
                super()
              rescue
                false
              end
            end rescue nil
          end
        end
      rescue
      end
    end

  end
rescue
end

# ---------------------------------------------------------------------------
# 12. MkxpNullMouse — $mouse entre sessões fica com objecto órfão
# ---------------------------------------------------------------------------
begin
  unless Object.const_defined?(:MkxpNullMouse)
    class MkxpNullMouse
      def method_missing(*_a); false; end
      def respond_to_missing?(*_a); true; end
      def x; 0; end
      def y; 0; end
    end
  end
  $mouse = MkxpNullMouse.new
rescue
end

# ---------------------------------------------------------------------------
# 13. FmodEx stub — forks PE redefinir Audio via FmodEx DLL
#     No 3DS não há DLL; redirigir tudo para Audio nativo
# ---------------------------------------------------------------------------
begin
  unless Object.const_defined?(:FmodEx)
    module FmodEx
      def self.init(*_a); end

      def self.bgm_play(f, v = 100, p = 100, pos = 0)
        begin; Audio.bgm_play(f, v, p, pos); rescue; begin; Audio.bgm_play(f, v, p); rescue; end; end
        nil
      end
      def self.bgm_fade(ms);  begin; Audio.bgm_fade(ms);  rescue; end; end
      def self.bgm_stop;      begin; Audio.bgm_stop;      rescue; end; end

      def self.bgs_play(f, v = 80, p = 100)
        begin; Audio.bgs_play(f, v, p); rescue; end; nil
      end
      def self.bgs_fade(ms);  begin; Audio.bgs_fade(ms);  rescue; end; end
      def self.bgs_stop;      begin; Audio.bgs_stop;      rescue; end; end

      def self.me_play(f, v = 100, p = 100)
        begin; Audio.me_play(f, v, p); rescue; end; nil
      end
      def self.me_fade(ms);   begin; Audio.me_fade(ms);   rescue; end; end
      def self.me_stop;       begin; Audio.me_stop;       rescue; end; end

      def self.se_play(f, v = 80, p = 100)
        begin; Audio.se_play(f, v, p); rescue; end; nil
      end
      def self.se_stop; begin; Audio.se_stop; rescue; end; end
    end
  end
rescue
end

# ---------------------------------------------------------------------------
# 14. DL stub — scripts antigos PE fazem DL.dlopen('user32')
# ---------------------------------------------------------------------------
begin
  unless Object.const_defined?(:DL)
    module DL
      class CFunc
        def initialize(func, _type = "i")
          @func_name = func.to_s
        end
        def call(*_a); 0; end
        def to_s;      @func_name; end
        def to_str;    @func_name; end
      end

      def self.dlopen(_lib = "")
        # Devolver hash que aceita qualquer chave e devolve string vazia
        h = {}
        def h.[](k); k.to_s; end rescue nil
        h
      end
    end
  end
rescue
end

# ---------------------------------------------------------------------------
# 15. require intercept para paths de rede / stdlib em falta
#     mruby não tem Kernel#require real mas alguns bindings expõem-no.
#     Se existir, interceptar LoadError de paths conhecidos.
# ---------------------------------------------------------------------------
begin
  if Kernel.method_defined?(:require) || Kernel.respond_to?(:require)
    NETWORK_REQUIRE_PATHS_3DS = [
      "socket", "resolv", "resolv-replace",
      "openssl", "digest", "uri", "ipaddr",
      "net", "httparty", "rest-client", "rest_client",
      "discord", "discord-rpc", "discordrb",
      "poke-api-v2", "pokeapi",
      "websocket", "json-jwt", "jwt",
      "dl", "fiddle", "win32ole",
    ]

    orig_req = Kernel.instance_method(:require) rescue nil
    if orig_req
      Kernel.define_method(:require) do |path|
        begin
          orig_req.bind(self).call(path)
        rescue LoadError
          str = path.to_s
          matched = NETWORK_REQUIRE_PATHS_3DS.any? do |e|
            e.end_with?("/") ? str.start_with?(e) : str == e
          end
          raise unless matched
          false
        end
      end rescue nil
    end
  end
rescue
end

# ---------------------------------------------------------------------------
# 16. Graphics aliases — poke_* / mkxp_snap_to_bitmap / haveresizescreen
#     O binding 3DS já define estes, mas por segurança garantir
# ---------------------------------------------------------------------------
begin
  module Graphics
    unless respond_to?(:poke_width)
      def self.poke_width;  width;  end
    end
    unless respond_to?(:poke_height)
      def self.poke_height; height; end
    end
    unless respond_to?(:poke_snap_to_bitmap)
      def self.poke_snap_to_bitmap; snap_to_bitmap; end
    end
    unless respond_to?(:mkxp_snap_to_bitmap)
      def self.mkxp_snap_to_bitmap; snap_to_bitmap; end
    end
    unless respond_to?(:poke_resize_screen)
      def self.poke_resize_screen(w, h); resize_screen(w, h); end
    end
    unless respond_to?(:haveresizescreen)
      def self.haveresizescreen; true; end
    end
    unless respond_to?(:zeus_play_movie)
      def self.zeus_play_movie(f, *_rest); end
    end
  end
rescue
end

# ---------------------------------------------------------------------------
# 17. MKXP module — alguns scripts verificam MKXP.puts / MKXP.zinflate
# ---------------------------------------------------------------------------
begin
  unless Object.const_defined?(:MKXP)
    module MKXP
      def self.zinflate(s)
        begin; Zlib::Inflate.inflate(s); rescue; s; end
      end
      def self.zdeflate(s, _level = -1)
        begin; Zlib::Deflate.deflate(s); rescue; s; end
      end
      def self.data_directory(*a)
        "sdmc:/mkxp/game/Save"
      end
      def self.puts(*a)
        Kernel.puts(*a) rescue nil
      end
    end
  end
rescue
end

# ---------------------------------------------------------------------------
# 18. Pokemon globals reset — limpar estado PE entre sessões
#     (no 3DS normalmente há só uma sessão, mas por segurança)
# ---------------------------------------------------------------------------
begin
  $mouse              = MkxpNullMouse.new rescue nil
  $game_exists        = nil
  $PokemonSystem      = nil unless $PokemonSystem
  $PokemonTemp        = nil unless $PokemonTemp
  $PokemonGlobal      = nil unless $PokemonGlobal
  $PokemonBag         = nil unless $PokemonBag
  $PokemonStorage     = nil unless $PokemonStorage
  $Trainer            = nil unless $Trainer
rescue
end

puts "[pokemon_compat 3DS] carregado OK"
