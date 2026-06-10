# =============================================================================
# debug_probe.rb -- v3: probe de diagnostico para mkxp-3ds
# Carregar do SD: sdmc:/mkxp/debug_probe.rb
# Escreve para sdmc:/mkxp/probe_out.txt E para MKXPDebug/printf
# =============================================================================

# --- Log dedicado independente de MKXPDebug ---
$_probe_file = nil
begin
  $_probe_file = File.open("sdmc:/mkxp/probe_out.txt", "w")
rescue
  begin; $_probe_file = File.open("probe_out.txt", "w"); rescue; $_probe_file = nil; end
end

module Probe
  def self.log(msg)
    line = "[PRB] #{msg}"
    begin
      if $_probe_file
        $_probe_file.write(line + "\n")
        $_probe_file.flush rescue nil
      end
    rescue; end
    begin; printf("#{line}\n"); rescue; end
    begin; MKXPDebug.log(line); rescue; end
  end

  def self.fmt(v)
    return "nil" if v.nil?
    return "true" if v == true
    return "false" if v == false
    begin
      s = v.inspect
      s.length > 80 ? s[0, 80] + "..." : s
    rescue
      begin
        s = v.to_s
        s.length > 80 ? s[0, 80] + "..." : s
      rescue
        "#<#{v.class}>"
      end
    end
  end

  def self.dump_globals
    pairs = {
      "$scene"         => $scene,
      "$game_map"      => $game_map,
      "$game_player"   => $game_player,
      "$game_temp"     => $game_temp,
      "$MapFactory"    => $MapFactory,
      "$PokemonGlobal" => $PokemonGlobal,
      "$DEBUG"         => $DEBUG,
      "$MKXP"          => $MKXP,
    }
    log "-- globals --"
    pairs.each { |n, v| log "  #{n} = #{v.nil? ? 'nil' : v.class}" }
    log "-- /globals --"
  end

  def self.dump_bt(bt_or_exc, max=12)
    bt = bt_or_exc.is_a?(Exception) ? (bt_or_exc.backtrace rescue nil) :
         bt_or_exc.is_a?(Array)     ? bt_or_exc : nil
    return log "  (sem backtrace)" unless bt && !bt.empty?
    bt[0, max].each_with_index { |l, i| log "  bt[#{i}]: #{l}" }
    log "  ... (+#{bt.length - max})" if bt.length > max
  end

  def self.dump_error(label, err)
    log "ERROR #{label}: #{err.class}: #{err.message}"
    dump_bt(err)
  end
end

Probe.log "debug_probe.rb v3 carregado"

# =============================================================================
# DIAGNÓSTICO CRÍTICO 1: Sprite#initialize -- C-native ou Ruby override?
# =============================================================================
begin
  if Object.const_defined?(:Sprite)
    src = Sprite.instance_method(:initialize).source_location rescue nil
    if src.nil?
      Probe.log "[SPRITE_CHECK] Sprite#initialize = C-native (OK -- spr_init activo)"
    else
      Probe.log "[SPRITE_CHECK] PROBLEMA: Sprite#initialize = Ruby #{src.inspect}"
      Probe.log "[SPRITE_CHECK] Aplicando fix: remove_method(:initialize)..."
      fixed = false
      begin
        Sprite.send(:remove_method, :initialize)
        src2 = Sprite.instance_method(:initialize).source_location rescue nil
        if src2.nil?
          Probe.log "[SPRITE_CHECK] FIXED via remove_method: agora C-native"
          fixed = true
        else
          Probe.log "[SPRITE_CHECK] ainda Ruby apos remove_method: #{src2.inspect}"
        end
      rescue => e
        Probe.log "[SPRITE_CHECK] remove_method falhou: #{e.class}: #{e.message}"
      end
      unless fixed
        begin
          Sprite.class_eval { undef_method :initialize }
          src3 = Sprite.instance_method(:initialize).source_location rescue nil
          Probe.log "[SPRITE_CHECK] apos undef_method: #{src3.nil? ? 'C-native OK' : "ainda Ruby #{src3.inspect}"}"
        rescue => e2
          Probe.log "[SPRITE_CHECK] undef_method tambem falhou: #{e2.message}"
        end
      end
    end

    # Listar TODOS os metodos Ruby definidos directamente em Sprite
    begin
      ruby_methods = []
      Sprite.instance_methods(false).each do |m|
        src = Sprite.instance_method(m).source_location rescue nil
        ruby_methods << [m, src] if src
      end
      if ruby_methods.empty?
        Probe.log "[SPRITE_RUBY_METHODS] nenhum metodo Ruby directo em Sprite (OK)"
      else
        Probe.log "[SPRITE_RUBY_METHODS] #{ruby_methods.size} metodos Ruby em Sprite:"
        ruby_methods.each { |m, src| Probe.log "[SPRITE_RUBY_METHODS]   #{m} => #{src.inspect}" }
      end
    rescue => e
      Probe.log "[SPRITE_RUBY_METHODS] erro: #{e.message}"
    end

  else
    Probe.log "[SPRITE_CHECK] Sprite nao existe"
  end
