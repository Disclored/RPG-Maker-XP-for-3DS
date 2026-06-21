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
    # censo de sprites de personagem (diagnostico de duplicacao no quarto)
    @census_done  = false
    @map_change_f = 0

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
            @census_done  = false
            @map_change_f = @frame
            w("[MAPDUMP] >>>>>> MUDANCA DE MAPA no frame #{@frame} (novo map_id=#{mid}) <<<<<<")
            dump_map(scene)
          end

          # ── CENSO DE SPRITES (1x por mapa, ~30 frames apos entrar) ──────────
          # Enumera os Sprite_Character REALMENTE criados no spriteset e regista
          # nome do grafico + tile + ecra + visivel + bitmap + src_rect. Se o
          # mesmo character_name aparecer N vezes na mesma posicao -> DUPLICACAO.
          # 100% leitura (zero alteracao de estado) e tudo dentro de rescue.
          if !@census_done && (@frame - @map_change_f) > 30
            @census_done = true
            begin
              ss = nil
              spr = (scene.instance_variable_get(:@spritesets) rescue nil)
              if spr
                ss = (spr[mid] rescue nil) || (spr.values[0] rescue nil)
              end
              cs = ss ? (ss.instance_variable_get(:@character_sprites) rescue nil) : nil
              if cs
                w("[SPRITECENSUS] map=#{mid} total_character_sprites=#{cs.size}")
                counts = Hash.new(0)
                cs.each_with_index do |sp, i|
                  next if sp.nil?
                  ch  = (sp.instance_variable_get(:@character) rescue nil)
                  nm  = ch ? (ch.character_name rescue '?') : 'nil'
                  cx  = ch ? (ch.x rescue '?') : '?'
                  cy  = ch ? (ch.y rescue '?') : '?'
                  ev  = if ch.equal?($game_player) then 'PLAYER'
                        elsif ch.respond_to?(:id) then (ch.id rescue '?')
                        else '?' end
                  vis = (sp.visible rescue '?')
                  sx  = (sp.x rescue '?'); sy = (sp.y rescue '?')
                  bw  = ((sp.bitmap && !sp.bitmap.disposed?) ? sp.bitmap.width  : 0) rescue 0
                  bh  = ((sp.bitmap && !sp.bitmap.disposed?) ? sp.bitmap.height : 0) rescue 0
                  sr  = (sp.src_rect rescue nil)
                  srs = sr ? "[#{sr.x},#{sr.y},#{sr.width},#{sr.height}]" : '?'
                  counts[nm.to_s] += 1
                  w("[SPRITECENSUS]  ##{i} char='#{nm}' ev=#{ev} tile=(#{cx},#{cy}) scr=(#{sx},#{sy}) vis=#{vis} bmp=#{bw}x#{bh} src=#{srs}")
                end
                dups = counts.select { |k, v| v > 1 && k != '' && k != 'nil' }
                w("[SPRITECENSUS] map=#{mid} char_name repetido (>1x) = #{dups.inspect}  <-- se nao vazio, ha DUPLICACAO")
              else
                w("[SPRITECENSUS] map=#{mid} sem @character_sprites acessivel no spriteset")
              end
            rescue => e
              w("[SPRITECENSUS] EXCECAO: #{e.class}: #{e.message}")
            end
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
#  TRAINERIDFIX  —  Trainer Card vazio porque $Trainer fica nil (Player.new falha)
# -----------------------------------------------------------------------------
#  RAIZ (provada pelo log: "[NAMEIN] Player.new falhou (tt=:POKEMONTRAINER_Red)
#  - $Trainer continua nil" + cartao vazio):
#    Trainer#initialize (all_core 86353) faz:
#       @id = rand(2 ** 16) | rand(2 ** 16) << 16
#    Maximo = 65535 | (65535 << 16) = 4294967295 (2^32-1). O mruby do 3DS e'
#    MRB_INT32 (ARM 32-bit -> default do mrbconf.h), cujo mrb_int e' int32 COM
#    SINAL (max 2147483647). O `<< 16` ESTOURA -> o mruby promove a Float ->
#    `Integer | Float` rebenta (TypeError) -> Trainer#initialize falha ->
#    Player.new falha -> SaveData.new_game_value falha -> $Trainer fica NIL.
#    O cartao avalia $Trainer.public_ID (= @id & 0xFFFF); com $Trainer nil
#    NENHUM campo e' desenhado (so' o template vermelho). A excecao e' engolida
#    pelos `rescue` (new_game_value + NAMEIN), por isso nao aparece no log.
#
#  CORRECAO: redefinir Trainer#initialize com um @id que CABE em int32 (ID
#  secreto a 15 bits): rand(2**16) | (rand(2**15) << 16). Max = 65535 |
#  (32767<<16) = 2147483647 = INT32_MAX exacto. public_ID mantem 16 bits;
#  secret_ID (= @id >> 16) fica com 15 bits (suficiente). Assim o new_game_value
#  do JOGO cria o $Trainer correctamente e o cartao preenche. Replica fielmente
#  as outras 4 linhas do metodo original.
# =============================================================================
begin
  if defined?(Trainer) && Trainer.method_defined?(:initialize) && !$__traineridfix_installed
    $__traineridfix_installed = true
    class Trainer
      def initialize(name, trainer_type)
        @trainer_type = GameData::TrainerType.get(trainer_type).id
        @name         = name
        @id           = rand(2 ** 16) | (rand(2 ** 15) << 16)   # int32-safe
        @language     = (pbGetLanguage rescue 2)
        @party        = []
      end
    end
    (MKXPDebug.log("[TRAINERIDFIX] Trainer#initialize redefinido com @id seguro p/ int32 ($Trainer passa a criar-se)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[TRAINERIDFIX] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  BOOTSTUBS  —  metodos em falta que rebentaram scripts no boot. O boot ja'
#  passou (estes correm depois), mas adiciona-los evita falhas posteriores se
#  codigo de runtime os chamar. So' os adiciona se NAO existirem.
# =============================================================================
begin
  unless Numeric.method_defined?(:round)
    class Numeric
      def round(ndigits = 0)
        return self if ndigits > 0   # Float#round/Integer#round tratam o caso real
        self.to_i
      end
    end
    (MKXPDebug.log("[BOOTSTUBS] Numeric#round (fallback) adicionado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[BOOTSTUBS] Numeric#round falhou: #{e.class}: #{e.message}") rescue nil)
end
begin
  unless (Dir.respond_to?(:delete) rescue true)
    def Dir.delete(*a); false; end
    (MKXPDebug.log("[BOOTSTUBS] Dir.delete (stub no-op) adicionado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[BOOTSTUBS] Dir.delete falhou: #{e.class}: #{e.message}") rescue nil)
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
            MKXPDebug.log("[PROBE-CONFIRM] TRAINERIDFIX=#{!!$__traineridfix_installed} " \
                          "BMPFIX=#{!!$__bmpfix_installed} " \
                          "$Trainer=#{($Trainer ? $Trainer.class : 'nil') rescue 'erro'} " \
                          "public_ID=#{(($Trainer && $Trainer.respond_to?(:public_ID)) ? ($Trainer.public_ID rescue '?') : '-')}")
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

# =============================================================================
#  BMPFIX  —  AnimatedBitmap / PngAnimatedBitmap SEM regex (corrige crash loadbg)
# -----------------------------------------------------------------------------
#  SINTOMA (log): no ecra de carregamento (pbStartLoadScreen), ao por o fundo:
#     [STRREGEX] split(Integer)  ->  split ERRO TypeError: expected String
#     [SCENE-CRASH] RuntimeError: Invalid frame count in Graphics/Pictures/loadbg_4
#
#  CAUSA PROVADA: neste build um LITERAL regex (/.../), em certos casos pontuais,
#  e' avaliado como um Integer. Em AnimatedBitmap#initialize (all_core ~19807):
#       split_file = file.split(/[\\/]/)   # /.../ -> Integer  =>  split(Integer)
#  o split(Integer) rebenta e o rescue do nosso String#split devolve [self], logo
#  filename passa a ser o caminho COMPLETO "Graphics/Pictures/loadbg_4". A seguir:
#       if filename[/^\[(\d+)...\]/]        # /.../ -> Integer  =>  filename.slice(Int)
#  devolve um caracter (truthy) -> entra ERRADAMENTE no ramo PngAnimatedBitmap,
#  onde $1 fica vazio -> numFrames=0 -> raise "Invalid frame count in <path>".
#  (a regex global NAO esta' partida -- o jogo arranca com milhares de regexes --
#   e' um caso intermitente; por isso o loader nao pode depender de regex.)
#
#  FIX (so' Ruby, SEM recompilar): redefinir os 2 initialize para (a) separar
#  dir/ficheiro e (b) detectar o prefixo [N]/[N,M] APENAS com operacoes de String
#  (index/slice), nunca regex. loadbg_4 (sem parenteses) -> GifBitmap (seguro,
#  com fallback). Ficheiros [N]xxx reais continuam a animar, mas NUNCA rebentam
#  (caem para 1 frame se a largura nao for divisivel/algo falhar).
# =============================================================================
begin
  module MKXPBmpFix
    # "a/b/c" -> ["a/b/", "c"]  (ultimo "/" ou "\") -- sem regex
    def self.split_path(file)
      s = file.to_s
      return [s, ""] if s.length > 0 && (s[-1, 1] == "/" || s[-1, 1] == "\\")
      last = -1
      i = 0
      s.each_char do |c|
        last = i if (c == "/" || c == "\\")
        i += 1
      end
      if last >= 0
        [(s[0..last] || ""), (s[(last + 1)..-1] || "")]
      else
        ["", s]
      end
    end

    # [numFrames, delay] se base comeca por [N] ou [N,M] (N,M digitos); senao nil
    def self.parse_frames(base)
      return nil if base.nil?
      b = base.to_s
      return nil if b.length < 3 || b[0, 1] != "["
      close = b.index("]")
      return nil if close.nil? || close < 2
      inner = b[1...close]
      return nil if inner.nil? || inner.length == 0
      comma = inner.index(",")
      nstr  = comma ? inner[0...comma] : inner
      mstr  = comma ? (inner[(comma + 1)..-1] || "") : nil
      return nil if nstr.nil? || nstr.length == 0
      nstr.each_char { |c| return nil unless c >= "0" && c <= "9" }
      n = nstr.to_i
      return nil if n <= 0
      m = 10
      if mstr && mstr.length > 0
        okm = true
        mstr.each_char { |c| okm = false unless c >= "0" && c <= "9" }
        m = mstr.to_i if okm
      end
      m = 10 if m <= 0
      [n, m]
    end
  end

  # -- AnimatedBitmap#initialize sem regex --------------------------------------
  if Object.const_defined?(:AnimatedBitmap)
    class AnimatedBitmap
      def initialize(file, hue = 0)
        raise "Filename is nil (missing graphic)." if file.nil?
        dir, base = MKXPBmpFix.split_path(file)
        fr = MKXPBmpFix.parse_frames(base)
        if fr && Object.const_defined?(:PngAnimatedBitmap)
          @bitmap = PngAnimatedBitmap.new(dir, base, hue)
        elsif Object.const_defined?(:GifBitmap)
          @bitmap = GifBitmap.new(dir, base, hue)
        else
          begin
            @bitmap = PngAnimatedBitmap.new(dir, base, hue)
          rescue
            @bitmap = (Bitmap.new("#{dir}#{base}") rescue Bitmap.new(1, 1))
          end
        end
      end
    end
    (MKXPDebug.log("[BMPFIX] AnimatedBitmap#initialize SEM regex instalado") rescue nil)
  end

  # -- PngAnimatedBitmap#initialize sem regex + NUNCA rebenta -------------------
  if Object.const_defined?(:PngAnimatedBitmap)
    class PngAnimatedBitmap
      def initialize(dir, filename, hue = 0)
        @frames       = []
        @currentFrame = 0
        @framecount   = 0
        @frameDelay   = 10
        panorama = nil
        begin
          panorama = RPG::Cache.load_bitmap(dir, filename, hue)
        rescue
          panorama = nil
        end
        if panorama.nil?
          begin
            panorama = Bitmap.new("#{dir}#{filename}")
          rescue
            panorama = (BitmapWrapper.new(32, 32) rescue Bitmap.new(32, 32))
          end
        end
        fr = MKXPBmpFix.parse_frames(filename)
        if fr
          numFrames, delay = fr
          @frameDelay = delay
          ok = (numFrames > 0 && panorama && (panorama.width rescue 0) > 0 &&
                (panorama.width % numFrames == 0))
          if ok
            subWidth = panorama.width / numFrames
            for i in 0...numFrames
              subBitmap = BitmapWrapper.new(subWidth, panorama.height)
              subBitmap.blt(0, 0, panorama, Rect.new(subWidth * i, 0, subWidth, panorama.height))
              @frames.push(subBitmap)
            end
            panorama.dispose
          else
            @frames = [panorama]   # invalido -> 1 frame, SEM raise
          end
        else
          @frames = [panorama]     # nao-animado -> 1 frame
        end
      end
    end
    (MKXPDebug.log("[BMPFIX] PngAnimatedBitmap#initialize SEM regex (sem raise) instalado") rescue nil)
  end

  $__bmpfix_installed = true
rescue => e
  (MKXPDebug.log("[BMPFIX] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  TSCACHE — cache do TILESET PRINCIPAL por nome  (corrige o FREEZE em transicoes)
# -----------------------------------------------------------------------------
#  SINTOMA: ao cruzar mapas LIGADOS (log: MapFactory.maps=Array(2)), Spriteset_Map
#  e' recriado e chama pbGetTileset (all_core 12374), que faz
#  BitmapWrapper.new("Graphics/Tilesets/<nome>") = DECODE de 30MB (Gen4 Outside
#  Town 256x31040, ~2.6s) SEM cache. Cada transicao re-descodifica os tilesets
#  gigantes (Town+Route ~5s seguidos) -> parece congelado.
#  CAUSA: pbGetTileBitmap tem cache, mas e' SO' p/ graficos de EVENTOS (moveis);
#  o tileset do @tilemap (via pbGetTileset, post-scripts ~4824) nao tinha cache.
#  FIX: cachear o BitmapWrapper por nome com never_dispose=true. SEGURO porque
#  BitmapWrapper#dispose so' liberta de facto se @refcount<=0 E !@never_dispose
#  (binding_3ds ~4774): um @tilemap vivo mantem refcount>0, logo a eviccao
#  (never_dispose=false + dispose) NUNCA liberta um tileset que um tilemap ainda
#  usa -- so' o marca libertavel quando o ultimo tilemap o largar.
#  Budget=2 = par de mapas ligados (~30MB, pico JA' provado estavel a t=108s).
#  Re-entrar reusa o decode -> SEM re-decode, SEM freeze.
#  (DEFERIDO p/ o 1o frame de Scene_Map: corre DEPOIS dos post-scripts que
#   definem pbGetTileset a t=3.98s; ao nivel-de-topo seria sobreposto.)
#  Spriteset_Map chama pbGetTileset SEM receptor -> resolve p/ Object#pbGetTileset
#  (Object esta' antes de Kernel na cadeia), por isso so' redefinimos esse.
#
#  FPSPROBE — medir onde vao os ~42ms/frame perdidos (update=55ms mas a soma dos
#  metodos instrumentados ~13ms). Cronometra Scene_Map#update TOTAL partido em
#  updateMaps / updateSpritesets / resto + Events.onMapUpdate.trigger (suspeito:
#  hooks por-frame de plugins -- sombras/seguidor p/ 54 eventos). O proximo log
#  diz EXATAMENTE onde estao os 42ms.
# =============================================================================
begin
  if defined?(Scene_Map) && Scene_Map.method_defined?(:update) && !$__fixhook_installed
    $__fixhook_installed = true
    $__fh_n = 0; $__fh_tot = 0.0; $__fh_um = 0.0; $__fh_us = 0.0; $__fh_omu = 0.0

    # --- timing de updateMaps / updateSpritesets (comportamento inalterado) ----
    if Scene_Map.method_defined?(:updateMaps) && Scene_Map.method_defined?(:updateSpritesets) &&
       !Scene_Map.method_defined?(:__fh_orig_updateMaps)
      class Scene_Map
        alias_method :__fh_orig_updateMaps, :updateMaps
        def updateMaps(*a)
          __t = (__mkxp_ms rescue 0.0); __r = __fh_orig_updateMaps(*a)
          $__fh_um += ((__mkxp_ms rescue 0.0) - __t); __r
        end
        alias_method :__fh_orig_updateSpritesets, :updateSpritesets
        def updateSpritesets(*a)
          __t = (__mkxp_ms rescue 0.0); __r = __fh_orig_updateSpritesets(*a)
          $__fh_us += ((__mkxp_ms rescue 0.0) - __t); __r
        end
      end
    end

    # --- timing de Events.onMapUpdate.trigger (hooks de plugins por-frame) ------
    begin
      _omu = (Events.onMapUpdate rescue nil)
      if _omu && !(class << _omu; method_defined?(:__fh_orig_trigger); end)
        class << _omu
          alias_method :__fh_orig_trigger, :trigger
          def trigger(*a)
            __t = (__mkxp_ms rescue 0.0); __r = __fh_orig_trigger(*a)
            $__fh_omu += ((__mkxp_ms rescue 0.0) - __t); __r
          end
        end
      end
    rescue; end

    # --- hook principal: instala TSCACHE no 1o frame; cronometra update total --
    class Scene_Map
      alias_method :__fh_orig_update, :update
      def update(*a)
        unless $__tscache_installed
          $__tscache_installed = true
          $__tscache     = {}     # nome -> BitmapWrapper
          $__tscache_lru = []     # nomes, mais-recente ao fim
          $__tscache_max = 2
          begin
            Object.class_eval do
              def pbGetTileset(name, hue = 0)
                nm = (name.to_s rescue "")
                if nm.nil? || nm == ""
                  return (BitmapWrapper.new(32, 32) rescue Bitmap.new(32, 32))
                end
                c = $__tscache[nm]
                if c && !(c.disposed? rescue true)
                  $__tscache_lru.delete(nm); $__tscache_lru.push(nm)
                  (MKXPDebug.log("[TSCACHE] HIT '#{nm}' (sem re-decode)") rescue nil)
                  return c
                end
                # MISS -- ANTES de descodificar (~15MB), libertar tilesets de mapas
                # que JA NAO estao activos, p/ nao haver pico de RAM (30MB velho +
                # 15MB novo). So' se expulsa um tileset que NENHUM mapa activo use
                # (senao o tilemap vivo do vizinho guarda o ponteiro cru -> UAF).
                begin
                  active = {}
                  ms = ($MapFactory.maps rescue nil)
                  ms.each { |gm| (n = (gm.tileset_name rescue nil)) && (active[n.to_s] = true) } if ms
                  while $__tscache_lru.length >= $__tscache_max
                    victim = nil
                    $__tscache_lru.each { |k| (victim = k; break) unless active[k] }
                    break if victim.nil?   # todos cacheados em uso -> NAO expulsar (seguro)
                    $__tscache_lru.delete(victim)
                    ob = $__tscache.delete(victim)
                    begin
                      (ob.never_dispose = false) rescue nil
                      ob.dispose   # so' liberta de facto se refcount<=0 (ninguem vivo o usa)
                      (MKXPDebug.log("[TSCACHE] EVICT '#{victim}' (nao-activo, antes do decode)") rescue nil)
                    rescue; end
                  end
                rescue; end
                bm = nil
                begin
                  bm = BitmapWrapper.new("Graphics/Tilesets/#{nm}")
                rescue => e
                  (MKXPDebug.log("[TSCACHE] decode '#{nm}' FALHOU: #{e.class}: #{e.message}") rescue nil)
                  return (BitmapWrapper.new(32, 32) rescue Bitmap.new(32, 32))
                end
                (bm.never_dispose = true) rescue nil
                $__tscache[nm] = bm
                $__tscache_lru.delete(nm); $__tscache_lru.push(nm)
                (MKXPDebug.log("[TSCACHE] MISS '#{nm}' cacheado (#{$__tscache_lru.length}/#{$__tscache_max})") rescue nil)
                bm
              end
            end
            (MKXPDebug.log("[TSCACHE] pbGetTileset com cache instalado (max #{$__tscache_max}, never_dispose+refcount)") rescue nil)
          rescue => e
            (MKXPDebug.log("[TSCACHE] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
          end
        end

        __t0 = (__mkxp_ms rescue 0.0)
        __r  = __fh_orig_update(*a)
        $__fh_tot += ((__mkxp_ms rescue 0.0) - __t0)
        $__fh_n += 1
        if $__fh_n % 120 == 0
          tot = $__fh_tot / 120.0; um = $__fh_um / 120.0
          us  = $__fh_us / 120.0;  omu = $__fh_omu / 120.0
          rest = tot - um - us
          (MKXPDebug.log("[FPSPROBE] update=#{(tot*10).to_i/10.0}ms | updateMaps=#{(um*10).to_i/10.0} updateSpritesets=#{(us*10).to_i/10.0} (onMapUpdate=#{(omu*10).to_i/10.0}) resto=#{(rest*10).to_i/10.0} | RUBYPROF: game_map~4 spriteset~9") rescue nil)
          $__fh_tot = 0.0; $__fh_um = 0.0; $__fh_us = 0.0; $__fh_omu = 0.0
        end
        __r
      end
    end
    (MKXPDebug.log("[FPSPROBE] instalado: update total + updateMaps/updateSpritesets/onMapUpdate") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[FIXHOOK] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  THROTTLE — spriteset de mapa VIZINHO (ligado) so' actualiza em frames pares
# -----------------------------------------------------------------------------
#  FPSPROBE provou: updateSpritesets=21.4ms = a parte mais cara do frame, e nao e'
#  o onMapUpdate (0.1ms). Em mapas LIGADOS, updateSpritesets faz
#  `for i in keys: @spritesets[i].update` p/ TODOS os mapas -> 2 spritesets, cada
#  ~9ms de sprites de personagem. O mapa vizinho esta' quase sempre FORA do ecra;
#  actualiza-lo todos os frames e' desperdicio. Saltar o vizinho em frames impares
#  poupa ~5ms/frame (30->~37 FPS) e e' invisivel enquanto o vizinho esta' fora do
#  ecra. O mapa ACTUAL ($game_map.map_id) NUNCA e' saltado.
#  Se notares jitter/atraso na COSTURA entre mapas, diz-me que afino (ou pomos so'
#  off-screen). Para desligar: apaga este bloco do debug_probe.rb.
# =============================================================================
begin
  if defined?(Spriteset_Map) && Spriteset_Map.method_defined?(:update) && !$__spr_throttle
    $__spr_throttle = true
    class Spriteset_Map
      alias_method :__thr_orig_update, :update
      def update(*a)
        begin
          cur  = ($game_map.map_id rescue nil)
          mine = (@map.map_id rescue nil)
          if mine && cur && mine != cur && (($__fh_n || 0) % 2 != 0)
            return nil   # vizinho em frame impar -> saltar
          end
        rescue; end
        __thr_orig_update(*a)
      end
    end
    (MKXPDebug.log("[THROTTLE] Spriteset_Map vizinho a cada 2 frames (mapa actual sempre)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[THROTTLE] falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  DIAG-CARD — estado do $Trainer no momento em que a Trainer Card abre
# -----------------------------------------------------------------------------
#  O log prova que $Trainer foi CRIADO e o nome "AA" aplicado, e money/public_ID
#  estao definidos no Player. Logo o cartao vazio e' RENDER ou o $Trainer mudou
#  ate' la'. Este hook regista nome/money/ID/badges QUANDO o cartao desenha. Se
#  vierem validos -> e' render (proximo passo: a fonte/overlay). Se vierem nil ->
#  o $Trainer foi reposto. So' logging, nao altera nada.
# =============================================================================
begin
  if defined?(PokemonTrainerCard_Scene) && PokemonTrainerCard_Scene.method_defined?(:pbRedrawSide) && !$__diagcard
    $__diagcard = true
    class PokemonTrainerCard_Scene
      alias_method :__dc_pbRedrawSide, :pbRedrawSide
      def pbRedrawSide(*a)
        begin
          t = ((defined?($Trainer) && $Trainer) || (defined?($player) && $player) || nil)
          (MKXPDebug.log("[DIAG-CARD] pbRedrawSide: trainer=#{t ? t.class : 'nil'} name=#{(t.name rescue '?').inspect} money=#{(t.money rescue '?')} money_fmt=#{(t.money.to_s_formatted rescue 'ERRO-to_s_formatted')} public_ID=#{(t.public_ID rescue 'ERRO')} badges=#{(t.badge_count rescue 'ERRO')}") rescue nil)
          # testar os acessos EXACTOS do pbDrawTrainerCardFront base (qualquer um que falhe = cartao em branco)
          pg = (defined?($PokemonGlobal) && $PokemonGlobal) || nil
          (MKXPDebug.log("[DIAG-CARD]   $PokemonGlobal=#{pg ? 'ok' : 'NIL <-- causa provavel'} startTime=#{(pg.startTime rescue 'ERRO-startTime')} pokedex=#{(t.pokedex ? 'ok' : 'NIL') rescue 'ERRO'} owned=#{(t.pokedex.owned_count rescue 'ERRO-owned')} coins_fmt=#{(t.coins.to_s_formatted rescue 'ERRO-coins')} bp_fmt=#{(t.battle_points.to_s_formatted rescue 'ERRO-bp')}") rescue nil)
        rescue; end
        __dc_pbRedrawSide(*a)
      end
    end
    (MKXPDebug.log("[DIAG-CARD] hook pbRedrawSide instalado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[DIAG-CARD] falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  DIAG-CARDFRONT (passo a passo, bug 2/3) — porque o texto do cartao nao aparece
# -----------------------------------------------------------------------------
#  O DIAG-CARD prova que pbRedrawSide corre e os dados estao certos, mas o lado
#  C++ (DIAG-DT) prova que draw_text NUNCA toca num bitmap 512x384. Logo o
#  pbDrawTrainerCardFront ou (a) nao e' chamado, ou (b) rebenta a meio antes do
#  pbDrawTextPositions, ou (c) o overlay tem outro tamanho. Este hook (Ruby puro,
#  SEM recompilar) regista @cardMode/@front, o tamanho REAL do overlay, e se o
#  pbDrawTrainerCardFront CHEGA AO FIM ou lanca EXCECAO. So' logging; re-lanca a
#  excecao para nao alterar o comportamento.
# =============================================================================
begin
  if defined?(PokemonTrainerCard_Scene) && PokemonTrainerCard_Scene.method_defined?(:pbDrawTrainerCardFront) && !$__diagcardfront
    $__diagcardfront = true
    class PokemonTrainerCard_Scene
      alias_method :__dcf_pbDrawTrainerCardFront, :pbDrawTrainerCardFront
      def pbDrawTrainerCardFront(*a)
        # [CARDTEST] (bug 2/3) testa cada acesso de risco do pbDrawTrainerCardFront
        # base e CORRIGE a causa provavel: $PokemonGlobal.startTime corrompido
        # (o DIAG-CARD mostrou-o vazio) -> .mon rebenta com "cannot be converted
        # to Integer" ANTES de desenhar o texto. So' Ruby, sem recompilar.
        begin
          pg = ($PokemonGlobal rescue nil)
          (MKXPDebug.log("[CARDTEST] startTime.class=#{(pg.startTime.class rescue 'ERR')} mon=#{(pg.startTime.mon rescue 'ERR-mon')} day=#{(pg.startTime.day rescue 'ERR-day')} year=#{(pg.startTime.year rescue 'ERR-year')}") rescue nil)
          # startTime e' um Object que FINGE ter .mon (passa o respond_to?) mas
          # devolve lixo -> pbGetAbbrevMonthName(startTime.mon) faz array[Object]
          # -> "Object cannot be converted to Integer". Testa se .mon e' MESMO um
          # Integer; se nao, repoe pela melhor fonte (pbGetTimeNow/Time.now/stub).
          if pg && !((pg.startTime.mon.is_a?(Integer)) rescue false)
            newt = (pbGetTimeNow rescue nil)
            newt = (Time.now rescue nil)    unless ((newt.respond_to?(:mon) && newt.mon.is_a?(Integer)) rescue false)
            newt = ProbeFakeTime.new        unless ((newt.respond_to?(:mon) && newt.mon.is_a?(Integer)) rescue false)
            pg.startTime = newt
            (MKXPDebug.log("[CARDTEST] startTime.mon invalido -> reposto p/ #{(newt.class rescue '?')} (mon=#{(newt.mon rescue '?')})") rescue nil)
          end
          t = ($Trainer rescue nil)
          (MKXPDebug.log("[CARDTEST] public_ID=#{(t.public_ID.class rescue 'ERR')} owned=#{(t.pokedex.owned_count rescue 'ERR-owned')} seen=#{(t.pokedex.seen_count rescue 'ERR-seen')} money=#{(t.money.class rescue 'ERR')} coins=#{(t.coins.class rescue 'ERR')} bp=#{(t.battle_points.class rescue 'ERR')}") rescue nil)
        rescue; end
        begin
          r = __dcf_pbDrawTrainerCardFront(*a)
          (MKXPDebug.log("[DIAG-CARDFRONT] TERMINOU OK (texto desenhado no overlay)") rescue nil)
          r
        rescue => ie
          (MKXPDebug.log("[DIAG-CARDFRONT] EXCECAO: #{ie.class}: #{ie.message} | bt0=#{(ie.backtrace[0] rescue '?')} bt1=#{(ie.backtrace[1] rescue '?')}") rescue nil)
          nil
        end
      end
    end
    (MKXPDebug.log("[DIAG-CARDFRONT] hook pbDrawTrainerCardFront instalado") rescue nil)
  else
    (MKXPDebug.log("[DIAG-CARDFRONT] pbDrawTrainerCardFront ausente ou ja' hookado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[DIAG-CARDFRONT] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  FIX-PRETO (ecra preto apos receber o inicial) — DependentEventSprites#update
# -----------------------------------------------------------------------------
#  Log (provado): ao transferir p/ mapa 43 ja' com seguidor, o caminho
#  __followingpkmn__transfer_player -> updateSpritesets -> __followingpkmn__update
#  -> (each) -> DependentEventSprites#update REBENTA (undefined, porque o EBDX
#  esta' desativado no porte) -> createSpritesets parte -> ecra preto. O DEPGUARD
#  ja' detetou que DependentEventSprites NAO tem update proprio. Solucao minima e
#  segura: definir um update no-op (impede o crash; no pior caso o seguidor nao
#  anima, mas o mapa volta a desenhar). So' Ruby, sem recompilar.
# =============================================================================
begin
  if defined?(DependentEventSprites)
    if !DependentEventSprites.method_defined?(:update)
      class DependentEventSprites
        def update(*a); end
      end
      (MKXPDebug.log("[FIX-PRETO] DependentEventSprites#update no-op definido (anti-ecra-preto)") rescue nil)
    else
      (MKXPDebug.log("[FIX-PRETO] DependentEventSprites#update ja' existe -- sem accao") rescue nil)
    end
  else
    (MKXPDebug.log("[FIX-PRETO] classe DependentEventSprites ausente") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[FIX-PRETO] falhou: #{e.class}: #{e.message}") rescue nil)
end

# Stub de tempo valido (ultimo recurso) para o cartao, caso pbGetTimeNow/Time.now
# nao deem .mon Integer neste mruby. So' precisa devolver Integers p/ desenhar.
unless defined?(ProbeFakeTime)
  class ProbeFakeTime
    def mon;  1;    end
    def day;  1;    end
    def year; 2024; end
    def hour; 0;    end
    def min;  0;    end
    def sec;  0;    end
    def to_i; 0;    end
  end
end

# =============================================================================
#  FIX-CANCELAR — sair do cartao / cancelar em qualquer menu (Input::BACK no 3DS)
# -----------------------------------------------------------------------------
#  Provado: Essentials usa BACK = B = Input::B (=12 = RMXP_B). A tabela do porte
#  (input_3ds.cpp) mapeia KEY_B -> RMXP_X (=14 = Input::X) e NADA -> RMXP_B.
#  Logo Input.trigger?(Input::BACK) nunca dispara -> impossivel sair do cartao
#  (mostra "ESC", que so' existe no PC) nem cancelar noutros menus.
#  Solucao (Ruby, sem recompilar): fazer trigger?/press?/repeat?(Input::B=12)
#  responder TAMBEM ao botao 3DS B (que o porte entrega como Input::X=14). O B
#  passa a CANCELAR em todo o lado, sem perder o uso atual. Additivo e seguro,
#  so' afeta a tecla BACK. (Fix permanente = 1 linha C++: KEY_B -> RMXP_B.)
# =============================================================================
begin
  if defined?(Input) && Input.respond_to?(:trigger?) && !$__fix_cancelar
    $__fix_cancelar = true
    module Input
      class << self
        unless method_defined?(:__fc_trigger)
          alias_method :__fc_trigger, :trigger?
          alias_method :__fc_press,   :press?
          alias_method :__fc_repeat,  :repeat?
        end
        # Essentials: BACK = B = 12 (RMXP_B). O 3DS B chega como X = 14 (RMXP_X).
        def trigger?(k)
          if k == 12 && __fc_trigger(14)
            ($__fc_logged ||= ((MKXPDebug.log("[FIX-CANCELAR] B(3DS) disparou Input::BACK -- cancelar OK") rescue nil) || true))
            return true
          end
          __fc_trigger(k)
        end
        def press?(k)
          return true if k == 12 && __fc_press(14)
          __fc_press(k)
        end
        def repeat?(k)
          return true if k == 12 && __fc_repeat(14)
          __fc_repeat(k)
        end
      end
    end
    (MKXPDebug.log("[FIX-CANCELAR] hook instalado: B(3DS)/Input::X(14) -> Input::BACK/Input::B(12)") rescue nil)
  else
    (MKXPDebug.log("[FIX-CANCELAR] Input ausente ou ja' hookado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[FIX-CANCELAR] falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  MOVEDBG — porque o jogador nao anda (input lido + as 4 condicoes EXATAS que
#  o Game_Player#update_command_new verifica antes de deixar andar)
# -----------------------------------------------------------------------------
#  O B/S sai do cartao -> o input CHEGA ao jogo. Mas o boneco nao anda no quarto.
#  O Essentials so' move se NENHUMA destas estiver activa:
#     pbMapInterpreterRunning?  message_window_showing  miniupdate  in_menu
#  Isto regista, a cada 15 frames, o dir4 (direcao lida do input) e essas 4.
#  Leitura: dir4!=0 ao carregar => input chega; se uma flag=true => e' o que
#  tranca; se todas false e moving=false => e' timing/colisao. So' logging.
# =============================================================================
begin
  if defined?(Scene_Map) && Scene_Map.method_defined?(:update) && !$__movedbg
    $__movedbg = true
    class Scene_Map
      alias_method :__md_update, :update
      def update(*a)
        begin
          $__movedbg_n = ($__movedbg_n || 0) + 1
          if ($__movedbg_n % 15) == 0 && defined?($game_player) && $game_player
            gp   = $game_player
            d4   = (Input.dir4 rescue 'ERR')
            mir  = (pbMapInterpreterRunning? rescue 'ERR')
            msg  = ($game_temp.message_window_showing rescue 'ERR')
            mini = ($PokemonTemp.miniupdate rescue 'ERR')
            menu = ($game_temp.in_menu rescue 'ERR')
            mov  = (gp.moving? rescue 'ERR')
            ld   = (gp.instance_variable_get(:@lastdir) rescue 'ERR')
            mlf  = (gp.instance_variable_get(:@moved_last_frame) rescue 'ERR')
            (MKXPDebug.log("[MOVEDBG] dir4=#{d4} BLOQUEIOS: interp=#{mir} msg=#{msg} mini=#{mini} menu=#{menu} | moving=#{mov} lastdir=#{ld} moved_last=#{mlf}") rescue nil)
          end
        rescue; end
        __md_update(*a)
      end
    end
    (MKXPDebug.log("[MOVEDBG] hook instalado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[MOVEDBG] falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  MENUGUARD — garante in_menu=false mesmo se o menu de pausa rebentar (FIX RAIZ)
# -----------------------------------------------------------------------------
#  PROVADO no log: no quarto (mapa 3), o jogador nao anda porque $game_temp.in_menu
#  esta' preso a true (MOVEDBG: menu=true, interp=false). O call_menu do Voltseon
#  poe in_menu=true, abre o menu (pbStartPokemonMenu, BLOQUEANTE) e so' poe
#  in_menu=false NO FIM. Se o menu rebentar a meio (este menu e' pesado e bate nas
#  incompatibilidades do mruby), a linha do reset nunca corre -> fica preso.
#  Aqui envolvo o call_menu num ensure: aconteca o que acontecer (sucesso OU
#  excecao), in_menu volta a false. Tambem registo se o menu rebentou, p/ sabermos.
# =============================================================================
begin
  if defined?(Scene_Map) && Scene_Map.method_defined?(:call_menu) && !$__menuguard
    $__menuguard = true
    class Scene_Map
      alias_method :__menuguard_orig_call_menu, :call_menu
      def call_menu(*a, &b)
        (MKXPDebug.log("[MENUGUARD] call_menu ENTROU (menu de pausa a abrir)") rescue nil)
        begin
          __menuguard_orig_call_menu(*a, &b)
          (MKXPDebug.log("[MENUGUARD] call_menu OK (menu fechou normalmente)") rescue nil)
        rescue => e
          (MKXPDebug.log("[MENUGUARD] call_menu REBENTOU: #{e.class}: #{e.message} -- in_menu reposto, jogo continua") rescue nil)
        ensure
          ($game_temp.in_menu = false) rescue nil
        end
      end
    end
    (MKXPDebug.log("[MENUGUARD] hook call_menu instalado (in_menu garantido a false no fim)") rescue nil)
  else
    (MKXPDebug.log("[MENUGUARD] Scene_Map#call_menu ausente ou ja' instalado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[MENUGUARD] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  FIX-MENULOCK — rede de seguranca: destranca o jogador se in_menu ficar preso
# -----------------------------------------------------------------------------
#  Complementa o MENUGUARD. Se in_menu ficar preso a true por QUALQUER outro
#  caminho, este hook deteta-o no loop normal do mapa e repoe a false.
#  Porque e' SEGURO: o menu de pausa do Voltseon e' BLOQUEANTE (pbStartPokemonMenu
#  corre o loop do menu ate' fechar). Enquanto o menu esta' MESMO aberto, o
#  Scene_Map#update NAO corre. Logo, se chegamos aqui com in_menu=true, o menu
#  NAO esta' aberto agora -> a flag e' resto preso. Repor a false nunca fecha um
#  menu legitimo (esse bloqueia este update), so' limpa lixo.
# =============================================================================
begin
  if defined?(Scene_Map) && Scene_Map.method_defined?(:update) && !$__menulock
    $__menulock = true
    class Scene_Map
      alias_method :__menulock_orig_update, :update
      def update(*a)
        gt = $game_temp
        if gt && (gt.in_menu rescue false)
          (gt.in_menu = false) rescue nil
          $__menulock_n = ($__menulock_n || 0) + 1
          if $__menulock_n <= 3 || ($__menulock_n % 120) == 0
            (MKXPDebug.log("[FIX-MENULOCK] in_menu preso a true no loop do mapa -> reposto a false (#{$__menulock_n}x)") rescue nil)
          end
        end
        __menulock_orig_update(*a)
      end
    end
    (MKXPDebug.log("[FIX-MENULOCK] hook instalado (destranca jogador se in_menu ficar preso)") rescue nil)
  else
    (MKXPDebug.log("[FIX-MENULOCK] Scene_Map#update ausente ou ja' instalado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[FIX-MENULOCK] falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  PERF-BLANK — saltar update de Sprite_Character SEM grafico (OTIMIZACAO)
# -----------------------------------------------------------------------------
#  Provado no RUBYPROF (mapa do quarto): o grosso do tempo/frame e' atualizar
#  ~44 sprites de evento (sprite_char=24.7ms). Muitos eventos NAO tem grafico
#  (char='' no log: trofeus, triggers, EV0xx). Cada um corre update + dezenas de
#  setters SEM desenhar nada. Salto apenas o update do SPRITE (visual) para chars
#  sem grafico. A logica do evento (Game_Event#update: movimento, autorun,
#  triggers, colisao) fica 100% INTACTA. Retoma assim que o char ganhe grafico.
# =============================================================================
begin
  if defined?(Sprite_Character) && Sprite_Character.method_defined?(:update) && !$__perf_blank_on
    $__perf_blank_on = true
    class Sprite_Character
      alias_method :__perf_blank_orig_update, :update unless method_defined?(:__perf_blank_orig_update)
      def update
        c = @character
        blank = (c &&
                 (c.character_name.nil? || c.character_name == "") &&
                 ((c.tile_id rescue 0).nil? || (c.tile_id rescue 0) == 0))
        if blank
          return if @__pb_done
          @__pb_done = true
          return __perf_blank_orig_update
        else
          @__pb_done = false
        end
        __perf_blank_orig_update
      end
    end
    (MKXPDebug.log("[PERF-BLANK] skip de update p/ sprites SEM grafico instalado") rescue nil)
  else
    (MKXPDebug.log("[PERF-BLANK] Sprite_Character ausente ou ja' aplicado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[PERF-BLANK] falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  PERF-STATIC — saltar update de Sprite_Character de EVENTOS PARADOS (OTIMIZACAO)
# -----------------------------------------------------------------------------
#  Complementa o PERF-BLANK. Muitos eventos estao PARADOS (mobilia, trofeus, NPCs
#  sem rota). O Essentials desenha-os 1x mas continua a correr update + dezenas de
#  setters todos os frames mesmo sem nada mudar. Calculo uma "assinatura" barata
#  dos campos que afetam o visual e, se for igual a' do frame anterior, SALTO.
#  Seguranca: SO' Game_Event (jogador e seguidor NUNCA saltam); rede de seguranca
#  forca update completo a cada 16 frames (apanha tom/blend/bush). screen_x/y ja'
#  sao nativos (ACCEL) -> a assinatura e' barata. Flag $MKXP_PERF_STATIC (ON por
#  omissao); se algum objeto parado parecer congelado, poe a false no topo.
# =============================================================================
begin
  $MKXP_PERF_STATIC = true if $MKXP_PERF_STATIC.nil?
  if defined?(Sprite_Character) && Sprite_Character.method_defined?(:update) && !$__perf_static_on
    $__perf_static_on = true
    class Sprite_Character
      alias_method :__perf_static_orig_update, :update unless method_defined?(:__perf_static_orig_update)
      def update
        if $MKXP_PERF_STATIC && @character.is_a?(Game_Event)
          c = @character
          sig = [(c.tile_id rescue 0), (c.character_name rescue ""),
                 (c.pattern rescue 0), (c.direction rescue 2),
                 (c.screen_x rescue 0), (c.screen_y rescue 0),
                 (c.opacity rescue 255), (c.transparent rescue false)]
          now = (Graphics.frame_count rescue 0)
          if @__pst_sig == sig && @__pst_fc && (now - @__pst_fc) < 16
            return
          end
          @__pst_sig = sig
          @__pst_fc  = now
        end
        __perf_static_orig_update
      end
    end
    (MKXPDebug.log("[PERF-STATIC] skip de update p/ eventos parados instalado") rescue nil)
  else
    (MKXPDebug.log("[PERF-STATIC] Sprite_Character ausente ou ja' aplicado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[PERF-STATIC] falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  DIAG-DUP — dimensoes REAIS do bitmap + src_rect por character_name (dedupe)
# -----------------------------------------------------------------------------
#  O FRAMEDUMP mostrou Sprite_Character bmp=32x32 com src=[0,0,8,8] (=bitmap/4).
#  O grid de personagens no quarto = ou sheet inteiro sem recorte, ou bitmap lido
#  como 32x32 quando devia ser maior (ex 128x128). Este hook regista, na 1a vez
#  que cada character_name aparece: o bitmap REAL (W x H do Ruby), o src_rect, e o
#  frame esperado. Diz EXATAMENTE se o bitmap esta' a ser mal lido ou se o recorte
#  de frame falha -> a' proxima corrijo a fonte certa. So' logging.
# =============================================================================
begin
  if defined?(Sprite_Character) && Sprite_Character.method_defined?(:update) && !$__diagdup
    $__diagdup = true
    $__diagdup_seen = {}
    class Sprite_Character
      alias_method :__dd_update, :update
      def update(*a)
        r = __dd_update(*a)
        begin
          cn = nil
          cn = (@character.character_name rescue nil) if defined?(@character) && @character
          cn = (@character_name rescue nil) if (cn.nil? || cn == "") && defined?(@character_name)
          if cn && cn != "" && !$__diagdup_seen[cn]
            $__diagdup_seen[cn] = true
            bmp = (self.bitmap rescue nil)
            bw = (bmp ? (bmp.width rescue -1) : -1)
            bh = (bmp ? (bmp.height rescue -1) : -1)
            sr = (self.src_rect rescue nil)
            srs = sr ? "[#{(sr.x rescue '?')},#{(sr.y rescue '?')},#{(sr.width rescue '?')},#{(sr.height rescue '?')}]" : 'nil'
            fw = (bw.is_a?(Integer) && bw > 0) ? bw / 4 : -1
            fh = (bh.is_a?(Integer) && bh > 0) ? bh / 4 : -1
            (MKXPDebug.log("[DIAG-DUP] char='#{cn}' bitmap=#{bw}x#{bh} src_rect=#{srs} frame_div4=#{fw}x#{fh}") rescue nil)
          end
        rescue; end
        r
      end
    end
    (MKXPDebug.log("[DIAG-DUP] hook Sprite_Character instalado (dedupe por character_name)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[DIAG-DUP] falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  SEQ — sequencia de eventos ESPERADA vs REAL (novo jogo / intro / quarto)
# -----------------------------------------------------------------------------
#  Pedido: registar no log a sequencia que DEVIA acontecer, para comparar com o
#  que acontece e agilizar o debug. Imprime uma REFERENCIA (uma vez) e depois
#  marca [SEQ>] sempre que muda o mapa, a cena, ou nascem $Trainer/$PokemonGlobal.
#  Hook em Scene_Map#update (padrao seguro, ja' usado neste probe). So' logging.
# =============================================================================
begin
  if defined?(Scene_Map) && Scene_Map.method_defined?(:update) && !$__seq_hook
    $__seq_hook = true
    $__seq = { scene: nil, map: nil, trainer: false, pg: false, ctrl: nil }
    (MKXPDebug.log("[SEQ] ===== SEQUENCIA ESPERADA (novo jogo Essentials) =====") rescue nil)
    ["1. Titulo (PokemonLoadScreen / Intro)",
     "2. 'New Game' escolhido",
     "3. Cena de intro (Modular Title / professor)",
     "4. Mapa do QUARTO carregado (mapa 'Intro')",
     "5. Mae fala: 'UP AND AT 'EM! IT'S MORNING!'",
     "6. Entrada do NOME -> \$Trainer + \$PokemonGlobal criados",
     "7. Jogador GANHA CONTROLO no quarto",
     "8. Sai do quarto -> escadas -> exterior (transferencias de mapa)"
    ].each { |e| (MKXPDebug.log("[SEQ]   #{e}") rescue nil) }
    (MKXPDebug.log("[SEQ] ===== fim referencia; eventos REAIS abaixo como [SEQ>] =====") rescue nil)
    class Scene_Map
      alias_method :__seq_update, :update
      def update(*a)
        begin
          sc = ($scene.class.to_s rescue nil)
          if sc && sc != $__seq[:scene]
            $__seq[:scene] = sc
            (MKXPDebug.log("[SEQ>] cena -> #{sc}") rescue nil)
          end
          mp = (defined?($game_map) && $game_map) ? ($game_map.map_id rescue nil) : nil
          if mp && mp != $__seq[:map]
            $__seq[:map] = mp
            nm = ($game_map.name rescue '?') rescue '?'
            (MKXPDebug.log("[SEQ>] MAPA -> #{mp} '#{nm}'") rescue nil)
          end
          if (defined?($Trainer) && $Trainer) && !$__seq[:trainer]
            $__seq[:trainer] = true
            (MKXPDebug.log("[SEQ>] \$Trainer criado: name=#{($Trainer.name rescue '?').inspect} money=#{($Trainer.money rescue '?')}") rescue nil)
          end
          if (defined?($PokemonGlobal) && $PokemonGlobal) && !$__seq[:pg]
            $__seq[:pg] = true
            st = ($PokemonGlobal.startTime rescue 'nil')
            (MKXPDebug.log("[SEQ>] \$PokemonGlobal criado (startTime=#{st})") rescue nil)
          end
          mv = (defined?($game_player) && $game_player) ? (($game_player.moving? rescue nil) ? 'movimento' : 'parado') : nil
          if mv && mv != $__seq[:ctrl]
            $__seq[:ctrl] = mv
            (MKXPDebug.log("[SEQ>] jogador: #{mv}") rescue nil) if mv == 'movimento' && $__seq[:ctrl_once].nil?
            $__seq[:ctrl_once] = true if mv == 'movimento'
          end
        rescue; end
        __seq_update(*a)
      end
    end
    (MKXPDebug.log("[SEQ] hook Scene_Map instalado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[SEQ] falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  EVENTSEQ — sequencia de eventos ESPERADA vs REAL (novo jogo / intro)
# -----------------------------------------------------------------------------
#  Pedido: registar no log o que DEVIA acontecer p/ agilizar o debug. Abaixo a
#  sequencia canonica de um novo jogo Essentials; os hooks marcam cada passo REAL
#  a medida que acontece. Procura "[EVENTSEQ]" no log e compara com a lista.
# =============================================================================
begin
  unless $__eventseq
    $__eventseq = true
    (MKXPDebug.log("[EVENTSEQ] ===== SEQUENCIA ESPERADA (novo jogo Essentials) =====") rescue nil)
    [
      "1. Tela de titulo (Modular Title Screen / pbCallTitle)",
      "2. Jogador escolhe 'New Game'",
      "3. $Trainer/$player criado (Player.new) -> ver [NAMEIN] $Trainer CRIADO",
      "4. Entrada do nome (teclado) -> ver [NAMEIN] pbEnterText -> nome",
      "5. Mapa inicial (quarto) carregado -> ver [EVENTSEQ] MAPA id=...",
      "6. Evento autorun da intro (mae: UP AND AT EM) -> ver [PBMSG]",
      "7. Controlo ao jogador; transferencias -> ver [EVENTSEQ] TRANSFER + MAPA",
    ].each { |s| (MKXPDebug.log("[EVENTSEQ]   esperado: #{s}") rescue nil) }
    (MKXPDebug.log("[EVENTSEQ] ===== marcadores REAIS abaixo =====") rescue nil)
  end
rescue; end

begin
  if defined?(Game_Map) && Game_Map.method_defined?(:setup) && !$__es_map
    $__es_map = true
    class Game_Map
      alias_method :__es_setup, :setup
      def setup(map_id)
        __es_setup(map_id)
        begin
          nm = '?'
          nm = ($data_map_infos[map_id].name rescue '?') if defined?($data_map_infos) && $data_map_infos
          $__es_mapn = ($__es_mapn || 0) + 1
          (MKXPDebug.log("[EVENTSEQ] (#{$__es_mapn}) MAPA carregado: id=#{map_id} nome='#{nm}'") rescue nil)
        rescue; end
      end
    end
    (MKXPDebug.log("[EVENTSEQ] hook Game_Map#setup instalado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[EVENTSEQ] hook mapa falhou: #{e.class}: #{e.message}") rescue nil)
end

begin
  if defined?(Interpreter) && Interpreter.method_defined?(:command_201) && !$__es_tr
    $__es_tr = true
    class Interpreter
      alias_method :__es_cmd201, :command_201
      def command_201(*a)
        begin
          (MKXPDebug.log("[EVENTSEQ] TRANSFER (command_201) chamado") rescue nil)
        rescue; end
        __es_cmd201(*a)
      end
    end
    (MKXPDebug.log("[EVENTSEQ] hook Interpreter#command_201 instalado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[EVENTSEQ] hook transfer falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  DIAG-CAM — posicao da camara (display_x/y) + jogador durante a intro
# -----------------------------------------------------------------------------
#  "a camara a mover-se na intro": este hook regista o scroll do mapa
#  ($game_map.display_x/display_y) e a posicao do jogador a cada 30 frames, e
#  SINALIZA saltos grandes (>256 = 1 tile). Mostra se a camara mexe quando nao
#  devia, ou se e' um pan de evento autorun. So' logging.
# =============================================================================
begin
  if defined?(Game_Player) && Game_Player.method_defined?(:update) && !$__diagcam
    $__diagcam = true
    $__cam_f = 0
    $__cam_last = nil
    class Game_Player
      alias_method :__cam_update, :update
      def update(*a)
        r = __cam_update(*a)
        begin
          $__cam_f += 1
          dx = ($game_map.display_x rescue -1)
          dy = ($game_map.display_y rescue -1)
          px = (self.x rescue -1); py = (self.y rescue -1)
          rx = (self.real_x rescue -1); ry = (self.real_y rescue -1)
          mv = (self.moving? rescue '?')
          jump = false
          jump = ((dx - $__cam_last[0]).abs > 256 || (dy - $__cam_last[1]).abs > 256) if $__cam_last
          if jump || ($__cam_f % 30 == 1)
            (MKXPDebug.log("[DIAG-CAM] f=#{$__cam_f} display=(#{dx},#{dy}) player=(#{px},#{py}) real=(#{rx},#{ry}) moving=#{mv}#{jump ? ' <<< SALTO' : ''}") rescue nil)
          end
          $__cam_last = [dx, dy]
        rescue; end
        r
      end
    end
    (MKXPDebug.log("[DIAG-CAM] hook Game_Player#update instalado (camara/scroll)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[DIAG-CAM] falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  INTERP-STUCK — quando o interpretador de eventos fica preso, diz QUE comando
# -----------------------------------------------------------------------------
#  O log mostrou interp_idx=151 preso na Map43 + loop p/ Map003 (congela na
#  intro "PETI & GELATO"). Este hook, quando o @index nao avanca ~120 frames (~2s),
#  regista o code+parametros do comando preso. code: 101=msg 201=transfer
#  209=move-route 106=wait 208=transparencia 223=tom-ecra 224=flash etc.
#  Assim a' proxima sei EXATAMENTE o que trava a intro. So' logging.
# =============================================================================
begin
  if defined?(Interpreter) && Interpreter.method_defined?(:update) && !$__interpstuck
    $__interpstuck = true
    class Interpreter
      alias_method :__is_update, :update
      def update(*a)
        begin
          idx = (@index rescue nil)
          lst = (@list rescue nil)
          if idx && lst
            if idx == (@__last_idx ||= -999)
              @__stuck = (@__stuck || 0) + 1
              if @__stuck == 120
                cmd = (lst[idx] rescue nil)
                code = (cmd.code rescue '?')
                par = (cmd.parameters.inspect[0,90] rescue '?')
                (MKXPDebug.log("[INTERP-STUCK] map=#{($game_map.map_id rescue '?')} idx=#{idx} code=#{code} params=#{par} (preso ~2s)") rescue nil)
              end
            else
              @__stuck = 0
            end
            @__last_idx = idx
          end
        rescue; end
        __is_update(*a)
      end
    end
    (MKXPDebug.log("[INTERP-STUCK] hook Interpreter#update instalado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[INTERP-STUCK] falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  DEPGUARD — protege o update do Following Pokemon EX (causa do ECRA PRETO)
# -----------------------------------------------------------------------------
#  ROOT (provado no log): DependentEventSprites#update rebenta com
#  "ArgumentError: cannot set instance variable" no loop do status-tone
#  (realEvents[i]/@sprites[i] vem como valor imediato). O crash sobe ate
#  createSpritesets -> a cena nao termina -> Graphics.transition() nunca e'
#  chamado -> ecra fica PRETO ao entrar no mapa 43 (apos a intro da professora).
#  Aqui embrulho o update num rescue: a base ja' correu (primeira linha do
#  update do plugin), por isso o seguidor aparece; so' o efeito de tom e'
#  saltado. A cena termina e o transition corre -> SEM ecra preto.
# =============================================================================
#  >>> PORQUE A VERSAO ANTERIOR (imediata) NAO FUNCIONOU (provado no log) <<<
#  O probe e' carregado ANTES dos plugins. Quando este bloco corria no LOAD,
#  `DependentEventSprites.method_defined?(:update)` dava FALSE (o Following
#  Pokemon EX ainda nao tinha corrido; alem disso o boot mostra
#  "Sprite.rb: NameError: undefined method 'update' for class DependentEventSprites"
#  -> o proprio alias do plugin falhou). Logo o hook nunca instalava e o crash
#  continuava -> createSpritesets nao terminava -> Graphics.transition() nao
#  corria -> ecra PRETO/congelado na imagem da intro ao entrar no mapa 43.
#
#  CORRECAO (3 camadas, todas DIFERIDAS p/ o 1o frame de Scene_Map, ja' depois
#  dos plugins -- o mesmo padrao do TSCACHE, que o log prova instalar OK):
#    (1) SPRGUARD: embrulha o Spriteset_Map#update OUTERMOST (pos-plugins) num
#        rescue. A 1a linha do update do plugin (__followingpkmn__update) ja'
#        corre -> sprites principais actualizam; so' o seguidor (que rebenta)
#        e' ignorado. O createSpritesets TERMINA -> transition corre.
#    (2) DEPGUARD: se DependentEventSprites tiver mesmo um update, embrulha-o
#        tambem (granular: so' o seguidor e' afectado).
#    (3) TRANSITIONGUARD: rede de seguranca -- se mesmo assim o ecra ficar
#        congelado (Graphics.frozen? por N frames numa cena viva), forca
#        Graphics.transition() p/ NUNCA ficar preso, venha o crash de onde vier.
begin
  if defined?(Scene_Map) && Scene_Map.method_defined?(:update) && !$__sprguard_defer
    $__sprguard_defer = true
    class Scene_Map
      alias_method :__sprguard_defer_orig_update, :update
      def update(*a)
        # --- instalacao unica, no 1o frame (plugins ja' carregados) -----------
        unless $__sprguard_done
          $__sprguard_done = true
          begin
            # (1) SPRGUARD -- Spriteset_Map#update outermost
            if defined?(Spriteset_Map) && Spriteset_Map.method_defined?(:update) &&
               !Spriteset_Map.method_defined?(:__sprguard_orig_update)
              Spriteset_Map.class_eval do
                alias_method :__sprguard_orig_update, :update
                def update(*aa)
                  begin
                    __sprguard_orig_update(*aa)
                  rescue => e
                    unless $__sprguard_logged
                      $__sprguard_logged = true
                      (MKXPDebug.log("[SPRGUARD] Spriteset_Map#update protegido: #{e.class}: #{e.message} -- cena termina, transition corre, SEM ecra preso") rescue nil)
                    end
                    nil
                  end
                end
              end
              (MKXPDebug.log("[SPRGUARD] hook Spriteset_Map#update instalado no 1o frame (outermost, pos-plugins)") rescue nil)
            else
              (MKXPDebug.log("[SPRGUARD] Spriteset_Map#update indisponivel ou ja' protegido") rescue nil)
            end
            # (2) DEPGUARD -- granular, so' se houver update
            if defined?(DependentEventSprites) && DependentEventSprites.method_defined?(:update) &&
               !DependentEventSprites.method_defined?(:__dg_update)
              DependentEventSprites.class_eval do
                alias_method :__dg_update, :update
                def update(*aa)
                  begin
                    __dg_update(*aa)
                  rescue => e
                    unless $__depguard_logged
                      $__depguard_logged = true
                      (MKXPDebug.log("[DEPGUARD] DependentEventSprites#update protegido: #{e.class}: #{e.message}") rescue nil)
                    end
                    nil
                  end
                end
              end
              (MKXPDebug.log("[DEPGUARD] hook DependentEventSprites#update instalado no 1o frame") rescue nil)
            else
              (MKXPDebug.log("[DEPGUARD] DependentEventSprites sem update proprio (alias do plugin falhou no boot) -- SPRGUARD cobre o caso") rescue nil)
            end
            # (4) CARTAO-DRAW -- DIAGNOSTICO do Trainer Card vazio.
            # O cartao (Peti) desenha o texto via @sprites["text"]=TextSprite.new
            # (Marin's Utilities) -> TextSprite#draw -> pbDrawTextPositions(bmp 512x384).
            # NAO e' o overlay nem draw_text direto. Aqui registamos, do lado Ruby,
            # CADA chamada a TextSprite#draw: tamanho da bitmap, se o sprite esta'
            # VISIVEL, o z, viewport, opacidade, e o texto+cor base de cada entrada.
            # Cruzado com [CARTAO-TXT] (lado C++, se o draw_text e' mesmo chamado e
            # com que cor) isto localiza a causa: texto invisivel (cor/alpha),
            # sprite escondido (z/vis), ou via de desenho diferente.
            if defined?(TextSprite) && TextSprite.method_defined?(:draw) &&
               !TextSprite.method_defined?(:__cartao_orig_draw)
              TextSprite.class_eval do
                alias_method :__cartao_orig_draw, :draw
                def draw(*aa)
                  begin
                    $__cartao_n = ($__cartao_n || 0) + 1
                    if $__cartao_n <= 80
                      _bmp = (self.bitmap rescue nil)
                      _sz  = _bmp ? "#{_bmp.width}x#{_bmp.height}" : "nil"
                      _vis = (self.visible rescue '?')
                      _z   = (self.z rescue '?')
                      _vp  = ((self.viewport ? 'ok' : 'nil') rescue '?')
                      _op  = (self.opacity rescue '?')
                      _desc = ""
                      _ent = aa[0]
                      if _ent.is_a?(Array)
                        _rows = _ent[0].is_a?(Array) ? _ent : [_ent]
                        _rows.first(4).each do |e|
                          next unless e.is_a?(Array)
                          _bc = e[4]
                          _bcs = _bc ? "#{(_bc.red rescue '?').to_i},#{(_bc.green rescue '?').to_i},#{(_bc.blue rescue '?').to_i},a#{(_bc.alpha rescue 255).to_i}" : "nil"
                          _desc << " | '#{e[0].to_s[0,20]}'@(#{e[1]},#{e[2]}) al=#{e[3]} base=#{_bcs}"
                        end
                      end
                      (MKXPDebug.log("[CARTAO-DRAW] TextSprite#draw bmp=#{_sz} vis=#{_vis} z=#{_z} vp=#{_vp} op=#{_op}#{_desc}") rescue nil)
                    end
                  rescue => _le
                    (MKXPDebug.log("[CARTAO-DRAW] log falhou: #{_le.class}: #{_le.message}") rescue nil)
                  end
                  __cartao_orig_draw(*aa)
                end
              end
              (MKXPDebug.log("[CARTAO-DRAW] hook TextSprite#draw instalado (diagnostico cartao)") rescue nil)
            else
              (MKXPDebug.log("[CARTAO-DRAW] TextSprite indisponivel ou ja' instrumentado") rescue nil)
            end
          rescue => e
            (MKXPDebug.log("[SPRGUARD] defer falhou: #{e.class}: #{e.message}") rescue nil)
          end
        end
        # --- (3) TRANSITIONGUARD -- rede de seguranca todos os frames ----------
        # Se a cena esta' viva mas o ecra continua FREEZE (Graphics.frozen?) por
        # varios frames, e' o sintoma classico de "freeze sem transition" (o
        # createSpritesets rebentou antes do transition). Forcamos a transicao.
        begin
          if defined?(Graphics) && Graphics.respond_to?(:frozen?)
            if (Graphics.frozen? rescue false)
              $__tg_frozen = ($__tg_frozen || 0) + 1
              if $__tg_frozen >= 8   # ~8 frames congelado numa cena que corre = preso
                (MKXPDebug.log("[TRANSITIONGUARD] ecra congelado ha' #{$__tg_frozen} frames numa cena viva -> a forcar Graphics.transition()") rescue nil)
                (Graphics.transition(8) rescue (Graphics.transition rescue nil))
                $__tg_frozen = 0
              end
            else
              $__tg_frozen = 0
            end
          end
        rescue; end
        __sprguard_defer_orig_update(*a)
      end
    end
    (MKXPDebug.log("[SPRGUARD] defer-installer registado em Scene_Map#update") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[SPRGUARD] defer instalacao falhou: #{e.class}: #{e.message}") rescue nil)
end
