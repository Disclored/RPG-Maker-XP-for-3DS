# =============================================================================
#  debug_probe.rb  —  LOGGER SUPER DETALHADO POR MAPA (frame-a-frame)
# -----------------------------------------------------------------------------
#  COMO USAR:
#    - Copia este ficheiro para:  sdmc:/mkxp/debug_probe.rb
#    - O motor carrega-o automaticamente no arranque (mrb_load_string).
#    - TUDO é escrito em sdmc:/mkxp/debug_binding.log (via MKXPDebug.log),
#      com o prefixo [MFD] que o motor acrescenta. Procura os prefixos abaixo.
#    - Para DESLIGAR: renomeia para  debug_probe.rb.off
#
#  O QUE REGISTA (sem especulação — factos do estado real do jogo):
#    [MAPDUMP]  ao ENTRAR/MUDAR de mapa: id, nome, tamanho, tileset (id+nome),
#               panorama/fog, nº de eventos.
#    [TSTAT]    dimensões REAIS do tileset carregado (ex: 256x1280) ou
#               "32x32 MAGENTA(FALHOU)" — diz logo se o tileset do mapa falhou.
#    [MEVENT]   cada evento do mapa: id, nome, posição, charset, página ativa.
#    [TF]       UMA linha por FRAME: cena, map, câmara, jogador, índice do
#               comando do interpreter em execução.
#    [TCMD]     cada comando de evento à medida que EXECUTA (code + params) —
#               o "guião" real a correr, para comparar com o esperado.
#    [TPIC]     imagens (Game_Picture) mostradas/movidas: número, nome, x/y, opac.
#    [TERR]     qualquer erro interno do próprio logger (nunca rebenta o jogo).
#
#  FPS: este logger é PESADO de propósito (escreve muito ao SD). É só p/ debug.
# =============================================================================