rescue => e
  Probe.log "[SPRITE_CHECK] erro geral: #{e.message}"
end

# =============================================================================
# DIAGNÓSTICO CRÍTICO 2: BitmapWrapper MRB_TT_DATA
# =============================================================================
begin
  if Object.const_defined?(:BitmapWrapper) && Object.const_defined?(:Sprite)
    anc = BitmapWrapper.ancestors.first(3).map(&:to_s) rescue []
    Probe.log "[BW_CHECK] BitmapWrapper ancestors=#{anc.inspect}"

    # --- Inspeccionar initialize do BitmapWrapper ---
    begin
      src_bw = BitmapWrapper.instance_method(:initialize).source_location rescue nil
      Probe.log "[BW_INIT] initialize source_location=#{src_bw.inspect}"
    rescue => e
      Probe.log "[BW_INIT] erro: #{e.message}"
    end

    # --- Listar todos os metodos Ruby definidos directamente em BitmapWrapper ---
    begin
      bw_ruby_methods = []
      BitmapWrapper.instance_methods(false).each do |m|
        src = BitmapWrapper.instance_method(m).source_location rescue nil
        bw_ruby_methods << [m, src]
      end
      Probe.log "[BW_METHODS] #{bw_ruby_methods.size} metodos em BitmapWrapper:"
      bw_ruby_methods.each { |m, src| Probe.log "[BW_METHODS]   #{m} => #{src.inspect}" }
    rescue => e
      Probe.log "[BW_METHODS] erro: #{e.message}"
    end

    _bw = BitmapWrapper.new(4, 4) rescue nil
    if _bw
      Probe.log "[BW_CHECK] BitmapWrapper.new(4,4) OK: #{_bw.width rescue '?'}x#{_bw.height rescue '?'}"

      # --- CRITICO: listar todas as ivars da instancia ---
      begin
        ivars = _bw.instance_variables rescue []
        Probe.log "[BW_IVARS] instance_variables=#{ivars.inspect}"
        ivars.each do |iv|
          val = _bw.instance_variable_get(iv) rescue "ERRO"
          Probe.log "[BW_IVARS]   #{iv} = #{val.nil? ? 'nil' : val.class} (#{val.inspect rescue val.to_s rescue '?'})"
        end
        if ivars.empty?
          Probe.log "[BW_IVARS] NENHUMA ivar! BitmapWrapper nao guarda o Bitmap base como ivar."
          Probe.log "[BW_IVARS] O DATA_PTR tem de estar na propria instancia ou via heranca C."
        end
      rescue => e
        Probe.log "[BW_IVARS] erro: #{e.message}"
      end

      # --- Testar todos os metodos de acesso possiveis ---
      begin
        [:bitmap, :base, :__bmp__, :inner, :raw, :data, :wrapped].each do |m|
          if _bw.respond_to?(m)
            v = _bw.send(m) rescue "ERRO"
            Probe.log "[BW_ACCESSOR] #{m} -> #{v.nil? ? 'nil' : v.class}"
          end
        end
      rescue => e
        Probe.log "[BW_ACCESSOR] erro: #{e.message}"
      end

      _spr = Sprite.new rescue nil
      if _spr
        begin
          _spr.bitmap = _bw
          _got = _spr.bitmap rescue nil
          Probe.log "[BW_CHECK] Sprite.bitmap= OK: bitmap=#{_got.nil? ? 'nil (DATA_PTR FALHOU!)' : _got.class}"
        rescue => e
          Probe.log "[BW_CHECK] Sprite.bitmap= CRASH: #{e.class}: #{e.message}"
        ensure
          _spr.dispose rescue nil
        end
      else
        Probe.log "[BW_CHECK] Sprite.new falhou"
      end
    else
      Probe.log "[BW_CHECK] BitmapWrapper.new falhou"
    end
  end
rescue => e
  Probe.log "[BW_CHECK] erro: #{e.message}"
end

# =============================================================================
# DIAGNÓSTICO 3: Subclasses de Sprite com initialize Ruby
# =============================================================================
begin
  [
    "Sprite_Character", "Sprite_Reflection", "Sprite_SurfBase",
    "Sprite_Timer", "Sprite_Picture", "Sprite_DynamicShadows",
    "Sprite_Battler", "IconSprite", "AnimatedSprite", "MosaicSprite",
  ].each do |klass_name|
    next unless Object.const_defined?(klass_name)
    klass = Object.const_get(klass_name)
    next unless klass.method_defined?(:initialize)
    src = klass.instance_method(:initialize).source_location rescue nil
    if src
      Probe.log "[SUBSPRITE_CHECK] #{klass_name}#initialize = Ruby #{src.inspect}"
    else
      Probe.log "[SUBSPRITE_CHECK] #{klass_name}#initialize = C-native"
    end
  end
rescue => e
  Probe.log "[SUBSPRITE_CHECK] erro: #{e.message}"
end

# =============================================================================
# PATCH: method_missing verboso
# =============================================================================
begin
  class Object
    def method_missing(m, *a, &blk)
      n   = m.to_s
      key = "#{self.class}##{n}"
      if $_seen_errors && $_seen_errors[key]
        $_seen_errors[key] += 1
      else
        $_seen_errors[key] = 1 if $_seen_errors
        $_error_order << key if $_error_order
        args_s = a.length > 0 ? "(#{a.map{|x| x.inspect rescue '?'}.join(',')[0,60]})" : "()"
        Probe.log "MISSING #{key}#{args_s}"
      end
      n2 = n
      return nil   if n2.start_with?("update","draw","dispose","refresh","clear",
                                     "create","setup","start","terminate","pbOn",
                                     "pbOff","set","load","save","init","reset",
                                     "show","hide")
      return false if n2.end_with?("?")
      return false if n2.start_with?("is_","has_","can_","should_","will_")
      return []    if n2.start_with?("all","list","each","get_all","find_all")
      return ""    if n2.start_with?("name","title","filename","path",
                                    "character","charset","tileset","text")
      nil
    end
    def respond_to_missing?(m, ip=false); true; end
  end
  Probe.log "PATCH method_missing OK"
rescue => e
  Probe.log "PATCH method_missing falhou: #{e.message}"
end

# =============================================================================
# PATCH: SceneProbe
# =============================================================================
begin
  $_probe_scene_crashes = {}

  module SceneProbe
    def self.run(scene)
      return if scene.nil?
      klass = scene.class.to_s
      Probe.log "SCENE start: #{klass}"
      begin
        Graphics.transition(0)
      rescue => e
        Probe.log "SCENE transition FALHOU: #{e.message}"
      end
      begin
        scene.main
        Probe.log "SCENE end ok: #{klass} -> $scene=#{$scene.nil? ? 'nil' : $scene.class}"
        $_probe_scene_crashes[klass] = 0
      rescue => e
        count = ($_probe_scene_crashes[klass] ||= 0) + 1
        $_probe_scene_crashes[klass] = count
        Probe.log "SCENE CRASH ##{count}: #{klass}: #{e.class}: #{e.message}"
        Probe.dump_bt(e)
        Probe.dump_globals
        $scene = nil
      end
    end
  end
  Probe.log "PATCH SceneProbe OK"
rescue => e
  Probe.log "PATCH SceneProbe falhou: #{e.message}"
end