begin
  # NOTA: NAO mexer em $MKXP_VERBOSE. Os nossos prefixos ([MAPDUMP], [TF], [TCMD],
  # [TSTAT], [MEVENT], [TPIC], [PROBE]) NAO estao na lista de supressao do motor,
  # por isso passam sempre. (Versao anterior punha $MKXP_VERBOSE=true sem necessidade.)

  # >>> ETIQUETA DE VERSAO <<< — confirma no log que estas a correr ESTA versao.
  # Se NAO vires "[PROBE] === debug_probe V4 ===" no debug_binding.log, estas a
  # correr um ficheiro antigo (o motor le' o ficheiro errado / nao foi substituido).
  (MKXPDebug.log("[PROBE] === debug_probe V4 (logger + TSFIX disposeSpritesets, SEM GC.start) ATIVO ===") rescue nil)

  module DBGTrace
    @frame        = 0
    @last_map     = -999999
    @ts_confirmed = false
    @last_cmd_key = nil
    @last_pic_key = {}

    class << self
      # --- escrita segura no debug_binding.log -------------------------------
      def w(s)
        begin
          MKXPDebug.log(s.to_s)
        rescue
          # último recurso: não fazer nada (nunca crashar por causa de log)
        end
      end

      def dims(b)
        return "nil" if b.nil?
        begin
          return "disposed" if b.disposed?
        rescue
        end
        begin
          return "#{b.width}x#{b.height}"
        rescue
          return "?"
        end
      end

      # --- tenta obter o bitmap do tileset REALMENTE carregado no spriteset ---
      # Devolve string de dimensões (ou "?"). 32x32 = fallback magenta.
      def tileset_bitmap_dims(scene)
        begin
          spritesets = scene.instance_variable_get(:@spritesets)
          return "?(sem @spritesets)" unless spritesets
          ss = nil
          begin
            ss = spritesets[$game_map.map_id]
          rescue
          end
          ss ||= (spritesets.values[0] rescue nil)
          return "?(sem spriteset)" unless ss
          tm = ss.instance_variable_get(:@tilemap)
          return "?(sem @tilemap)" unless tm
          # CustomTilemap (PE) guarda @tileset; TilemapLoader pode variar
          tb = tm.instance_variable_get(:@tileset)
          return dims(tb)
        rescue => e
          return "?(#{e.class})"
        end
      end

      # --- bloco detalhado ao mudar de mapa ----------------------------------
      def dump_map(scene)
        begin
          m = $game_map
          mid = (m.map_id rescue -1)
          name = nil
          name = (pbGetMapNameFromId(mid) rescue nil)
          name = name.to_s if name   # sem regex (mruby pode nao ter Onigmo)
          size = "?"
          begin; size = "#{m.width}x#{m.height}"; rescue; end
          tsid = nil; tsname = nil
          begin
            md = m.instance_variable_get(:@map)
            tsid = md.tileset_id
            tsname = $data_tilesets[tsid].tileset_name
          rescue
          end
          pano = nil; fog = nil
          begin
            md = m.instance_variable_get(:@map)
            pano = (md.panorama_name rescue nil)
            fog  = (md.fog_name rescue nil)
          rescue
          end
          nev = 0
          begin; nev = m.events.size; rescue; end
          w("[MAPDUMP] ===== MAPA #{mid} '#{name}' size=#{size} " \
            "tileset_id=#{tsid} tileset='#{tsname}' " \
            "panorama='#{pano}' fog='#{fog}' eventos=#{nev}")

          # eventos do mapa
          begin
            m.events.each do |id, ev|
              evn = (ev.name rescue "?")
              ex = (ev.x rescue -1); ey = (ev.y rescue -1)
              ch = ""
              begin
                pg = ev.instance_variable_get(:@page)
                ch = (pg.graphic.character_name rescue "") if pg
              rescue
              end
              w("[MEVENT]   ev=#{id} '#{evn}' xy=(#{ex},#{ey}) char='#{ch}'")
            end
          rescue => e
            w("[MEVENT|ERR] #{e.class}: #{e.message}")
          end
        rescue => e
          w("[MAPDUMP|ERR] #{e.class}: #{e.message}")
        end
      end

      # --- interpreter atual (qual comando corre) ----------------------------
      def current_interp
        ints = []
        begin
          gi = ($game_system.map_interpreter rescue nil)
          ints << gi if gi
        rescue
        end
        begin
          mi = ($game_map.interpreter rescue nil)
          ints << mi if mi
        rescue
        end
        ints.each do |it|
          begin
            next unless it.running?
            return it
          rescue
          end
        end
        nil
      end

      def interp_index(it)
        return -1 unless it
        (it.instance_variable_get(:@index) rescue -1)
      end

      # --- chamado TODOS os frames ------------------------------------------
      def per_frame(scene)
        @frame += 1
        begin
          gm = $game_map
          mid = (gm.map_id rescue -999)

          # mudança de mapa -> bloco detalhado + (re)confirmar tileset
          if mid != @last_map
            @last_map = mid
            @ts_confirmed = false
            @last_cmd_key = nil
            @last_pic_key = {}
            w("[MAPDUMP] >>>>>> MUDANCA DE MAPA no frame #{@frame} (novo map_id=#{mid}) <<<<<<")
            dump_map(scene)
          end

          # dimensões reais do tileset carregado (tenta até conseguir)
          unless @ts_confirmed
            d = tileset_bitmap_dims(scene)
            if d && d[0, 1] != "?"
              flag = (d == "32x32") ? " <<<< 32x32 = FALLBACK MAGENTA (TILESET FALHOU)" : ""
              w("[TSTAT] map=#{mid} tileset_bitmap=#{d}#{flag}")
              @ts_confirmed = true
            end
          end

          # linha compacta por frame
          sc  = (scene.class.name rescue "?")
          cam = "-"
          begin; cam = "(#{gm.display_x},#{gm.display_y})"; rescue; end
          plr = "-"
          begin
            plr = "(#{$game_player.x},#{$game_player.y}/d#{$game_player.direction}" \
                  "#{$game_player.moving? ? '/mov' : ''})"
          rescue
          end
          it = current_interp
          idx = interp_index(it)
          w("[TF] f=#{@frame} sc=#{sc} map=#{mid} cam=#{cam} plr=#{plr} interp_idx=#{idx}")

          # comando do interpreter em execução (só quando muda)
          if it
            begin
              eid = (it.instance_variable_get(:@event_id) rescue -1)
              lst = (it.instance_variable_get(:@list) rescue nil)
              if lst && lst[idx]
                cmd = lst[idx]
                code = (cmd.code rescue -1)
                par  = (cmd.parameters rescue [])
                key  = "#{mid}:#{eid}:#{idx}:#{code}"
                if key != @last_cmd_key
                  @last_cmd_key = key
                  ps = par.inspect rescue "?"
                  ps = ps[0, 160] if ps.length > 160
                  w("[TCMD] map=#{mid} ev=#{eid} idx=#{idx} code=#{code} params=#{ps}")
                end
              end
            rescue => e
              w("[TCMD|ERR] #{e.class}: #{e.message}")
            end
          end

          # imagens (Game_Picture) — log quando mudam
          begin
            scr = ($game_screen rescue nil)
            if scr
              pics = (scr.pictures rescue nil)
              if pics
                pics.each_with_index do |pic, i|
                  next if pic.nil?
                  nm = (pic.name rescue "")
                  next if nm.nil? || nm == ""
                  px = (pic.x rescue 0); py = (pic.y rescue 0)
                  op = (pic.opacity rescue 0)
                  key = "#{nm}|#{px.to_i}|#{py.to_i}|#{op.to_i}"
                  if @last_pic_key[i] != key
                    @last_pic_key[i] = key
                    w("[TPIC] map=#{mid} pic##{i} name='#{nm}' xy=(#{px.to_i},#{py.to_i}) opac=#{op.to_i}")
                  end
                end
              end
            end
          rescue => e
            w("[TPIC|ERR] #{e.class}: #{e.message}")
          end

        rescue => e
          w("[TERR] frame #{@frame}: #{e.class}: #{e.message}")
        end
      end
    end
  end

  # --- HOOK: Scene_Map#update corre TODOS os frames em qualquer mapa ----------
  # IMPORTANTE: usamos ALIAS-CHAINING (NAO prepend). O porte aplica os seus
  # proprios patches a Scene_Map#update com `alias __orig update` DEPOIS deste
  # probe. Com PREPEND, esse alias do porte captura o nosso modulo prepended e
  # cria uma cadeia re-entrante partida -> o Scene_Map#update ORIGINAL (que corre
  # pbMapInterpreter.update = o autorun) nunca e' alcançado -> ecra preto.
  # Com alias-chaining, o porte encadeia POR CIMA do nosso (port -> nosso ->
  # original), tudo corre, e o autorun arranca normalmente.
  if defined?(Scene_Map)
    class Scene_Map
      unless method_defined?(:__dbgtrace_orig_update)
        alias_method :__dbgtrace_orig_update, :update
        def update(*a, &b)
          __dbgtrace_orig_update(*a, &b)   # corre o update REAL (autorun incluido)
          DBGTrace.per_frame(self)         # so' DEPOIS, logamos o estado
        end
      end
    end
    MKXPDebug.log("[PROBE] DBGTrace instalado em Scene_Map#update via alias-chaining (logger por-mapa frame-a-frame)")
  else
    MKXPDebug.log("[PROBE|AVISO] Scene_Map ainda nao definido — hook NAO instalado")
  end

  # ===========================================================================
  #  FIX (NAO e' so' log!) — DESTRUIR SPRITESETS ANTIGOS ANTES DE CRIAR OS NOVOS
  # ---------------------------------------------------------------------------
  #  CAUSA RAIZ (provada pelo log): os tilesets NAO sao cacheados (pbGetTileset
  #  cria um AnimatedBitmap novo). Ao mudar de mapa, o createSpritesets faz
  #  `@spritesets = {}` -> DESREFERENCIA o spriteset antigo mas NAO o destroi.
  #  O tileset antigo (ex: 'Gen4 Outside Town' 15.5MB RGBA4444) fica em malloc
  #  a' espera do GC; quando o tileset do mapa novo (ex: 'Gen4 Interior' 8.5MB)
  #  carrega, o malloc esta' cheio com o orfao -> falha -> magenta (Map3 casa).
  #
  #  PORQUE NAO GC.start: neste porte os Bitmap tem TEXTURA C++ por baixo. Um GC
  #  global finaliza objetos cujas texturas o renderer ainda usa e mexe na
  #  afinacao geracional do porte -> ECRA PRETO na cutscene (regressao observada).
  #
  #  FIX CIRURGICO: chamar disposeSpritesets() no inicio do createSpritesets. Isso
  #  destroi os spritesets antigos PELA VIA NORMAL do jogo -> Spriteset_Map#dispose
  #  faz `@tilemap.tileset.dispose` (liberta o tileset 4444, 15.5MB) ANTES de os
  #  novos serem criados. Sem GC global, so' liberta o que ja' nao e' usado.
  #  SEGURANCA: no 1o mapa (cutscene da professora) @spritesets e' nil ->
  #  disposeSpritesets faz `return if !@spritesets` -> NAO destroi nada -> a
  #  cutscene fica 100% intacta. So' a vila/casa (mapas seguintes) disparam isto.
  #  alias-chaining (o porte tambem faz wrapper de createSpritesets).
  if defined?(Scene_Map) && Scene_Map.method_defined?(:createSpritesets)
    class Scene_Map
      unless method_defined?(:__dbgfix_orig_createSpritesets)
        alias_method :__dbgfix_orig_createSpritesets, :createSpritesets
        def createSpritesets(*a, &b)
          begin
            had = (@spritesets && !@spritesets.empty?)
            if had
              MKXPDebug.log("[TSFIX] createSpritesets -> disposeSpritesets primeiro (libertar #{@spritesets.size} spriteset(s)+tileset(s) antigos)") rescue nil
              disposeSpritesets
            end
          rescue => e
            MKXPDebug.log("[TSFIX|ERR] #{e.class}: #{e.message}") rescue nil
          end
          __dbgfix_orig_createSpritesets(*a, &b)
        end
      end
    end
    MKXPDebug.log("[PROBE] TSFIX instalado: disposeSpritesets no inicio de Scene_Map#createSpritesets")
  else
    MKXPDebug.log("[PROBE|AVISO] Scene_Map#createSpritesets nao definido — TSFIX NAO instalado")
  end