# =============================================================================
# PATCH: mainFunctionDebug verboso
# =============================================================================
begin
  if respond_to?(:mainFunctionDebug)
    alias __probe_mFD_orig mainFunctionDebug

    def mainFunctionDebug
      step = 0
      Probe.log "mainFunctionDebug START"

      # =======================================================================
      # FIX CRÍTICO: BitmapWrapper#initialize Ruby -> remover para restaurar C-native
      # O patch MFD em C++ adiciona um initialize Ruby ao BitmapWrapper que nao
      # chama super, por isso DATA_PTR fica NULL. Este fix corre aqui, DEPOIS
      # do patch MFD ter sido aplicado (ao contrario do rgss_stubs.rb que corre antes).
      # =======================================================================
      begin
        if Object.const_defined?(:BitmapWrapper)
          src = BitmapWrapper.instance_method(:initialize).source_location rescue nil
          Probe.log "[BW_FIX] initialize source_location=#{src.inspect}"
          if src
            removed = false
            begin
              BitmapWrapper.send(:remove_method, :initialize)
              src2 = BitmapWrapper.instance_method(:initialize).source_location rescue nil
              if src2.nil?
                Probe.log "[BW_FIX] remove_method OK -- C-native restaurado"
                removed = true
              else
                Probe.log "[BW_FIX] remove_method OK mas ainda Ruby: #{src2.inspect}"
              end
            rescue => e
              Probe.log "[BW_FIX] remove_method falhou: #{e.message}"
            end
            unless removed
              begin
                BitmapWrapper.class_eval { undef_method :initialize }
                src3 = BitmapWrapper.instance_method(:initialize).source_location rescue nil
                Probe.log "[BW_FIX] undef_method: #{src3.nil? ? 'C-native OK' : "ainda Ruby #{src3.inspect}"}"
              rescue => e2
                Probe.log "[BW_FIX] undef_method tambem falhou: #{e2.message}"
              end
            end
            # Verificar com instancia real
            begin
              _t = BitmapWrapper.new(4, 4)
              Probe.log "[BW_FIX] BitmapWrapper.new(4,4) apos fix: #{_t.width rescue '?'}x#{_t.height rescue '?'}"
              _s = Sprite.new rescue nil
              if _s
                _s.bitmap = _t
                got = _s.bitmap rescue nil
                Probe.log "[BW_FIX] Sprite.bitmap= apos fix: #{got.nil? ? 'nil (AINDA FALHOU)' : got.class + ' (OK!)'}"
                _s.dispose rescue nil
              end
            rescue => e3
              Probe.log "[BW_FIX] teste pos-fix falhou: #{e3.message}"
            end
          else
            Probe.log "[BW_FIX] initialize ja e C-native antes do fix (OK)"
          end
        else
          Probe.log "[BW_FIX] BitmapWrapper nao existe"
        end
      rescue => e
        Probe.log "[BW_FIX] erro geral: #{e.message}"
      end

      begin
        step = 1; MessageTypes.loadMessageFile("Data/messages.dat") if safeExists?("Data/messages.dat")
        step = 2; PluginManager.runPlugins
        step = 3; Compiler.main
        step = 4; Game.initialize
        step = 5; Game.set_up_system
        begin; $PokemonSystem = PokemonSystem.new unless $PokemonSystem; rescue; end
        Probe.log "step5 OK"; Probe.dump_globals
        step = 6; Graphics.update
        step = 7; Graphics.freeze
        step = 8
        $scene = pbCallTitle
        Probe.log "step8 OK: $scene=#{$scene.nil? ? 'nil' : $scene.class}"
        step = 9
        if $scene.nil?
          Probe.log "WARNING: $scene nil antes do scene loop"
        else
          scene_crashes = 0
          loop do
            break if $scene.nil?
            break if scene_crashes >= 30
            prev = $scene
            SceneProbe.run($scene)
            scene_crashes += 1 if $scene.nil? && !prev.nil?
          end
          Probe.log "scene loop end (crashes=#{scene_crashes})"
        end
        step = 10; Graphics.transition(20)
        Probe.log "mainFunctionDebug DONE -> 1"
        return 1
      rescue => e
        Probe.log "CRASH step #{step}: #{e.class}: #{e.message}"
        Probe.dump_bt(e)
        Probe.dump_globals
        raise RuntimeError, "[STEP #{step}] #{e.class}: #{e.message}"
      end
    end
    Probe.log "PATCH mainFunctionDebug OK"
  else
    Probe.log "AVISO: mainFunctionDebug nao existe ainda"
  end