rescue => e
  begin
    MKXPDebug.log("[PROBE|FATAL] #{e.class}: #{e.message}")
  rescue
  end
end

# =============================================================================
# [TMDBG] DIAGNOSTICO DO QUARTO PRETO (render do tilemap com tileset 4444 gigante)
# -----------------------------------------------------------------------------
# Tudo no caminho ESTATICO esta correto (tileset 4444 carrega, mega?=false,
# px_read le 4444, bmp_blt le qualquer y, camada 832x704 como o Map1). O preto e'
# RUNTIME. Este hook regista, 1x por mapa, o estado REAL no momento do render:
#   - se refreshLayer0 corre para o mapa do quarto (Map3)
#   - @tileset: nil / DISPOSTO / dims + native_4444?  (e' o suspeito principal)
#   - @layer0.bitmap: nil / DISPOSTO / dims
#   - @shouldWrap (deve ser false), @firsttime, @usedsprites
#   - se o refresh apanha alguma EXCECAO (que o jogo engole -> preto)
# Com isto, o proximo log diz exatamente porque o quarto fica preto.
# =============================================================================
begin
  if Object.const_defined?(:CustomTilemap)
    klass = CustomTilemap
    if klass.method_defined?(:refreshLayer0) && !klass.method_defined?(:__tmdbg_refreshLayer0)
      klass.class_eval do
        alias_method :__tmdbg_refreshLayer0, :refreshLayer0
        def refreshLayer0(autotiles = false)
          begin
            @@__tmdbg_seen ||= {}
            mid = ($game_map.map_id rescue -1)
            key = "#{mid}_#{autotiles}"
            unless @@__tmdbg_seen[key]
              @@__tmdbg_seen[key] = true
              ts = (@tileset rescue nil)
              tsdesc =
                if ts.nil? then "nil"
                elsif (ts.disposed? rescue false) then "DISPOSTO!"
                else "#{(ts.width rescue '?')}x#{(ts.height rescue '?')} 4444=#{(ts.native_4444? rescue '?')}"
                end
              lb = (@layer0.bitmap rescue nil)
              lbdesc =
                if lb.nil? then "nil"
                elsif (lb.disposed? rescue false) then "DISPOSTO!"
                else "#{(lb.width rescue '?')}x#{(lb.height rescue '?')}"
                end
              md = (@map_data rescue nil)
              mddesc = md.nil? ? "nil" : "#{(md.xsize rescue '?')}x#{(md.ysize rescue '?')}x#{(md.zsize rescue '?')}"
              MKXPDebug.log("[TMDBG] refreshLayer0 map=#{mid} auto=#{autotiles} " \
                "@tileset=#{tsdesc} @layer0.bmp=#{lbdesc} map_data=#{mddesc} " \
                "shouldWrap=#{(@shouldWrap rescue '?')} firsttime=#{(@firsttime rescue '?')} " \
                "usedsprites=#{(@usedsprites rescue '?')} diffsizes=#{(@diffsizes rescue '?')}") rescue nil
            end
          rescue => ie
            (MKXPDebug.log("[TMDBG] log-estado falhou: #{ie.class}: #{ie.message}") rescue nil)
          end
          begin
            return __tmdbg_refreshLayer0(autotiles)
          rescue => e
            (MKXPDebug.log("[TMDBG] refreshLayer0 EXCECAO map=#{($game_map.map_id rescue -1)}: #{e.class}: #{e.message}") rescue nil)
            raise
          end
        end
      end
      (MKXPDebug.log("[TMDBG] hook refreshLayer0 instalado OK") rescue nil)
    else
      (MKXPDebug.log("[TMDBG] refreshLayer0 ausente ou ja' hookado") rescue nil)
    end
  else
    (MKXPDebug.log("[TMDBG] CustomTilemap AUSENTE — render e' outro caminho") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[TMDBG] instalacao do hook falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
# [LOADMAP] TIMELINE DE CARGAS DE TILESET (responde a "o que carrega em cada momento")
# -----------------------------------------------------------------------------
# Embrulha pbGetTileset para registar UMA linha consolidada por carga:
#   - que MAPA esta' a carregar (map_id)
#   - que TILESET (nome) e dimensoes (WxH) -> ve's logo os gigantes (256x31040)
#   - tempo de DECODE em ms -> ve's o gargalo (~2800ms nos gigantes)
# Assim, filtrando "[LOADMAP]" no log, tens a sequencia exata: que mapa puxou que
# tileset e quanto custou. So' logging -- chama o pbGetTileset original, nao altera
# comportamento. (O decode real continua a ser logado pelo [BMP|PNG4444] do C++.)
# =============================================================================
begin
  if Object.respond_to?(:pbGetTileset, true) && !$__mkxp_loadmap_hooked
    $__mkxp_loadmap_hooked = true
    $__mkxp_loadmap_seq ||= 0
    class Object
      alias __loadmap_orig_pbGetTileset pbGetTileset
      def pbGetTileset(name, hue = 0)
        st = (Time.now rescue nil)
        r = __loadmap_orig_pbGetTileset(name, hue)
        begin
          dt = st ? ((Time.now - st) * 1000.0).to_i : -1
          mid = ($game_map.map_id rescue -1)
          w = (r.width rescue '?'); h = (r.height rescue '?')
          $__mkxp_loadmap_seq += 1
          giant = (h.is_a?(Integer) && h > 4096) ? " GIGANTE" : ""
          MKXPDebug.log("[LOADMAP] ##{$__mkxp_loadmap_seq} map=#{mid} tileset='#{name}' #{w}x#{h}#{giant} decode=#{dt}ms") rescue nil
        rescue; end
        r
      end
    end
    (MKXPDebug.log("[LOADMAP] timeline de cargas de tileset instalada") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[LOADMAP] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  PLAYERFIX  —  $game_player estava nil (NilClass) => jogador "congelado" (0,0)
# -----------------------------------------------------------------------------
#  RAIZ (provada pelo log:  "[Scene_Map] $game_player real=, class=NilClass"):
#  SaveData.load_new_game_values falhou e $game_player ficou NIL. O port adicionou
#  metodos a NilClass (nil.x -> 0, nil.real_x -> 0, nil.moveto -> NAO faz nada),
#  por isso o jogador FINGE estar sempre em (0,0):
#     - os "Transfer Player" (ex.: intro -> quarto em 22,8) NAO o movem
#     - os "Set Move Route" (caminhada da intro) NAO funcionam
#     - a camara fica presa em (0,0)  (display_x = real_x - centro = 0 - centro)
#  => o QUARTO mostra o floor 0 (NPCs) em vez do floor 1, e a intro nao "anda".
#
#  O port JA' corrige $game_switches/$game_variables/$game_temp/$game_system da
#  mesma maneira em Scene_Map#main, mas ESQUECEU-SE do $game_player. Aqui criamos
#  um Game_Player REAL antes de o mapa arrancar. Com um player real, o moveto
#  volta a funcionar e TUDO (posicao + camara + move-routes) fica correcto.
#  (Confirma no log: deves ver "[PLAYERFIX] ... criado Game_Player REAL" e, a
#   seguir, o [TF] deixa de mostrar plr=(0,0) — passa a mostrar a posicao real.)
# =============================================================================
begin
  if defined?(Scene_Map) && Scene_Map.method_defined?(:main) && !$__playerfix_installed
    $__playerfix_installed = true
    class Scene_Map
      alias_method :__playerfix_orig_main, :main
      def main
        begin
          real = ($game_player && $game_player.is_a?(Game_Player)) rescue false
          unless real
            if Object.const_defined?(:Game_Player)
              $game_player = Game_Player.new
              sx = 0; sy = 0
              begin
                sx = $data_system.start_x.to_i if $data_system.respond_to?(:start_x)
                sy = $data_system.start_y.to_i if $data_system.respond_to?(:start_y)
              rescue; end
              ($game_player.moveto(sx, sy) rescue nil)
              (MKXPDebug.log("[PLAYERFIX] $game_player era nil/stub -> criado Game_Player REAL @ (#{sx},#{sy}). Transfers/move-routes/camara passam a funcionar.") rescue nil)
            end
          else
            (MKXPDebug.log("[PLAYERFIX] $game_player ja' era real (#{$game_player.class}) -- sem accao") rescue nil)
          end
        rescue => e
          (MKXPDebug.log("[PLAYERFIX] falhou: #{e.class}: #{e.message}") rescue nil)
        end
        __playerfix_orig_main
      end
    end
    (MKXPDebug.log("[PLAYERFIX] hook Scene_Map#main instalado (cria $game_player real se faltar)") rescue nil)
  else
    (MKXPDebug.log("[PLAYERFIX|AVISO] Scene_Map#main indisponivel ou ja' instalado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[PLAYERFIX] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  TMGUARD  —  CustomTilemap#refresh rebenta com @map_data nil durante transfer
# -----------------------------------------------------------------------------
#  CAUSA (provada pelo log, stack: clamp <- refresh):
#    all_core.rb:15117  minX = minX.clamp(0, xsize - 1)
#  Durante um transfer o tilemap novo e' criado com @map_data = nil. Como o port
#  pos metodos no NilClass, nil.xsize devolve 0 -> clamp(0, 0-1) = clamp(0,-1)
#  -> "min argument must be smaller than max argument" -> Spriteset_Map#update
#  rebenta -> ecra preto. Antes (player nil) os efeitos nunca chamavam este
#  refresh; com o player REAL (correcto), as pegadas/efeitos chamam-no enquanto
#  o mapa ainda carrega. Saltamos o refresh enquanto @map_data nao esta pronto.
# =============================================================================
begin
  if defined?(CustomTilemap) && CustomTilemap.method_defined?(:refresh) && !$__tmguard_installed
    $__tmguard_installed = true
    class CustomTilemap
      alias_method :__tmguard_orig_refresh, :refresh
      def refresh(*a)
        md = (@map_data rescue nil)
        if md.nil? || (md.xsize rescue 0).to_i <= 0 || (md.ysize rescue 0).to_i <= 0
          unless $__tmguard_logged
            (MKXPDebug.log("[TMGUARD] refresh saltado: @map_data nil/vazio (mapa a carregar) -- evita clamp(0,-1)") rescue nil)
            $__tmguard_logged = true
          end
          return
        end
        __tmguard_orig_refresh(*a)
      end
    end
    (MKXPDebug.log("[TMGUARD] CustomTilemap#refresh protegido (salta se @map_data nil/vazio)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[TMGUARD] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  FXGUARD  —  efeitos EBDX (pegadas/bolhas/sombras/pkmn-a-seguir) rebentam
# -----------------------------------------------------------------------------
#  CAUSA (provada pelo log): com o player REAL, a cadeia Sprite_Character#update
#  (footsteps_update / water_buble_update / sh_update / __followingpkmn__update)
#  rebenta durante createSpritesets:
#    - ArgumentError: min argument must be smaller than max argument   (clamp)
#    - ArgumentError: cannot set instance variable                    (mruby)
#  Isto MATA o createSpritesets -> spriteset sem sprites -> ecra preto.
#  IMPORTANTE: na cadeia de alias, o update BASE (desenhar o personagem) corre
#  PRIMEIRO; os efeitos correm A SEGUIR. Por isso apanhar o erro aqui NAO perde
#  o desenho do personagem -- so' o efeito visual (que ja' estava partido) e'
#  ignorado. O jogo deixa de ficar preto e fica jogavel.
# =============================================================================
begin
  if defined?(Sprite_Character) && Sprite_Character.method_defined?(:update) && !$__fxguard_installed
    $__fxguard_installed = true
    class Sprite_Character
      alias_method :__fxguard_orig_update, :update
      def update
        __fxguard_orig_update
      rescue => e
        unless $__fxguard_logged
          (MKXPDebug.log("[FXGUARD] efeito EBDX em Sprite_Character#update rebentou: #{e.class}: #{e.message} -- ignorado (personagem na' mesmo desenha)") rescue nil)
          $__fxguard_logged = true
        end
        nil
      end
    end
    (MKXPDebug.log("[FXGUARD] Sprite_Character#update protegido (efeitos EBDX nao matam o spriteset)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[FXGUARD] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  CACHEFIX  —  RPG::Cache.clear era no-op => tilesets gigantes acumulam => OOM
# -----------------------------------------------------------------------------
#  RAIZ (provada pelo log e pelo codigo):
#   * A module Cache do jogo NAO define `clear`; no port cai num stub no-op.
#   * transfer_player chama RPG::Cache.clear (apos disposeSpritesets) MAS, sendo
#     no-op, NAO liberta nada -> cada mapa deixa o seu tileset GIGANTE na cache:
#        mapa 43 'Gardenia Town'  -> 256x31040 (~15MB)
#        mapa 3  'AA's House'     -> 256x29632 (~14MB)
#   * Ao entrar no quarto com o tileset do 43 ainda em memoria:
#        [TEX|MEM] bytes_tex=25MB | linear_livre=593KB  -> OOM -> loop de
#        [BMP|GC-OOM] -> TRAVA a carregar o mapa 3.
#  Antes (player nil) o jogador nao andava, menos mapas carregavam, e cabia.
#  Agora (player real, correcto) anda no 43, carrega vizinhos, e estoura no 3.
#
#  FIX: definir um RPG::Cache.clear REAL que LIBERTA os bitmaps GIGANTES da cache
#  (so' altura>=4096 -> tilesets; mantem sprites/icones pequenos em cache para
#  nao perder performance). Corre no transfer, ANTES de createSpritesets, por
#  isso liberta o tileset do mapa anterior mesmo a tempo de caber o novo.
#  Seguro: nesse ponto os sprites antigos ja' foram dispostos -> nada vivo
#  referencia estes tilesets; o mapa novo recarrega o seu.
# =============================================================================
begin
  if defined?(RPG) && defined?(RPG::Cache) && !$__cachefix_installed
    $__cachefix_installed = true
    module RPG
      module Cache
        def self.clear
          c = (@cache rescue nil)
          return unless c.is_a?(Hash)
          n = 0; freedkb = 0
          c.each do |k, b|
            begin
              next if b.nil? || (b.disposed? rescue true)
              next if (b.never_dispose rescue false)         # respeitar retidos (sprite do player, etc.)
              h = (b.height rescue 0).to_i
              w = (b.width rescue 0).to_i
              next if h < 4096                                 # so' tilesets GIGANTES
              kb = (w * h * 4) / 1024
              12.times { break if (b.disposed? rescue true); b.dispose }   # forcar refcount->0
              if (b.disposed? rescue false); n += 1; freedkb += kb; end
            rescue; end
          end
          begin; c.reject! { |k, b| b.nil? || (b.disposed? rescue true) }; rescue; end
          (GC.start rescue nil) if n > 0
          (MKXPDebug.log("[CACHEFIX] clear libertou #{n} tilesets gigantes (~#{freedkb}KB) -- anti-OOM") rescue nil) if n > 0
          nil
        end
      end
    end
    (MKXPDebug.log("[CACHEFIX] RPG::Cache.clear REAL instalado (liberta tilesets gigantes no transfer; estava no-op)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[CACHEFIX] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  PROBE-CONFIRM  —  confirmacao de install LEGIVEL no log (corre no 1o frame)
# -----------------------------------------------------------------------------
#  PORQUE: este ficheiro e' carregado pelo motor ANTES de o modulo MKXPDebug
#  existir (o MKXPDebug e' definido nos post-scripts permanentes, que correm
#  DEPOIS). Por isso TODAS as chamadas MKXPDebug.log feitas durante o LOAD deste
#  probe (incl. as mensagens "[PLAYERFIX] instalado", "[TMGUARD] protegido", a
#  etiqueta de versao "[PROBE] V4", etc.) caem no `rescue nil` e NUNCA aparecem
#  no log -- AINDA QUE os hooks tenham sido instalados (alias_method nao precisa
#  do MKXPDebug). Resultado: o log parecia nao ter os fixes, mas eles ESTAO la'.
#  Este bloco corre no PRIMEIRO frame de Scene_Map (altura em que o MKXPDebug ja'
#  existe) e regista, UMA vez, quais dos 4 fixes ficaram instalados -- dando-te
#  prova real no debug_binding.log. Se vires esta linha, estas a correr ESTE
#  probe (o atualizado).
# =============================================================================
begin
  if defined?(Scene_Map) && Scene_Map.method_defined?(:update) && !$__probe_confirm_installed
    $__probe_confirm_installed = true
    class Scene_Map
      alias_method :__probe_confirm_orig_update, :update
      def update(*a)
        unless $__probe_confirmed
          $__probe_confirmed = true
          begin
            MKXPDebug.log("[PROBE-CONFIRM] ===== debug_probe ATUALIZADO a correr (confirmado no 1o frame) =====")
            MKXPDebug.log("[PROBE-CONFIRM] PLAYERFIX=#{!!$__playerfix_installed} " \
                          "TMGUARD=#{!!$__tmguard_installed} " \
                          "FXGUARD=#{!!$__fxguard_installed} " \
                          "CACHEFIX=#{!!$__cachefix_installed}")
            MKXPDebug.log("[PROBE-CONFIRM] (se algum estiver false, o hook respetivo nao instalou -- avisa-me)")
          rescue => e
            (MKXPDebug.log("[PROBE-CONFIRM] log falhou: #{e.class}: #{e.message}") rescue nil)
          end
        end
        __probe_confirm_orig_update(*a)
      end
    end
  end
rescue => e
  (MKXPDebug.log("[PROBE-CONFIRM] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
end