rescue => e
  Probe.log "PATCH mainFunctionDebug falhou: #{e.message}"
end

# =============================================================================
# PATCH: wrap initialize das classes principais
# =============================================================================
begin
  [
    "Spriteset_Map", "Spriteset_Global",
    "Scene_Map", "Scene_DebugIntro",
    "Sprite_Character", "Sprite_Reflection", "Sprite_SurfBase",
  ].each do |klass_name|
    begin
      klass = Object.const_get(klass_name)
      next unless klass.method_defined?(:initialize)
      next if klass.method_defined?(:__probe_init_orig)
      klass.class_eval do
        alias __probe_init_orig initialize
        def initialize(*a, &blk)
          Probe.log "INIT #{self.class}(#{a.map{|x| Probe.fmt(x)}.join(', ')})"
          begin
            __probe_init_orig(*a, &blk)
            Probe.log "INIT #{self.class} OK"
          rescue => e
            Probe.log "INIT #{self.class} CRASH: #{e.class}: #{e.message}"
            Probe.dump_bt(e)
            raise
          end
        end
      end
      Probe.log "PATCH INIT #{klass_name} OK"
    rescue => e
      Probe.log "PATCH INIT #{klass_name} falhou: #{e.message}"
    end
  end
rescue => e
  Probe.log "PATCH INIT blocks falhou: #{e.message}"
end

# =============================================================================
# PATCH: at_exit summary
# =============================================================================
begin
  at_exit do
    begin
      Probe.log "=== PROBE SUMMARY at_exit ==="
      Probe.dump_globals
      if $_error_order && !$_error_order.empty?
        sorted = $_error_order.sort_by { |k| -($_seen_errors[k] || 0) }
        Probe.log "Top 20 missing:"
        sorted[0, 20].each { |k| Probe.log "  [x#{$_seen_errors[k] || 0}] #{k}" }
      end
      Probe.log "=== /PROBE SUMMARY ==="
    rescue; end
    begin; $_probe_file.close rescue nil; rescue; end
  end
  Probe.log "PATCH at_exit OK"
rescue => e
  Probe.log "PATCH at_exit falhou: #{e.message}"
end

Probe.log "debug_probe.rb v3 todos os patches activos"

# =============================================================================
# >>> SONDA DE RENDER DO TILEMAP (acrescentada) <<<
# Usa o Probe.log existente (que escreve no debug_binding.log via MKXPDebug.log).
# Instrumenta CustomTilemap para descobrir porque o mapa fica preto.
# Procura no log por:  [PRB] TM
# Embrulhado em begin/rescue para NUNCA partir o arranque do jogo.
# =============================================================================
begin
  # amostra um bitmap numa grelha e conta pixels opacos (alpha>0)
  def Probe.bmp_stats(bmp, label, step = 24)
    begin
      return "#{label}=nil" if bmp.nil?
      w = (bmp.width  rescue 0); h = (bmp.height rescue 0)
      return "#{label}=#{w}x#{h}(vazio)" if w <= 0 || h <= 0
      samples = 0; opaque = 0; colored = 0; fx = -1; fy = -1
      y = 0
      while y < h
        x = 0
        while x < w
          c = (bmp.get_pixel(x, y) rescue nil)
          if c
            samples += 1
            a = (c.alpha rescue 0)
            if a && a > 0
              opaque += 1
              if fx < 0; fx = x; fy = y; end
              colored += 1 if ((c.red rescue 0).to_i + (c.green rescue 0).to_i + (c.blue rescue 0).to_i) > 0
            end
          end
          x += step
        end
        y += step
      end
      pct = samples > 0 ? (opaque * 100 / samples) : 0
      "#{label}=#{w}x#{h} amostras=#{samples} opacos=#{opaque}(#{pct}%) cor=#{colored} 1o@(#{fx},#{fy})"
    rescue => e
      "#{label}=ERRO(#{e.message})"
    end
  end

  def Probe.mapdata_stats(md)
    begin
      return "map_data=nil" if md.nil?
      xs = (md.xsize rescue 0); ys = (md.ysize rescue 0); zs = (md.zsize rescue 0)
      return "map_data=#{xs}x#{ys}x#{zs}(vazio)" if xs <= 0 || ys <= 0 || zs <= 0
      nz = 0; tot = 0; maxid = 0; ge = 0
      z = 0
      while z < zs
        y = 0
        while y < ys
          x = 0
          while x < xs
            id = (md[x, y, z] rescue 0); id = 0 if id.nil?
            tot += 1
            if id != 0; nz += 1; maxid = id if id > maxid; ge += 1 if id >= 384; end
            x += 1
          end
          y += 1
        end
        z += 1
      end
      "map_data=#{xs}x#{ys}x#{zs} nonzero=#{nz}/#{tot} tiles>=384=#{ge} maxid=#{maxid}"
    rescue => e
      "map_data=ERRO(#{e.message})"
    end
  end

  def Probe.tiv(o, n); (o.instance_variable_get(n) rescue nil); end

  if Object.const_defined?(:CustomTilemap)
    class CustomTilemap

      unless method_defined?(:__p_tileset_set)
        alias_method :__p_tileset_set, :"tileset="
        def tileset=(v)
          Probe.log "TM tileset= IN  #{Probe.bmp_stats(v, 'IN')}"
          r = __p_tileset_set(v)
          Probe.log "TM tileset= OUT #{Probe.bmp_stats(Probe.tiv(self, :@tileset), '@tileset')} shouldWrap=#{Probe.fmt(Probe.tiv(self, :@shouldWrap))}"
          r
        end
      end

      unless method_defined?(:__p_mapdata_set)
        alias_method :__p_mapdata_set, :"map_data="
        def map_data=(v)
          Probe.log "TM map_data= #{Probe.mapdata_stats(v)}"
          __p_mapdata_set(v)
        end
      end

      unless method_defined?(:__p_update)
        alias_method :__p_update, :update
        def update
          $tm_upd = ($tm_upd || 0) + 1
          u = $tm_upd
          Probe.log "TM update ##{u}: tilesetChanged=#{Probe.fmt(Probe.tiv(self,:@tilesetChanged))} ox=#{Probe.fmt(Probe.tiv(self,:@ox))} oy=#{Probe.fmt(Probe.tiv(self,:@oy))} oldOx=#{Probe.fmt(Probe.tiv(self,:@oldOx))} firsttime=#{Probe.fmt(Probe.tiv(self,:@firsttime))} usedsprites=#{Probe.fmt(Probe.tiv(self,:@usedsprites))}" if u <= 4
          r = __p_update
          if (u >= 2 && u <= 6) || (u % 120 == 0)
            l0 = Probe.tiv(self, :@layer0)
            bm = l0 ? (l0.bitmap rescue nil) : nil
            Probe.log "TM watch ##{u}: #{Probe.bmp_stats(bm, 'layer0')} layer0.visible=#{l0 ? (l0.visible rescue '?') : 'nil'}"
          end
          r
        end
      end

      unless method_defined?(:__p_refresh)
        alias_method :__p_refresh, :refresh
        def refresh(autotiles = false)
          $tm_ref = ($tm_ref || 0) + 1
          Probe.log "TM refresh ##{$tm_ref}(autotiles=#{autotiles})" if $tm_ref <= 6
          __p_refresh(autotiles)
        end
      end

      unless method_defined?(:__p_rl0)
        alias_method :__p_rl0, :refreshLayer0
        def refreshLayer0(autotiles = false)
          $tm_rl0 = ($tm_rl0 || 0) + 1
          n = $tm_rl0
          deep = (n <= 3)
          if deep
            Probe.log "TM ----- refreshLayer0 ##{n} (autotiles=#{autotiles}) -----"
            Probe.log "TM   firsttime=#{Probe.fmt(Probe.tiv(self,:@firsttime))} usedsprites=#{Probe.fmt(Probe.tiv(self,:@usedsprites))} visible=#{Probe.fmt(Probe.tiv(self,:@visible))} ox=#{Probe.fmt(Probe.tiv(self,:@ox))} oy=#{Probe.fmt(Probe.tiv(self,:@oy))} oxL0=#{Probe.fmt(Probe.tiv(self,:@oxLayer0))} oyL0=#{Probe.fmt(Probe.tiv(self,:@oyLayer0))} layer0clip=#{Probe.fmt(Probe.tiv(self,:@layer0clip))}" rescue nil
            (Probe.log "TM   shown?=#{self.shown? rescue 'ERRO'}") rescue nil
            begin
              vp = Probe.tiv(self, :@viewport); rc = vp ? (vp.rect rescue nil) : nil
              Probe.log "TM   viewport.rect=#{rc ? "#{rc.width rescue '?'}x#{rc.height rescue '?'}" : 'nil'} vp.ox=#{vp ? (vp.ox rescue '?') : '?'} vp.oy=#{vp ? (vp.oy rescue '?') : '?'}"
            rescue; end
            (Probe.log "TM   #{Probe.mapdata_stats(Probe.tiv(self, :@map_data))}") rescue nil
            (Probe.log "TM   priorities=#{Probe.fmt(Probe.tiv(self,:@priorities))} terrain_tags=#{Probe.fmt(Probe.tiv(self,:@terrain_tags))}") rescue nil
            (Probe.log "TM   #{Probe.bmp_stats(Probe.tiv(self, :@tileset), '@tileset')}") rescue nil
            begin
              l0 = Probe.tiv(self, :@layer0)
              bm = l0 ? (l0.bitmap rescue nil) : nil
              sr = l0 ? (l0.src_rect rescue nil) : nil
              Probe.log "TM   layer0: visible=#{l0 ? (l0.visible rescue '?') : 'nil'} z=#{l0 ? (l0.z rescue '?') : '?'} ox=#{l0 ? (l0.ox rescue '?') : '?'} oy=#{l0 ? (l0.oy rescue '?') : '?'} src_rect=#{sr ? "#{sr.x rescue '?'},#{sr.y rescue '?'},#{sr.width rescue '?'}x#{sr.height rescue '?'}" : 'nil'}"
              Probe.log "TM   #{Probe.bmp_stats(bm, 'layer0.bmp ANTES')}"
            rescue; end
            # TESTE DE BLT: copiar 1 tile do tileset para bitmap temporario
            begin
              ts = Probe.tiv(self, :@tileset)
              if ts && (ts.width rescue 0) > 0
                tmp = Bitmap.new(32, 32)
                tmp.blt(0, 0, ts, Rect.new(0, 0, 32, 32))
                Probe.log "TM   BLT-TEST(tile 0,0): #{Probe.bmp_stats(tmp, 'tmp', 8)}"
                tmp.dispose rescue nil
              end
            rescue => e2; Probe.log "TM   BLT-TEST ERRO #{e2.message}"; end
          end
          r = nil
          begin
            r = __p_rl0(autotiles)
          rescue => e3
            Probe.log "TM   refreshLayer0 ##{n} REBENTOU: #{e3.class}: #{e3.message}"
            (e3.backtrace[0,6].each { |l| Probe.log "TM     bt: #{l}" } rescue nil)
            raise
          end
          if deep
            begin
              l0 = Probe.tiv(self, :@layer0)
              bm = l0 ? (l0.bitmap rescue nil) : nil
              Probe.log "TM   -> ret=#{Probe.fmt(r)} usedsprites_depois=#{Probe.fmt(Probe.tiv(self,:@usedsprites))}"
              Probe.log "TM   #{Probe.bmp_stats(bm, 'layer0.bmp DEPOIS')}"
            rescue; end
            Probe.log "TM ----- /refreshLayer0 ##{n} -----"
          end
          r
        end
      end

    end
    Probe.log "TM CustomTilemap instrumentado OK"
  else
    Probe.log "TM AVISO: CustomTilemap nao existe"
  end
rescue => e
  begin; Probe.log "TM setup FALHOU: #{e.class}: #{e.message}"; rescue; end
end
