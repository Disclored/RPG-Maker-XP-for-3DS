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
  # Se NAO vires "[PROBE] === debug_probe V15 ===" no debug_binding.log, estas a
  # correr um ficheiro antigo (o motor le' o ficheiro errado / nao foi substituido).
  (MKXPDebug.log("[PROBE] === debug_probe V16 (SHADOWZ-PL DESLIGADO -- reverter regressao do player invisivel) ATIVO ===") rescue nil)

  # ─── KILL-SWITCH SOMBRA (V16) ────────────────────────────────────────────
  # O utilizador reportou: o player aparecia ANTES e deixou de aparecer DEPOIS
  # do fix da sombra (SHADOWZ-PL, v12). Embora o hook so' pareca tocar na sombra
  # (@sprite, 24x16) e nao no sprite do player (@rsprite, 256x256 -- verificado:
  # grelha 4x4 com as 16 frames cheias, carrega bem), a observacao empirica do
  # utilizador manda. Desligamos o SHADOWZ-PL POR COMPLETO: marcando o guard de
  # instalacao como ja-feito, o hook em Sprite_OWShadow#update NUNCA e' instalado
  # -> a sombra volta ao comportamento ORIGINAL do plugin (dono.z - 1), sem
  # qualquer alteracao de z/oy minha. Se o player voltar a aparecer, confirma-se
  # que era isto. (Trade-off: a sombra nas escadas volta ao bug antigo -- menos
  # importante que o player ser visivel. Re-ativar: por estas 3 flags a false.)
  $__shadowz_installed     = true   # impede a instalacao do hook SHADOWZ-PL
  $MKXP_SHADOW_PLAYER_FIX  = false  # (defesa extra, caso o hook ja' existisse)
  $MKXP_SHADOW_PLAYER_POS  = false

  # ============================================================================
  # CEFIX-RECON: reconstrução DIRETA das command-lists do CE#34/#212/#173.
  # O Marshal do mruby (mkxp-3ds) carrega o objeto CommonEvent mas deixa a @list
  # a NIL (o ficheiro CommonEvents.rxdata esta' 100% OK em Ruby normal: 257 CEs,
  # 0 listas nil). Sem lista, o command_117 cria um filho que nao corre nada ->
  # era o bug da estante. Aqui construo os RPG::EventCommand A MAO (sem Marshal),
  # verificado com round-trip perfeito contra a rxdata. Usado pelo CEFIX como
  # fallback quando o reload (load_data) tambem devolve lista nil.
  # ============================================================================
  def __ec(code, indent, params)
    c = (RPG::EventCommand.new(code, indent, params) rescue nil)
    if c.nil?
      c = RPG::EventCommand.new
      (c.code = code rescue nil); (c.indent = indent rescue nil); (c.parameters = params rescue nil)
    end
    c
  end
  def __mc(code, params)
    m = (RPG::MoveCommand.new(code, params) rescue nil)
    if m.nil?
      m = RPG::MoveCommand.new
      (m.code = code rescue nil); (m.parameters = params rescue nil)
    end
    m
  end
  def __mr(rep, skip, wait, list)
    r = RPG::MoveRoute.new
    (r.repeat = rep rescue nil); (r.skippable = skip rescue nil)
    (r.wait = wait rescue nil); (r.list = list rescue nil)
    r
  end
  def __tone(r,g,b,gr); (Tone.new(r,g,b,gr) rescue (Tone.new rescue nil)); end
  def __color(r,g,b,a); (Color.new(r,g,b,a) rescue (Color.new rescue nil)); end
  def __af(n,v,p)
    a = (RPG::AudioFile.new(n,v,p) rescue nil)
    if a.nil?
      a = RPG::AudioFile.new
      (a.name = n rescue nil); (a.volume = v rescue nil); (a.pitch = p rescue nil)
    end
    a
  end
  def __build_ce_recon
    return $__ce_recon if (defined?($__ce_recon) && $__ce_recon && !$__ce_recon.empty?)
    $__ce_recon = {}
    begin
      $__ce_recon[212] = [
        __ec(355,0,["illegal = [:MT,:MT2,:HUMANOID,:MONSTER]"]),
        __ec(355,0,["$Trainer.party.each do |i|"]),
        __ec(655,0,["  if illegal.include?(i.species) || "]),
        __ec(655,0,["  (i.species == :F00 && (!i.foreign?($Trainer) || i.owner.gender != 1 || i.form > 0))"]),
        __ec(655,0,["    i.species = :PETILIL"]),
        __ec(655,0,["    i.reset_moves"]),
        __ec(655,0,["    i.calc_stats"]),
        __ec(655,0,["  end"]),
        __ec(655,0,["end"]),
        __ec(0,0,[])
      ]
      $__ce_recon[34] = [
        __ec(117,0,[212]),
        __ec(122,0,[5,5,0,1,117]),
        __ec(223,0,[__tone(-255.0,-255.0,-255.0,0.0),6]),
        __ec(106,0,[8]),
        __ec(117,0,[173]),
        __ec(209,0,[-1,__mr(false,false,nil,[__mc(16,[]),__mc(0,[])])]),
        __ec(509,0,[__mc(16,[])]),
        __ec(223,0,[__tone(0.0,0.0,0.0,0.0),6]),
        __ec(106,0,[8]),
        __ec(108,0,["// CHANGING"]),
        __ec(111,0,[12,"$game_map.map_id == 3"]),
        __ec(108,1,["// Player's Closet"]),
        __ec(111,1,[1,5,1,117,5]),
        __ec(117,2,[20]),
        __ec(249,2,[__af("Getting changed",100,100)]),
        __ec(101,2,["\\PN changed clothes.\\wtnp[30]"]),
        __ec(117,2,[21]),
        __ec(0,2,[]),
        __ec(412,1,[]),
        __ec(0,1,[]),
        __ec(411,0,[]),
        __ec(108,1,["// PC Closet"]),
        __ec(355,1,["base = pbGet(79)"]),
        __ec(655,1,["outfit = pbGet(117)"]),
        __ec(655,1,["$Trainer.outfit=base+(4*outfit)"]),
        __ec(209,1,[0,__mr(false,true,nil,[__mc(44,[__af("se_door_slide",100,100)]),__mc(15,[2]),__mc(17,[]),__mc(15,[2]),__mc(18,[]),__mc(15,[2]),__mc(19,[]),__mc(15,[2]),__mc(0,[])])]),
        __ec(509,1,[__mc(44,[__af("se_door_slide",100,100)])]),
        __ec(509,1,[__mc(15,[2])]),
        __ec(509,1,[__mc(17,[])]),
        __ec(509,1,[__mc(15,[2])]),
        __ec(509,1,[__mc(18,[])]),
        __ec(509,1,[__mc(15,[2])]),
        __ec(509,1,[__mc(19,[])]),
        __ec(509,1,[__mc(15,[2])]),
        __ec(210,1,[]),
        __ec(208,1,[1]),
        __ec(209,1,[-1,__mr(false,true,nil,[__mc(37,[]),__mc(39,[]),__mc(1,[]),__mc(38,[]),__mc(40,[]),__mc(0,[])])]),
        __ec(509,1,[__mc(37,[])]),
        __ec(509,1,[__mc(39,[])]),
        __ec(509,1,[__mc(1,[])]),
        __ec(509,1,[__mc(38,[])]),
        __ec(509,1,[__mc(40,[])]),
        __ec(210,1,[]),
        __ec(209,1,[0,__mr(false,true,nil,[__mc(15,[2]),__mc(18,[]),__mc(15,[2]),__mc(17,[]),__mc(15,[2]),__mc(16,[]),__mc(15,[2]),__mc(0,[])])]),
        __ec(509,1,[__mc(15,[2])]),
        __ec(509,1,[__mc(18,[])]),
        __ec(509,1,[__mc(15,[2])]),
        __ec(509,1,[__mc(17,[])]),
        __ec(509,1,[__mc(15,[2])]),
        __ec(509,1,[__mc(16,[])]),
        __ec(509,1,[__mc(15,[2])]),
        __ec(210,1,[]),
        __ec(111,1,[1,5,1,117,5]),
        __ec(117,2,[20]),
        __ec(249,2,[__af("Getting changed",100,100)]),
        __ec(101,2,["\\PN changed clothes.\\wtnp[30]"]),
        __ec(117,2,[21]),
        __ec(0,2,[]),
        __ec(412,1,[]),
        __ec(0,1,[]),
        __ec(412,0,[]),
        __ec(118,0,["End"]),
        __ec(0,0,[])
      ]
      $__ce_recon[173] = [
        __ec(122,0,[1,1,0,0,0]),
        __ec(122,0,[2,2,0,0,0]),
        __ec(122,0,[3,3,0,0,0]),
        __ec(122,0,[4,4,0,0,0]),
        __ec(118,0,["refresh"]),
        __ec(355,0,["bitmap = Bitmap.new(Graphics.width, Graphics.height)"]),
        __ec(655,0,["pbSetSystemFont(bitmap)"]),
        __ec(655,0,[""]),
        __ec(655,0,["base_x = 310"]),
        __ec(655,0,["base_y = 72"]),
        __ec(655,0,["line_height = 32"]),
        __ec(655,0,["base = MessageConfig::DARK_TEXT_MAIN_COLOR"]),
        __ec(655,0,["shadow = MessageConfig::DARK_TEXT_SHADOW_COLOR"]),
        __ec(655,0,["baseSel = Color.new(248,56,32)"]),
        __ec(655,0,["shadowSel = Color.new(224,152,144)"]),
        __ec(355,0,["optionID = [0,1]"]),
        __ec(355,0,["options = [\"Everyday Style\", \"Alternate Style\"]"]),
        __ec(355,0,["case ($Trainer.character_ID)"]),
        __ec(655,0,["when 0"]),
        __ec(655,0,["  # Sorin"]),
        __ec(655,0,["  options = [\"Everyday Style\", \"Chic Style\", \"Verdant Style\", \"Shade Style\", \"Lover Style\"]"]),
        __ec(655,0,["  optionID = [0,1,4,3,2]"]),
        __ec(655,0,["when 1"]),
        __ec(655,0,["  # Soleil"]),
        __ec(655,0,["  options = [\"Everyday Style\", \"Classy Style\", \"Mystic Style\", \"Brave Style\", \"Shadow Style\"]"]),
        __ec(655,0,["  optionID = [0,1,2,3,4]"]),
        __ec(355,0,["when 2"]),
        __ec(655,0,["  # Luan"]),
        __ec(655,0,["  options = [\"Everyday Style\", \"Cool Style\", \"Cyber Style\", \"Wilds Style\", \"Sports Style\"]"]),
        __ec(655,0,["  optionID = [0,1,2,3,4]"]),
        __ec(655,0,["when 3"]),
        __ec(655,0,["  # Lusine"]),
        __ec(655,0,["  options = [\"Everyday Style\", \"Cute Style\", \"Marine Style\", \"Rival Style\", \"Lucky Style\"]"]),
        __ec(655,0,["  optionID = [0,1,4,3,2]"]),
        __ec(355,0,["when 4"]),
        __ec(655,0,["  # Ethan"]),
        __ec(655,0,["  options = [\"Everyday Style\", \"Rocket Style\"]"]),
        __ec(655,0,["  optionID = [0,1]"]),
        __ec(655,0,["when 5"]),
        __ec(655,0,["  # Lyra"]),
        __ec(655,0,["  options = [\"Everyday Style\", \"Rocket Style\"]"]),
        __ec(655,0,["  optionID = [0,1]"]),
        __ec(355,0,["when 6"]),
        __ec(655,0,["  # Lucas"]),
        __ec(655,0,["  options = [\"Everyday Style\", \"Platinum Style\"]"]),
        __ec(655,0,["  optionID = [1,0]"]),
        __ec(655,0,["when 7"]),
        __ec(655,0,["  # Dawn"]),
        __ec(655,0,["  options = [\"Everyday Style\", \"Platinum Style\"]"]),
        __ec(655,0,["  optionID = [1,0]"]),
        __ec(355,0,["when 8"]),
        __ec(655,0,["  # Black"]),
        __ec(655,0,["  options = [\"Everyday Style\", \"Second Style\"]"]),
        __ec(655,0,["  optionID = [0,1]"]),
        __ec(655,0,["when 9"]),
        __ec(655,0,["  # White"]),
        __ec(655,0,["  options = [\"Everyday Style\", \"Second Style\"]"]),
        __ec(655,0,["  optionID = [0,1]"]),
        __ec(655,0,["end"]),
        __ec(355,0,["pbSet(2,optionID.length)"]),
        __ec(655,0,["pbSet(4,optionID)"]),
        __ec(355,0,["skinTone = pbGet(79)"]),
        __ec(655,0,["realOutfit = pbGet(117)"]),
        __ec(655,0,["outfit = optionID[pbGet(1)]"]),
        __ec(655,0,["$Trainer.outfit=skinTone+(4*outfit)"]),
        __ec(355,0,["if @sprites"]),
        __ec(655,0,["  @sprites.each_value do |s|"]),
        __ec(655,0,["    s.dispose if s && !s.disposed?"]),
        __ec(655,0,["  end"]),
        __ec(655,0,["end"]),
        __ec(655,0,["@viewport.dispose if @viewport && !@viewport.disposed?"]),
        __ec(655,0,[""]),
        __ec(655,0,["@sprites = {}"]),
        __ec(655,0,["@viewport = nil"]),
        __ec(355,0,["background = pbResolveBitmap(sprintf(\"Graphics/Pictures/outfitBG\"))"]),
        __ec(655,0,["addBackgroundPlane(@sprites,\"bg\",\"outfitBG\",@viewport)"]),
        __ec(355,0,["options.each_with_index do |option, i|"]),
        __ec(655,0,["  y = base_y + (i * line_height)"]),
        __ec(655,0,["  baseC = optionID[i] == realOutfit ? baseSel : base"]),
        __ec(655,0,["  shadowC = optionID[i] == realOutfit ? shadowSel : shadow"]),
        __ec(655,0,["  drawFormattedTextEx(bitmap,base_x,y,width=200,option,baseC,shadowC,line_height)"]),
        __ec(655,0,["end"]),
        __ec(655,0,[""]),
        __ec(655,0,["@sprites[\"options\"] = Sprite.new"]),
        __ec(655,0,["@sprites[\"options\"].bitmap = bitmap"]),
        __ec(655,0,["@sprites[\"options\"].viewport = @viewport"]),
        __ec(355,0,["@sprites[\"player\"] = IconSprite.new(64, 128, @viewport)"]),
        __ec(655,0,["@sprites[\"player\"].setBitmap(GameData::TrainerType.player_front_sprite_filename($Trainer.trainer_type))"]),
        __ec(655,0,["@sprites[\"player\"].zoom_x = 2"]),
        __ec(655,0,["@sprites[\"player\"].zoom_y = 2"]),
        __ec(355,0,["x = base_x - 24"]),
        __ec(655,0,["y = (base_y - 6) + (pbGet(1) * line_height)"]),
        __ec(655,0,["@sprites[\"arrow\"] = IconSprite.new(x, y, @viewport)"]),
        __ec(655,0,["@sprites[\"arrow\"].setBitmap(\"Graphics/Pictures/selarrow\")"]),
        __ec(655,0,["if pbGet(3)==0"]),
        __ec(655,0,["  pbFadeInAndShow(@sprites)"]),
        __ec(655,0,["  pbSet(3,1)"]),
        __ec(655,0,["end"]),
        __ec(108,0,["// Input"]),
        __ec(106,0,[4]),
        __ec(118,0,["pickingAnswer"]),
        __ec(111,0,[11,2]),
        __ec(355,1,["pbPlayCursorSE"]),
        __ec(655,1,["c = pbGet(1) + 1"]),
        __ec(655,1,["c = 0 if c > pbGet(2)-1"]),
        __ec(655,1,["pbSet(1,c)"]),
        __ec(119,1,["refresh"]),
        __ec(0,1,[]),
        __ec(411,0,[]),
        __ec(111,1,[11,8]),
        __ec(355,2,["pbPlayCursorSE"]),
        __ec(655,2,["c = pbGet(1) - 1"]),
        __ec(655,2,["c = pbGet(2)-1 if c < 0"]),
        __ec(655,2,["pbSet(1,c)"]),
        __ec(119,2,["refresh"]),
        __ec(0,2,[]),
        __ec(411,1,[]),
        __ec(111,2,[11,12]),
        __ec(355,3,["pbPlayCloseMenuSE"]),
        __ec(655,3,[""]),
        __ec(655,3,["skinTone = pbGet(79)"]),
        __ec(655,3,["realOutfit = pbGet(117)"]),
        __ec(655,3,["if realOutfit >= 0"]),
        __ec(655,3,["  $Trainer.outfit=skinTone+(4*realOutfit)"]),
        __ec(655,3,["else"]),
        __ec(655,3,["  $Trainer.outfit = skinTone+200"]),
        __ec(655,3,["end"]),
        __ec(119,3,["end"]),
        __ec(0,3,[]),
        __ec(411,2,[]),
        __ec(111,3,[11,13]),
        __ec(355,4,["pbPlayDecisionSE"]),
        __ec(655,4,["i = pbGet(4)[pbGet(1)]"]),
        __ec(655,4,["pbSet(117,i)"]),
        __ec(119,4,["refresh"]),
        __ec(0,4,[]),
        __ec(411,3,[]),
        __ec(106,4,[1]),
        __ec(119,4,["pickingAnswer"]),
        __ec(0,4,[]),
        __ec(412,3,[]),
        __ec(0,3,[]),
        __ec(412,2,[]),
        __ec(0,2,[]),
        __ec(412,1,[]),
        __ec(0,1,[]),
        __ec(412,0,[]),
        __ec(118,0,["end"]),
        __ec(355,0,["pbFadeOutAndHide(@sprites)"]),
        __ec(655,0,["@sprites.each_value do |s|"]),
        __ec(655,0,["  s.dispose if s && !s.disposed?"]),
        __ec(655,0,["end"]),
        __ec(655,0,["@viewport.dispose if @viewport && !@viewport.disposed?"]),
        __ec(655,0,[""]),
        __ec(655,0,["@sprites = {}"]),
        __ec(655,0,["@viewport = nil"]),
        __ec(122,0,[4,4,0,0,0]),
        __ec(0,0,[])
      ]
      (MKXPDebug.log("[CEFIX-RECON] listas reconstruidas em memoria: #{$__ce_recon.keys.sort.inspect} (212=#{($__ce_recon[212]||[]).size} 34=#{($__ce_recon[34]||[]).size} 173=#{($__ce_recon[173]||[]).size} cmds)") rescue nil)
    rescue => _be
      (MKXPDebug.log("[CEFIX-RECON] FALHOU a construir: #{_be.class}: #{_be.message.to_s[0,100]}") rescue nil)
      $__ce_recon = {}
    end
    $__ce_recon
  end


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
        # ─── FIX-CANCELAR2 (re-aplicado no 1o frame de JOGO) ──────────────────
        # PORQUE: nesta binary o botao 3DS B chega como Input::X=14 (a tabela
        # input_3ds.cpp compilada mapeia KEY_B->RMXP_X, nao RMXP_B). O menu de
        # pausa Voltseon FECHA com `Input.trigger?(Input::BACK=12) ||
        # Input.trigger?(Input::ACTION=11)` (plugin 8966). Como o B fisico nunca
        # vira 12, o BACK nunca dispara -> o menu nao fecha com B. (O Y vira
        # ACTION=11 e fecha, mas o mapa TAMBEM abre o menu no ACTION -> toggle.)
        # O FIX-CANCELAR de topo do probe e' apagado pelo FIX-INPUT3 do binding,
        # que corre as t~3.8s e SUBSTITUI Input.trigger? pelo nativo. Aqui, no 1o
        # frame de jogo (muito depois de t=3.8s), re-aplicamos o remap POR CIMA do
        # trigger?/press?/repeat? ATUAIS: trigger?(12=BACK) responde tambem a'
        # tecla 14 (onde o B aterra). Robusto p/ a binary atual (B->14) E p/ a
        # recompilada (B->12, em que o `__fc2_trigger(k)` fallback ja' resolve).
        # Sem recursao: o alias __fc2_* aponta para o trigger? nativo, criado
        # ANTES da redefinicao. Idempotente: $__fc2_done + `unless method_defined?`.
        # (Fix permanente = 1 linha C++ ja' presente em proj_new: KEY_B->RMXP_B;
        #  basta recompilar limpo para dispensar este workaround.)
        unless $__fc2_done
          $__fc2_done = true
          begin
            if defined?(Input) && Input.respond_to?(:trigger?)
              # GENERICO (V33): NAO redefinir trigger?/press?/repeat?.
              # O C++ ja' mapeia KEY_B->RMXP_B=12 e implementa a semantica RGSS
              # CORRETA e GENERICA (trigger?=flanco 1x/toque, repeat?=delay+intervalo,
              # press?=mantido). O override Ruby abaixo era um remendo para o binario
              # ANTIGO (B->RMXP_X=14) e ESPECIFICO deste jogo/menu -- REMOVIDO. O input
              # fica 100% nativo do motor, igual para qualquer jogo RPG Maker XP.
              w("[FIX-CANCELAR2] DESATIVADO (input generico) -- trigger?/press?/repeat? nativos do C++ (B->12 ja' correto)")
            else
              w("[FIX-CANCELAR2] Input indisponivel no 1o frame")
            end
          rescue => _e
            w("[FIX-CANCELAR2] falhou: #{_e.class}: #{_e.message}")
          end
        end

        # ─── MENU-V17: reescrita COMPLETA do input do menu de pausa (prepend) ──
        # CAUSA RAIZ (provada pelo INLOG do log anterior): o trigger? do binario
        # comporta-se como repeat? -- trigger?(USE=13) auto-repetia a cada ~0.2s
        # enquanto o A estava premido -> o menu SELECIONAVA o item atual (BAG) sem
        # parar ("entra sempre na BAG sozinho"). E trigger?(BACK=12) NUNCA dava true
        # porque o B fisico mapeia para a tecla 14 nesta binary (B->RMXP_X=14), que
        # alias e' JUMPUP (mudar tema) -- por isso o B nem fechava. Os MENUCLOSE
        # antigos so' ADICIONAVAM um "fechar" mas chamavam o update original, que
        # mantinha o trigger?(USE) a auto-repetir -> a BAG continuava.
        # SOLUCAO: substituir o update por completo, lendo input com FLANCO PROPRIO
        # derivado de press? (nivel fiavel -- o d-pad prova que press? funciona):
        #   fechar    = flanco de 12 OU 14 (B em qualquer mapeamento) OU 11 (ACTION)
        #   selecionar= flanco de 13 (USE) -> 1x por toque, SEM auto-repeat
        #   navegar   = press? 4/6 (nivel, igual ao original)
        # Guarda de 1o frame: ignora teclas seguradas ao abrir (nao auto-fecha nem
        # auto-seleciona). Re-seed apos submenu: o B usado para sair da BAG nao
        # fecha o menu por engano. NAO chama super (controla o update todo).
        begin
          unless $__menuv17_defined
            $__menuv17_defined = true
            Object.const_set(:MenuCloseFix, Module.new do
              def update(*a, &b)
                @__vif_held ||= {}
                # DIAG: registar teclas premidas nas 1as chamadas -> revela o codigo do B
                begin
                  $__mv17_dbg = 0 if !@__vif_inited   # reset por SESSAO de menu (nova instancia)
                  $__mv17_dbg = ($__mv17_dbg || 0)
                  if $__mv17_dbg < 8
                    $__mv17_dbg += 1
                    pressed = []
                    (0..25).each { |kk| pressed << kk if (Input.press?(kk) rescue false) }
                    (MKXPDebug.log("[MENU-V17] update#{$__mv17_dbg} premidas=#{pressed.inspect}") rescue nil)
                  end
                rescue; end
                _p = {}
                [2,4,6,8,10,11,12,13,14,15,16,17,18].each { |k| _p[k] = (Input.press?(k) rescue false) }
                _first = !@__vif_inited
                @__vif_inited = true
                _e = {}
                [10,11,12,14,15,16,17,18].each do |k|
                  _e[k] = (_first ? false : (_p[k] && !@__vif_held[k]))
                  @__vif_held[k] = _p[k]
                end
                _e[13] = (_first ? false : (_p[13] && !@__vif_held[13])); @__vif_held[13] = _p[13]
                _exit = false
                # fechar = flanco de qualquer tecla de "voltar/cancelar" (B em qualquer mapeamento:
                # 11=ACTION,12=BACK,14/15/16/17/18). NAO inclui navegacao (2/4/6/8) nem USE(13=select).
                # FALLBACK ROBUSTO (V32): fechar tambem com trigger?(BACK)/trigger?(14) DIRETO. No
                # binario novo o B mapeia para 12 (BACK) e o trigger? comporta-se como repeat?
                # (auto-repeat) -- inofensivo para FECHAR (so' poe shouldExit=true). Gated a !_first
                # para nao fechar no 1o frame com uma tecla ainda premida ao abrir o menu.
                _bk = (!_first && ((Input.trigger?(12) rescue false) || (Input.trigger?(14) rescue false)))
                if _bk || _e[10] || _e[11] || _e[12] || _e[14] || _e[15] || _e[16] || _e[17] || _e[18]   # fechar
                  @menu.shouldExit = true if @menu && (@menu.respond_to?(:shouldExit=) rescue false)
                  ($PokemonTemp.last_menu_selection = @currentSelection rescue nil)
                  (MKXPDebug.log("[MENU-V17] fechar (B/BACK bk=#{_bk} e12=#{_e[12]} e14=#{_e[14]}) -> shouldExit=true") rescue nil)
                  return
                elsif _p[4]                             # LEFT (nivel, como o original)
                  shiftCursor(-1)
                elsif _p[6] && (@displayIndexes ? @displayIndexes.length > 1 : false)  # RIGHT
                  shiftCursor(1)
                elsif _e[13]                            # USE (flanco) -> selecionar 1x
                  (pbPlayDecisionSE rescue nil)
                  _ent = (@entries[@currentSelection] rescue nil)
                  _exit = (_ent ? _ent.selected(@menu) : false)
                  (calculateMenuEntries rescue nil)
                  (calculateDisplayIndex rescue nil)
                  (redrawMenuIcons rescue nil)
                  (calculateXPositions(true) rescue nil)
                  @shouldRefresh = true
                  ($PokemonTemp.last_menu_selection = @currentSelection rescue nil)
                  # re-seed apos voltar do submenu: o B/A usado la' nao gera flanco aqui
                  [10,11,12,13,14,15,16,17,18].each { |k| @__vif_held[k] = (Input.press?(k) rescue false) }
                  (MKXPDebug.log("[MENU-V17] selecionar entry #{(@currentSelection rescue '?')} (flanco USE) exit=#{_exit}") rescue nil)
                end
                # (JUMPUP/JUMPDOWN/mudar-tema omitidos: tecla 14 = B fisico = fechar)
                if @shouldRefresh && @menu && !@menu.shouldExit
                  (refreshMenu rescue nil)
                  (@menu.shouldRefresh = true rescue nil)
                  @shouldRefresh = false
                  (@sprites["leftarrow"].visible  = !(@entries.length == 1) rescue nil)
                  (@sprites["rightarrow"].visible = (@displayIndexes ? @displayIndexes.length > 1 : false) rescue nil)
                end
                @menu.shouldExit = _exit if @menu && (@menu.respond_to?(:shouldExit=) rescue false)
              end
            end)
          end
          # ── V34: RE-ATIVAR o prepend ──────────────────────────────────────
          # A remocao do hack (V33) NAO resolveu: o menu CONTINUA a nao fechar com
          # B (log: Bag bg.png recarrega varias vezes = continua a entrar). A hipotese
          # de que o trigger?(BACK) original + trigger? nativo bastava estava ERRADA --
          # ou o B nao mapeia para BACK no binario, ou Input::BACK!=12, ou o trigger?
          # nao da' flanco limpo aqui. O MenuCloseFix fecha com FLANCO PROPRIO derivado
          # de press? (fiavel -- o d-pad prova-o) para 12 OU 14 (B em qualquer
          # mapeamento) e loga 'premidas' p/ revelar o codigo real do B no proximo log.
          if defined?(VoltseonsPauseMenu) && !$__menuv17_applied
            $__menuv17_applied = true
            begin
              VoltseonsPauseMenu.prepend(MenuCloseFix)
              (MKXPDebug.log("[MENU-V17] RE-ATIVADO (V34) -- prepend MenuCloseFix em VoltseonsPauseMenu | fecha por flanco 12/14 + trigger?(12/14) | diag premidas ON") rescue nil)
            rescue => _pe
              (MKXPDebug.log("[MENU-V17] prepend FALHOU: #{_pe.class}: #{_pe.message}") rescue nil)
            end
          end
        rescue => _e
          (MKXPDebug.log("[MENU-V17] falhou: #{_e.class}: #{_e.message}") rescue nil)
        end

        # ─── OUTFIT-DIAG: tracar a cadeia da estante (CE#34 -> CE#173) e apanhar
        # o erro exato que impede a UI de aparecer. command_117 loga so' as CE da
        # cadeia da roupa (34/173/212/20/21/139). execute_script re-loga o erro
        # (o execute_script do Essentials ja' faz `raise err`, logo so' espelhamos
        # a mensagem para o log antes de re-lancar -- comportamento inalterado).
        unless $__outfit_diag_done
          $__outfit_diag_done = true
          begin
            if defined?(Interpreter)
              Interpreter.class_eval do
                unless method_defined?(:__od_command_117)
                  alias_method :__od_command_117, :command_117
                  def command_117
                    _id = (@parameters[0] rescue nil)
                    if _id && [34,173,212,20,21,139].include?(_id)
                      (MKXPDebug.log("[OUTFIT] CallCE ##{_id} (cadeia roupa) depth=#{@depth rescue '?'}") rescue nil)
                      # CE#34 e' chamado mas o corpo nunca corre (o filho nao executa
                      # o cmd 0=CallCE#212). Aqui registo o ESTADO EXATO do interpretador
                      # no momento da chamada -> diz que' flag bloqueia o filho. Tudo
                      # num so' log para nao inundar.
                      if _id == 34
                        begin
                          _st = "menu_calling=#{($game_temp.menu_calling rescue '?')} " \
                                "msg_wait=#{(@message_waiting rescue '?')} " \
                                "mrw=#{(@move_route_waiting rescue '?')} " \
                                "wait=#{(@wait_count rescue '?')} " \
                                "list=#{(@list ? @list.size : 'nil')} idx=#{(@index rescue '?')} " \
                                "depth=#{(@depth rescue '?')} main=#{(@main rescue '?')} " \
                                "ce34_data_ok=#{(!$data_common_events[34].nil? rescue '?')} " \
                                "pl_mrf=#{($game_player.move_route_forcing rescue '?')}"
                          (MKXPDebug.log("[OUTFIT] >> estado@CE34: #{_st}") rescue nil)
                        rescue; end
                      end
                    end
                    _r = (__od_command_117 rescue (raise))
                    # ── CEFIX: o Marshal do mruby carrega o objeto CommonEvent mas deixa a
                    #    @list a NIL (o ficheiro esta' 100% OK em Ruby normal -- 0 listas nil
                    #    em 257 CEs). Sem lista, o filho criado pelo command_117 nao corre NADA
                    #    -> foi exatamente o bug da estante (CE#34 chamado 13x, corpo nunca corre,
                    #    CE#212/#173 nunca aparecem). FIX: se o filho ficou com @list nil, injeto
                    #    a lista RECONSTRUIDA A MAO (ver bloco CEFIX-RECON no topo -- objetos
                    #    RPG::EventCommand construidos sem Marshal, round-trip verificado contra a
                    #    rxdata) e re-faco o setup do filho. Tambem corrijo a fonte
                    #    ($data_common_events[id].list) p/ as proximas chamadas. So' tenho a
                    #    reconstrucao de 34/173/212 (os CEs da estante); outros CEs com lista nil
                    #    sao registados no log (se algum for preciso, reconstruo-o tambem).
                    begin
                      _ci = (@child_interpreter rescue nil)
                      _cl = _ci ? (_ci.instance_variable_get(:@list) rescue nil) : nil
                      if _ci && _cl.nil? && _id
                        # @list nil (o Marshal do mruby falha a desserializar as listas com
                        # objetos complexos -- Tone/MoveRoute/AudioFile). Em vez de recarregar
                        # a rxdata inteira (mesmo Marshal -> daria nil na mesma + duplicava 257
                        # CEs na memoria, que ja' esta' sob pressao), uso a lista RECONSTRUIDA
                        # A MAO (so' existe p/ 34/173/212 -- os CEs da estante; round-trip OK).
                        _fresh = (((__build_ce_recon rescue nil) || {})[_id] rescue nil)
                        if _fresh.is_a?(Array) && _fresh.size > 0
                          ($data_common_events[_id].list = _fresh) rescue nil   # corrigir a fonte p/ as proximas
                          (_ci.setup(_fresh, (@event_id rescue 0)) rescue nil)  # re-armar o filho ja' criado
                          if (($__cefix_n ||= 0) < 60)
                            $__cefix_n += 1
                            (MKXPDebug.log("[CEFIX] CE##{_id} tinha @list nil -> lista RECONSTRUIDA a mao (#{_fresh.size} cmds), filho re-setup OK") rescue nil)
                          end
                        elsif (($__cefix_fail ||= 0) < 12)
                          $__cefix_fail += 1
                          (MKXPDebug.log("[CEFIX] CE##{_id}: @list nil e SEM reconstrucao a mao (so' tenho 34/173/212) -> se este CE for preciso, diz-me") rescue nil)
                        end
                      end
                    rescue => _cee; (MKXPDebug.log("[CEFIX] erro: #{_cee.class}: #{_cee.message.to_s[0,90]}") rescue nil); end
                    # Confirmar o estado FINAL do filho do CE#34 (depois do CEFIX)
                    if _id == 34
                      begin
                        _ci3 = (@child_interpreter rescue nil)
                        _cl3 = _ci3 ? (_ci3.instance_variable_get(:@list) rescue nil) : nil
                        (MKXPDebug.log("[OUTFIT] >> filho CE34 (final pos-CEFIX): criado=#{!_ci3.nil?} running=#{(_ci3.running? rescue '?')} cmds=#{(_cl3 ? _cl3.size : 'nil')}") rescue nil)
                      rescue; end
                    end
                    _r
                  end
                end
                unless method_defined?(:__od_execute_script)
                  alias_method :__od_execute_script, :execute_script
                  def execute_script(script)
                    begin
                      __od_execute_script(script)
                    rescue Exception => _e
                      raise if _e.is_a?(SystemExit) || "#{_e.class}" == "Reset"
                      if (($__od_errs ||= 0) < 30)
                        $__od_errs += 1
                        (MKXPDebug.log("[OUTFIT] Script ERRO: #{(_e.message rescue _e.class).to_s[0,240].gsub(/\s+/,' ')}") rescue nil)
                      end
                      raise
                    end
                  end
                end
              end
              (MKXPDebug.log("[OUTFIT-DIAG] hooks instalados (command_117 + execute_script) em Interpreter") rescue nil)
            else
              (MKXPDebug.log("[OUTFIT-DIAG] Interpreter ausente -- hooks NAO instalados") rescue nil)
            end
          rescue => _e
            (MKXPDebug.log("[OUTFIT-DIAG] falhou: #{_e.class}: #{_e.message}") rescue nil)
          end
        end

        # ─── FOLLOWGUARD: guard defensivo p/ o Following Pokemon EX (crashes B1/B2)
        # Em transicoes de mapa, DependentEventSprites#update pode por character_hue=
        # num realEvent FROZEN (B1: "cannot set ivar") ou .color.set num sprite
        # nil/disposto (B2: NoMethodError). NAO disparam no log atual, mas um frame
        # mau rebenta o ciclo de update (e o tratamento de excecao repetido causa
        # picos de FPS). Prepend envolve SO' o update do plugin (cosmetico: hue de
        # shiny + tint de status); a posicao do sprite ja' foi atualizada pelo
        # __followingpkmn__update ANTES, logo engolir o resto e' seguro. Gate por
        # per_frame ate' o plugin estar carregado (mesmo padrao do OUTFIT-DIAG).
        unless $__followguard_done
          begin
            if Object.const_defined?(:DependentEventSprites)
              $__followguard_done = true
              _fg = Module.new do
                def update(*a)
                  begin
                    $__fgdiag_n = ($__fgdiag_n || 0) + 1
                    if ($__fgdiag_n % 180) == 1
                      dep = ($PokemonGlobal.dependentEvents rescue nil)
                      dep = ($game_temp.dependentEvents rescue nil) if dep.nil?
                      pl  = ($game_player rescue nil)
                      sz  = (dep.respond_to?(:size) ? (dep.size rescue '?') : '?')
                      smp = ''
                      begin
                        if dep.is_a?(Array) && dep[0].is_a?(Array)
                          smp = dep.first(2).map { |e| "[#{(e[0] rescue '?')},#{(e[1] rescue '?')},x#{(e[2] rescue '?')},y#{(e[3] rescue '?')}]" }.join(' ')
                        end
                      rescue; end
                      (MKXPDebug.log("[FOLLOWDIAG] dependentEvents=#{sz} player@(#{(pl.x rescue '?')},#{(pl.y rescue '?')}) map=#{($game_map.map_id rescue '?')} | #{smp}") rescue nil)
                    end
                  rescue; end
                  super
                rescue => _e
                  if (($__fg_errs ||= 0) < 20)
                    $__fg_errs += 1
                    (MKXPDebug.log("[FOLLOWGUARD] update Following Pkmn engolido (transiente): #{_e.class}: #{_e.message.to_s[0,120].gsub(/\s+/,' ')}") rescue nil)
                  end
                  nil
                end
              end
              DependentEventSprites.prepend(_fg)
              (MKXPDebug.log("[FOLLOWGUARD] guard instalado em DependentEventSprites#update (B1/B2 nunca rebentam um frame)") rescue nil)
            end
          rescue => _e
            $__followguard_done = true
            (MKXPDebug.log("[FOLLOWGUARD] falhou: #{_e.class}: #{_e.message}") rescue nil)
          end
        end

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
          (SUPERDBG.tick(self) rescue nil) # SUPERDBG: snapshot periodico + trainer + input
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
# [SUPERDBG] INSTRUMENTACAO PERIODICA -- DEFINIDO CEDO
# -----------------------------------------------------------------------------
#  (Antes estava no FIM do ficheiro e nao corria -- a cauda ficava fora do fluxo
#   executado. Aqui, na regiao que comprovadamente corre, fica garantido.)
#  Corre via SUPERDBG.tick(scene) no hook Scene_Map#update. Caps + dedup p/ nao
#  pendurar: [SNAP] cada 60 frames (mapa/jogador/tileset/sprites/bitmaps/memoria),
#  [TRAINERTRACE] so' quando o $Trainer muda, [INPUTTRACE] botoes premidos.
#  As mensagens de instalacao falham no load (MKXPDebug ainda nao existe), por
#  isso ha um CANARIO no 1o tick: [SUPERDBG] ATIVO.
# =============================================================================
begin
  $__sd = { snap_lastf: -999, tsig: nil, inlast: nil, in_n: 0, hello: false }
  module SUPERDBG
    # SNAP_EVERY: o snap corre 3x ObjectSpace.each_object (Sprite/Bitmap/Viewport)
    # -> com ~230K objetos vivos isso e' ~690K visitas por snap = hitch visivel.
    # Subido de 60 -> 300 frames (corta o hitch 5x; o snapshot de memoria/estado
    # continua a sair regularmente, so' menos vezes).
    SNAP_EVERY = 300
    CAP_IN     = 5000

    def self.snap_tick(scene)
      begin
        unless $__sd[:hello]
          $__sd[:hello] = true
          (MKXPDebug.log("[SUPERDBG] ATIVO (1o tick) mem_info=#{(MKXPDebug.mem_info rescue 'n/a (sem export C++)')}") rescue nil)
        end
        fc = (Graphics.frame_count rescue -1)
        return if (fc - ($__sd[:snap_lastf] || -999)).abs < SNAP_EVERY
        $__sd[:snap_lastf] = fc
        # CAUSA DO LAG PERIODICO (provado pelo log): cada SNAP fazia 3x
        # ObjectSpace.each_object (Sprite/Bitmap/Viewport) a percorrer um heap de
        # ~58MB -> CADA snap CONGELAVA o jogo ~6 SEGUNDOS (de 60 em 60 frames). O
        # mem_info (export C++, barato) ja' traz texturas/bitmaps. Por isso o
        # each_object passa a estar DESLIGADO por omissao; so' corre se o utilizador
        # ligar $MKXP_SNAP_DEEP = true (diagnostico pontual de fugas de objetos).
        if $MKXP_SNAP_DEEP == true
          nspr = (ObjectSpace.each_object(Sprite){} rescue -1)
          nbmp = (ObjectSpace.each_object(Bitmap){} rescue -1)
          nvp  = (ObjectSpace.each_object(Viewport){} rescue -1)
        else
          nspr = 'off'; nbmp = 'off'; nvp = 'off'   # contagem cara desligada (sem freeze)
        end
        mid  = ($game_map ? ($game_map.map_id rescue -1) : -1)
        tn   = ($game_map ? ($game_map.tileset_name rescue '?') : '?')
        px   = ($game_player ? "#{($game_player.x rescue '?')},#{($game_player.y rescue '?')}" : '?')
        mem  = (MKXPDebug.mem_info rescue nil)
        mems = mem ? " | #{mem}" : ""
        (MKXPDebug.log("[SNAP] frame=#{fc} map=#{mid} player=(#{px}) tileset='#{tn}' sprites=#{nspr} bitmaps=#{nbmp} viewports=#{nvp}#{mems}") rescue nil)
      rescue; end
    end

    def self.trainer_tick
      begin
        t = $Trainer
        return unless t
        sig = "#{(t.trainer_type.to_s rescue '?')}|#{(t.outfit rescue '?')}|#{(t.name.to_s rescue '?')}|#{(t.party.size rescue '?')}"
        if sig != $__sd[:tsig]
          $__sd[:tsig] = sig
          (MKXPDebug.log("[TRAINERTRACE] #{sig} (frame=#{Graphics.frame_count rescue -1})") rescue nil)
        end
      rescue; end
    end

    def self.input_tick
      begin
        return if ($__sd[:in_n] || 0) >= CAP_IN
        keys = []
        begin
          { "C"=>Input::C, "B"=>Input::B, "A"=>Input::A,
            "UP"=>Input::UP, "DOWN"=>Input::DOWN, "LEFT"=>Input::LEFT, "RIGHT"=>Input::RIGHT }.each do |nm,k|
            keys << nm if (Input.press?(k) rescue false)
          end
        rescue; end
        sig = keys.join("+")
        if sig != "" && sig != $__sd[:inlast]
          $__sd[:in_n] = ($__sd[:in_n] || 0) + 1
          (MKXPDebug.log("[INPUTTRACE] #{sig} (frame=#{Graphics.frame_count rescue -1})") rescue nil)
        end
        $__sd[:inlast] = sig
      rescue; end
    end

    def self.tick(scene)
      snap_tick(scene)
      trainer_tick
      input_tick
    end
  end
rescue
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
            # TILEPROF: mede o custo REAL do compose do tilemap (blitVWrappedPixels)
            # por frame -- o RUBYPROF nao instrumenta isto. Loga ms/frame a cada
            # 120 frames. CORRIGIDO: usa GLOBAIS nil-safe (no mruby, defined?(@@x)
            # NAO protege variaveis de classe -- rebentava NameError e partia o
            # cache do tilemap -> 1fps). E um rescue interno: a medicao NUNCA pode
            # rebentar o refreshLayer0.
            __tp0 = (__mkxp_ms rescue nil)
            __tpr = __tmdbg_refreshLayer0(autotiles)
            if __tp0
              begin
                __dt = (__mkxp_ms rescue __tp0) - __tp0
                $__tp_acc   = ($__tp_acc || 0.0) + __dt
                $__tp_calls = ($__tp_calls || 0) + 1
                __fc = (Graphics.frame_count rescue 0)
                $__tp_f0 ||= __fc
                if (__fc - $__tp_f0) >= 120
                  __frames = __fc - $__tp_f0
                  __frames = 1 if __frames < 1
                  (MKXPDebug.log("[TILEPROF] tilemap compose = #{($__tp_acc/__frames).round(2)}ms/frame " \
                    "(total #{$__tp_acc.round(1)}ms / #{__frames}f / #{$__tp_calls} chamadas refreshLayer0)") rescue nil)
                  $__tp_acc = 0.0; $__tp_calls = 0; $__tp_f0 = __fc
                end
              rescue => __tpe
                # medicao falhou -> ignora em silencio, NAO rebenta o tilemap
              end
            end
            return __tpr
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
        # J1 (PERFCUT) — durante uma MENSAGEM/cutscene o jogador esta' parado e a
        # camara fixa; os NPCs nao precisam do update completo (efeitos de overworld:
        # pegadas/sombras/bolhas/pathfinding) a cada frame. Mete os NAO-jogador a
        # meio-ritmo (frames impares saltam o update pesado, so' sincronizam x/y).
        # Ataca o pior FPS medido (cutscenes). Player sempre a full. $MKXP_PERFCUT=false desliga.
        if $MKXP_PERFCUT != false
          begin
            $__pc_f = (($__pc_f || 0) + 1)
            if ($__pc_f & 1) == 1 && ($game_temp && ($game_temp.message_window_showing rescue false))
              ch = (@character rescue nil)
              is_pl = ((defined?(Game_Player) && ch.is_a?(Game_Player)) ||
                       (defined?($game_player) && ch && ch.equal?($game_player)))
              unless is_pl
                if ch
                  ((self.x = ch.screen_x) rescue nil) if (ch.respond_to?(:screen_x) rescue false)
                  ((self.y = ch.screen_y) rescue nil) if (ch.respond_to?(:screen_y) rescue false)
                end
                return nil
              end
            end
          rescue; end
        end
        __fxguard_orig_update
      rescue => e
        unless $__fxguard_logged
          (MKXPDebug.log("[FXGUARD] efeito EBDX em Sprite_Character#update rebentou: #{e.class}: #{e.message} -- ressincronizo o sprite core") rescue nil)
          (e.backtrace || [])[0, 8].each_with_index { |ln, i| (MKXPDebug.log("[FXGUARD]   bt[#{i}] #{ln}") rescue nil) }
          $__fxguard_logged = true
        end
        # CORE-SYNC (Bug 1 das escadas): o crash do efeito abortou o update ANTES de
        # sincronizar posicao/z/opacidade/visibilidade -> o sprite (player incluido)
        # congelava ou desaparecia. Repor o essencial a partir do @character.
        begin
          ch = (@character rescue nil)
          if ch
            ((self.x       = ch.screen_x)        rescue nil) if (ch.respond_to?(:screen_x) rescue false)
            ((self.y       = ch.screen_y)        rescue nil) if (ch.respond_to?(:screen_y) rescue false)
            ((self.z       = ch.screen_z)        rescue nil) if (ch.respond_to?(:screen_z) rescue false)
            ((self.opacity = ch.opacity)         rescue nil) if (ch.respond_to?(:opacity)  rescue false)
            ((self.visible = !ch.transparent)    rescue nil) if (ch.respond_to?(:transparent) rescue false)
            # diagnostico: se for o PLAYER, loga z/op/vis/always_on_top (rate-limited)
            begin
              is_pl = ((defined?(Game_Player) && ch.is_a?(Game_Player)) ||
                       (defined?($game_player) && ch.equal?($game_player)))
              if is_pl
                $__fxg_pl_n = (($__fxg_pl_n || 0) + 1)
                if $__fxg_pl_n <= 6
                  aot = (ch.instance_variable_get(:@always_on_top) rescue '?')
                  (MKXPDebug.log("[FXGUARD|PLAYER] crash no sprite do PLAYER -> ressincronizado: z=#{(self.z rescue '?')} op=#{(self.opacity rescue '?')} vis=#{(self.visible rescue '?')} always_on_top=#{aot} map=#{($game_map.map_id rescue '?')}") rescue nil)
                end
              end
            rescue; end
          end
        rescue; end
        nil
      end
    end
    (MKXPDebug.log("[FXGUARD] Sprite_Character#update protegido (efeitos EBDX nao matam o spriteset)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[FXGUARD] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  MOVEWATCH  —  diagnostico do Bug 2 ("nao me mexo apos sair da casa")  [V22]
# -----------------------------------------------------------------------------
#  O log V21 prova que na Map43 (New Bark Town) NAO ha' evento preso na 2a visita
#  e o FPS estabiliza a ~30 (a lentidao e' o tileset de 30MB). Mas falta saber se
#  o INPUT chega ao player ou se ha' um bloqueio escondido. Este watch loga, ~1x/seg,
#  os flags que TRAVAM o movimento no Game_Player#update. Se todos = false e o
#  player na' mesmo nao anda -> e' so' performance (C++). Se algum fica preso em
#  true -> e' bug de controlo (Ruby-corrigivel). Puramente observacional, rescue total.
# =============================================================================
begin
  if defined?(Game_Player) && Game_Player.method_defined?(:update) && !$__movewatch_installed
    $__movewatch_installed = true
    class Game_Player
      alias_method :__movewatch_orig_update, :update
      def update(*a)
        begin
          $__mw_f = (($__mw_f || 0) + 1)
          if ($__mw_f % 60) == 0
            mid = ($game_map.map_id rescue -1)
            # so' loga em mapas pesados/relevantes p/ nao poluir (Map43 e vizinhos)
            interp = (($game_system.map_interpreter.running? rescue ($game_map.interpreter.running? rescue nil)))
            msg    = ($game_temp.message_window_showing rescue nil)
            xfer   = ($game_temp.player_transferring rescue nil)
            mrf    = (@move_route_forcing rescue nil)
            mov    = (self.moving? rescue nil)
            blocked = (interp == true) || (msg == true) || (xfer == true) || (mrf == true)
            (MKXPDebug.log("[MOVEWATCH] map=#{mid} TRAVADO=#{blocked} | interp_running=#{interp} msg=#{msg} transferring=#{xfer} move_route_forcing=#{mrf} moving=#{mov} transparent=#{(@transparent rescue '?')} x,y=#{(@x rescue '?')},#{(@y rescue '?')}") rescue nil)
          end
          # PASSDBG-MOVE: ao mudar de tile, logar o que o jogador PISA (id + passabilidade
          # de cada camada). Se uma parede mostrar pass=0/pri=0 -> e' por isso que se anda
          # la' por cima. Limitado a 30 logs p/ nao pesar.
          begin
            px = (@x rescue nil); py = (@y rescue nil)
            if px && py && (px != $__pdm_lx || py != $__pdm_ly)
              $__pdm_lx = px; $__pdm_ly = py
              $__pdm_n = ($__pdm_n || 0) + 1
              if $__pdm_n <= 30
                gmm = $game_map
                pas = gmm.instance_variable_get("@passages")
                pri = gmm.instance_variable_get("@priorities")
                ter = gmm.instance_variable_get("@terrain_tags")
                lay = [2, 1, 0].map do |z|
                  tid = (gmm.data[px, py, z] rescue '?')
                  "z#{z}=id#{tid}(pass#{(pas[tid] rescue '?')},pri#{(pri[tid] rescue '?')},ter#{(ter[tid] rescue '?')})"
                end.join(" ")
                (MKXPDebug.log("[PASSDBG-MOVE] map=#{(gmm.map_id rescue '?')} @(#{px},#{py}) #{lay}") rescue nil)
              end
            end
          rescue; end
        rescue; end
        __movewatch_orig_update(*a)
      end
    end
    (MKXPDebug.log("[MOVEWATCH] hook instalado em Game_Player#update (loga flags de bloqueio ~1x/seg)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[MOVEWATCH] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
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
#  TSCOMPACT-RT — compactacao de TILESETS GIGANTES em RUNTIME (jogo INTACTO)
# -----------------------------------------------------------------------------
# PROBLEMA (provado): 25 tilesets (Gen4 Outside/Interior/Cave) sao 256x~31000 =
# ~30MB cada. Carregadas esgotam a RAM -> sem memoria p/ bitmaps de tiles/sprites
# -> MAGENTA + full-GC constante -> 15fps. Cada mapa so' usa ~5% dos tiles.
# SOLUCAO (runtime, sem tocar nos dados do jogo): ao receber um tileset gigante,
#   1) recolhe os tiles que ESTE mapa usa (chao + graficos de eventos),
#   2) constroi uma bitmap pequena so' com esses (blt) + remap old->novo,
#   3) remapeia EM MEMORIA o $game_map.data + copias locais de passages/
#      priorities/terrain_tags (chao no indice novo; eventos no indice original),
#   4) liberta o original (~15MB 4444) e usa a compactada,
#   5) redireciona pbGetTileBitmap (moveis) p/ a compactada,
#   6) cacheia por map_id (nao reconstroi ao revisitar nesta sessao).
# Tudo em rescue: se algo falhar, cai no comportamento original (nunca pior).
# Idempotente: marca a Table de dados (@__tsc_rm) p/ nao remapear 2x.
# NOTA: cache em DISCO (persistir entre arranques) precisa de gravar bitmap->PNG,
# que e' so' C++ -> proximo passo. Por agora reconstroi 1x por mapa por sessao.
$__tscompact_rt = true
begin
  if Object.const_defined?(:CustomTilemap) && !$__tscompactrt_installed
    $__tscompactrt_installed = true
    $__tsc_cache ||= {}   # map_id => {bmp:, remap:(Hash old=>new), us:(Array old ordenado)}

    # helper: construir Table 1D a partir de um array
    def __tsc_mk_table(arr)
      t = Table.new(arr.size)
      # BUG no C++ (binding_3ds.cpp Table#[]=): a forma 1D `t[i]=v` e' interpretada
      # como `[]=(i,v)` e poe y=v (o valor!) -> idx=v*@x+i (fora dos limites) -> a
      # escrita e' DESCARTADA. Por isso `t[i]=arr[i]` nao escrevia nada e a tabela
      # ficava a zero (paredes passaveis). Solucao hot-load: escrever @data DIRETO.
      # A leitura Table#[] le @data (confirmado), logo o jogo le os valores certos.
      t.instance_variable_set(:@data, arr.map { |v| (v || 0).to_i })
      t
    end
    # altura de coluna POT (igual ao VWrap::TILESET_HEIGHT do motor: max_size - max_size%32)
    def __tsc_th
      th = (Bitmap.max_size rescue 1024) || 1024
      th -= (th % 32)
      th = 1024 if th <= 0
      th
    end
    # ler 1 tile (indice compactado k) da bitmap EMBRULHADA p/ dentro de dest@(0,0)
    def __tsc_blit_tile(dest, src, k)
      th = __tsc_th
      cy = (k >> 3) * 32
      col = cy / th
      dest.blt(0, 0, src, Rect.new(col * 256 + (k & 7) * 32, cy % th, 32, 32))
    end
    # helper: construir bitmap compactada JA EMBRULHADA em colunas de __tsc_th (POT, <=1024)
    # -> evita criar 256x1504 (que vira textura 2048 e falha) e alinha com o blitVWrappedPixels
    # do motor (que usa o mesmo TILESET_HEIGHT). Marca never_dispose p/ a eviccao nao a libertar.
    def __tsc_build(src, used_sorted)
      th = __tsc_th
      total_rows = [(used_sorted.size + 7) / 8, 1].max
      ncols = [((total_rows * 32) + th - 1) / th, 1].max
      # CRITICO: criar como BitmapWrapper (subclasse de Bitmap COM never_dispose).
      # Uma Bitmap.new simples e' evictada sob pressao de RAM -> @tileset DISPOSTO -> preto.
      dst = nil
      if Object.const_defined?(:BitmapWrapper)
        dst = (BitmapWrapper.new(256 * ncols, th) rescue nil)
      end
      dst ||= Bitmap.new(256 * ncols, th)
      (dst.never_dispose = true) rescue nil   # dispose() vira no-op (super so' se !never_dispose)
      # NOTA: never_dispose=true ja' liga prefer_vram automaticamente (fix no motor,
      # binding_3ds.cpp) -> a textura do tileset vai para VRAM, libertando o heap
      # linear p/ a camada de 4MB. Nao e' preciso marcar prefer_vram aqui.
      (dst.addRef) rescue nil                 # refcount>=1, defesa extra contra eviccao
      remap = {}
      used_sorted.each_with_index do |old, k|
        remap[old] = 384 + k
        i = old - 384
        cy = (k >> 3) * 32
        col = cy / th
        dst.blt(col * 256 + (k & 7) * 32, cy % th, src, Rect.new((i & 7) * 32, (i >> 3) * 32, 32, 32))
      end
      [dst, remap, ncols]
    end


    class CustomTilemap
      unless method_defined?(:__tsc_orig_tileset_eq)
        alias_method :__tsc_orig_tileset_eq, :tileset=
        def tileset=(value)
          gm  = ($game_map rescue nil)
          seq = ($__tsc_seq = (($__tsc_seq || 0) + 1))
          verbose = (seq <= 20)
          # ---- TRACE entrada: o que o setter recebe + thresholds ----
          gw = nil; gh = nil; giant = false; mid_l = 'nil-gm'
          begin
            gw = (value.width rescue nil); gh = (value.height rescue nil)
            d4 = (value.respond_to?(:native_4444?) ? (value.native_4444? rescue '?') : 'n/a')
            ds = (value.disposed? rescue '?')
            mid_l = (gm ? (gm.map_id rescue '?') : 'nil-gm')
            giant = (gh.is_a?(Integer) && gh > 8192 && gw == 256)
            if verbose || giant
              MKXPDebug.log("[TSC #{seq}] ENTRADA (#{gw}x#{gh} 4444=#{d4} disp=#{ds}) map=#{mid_l} | thr_h=8192 thr_w=256 => GIGANTE?=#{giant}")
            end
          rescue => e
            (MKXPDebug.log("[TSC #{seq}] erro-trace: #{e.class}: #{e.message}") rescue nil)
          end
          # ---- EARLY-RETURN: ja' compactado e em uso ----
          begin
            if gm && (ex = $__tsc_cache[gm.map_id]) && @tileset && @tileset.equal?(ex[:bmp])
              (MKXPDebug.log("[TSC #{seq}] EARLY-RETURN map=#{mid_l} (@tileset ja'==cache #{ex[:bmp].width}x#{ex[:bmp].height})") rescue nil) if verbose
              return ex[:bmp]
            end
          rescue; end
          # ---- CAMINHO PRINCIPAL (gigantes 256xN OU compactada-ja'-em-cache) ----
          begin
            _compcached = (gm && $__tsc_cache[gm.map_id]) ? true : false
            if gm && ( _compcached ||
               (value && !(value.disposed? rescue true) && value.respond_to?(:height) &&
                value.height > 8192 && value.width == 256) )
              mid = gm.map_id
              cache = $__tsc_cache[mid]
              if cache.nil?
                t0 = (Time.now rescue nil)
                used = {}; nchao = 0; nev = 0
                (gm.data.data || []).each { |v| if v && v >= 384 then used[v] = true; nchao += 1 end }
                (gm.events || {}).each_value do |ev|
                  (ev.pages || []).each { |pg| t = (pg.graphic.tile_id rescue 0) || 0; if t >= 384 then used[t] = true; nev += 1 end }
                end
                us = used.keys.sort
                maxold = (us.last || 0)
                th = __tsc_th
                rows = [(us.size + 7) / 8, 1].max
                (MKXPDebug.log("[TSC #{seq}] BUILD map=#{mid} | usados_unicos=#{us.size} (chao_ocorr=#{nchao} evt_ocorr=#{nev}) | id_orig_max=#{maxold} | th_col=#{th} rows=#{rows}") rescue nil)
                raw, remap, ncols = __tsc_build(value, us)
                cache = $__tsc_cache[mid] = { bmp: raw, remap: remap, us: us, ncols: ncols }
                peso_o = (gw.to_i * gh.to_i * 2)
                rw = (raw.width rescue nil); rh = (raw.height rescue nil)
                peso_n = ((rw || 0).to_i * (rh || 0).to_i * 4)
                dt = (t0 ? ((Time.now - t0) * 1000).round : '?') rescue '?'
                (MKXPDebug.log("[TSC #{seq}] OPT map=#{mid} | #{gw}x#{gh}@2B=#{peso_o / 1024}KB -> EMBRULHADA #{rw}x#{rh}@4B=#{peso_n / 1024}KB (#{ncols}col) | nd=#{(raw.never_dispose rescue '?')} disp=#{(raw.disposed? rescue '?')} | build=#{dt}ms") rescue nil)
                # ── TSC-PIXDBG: os tiles ALTOS (Y alto na fonte = casas/arvores) foram
                # mesmo copiados para o tileset compactado? Le alpha (max de 5 pontos) na
                # FONTE (256x31040 4444) e no COMPACTADO (posicao remapeada). Se src>0 mas
                # cmp=0 => o blt de Y-alto falhou silenciosamente (= casas invisiveis).
                begin
                  pts = [[16,16],[8,8],[24,24],[8,24],[24,8]]
                  amostra = (us.last(5) + us.first(3))
                  partes = []
                  amostra.each do |old|
                    i = old - 384
                    sxb = (i & 7) * 32; syb = (i >> 3) * 32
                    asrc = -1; pts.each { |p| vv=(value.get_pixel(sxb+p[0], syb+p[1]).alpha rescue -1); asrc=vv if vv>asrc }
                    nid = remap[old]; k = nid - 384; thh = __tsc_th
                    cyy = (k >> 3) * 32; ccol = cyy / thh
                    cxb = ccol * 256 + (k & 7) * 32; cyb = cyy % thh
                    acmp = -1; pts.each { |p| vv=(raw.get_pixel(cxb+p[0], cyb+p[1]).alpha rescue -1); acmp=vv if vv>acmp }
                    partes.push("#{old}(Y#{syb})s#{asrc.to_i}/c#{acmp.to_i}")
                  end
                  (MKXPDebug.log("[TSC-PIXDBG] map=#{mid} [5 ALTOS + 3 CHAO] #{partes.join(' ')} (sN/cN=alpha fonte/compactado; s>0 & c=0 => blt Y-alto FALHOU)") rescue nil)
                rescue => _pe
                  (MKXPDebug.log("[TSC-PIXDBG] map=#{mid} erro: #{_pe.class}: #{_pe.message}") rescue nil)
                end
              else
                (MKXPDebug.log("[TSC #{seq}] CACHE-HIT map=#{mid} (compactada #{cache[:bmp].width}x#{cache[:bmp].height})") rescue nil) if verbose
              end
              remap = cache[:remap]
              d = gm.data
              done_obj = (gm.instance_variable_get(:@__tsc_data_done) rescue nil)
              if !(done_obj.equal?(d) rescue false)
                evmax = 0
                (gm.events || {}).each_value { |ev| (ev.pages || []).each { |pg| t = (pg.graphic.tile_id rescue 0) || 0; evmax = t if t > evmax } }
                newsz = [384 + cache[:us].size, evmax + 1].max
                ntabs = {}
                # ── FIX V32: ler as tabelas da FONTE CORRETA ──────────────────────────
                # gm.@passages vem ZERADO para tilesets gigantes (provado pelo log: o
                # $data_tilesets[tsid].passages tem 2459 paredes e data.length=8144, MAS
                # a tabela reconstruida a partir de gm.@passages dava npass_nao_zero=0).
                # Lemos $data_tilesets[tsid] DIRETAMENTE (confirmado correto no runtime,
                # no mesmo instante) e so' caimos para gm.@attr se faltar.
                _tsid  = (gm.instance_variable_get(:@map).tileset_id rescue (gm.tileset_id rescue nil))
                _srcts = (_tsid ? ($data_tilesets[_tsid] rescue nil) : nil)
                %w[passages priorities terrain_tags].each do |attr|
                  src  = (_srcts ? (_srcts.send(attr) rescue nil) : nil)
                  gmv  = gm.instance_variable_get("@#{attr}")
                  orig = src || gmv
                  next unless orig
                  if attr == "passages" && seq <= 14
                    sl = (src ? (src.data.length rescue '?') : 'nil'); gl = (gmv ? (gmv.data.length rescue '?') : 'nil')
                    snz = 0; (src.data.each { |v| snz += 1 if (v || 0) != 0 } rescue nil) if src
                    gnz = 0; (gmv.data.each { |v| gnz += 1 if (v || 0) != 0 } rescue nil) if gmv
                    same = (src && gmv && (src.equal?(gmv) rescue false)) ? 'SIM' : 'NAO'
                    (MKXPDebug.log("[TSC-ORIGDBG] tsid=#{_tsid} $data_tilesets(len=#{sl},nz=#{snz}) vs gm.@passages(len=#{gl},nz=#{gnz}) mesmo_obj=#{same} -> uso=#{src ? '$data_tilesets' : 'gm'}") rescue nil)
                  end
                  arr = Array.new(newsz, 0)
                  (0...384).each { |i| arr[i] = (orig[i] rescue 0) || 0 }
                  cache[:us].each_with_index { |old, k| arr[384 + k] = (orig[old] rescue 0) || 0 }
                  (gm.events || {}).each_value { |ev| (ev.pages || []).each { |pg| t = (pg.graphic.tile_id rescue 0) || 0; (arr[t] = (orig[t] rescue 0) || 0) if t >= 384 && t < newsz } }
                  ntabs[attr] = __tsc_mk_table(arr)
                end
                # CRITICO: remapear o data via []= NATIVO (o @data ivar NAO afeta a Table nativa)
                xs = d.xsize; ys = d.ysize; zs = d.zsize
                cnt = 0; vmax_old = 0; vmax_new = 0
                (0...xs).each do |x|
                  (0...ys).each do |y|
                    (0...zs).each do |z|
                      v = d[x, y, z]
                      if v && v >= 384 && (nv = remap[v])
                        d[x, y, z] = nv; cnt += 1
                        vmax_old = v if v > vmax_old
                        vmax_new = nv if nv > vmax_new
                      end
                    end
                  end
                end
                ntabs.each { |attr, tb| gm.instance_variable_set("@#{attr}", tb) }
                (gm.instance_variable_set(:@__tsc_data_done, d) rescue nil)
                (MKXPDebug.log("[TSC #{seq}] REMAP-DATA map=#{mid} | celulas=#{xs}x#{ys}x#{zs}=#{xs * ys * zs} remapeadas=#{cnt} | id_max #{vmax_old}->#{vmax_new} | tabelas newsz=#{newsz} evmax=#{evmax}") rescue nil)
                # ── TSC-PASSDBG: confirmar que passabilidade/prioridade ficaram corretas ──
                # Compara, para algumas tiles usadas, o valor ORIGINAL ($data_tilesets, indexado
                # pelo id ANTIGO) vs o NOVO (@passages, indexado pelo id compactado). Se
                # orig!=0 mas novo==0 -> a escrita falhou. Se orig==0 em TUDO -> a Table original
                # nao desserializou (problema Marshal mruby, como os CommonEvents).
                begin
                  if seq <= 14
                    tsid = (gm.instance_variable_get(:@map).tileset_id rescue (gm.tileset_id rescue nil))
                    ots  = (tsid ? $data_tilesets[tsid] : nil)
                    op   = (ots.passages rescue nil)
                    opr  = (ots.priorities rescue nil)
                    np   = gm.instance_variable_get("@passages")
                    npr  = gm.instance_variable_get("@priorities")
                    nzero = 0; (0...[np.xsize, 999].min).each { |i| nzero += 1 if (np[i] rescue 0) != 0 } rescue nil
                    smp = cache[:us].first(6).map do |old|
                      kk = cache[:us].index(old); nid = 384 + kk
                      "#{old}/#{nid}:pass#{(op ? (op[old] rescue '?') : '?')}->#{(np[nid] rescue '?')},pri#{(opr ? (opr[old] rescue '?') : '?')}->#{(npr[nid] rescue '?')}"
                    end.join(" ")
                    (MKXPDebug.log("[TSC-PASSDBG] map=#{mid} tsid=#{tsid} npass_nao_zero=#{nzero} | #{smp}") rescue nil)
                    # NOVO V31: estado REAL da Table original do $data_tilesets.
                    # data.length vs xsize -> se length<xsize, o marshal TRUNCOU o array.
                    # ult_nz -> ultimo indice nao-zero (mostra ate' onde ha dados validos).
                    # Isto distingue: marshal trunca array (length curto) vs valores perdidos.
                    od = (op.data rescue nil)
                    odr = (opr.data rescue nil)
                    if od
                      ult_nz = -1; (0...od.length).each { |i| ult_nz = i if (od[i] || 0) != 0 }
                      cnt_nz = 0; od.each { |v| cnt_nz += 1 if (v || 0) != 0 }
                      (MKXPDebug.log("[TSC-PASSDBG-RAW] tsid=#{tsid} PASSAGES: data.length=#{od.length} xsize=#{(op.xsize rescue '?')} | nao_zero=#{cnt_nz} ult_indice_nz=#{ult_nz} (ficheiro espera ~8124 p/ ts28)") rescue nil)
                    end
                    if odr
                      ult_nzr = -1; (0...odr.length).each { |i| ult_nzr = i if (odr[i] || 0) != 0 }
                      cnt_nzr = 0; odr.each { |v| cnt_nzr += 1 if (v || 0) != 0 }
                      (MKXPDebug.log("[TSC-PASSDBG-RAW] tsid=#{tsid} PRIORITIES: data.length=#{odr.length} xsize=#{(opr.xsize rescue '?')} | nao_zero=#{cnt_nzr} ult_indice_nz=#{ult_nzr}") rescue nil)
                    end
                  end
                rescue => _pe
                  (MKXPDebug.log("[TSC-PASSDBG] erro: #{_pe.class}: #{_pe.message.to_s[0,80]}") rescue nil)
                end
              else
                (MKXPDebug.log("[TSC #{seq}] REMAP-SKIP map=#{mid} (data ja' remapeado, mesmo objeto Table)") rescue nil) if verbose
              end
              # NAO dispomos o gigante: a TSCACHE C++ (max 1) gere-o. @tileset aponta p/ a compactada.
              @tileset = cache[:bmp]
              @tilesetchanged = true
              (MKXPDebug.log("[TSC #{seq}] SET @tileset=#{(cache[:bmp].width rescue '?')}x#{(cache[:bmp].height rescue '?')} disp=#{(cache[:bmp].disposed? rescue '?')} nd=#{(cache[:bmp].never_dispose rescue '?')} | FIM-OK map=#{mid}") rescue nil) if verbose
              return cache[:bmp]
            else
              if verbose || giant
                porque = []
                porque << 'value-nil' if value.nil?
                porque << 'disposto' if (value && (value.disposed? rescue false))
                porque << "h=#{gh}(<=8192)" if !(gh.is_a?(Integer) && gh > 8192)
                porque << "w=#{gw}(!=256)" if gw != 256
                porque << 'sem-game_map' if gm.nil?
                (MKXPDebug.log("[TSC #{seq}] SKIP-NAO-GIGANTE map=#{mid_l} motivo=[#{porque.join(', ')}] -> setter ORIGINAL do motor") rescue nil)
              end
            end
          rescue => _e
            (MKXPDebug.log("[TSC #{seq}] FALHOU map=#{mid_l}: #{_e.class}: #{_e.message}") rescue nil)
          end
          __tsc_orig_tileset_eq(value)
        end
      end
    end

    # redirecionar pbGetTileBitmap (graficos de eventos / moveis) p/ a compactada
    if Object.method_defined?(:pbGetTileBitmap)
      class Object
        unless method_defined?(:__tsc_orig_pbgettile)
          alias_method :__tsc_orig_pbgettile, :pbGetTileBitmap
          def pbGetTileBitmap(filename, tile_id, hue, width = 1, height = 1)
            begin
              gm = ($game_map rescue nil)
              if gm && tile_id && tile_id >= 384 && width == 1 && height == 1 &&
                 (c = $__tsc_cache[gm.map_id]) && (nid = c[:remap][tile_id])
                i = nid - 384
                bmp = Bitmap.new(32, 32)
                __tsc_blit_tile(bmp, c[:bmp], i)
                $__tsc_pbget_hit = ($__tsc_pbget_hit || 0) + 1
                (MKXPDebug.log("[TSC-PBGET] hit map=#{gm.map_id} tile_id=#{tile_id}->#{nid} (compactada #{c[:bmp].width}x#{c[:bmp].height}) [hit##{$__tsc_pbget_hit}]") rescue nil) if $__tsc_pbget_hit <= 8
                return bmp
              elsif gm && tile_id && tile_id >= 384 && (c = $__tsc_cache[gm.map_id]) && width == 1 && height == 1
                $__tsc_pbget_miss = ($__tsc_pbget_miss || 0) + 1
                (MKXPDebug.log("[TSC-PBGET] MISS-no-remap map=#{gm.map_id} tile_id=#{tile_id} (nao esta' no remap -> setter original carrega gigante) [miss##{$__tsc_pbget_miss}]") rescue nil) if $__tsc_pbget_miss <= 8
              end
            rescue => _e
              (MKXPDebug.log("[TSC-PBGET] fallback erro: #{_e.class}: #{_e.message}") rescue nil)
            end
            __tsc_orig_pbgettile(filename, tile_id, hue, width, height)
          end
        end
      end
    end
    # ── ATDBG: estado dos sprites de tiles de PRIORIDADE (casas/arvores invisiveis) ──
    # Hook addTile: depois de criar o sprite, loga id, prioridade, posicao, z, visivel,
    # opacidade, viewport.z, dimensoes do bitmap, alpha central E src_rect. Se o bitmap
    # e' valido (a16=255) mas src_rect=0x0 -> o sprite nao desenha nada (= invisivel).
    begin
      if defined?(CustomTilemap)
        class CustomTilemap
          unless method_defined?(:__atdbg_orig_addTile)
            alias_method :__atdbg_orig_addTile, :addTile
            def addTile(tiles, count, xpos, ypos, id)
              r = __atdbg_orig_addTile(tiles, count, xpos, ypos, id)
              begin
                prio = (@priorities[id] rescue 0) || 0
                mid = ($game_map ? ($game_map.map_id rescue -1) : -1)
                $__atdbg_n ||= Hash.new(0)
                # captura 15/mapa; da' prioridade aos tiles de prioridade ALTA (>=3 = casas/topos)
                want = (prio >= 3) || ($__atdbg_n[mid] < 15)
                if prio > 0 && want && $__atdbg_n[mid] < 22
                  $__atdbg_n[mid] += 1
                  spr = tiles[count]
                  bw = (spr.bitmap.width rescue -1); bh = (spr.bitmap.height rescue -1)
                  bd = (spr.bitmap.disposed? rescue '?')
                  # opacidade DENSA: grelha 16x16 (256 pts, step 2). Distingue tile
                  # ESPARSO (poucos opacos, extracao CORRETA) de VAZIO (0 opacos = bug
                  # de extracao da coluna). O aMAX de 9 pontos confundia os dois.
                  opq = 0; amax = -1
                  if bw == 32 && bh == 32 && bd != true
                    yy = 0
                    while yy < 32
                      xx = 0
                      while xx < 32
                        aa = (spr.bitmap.get_pixel(xx, yy).alpha.to_i rescue 0)
                        opq += 1 if aa > 0
                        amax = aa if aa > amax
                        xx += 2
                      end
                      yy += 2
                    end
                  end
                  vis = (spr.visible rescue '?'); zz = (spr.z rescue '?')
                  vp = (spr.viewport rescue nil); vpz = (vp ? (vp.z rescue '?') : 'no-vp')
                  op = (spr.opacity rescue '?')
                  col = (id.is_a?(Integer) && (id - 384) >= 256) ? 1 : 0
                  # old id via remap inverso -> permite cruzar com a opacidade da FONTE
                  oldid = '?'
                  begin
                    tc = (defined?($__tsc_cache) && $__tsc_cache) ? $__tsc_cache[mid] : nil
                    if tc && tc[:remap]
                      $__atdbg_inv ||= {}
                      $__atdbg_inv[mid] ||= tc[:remap].invert
                      oldid = ($__atdbg_inv[mid][id] || '?')
                    end
                  rescue; end
                  # copq: opacidade do COMPACTADO (@tileset cache) na MESMA posicao
                  # embrulhada que a extracao do motor calcula (k=id-384). Distingue
                  # de forma DEFINITIVA: copq>0 & opq=0 => BUG da EXTRACAO (motor le
                  # mal); copq=0 => BUG do BUILD (o tile nao foi escrito no compactado).
                  copq = '?'
                  begin
                    cb = (tc && tc[:bmp]) ? tc[:bmp] : nil
                    if cb && id.is_a?(Integer) && !(cb.disposed? rescue true)
                      kk  = id - 384
                      cth = (Bitmap.max_size rescue 1024) || 1024
                      cth -= (cth % 32); cth = 1024 if cth <= 0
                      ccy = (kk >> 3) * 32
                      cxb = (ccy / cth) * 256 + (kk & 7) * 32
                      cyb = ccy % cth
                      cw  = (cb.width rescue 0); ch = (cb.height rescue 0)
                      cc = 0; y2 = 0
                      while y2 < 32
                        x2 = 0
                        while x2 < 32
                          px = cxb + x2; py = cyb + y2
                          if px < cw && py < ch
                            aa2 = (cb.get_pixel(px, py).alpha.to_i rescue 0)
                            cc += 1 if aa2 > 0
                          end
                          x2 += 2
                        end
                        y2 += 2
                      end
                      copq = "#{cc}@(#{cxb},#{cyb})/#{cw}x#{ch}"
                    end
                  rescue; end
                  (MKXPDebug.log("[ATDBG] map=#{mid} id=#{id} old=#{oldid} pri=#{prio} col=#{col} pos=(#{xpos.to_i},#{ypos.to_i}) z=#{zz} vis=#{vis} op=#{op} vpz=#{vpz} bmp=#{bw}x#{bh} disp=#{bd} opq=#{opq}/256 comp=#{copq} aMAX=#{amax}") rescue nil)
                end
              rescue; end
              r
            end
          end
        end
        (MKXPDebug.log("[ATDBG] hook addTile instalado (15+/mapa, prioriza pri>=3 casas, opacidade densa 256pts + old id)") rescue nil)
      end
    rescue => _ae
      (MKXPDebug.log("[ATDBG] instalacao falhou: #{_ae.class}: #{_ae.message}") rescue nil)
    end
    (MKXPDebug.log("[TSCOMPACT-RT] instalado (compactacao de tilesets gigantes em runtime, jogo intacto)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[TSCOMPACT-RT] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  BMPDUP — corrigir dup/clone de bitmaps (sprites/eventos invisiveis)
# -----------------------------------------------------------------------------
#  CAUSA (log + sprite_binding_3ds.cpp:498): [BMP|UNWRAP] FALHOU type=16
#  (MRB_TT_OBJECT) -> a bitmap de um sprite perdeu o DATA_PTR. Em mruby, dup/clone
#  de um objeto C-DATA (Bitmap) cria uma copia MRB_TT_OBJECT SEM dados ->
#  DATA_PTR=nullptr -> 0 blits -> sprite INVISIVEL. O motor corrige a CLASSE no
#  boot (bitmapBindingReinit) mas dup/clone de INSTANCIAS volta a corromper (comeca
#  num evento do map3, item-get @ t~227 -> 2007 falhas). FIX: redirecionar
#  dup/clone para uma copia REAL (nova bitmap C-DATA + blt). Idempotente; com
#  fallback ao dup original se algo falhar (nunca pior).
# =============================================================================
begin
  if Object.const_defined?(:Bitmap) && !$__bmpdup_done
    $__bmpdup_done = true
    _klasses = [Bitmap]
    _klasses << BitmapWrapper if Object.const_defined?(:BitmapWrapper)
    _klasses.uniq.each do |klass|
      klass.class_eval do
        unless method_defined?(:__bmpdup_orig_dup)
          (alias_method :__bmpdup_orig_dup, :dup) rescue nil
          (alias_method :__bmpdup_orig_clone, :clone) rescue nil
          def __bmpdup_copy
            w = (width rescue 0); h = (height rescue 0)
            # Fonte partida (width rebenta -> 0, ou bitmap ja' disposed): NUNCA
            # cair no dup original do mruby -- esse cria um T_DATA com
            # DATA_PTR=NULL (e' o bug que isto corrige -> [BMP|UNWRAP] FALHOU em
            # cascata, 33x no log do roupeiro). Devolver antes uma bitmap 1x1
            # VALIDA (self.class.new -> bmp_init -> bmp_alloc, que NUNCA devolve
            # NULL): o sprite fica inofensivamente vazio mas SEM ponteiro nulo e
            # SEM spam no log. Mantem a classe (BitmapWrapper) p/ resetRef etc.
            if !w || !h || w <= 0 || h <= 0
              safe = (self.class.new(1, 1) rescue (Bitmap.new(1, 1) rescue nil))
              $__bmpdup_nullsrc = ($__bmpdup_nullsrc || 0) + 1
              (MKXPDebug.log("[BMPDUP] fonte partida w=#{w.inspect} h=#{h.inspect} -> 1x1 valida ##{$__bmpdup_nullsrc} (evita DATA_PTR=NULL)") rescue nil) if ($__bmpdup_nullsrc || 0) <= 6
              return (safe || self)
            end
            nb = (self.class.new(w, h) rescue Bitmap.new(w, h))
            (nb.blt(0, 0, self, Rect.new(0, 0, w, h)) rescue nil)
            $__bmpdup_n = ($__bmpdup_n || 0) + 1
            (MKXPDebug.log("[BMPDUP] copia segura ##{$__bmpdup_n} #{w}x#{h}") rescue nil) if ($__bmpdup_n || 0) <= 6
            nb
          end
          def dup;   (__bmpdup_copy rescue (__bmpdup_orig_dup rescue self)); end
          def clone; (__bmpdup_copy rescue (__bmpdup_orig_clone rescue self)); end
        end
      end
    end
    (MKXPDebug.log("[BMPDUP] dup/clone de bitmaps -> copia real (anti sprite-invisivel)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[BMPDUP] falhou: #{e.class}: #{e.message}") rescue nil)
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
            MKXPDebug.log("[PROBE-CONFIRM] ===== debug_probe V35 a correr (confirmado no 1o frame) =====")
            MKXPDebug.log("[PROBE-CONFIRM] >>> VERSAO=V35 PASS+MENU+SOMBRA+VRAM | CEWRAP=#{!!$__cewrap_installed} " \
                          "PBANIMFIX=#{!!$__pbanimfix_done} " \
                          "TSCOMPACT_RT=#{!!$__tscompactrt_installed} " \
                          "TILETHROTTLE=#{!!$__tilethrottle_installed} " \
                          "PERF_STATIC=#{!!$__perf_static_on} " \
                          "TRANSFERFIX=#{!!$__sgpicfix && !!$__animposfix} <<<  " \
                          "(se VERSAO!=V35, o motor leu um ficheiro antigo)")
            MKXPDebug.log("[PROBE-CONFIRM] >>> PASSABILIDADE: __tsc_mk_table escreve @data DIRETO (contorna o bug C++ Table#[]=). Esperar [TSC-PASSDBG] npass_nao_zero>0. <<<")
            MKXPDebug.log("[PROBE-CONFIRM] >>> MENU: MenuCloseFix RE-ATIVADO -- fecha por flanco 12/14. Ver [MENU-V17] premidas. <<<")
            MKXPDebug.log("[PROBE-CONFIRM] >>> SOMBRAS NPC: OWShadowNPCFollow ATIVO (NPCs por screen_x/y; player intocado). Ver [OW-SHADOW-NPC]. <<<")
            MKXPDebug.log("[PROBE-CONFIRM] >>> VRAM(#5): tilesets em VRAM e' AUTOMATICO no motor (never_dispose liga prefer_vram). SO' FUNCIONA com o C++ recompilado. Ver [TEX|VRAM-PREFER] no log. <<<")
            MKXPDebug.log("[PROBE-CONFIRM] >>> PERF: SHOULDUPDATE=#{!!$__perf_su} | RESTO-PROFILE=#{!!$__perf_resto}. <<<")
            MKXPDebug.log("[PROBE-CONFIRM] >>> TSC_HOOK=#{((Object.const_defined?(:CustomTilemap) && CustomTilemap.method_defined?(:__tsc_orig_tileset_eq)) rescue false)} " \
                          "TSC_PBGET=#{((Object.method_defined?(:__tsc_orig_pbgettile)) rescue false)} " \
                          "VWRAP=#{(defined?(VWrap) ? true : false)} <<<  (se TSC_HOOK=false, o override NAO instalou)")
            MKXPDebug.log("[PROBE-CONFIRM] >>> BMPDUP=#{($__bmpdup_done == true)} (dup/clone seguro p/ sprites; deve eliminar [BMP|UNWRAP] FALHOU) <<<")
            MKXPDebug.log("[PROBE-CONFIRM] >>> PASSDBG=on (V30: [TSC-PASSDBG] compara passabilidade orig vs reconstruida; [PASSDBG-MOVE] loga o que o jogador pisa) <<<")
            MKXPDebug.log("[PROBE-CONFIRM] >>> TSC_ENV: thr_compactar=8192 " \
                          "TILESET_HEIGHT=#{(VWrap::TILESET_HEIGHT rescue '?')} " \
                          "MAX_TEX_SIZE=#{(VWrap::MAX_TEX_SIZE rescue '?')} " \
                          "Bitmap.max_size=#{(Bitmap.max_size rescue '?')} " \
                          "helper_build=#{(respond_to?(:__tsc_build) rescue false)} " \
                          "helper_mktable=#{(respond_to?(:__tsc_mk_table) rescue false)} " \
                          "cache_size=#{($__tsc_cache ? $__tsc_cache.size : 'nil')} <<<")
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
#  CESCAN  —  diagnostico decisivo das Common Event lists (1x, 1o frame)  [V21]
# -----------------------------------------------------------------------------
#  PORQUE (contradicao encontrada no log do V20): a estante (CE#34) so' abriu
#  DEPOIS do CEFIX injetar a lista a mao -- ate' aqui tudo certo. MAS o log do
#  marshal NAO bate com a hipotese "Tone/MoveRoute/AudioFile nao desserializam":
#    [RXCACHE] HIT CommonEvents  ->  [marshal] load OK (1704639 bytes)
#       userdefs=95  tables=0  class=93  alloc=42545 objs  ivset=127179 ivars
#       (e SEM um unico "[Marshal] WARN classe nao definida", SEM "[marshal] FALHA")
#  Ou seja: o grafo de objetos COMPLETO (42 mil objs, 127 mil ivars!) entra em
#  memoria; Color/Tone ja' sao reconstruidas nativamente no marshal.cpp. Logo o
#  "@list nil" NAO vem de o marshal abortar nem de classe em falta -- so' se
#  esclarece a olhar para o objeto vivo. Este bloco corre 1x e, SEM escrever nada
#  nos dados do jogo, mede:
#    (A) BLAST RADIUS  : de TODOS os CEs, quais teem .list (metodo) nil.
#    (B) GETTER vs DADOS: para cada nil, ve se o @list (ivar) cru TAMBEM e' nil.
#        - .list(metodo)=nil MAS @list(ivar)=Array  => GETTER partido  => fix
#          UNIVERSAL de 1 linha (redefinir RPG::CommonEvent#list) resolve os 257.
#        - ambos nil                                 => DADOS em falta  => so'
#          reconstruindo CE a CE (gera-se a partir da rxdata).
#    (C) DEEP DUMP CE#34: classe, is_a CommonEvent?, ivars, .list vs @list,
#        @name/@id/@trigger -> confirma se e' CommonEvent bem-formado a quem so'
#        falta a lista, ou objeto malformado.
#  Read-only. Gated por $__cescan_done. NAO toca em SHADOWZ/CACHEFIX/TRANSFERFIX/
#  MENU/CEFIX -- e' so' medicao. Corre ANTES do CEFIX patchar (frame 1), por isso
#  ve o estado PRISTINO pos-load das listas.
# =============================================================================
begin
  if defined?(Scene_Map) && Scene_Map.method_defined?(:update) && !$__cescan_installed
    $__cescan_installed = true
    class Scene_Map
      alias_method :__cescan_orig_update, :update
      def update(*a)
        unless $__cescan_done
          $__cescan_done = true
          begin
            dce = ($data_common_events rescue nil)
            if dce.nil?
              (MKXPDebug.log("[CESCAN] $data_common_events=nil -- nada a medir") rescue nil)
            else
              _nil_meth = []   # .list (metodo) vem nil
              _accbug   = []   # .list nil MAS @list(ivar)=Array nao-vazio  -> getter partido
              _both_nil = []   # .list nil E @list(ivar) nil/vazio          -> dados em falta
              _tot = 0
              begin
                _i = 0
                while _i < dce.size
                  _ce = dce[_i]
                  if _ce
                    _tot += 1
                    _ml = (_ce.list rescue :err)
                    if _ml.nil?
                      _nil_meth << _i
                      _iv = (_ce.instance_variable_get(:@list) rescue nil)
                      if _iv.is_a?(Array) && _iv.size > 0
                        _accbug << _i
                      else
                        _both_nil << _i
                      end
                    end
                  end
                  _i += 1
                end
              rescue => _e2
                (MKXPDebug.log("[CESCAN] varrimento abortou apos #{_tot}: #{_e2.class}: #{_e2.message.to_s[0,80]}") rescue nil)
              end
              (MKXPDebug.log("[CESCAN] CEs totais=#{_tot} | .list NIL=#{_nil_meth.size}") rescue nil)
              (MKXPDebug.log("[CESCAN]   getter-partido (.list=nil mas @list=Array): #{_accbug.size} -> #{_accbug.inspect}") rescue nil)
              (MKXPDebug.log("[CESCAN]   dados-em-falta (.list=nil e @list vazio):   #{_both_nil.size} -> #{_both_nil.inspect}") rescue nil)
              if _accbug.size > 0 && _both_nil.size == 0
                (MKXPDebug.log("[CESCAN]   >>> VEREDITO: GETTER PARTIDO. Fix universal de 1 linha resolve TODOS (sem reconstruir CE a CE).") rescue nil)
              elsif _both_nil.size > 0 && _accbug.size == 0
                (MKXPDebug.log("[CESCAN]   >>> VEREDITO: DADOS EM FALTA. Reconstrucao por CE (gerar da rxdata) e' o caminho.") rescue nil)
              elsif _nil_meth.size == 0
                (MKXPDebug.log("[CESCAN]   >>> VEREDITO: nenhuma .list nil neste arranque. Se a estante falhar, a causa e' outra.") rescue nil)
              else
                (MKXPDebug.log("[CESCAN]   >>> VEREDITO: MISTO (getter + dados). Ver as duas listas acima.") rescue nil)
              end

              # (C) DEEP DUMP do CE#34 (estado pristino -- o CEFIX ainda nao correu)
              begin
                _c = dce[34]
                if _c
                  _kl  = (_c.class rescue '?')
                  _is  = (_c.is_a?(RPG::CommonEvent) rescue '?')
                  _ivs = (_c.instance_variables.map { |x| x.to_s }.sort rescue [])
                  _mL  = (_c.list rescue :err)
                  _rL  = (_c.instance_variable_get(:@list) rescue :err)
                  _md  = _mL.nil? ? "nil" : (_mL.is_a?(Array) ? "Array(#{_mL.size})" : _mL.class.to_s)
                  _rd  = _rL.nil? ? "nil" : (_rL.is_a?(Array) ? "Array(#{_rL.size})" : _rL.class.to_s)
                  _nm  = (_c.instance_variable_get(:@name) rescue nil)
                  _id  = (_c.instance_variable_get(:@id) rescue nil)
                  _tg  = (_c.instance_variable_get(:@trigger) rescue nil)
                  (MKXPDebug.log("[CESCAN] CE#34 DEEP: class=#{_kl} is_a_CE=#{_is} .list(metodo)=#{_md} @list(ivar)=#{_rd}") rescue nil)
                  (MKXPDebug.log("[CESCAN] CE#34 DEEP: ivars=#{_ivs.inspect} @name=#{_nm.inspect} @id=#{_id.inspect} @trigger=#{_tg.inspect}") rescue nil)
                end
              rescue => _e3
                (MKXPDebug.log("[CESCAN] deep dump CE#34 falhou: #{_e3.class}: #{_e3.message.to_s[0,80]}") rescue nil)
              end
            end
          rescue => _e
            (MKXPDebug.log("[CESCAN] falhou: #{_e.class}: #{_e.message.to_s[0,100]}") rescue nil)
          end
        end
        __cescan_orig_update(*a)
      end
    end
  end
rescue => _e
  (MKXPDebug.log("[CESCAN] instalacao falhou: #{_e.class}: #{_e.message}") rescue nil)
end

# =============================================================================
#  CEWRAP  —  FIX UNIVERSAL das Common Events (1x, 1o frame)  [V22]
# -----------------------------------------------------------------------------
#  CAUSA-RAIZ (provada pelo CESCAN no log V21): RPG::CommonEvent NAO esta' definida
#  neste mruby (compat_stubs.h define RPG::EventCommand/MoveRoute/AudioFile mas NAO
#  CommonEvent). Por isso, ao carregar, o const_missing devolve NDS::NullStub e os
#  256 objetos CommonEvent viram NullStub. O NullStub#method_missing devolve nil ->
#  common_event.list = nil  (ESCONDENDO o @list real, que esta' la' completo:
#  CESCAN mostrou 256/256 com @list=Array). is_a?(RPG::CommonEvent)=true porque o
#  proprio RPG::CommonEvent resolve para NullStub. Resultado: command_117 chama
#  setup(nil) -> filho vazio -> a estante e TUDO o que usa Common Events nao corre.
#
#  FIX (resolve os 256 de uma vez, dispensa a reconstrucao a mao do CEFIX):
#   1) Definir RPG::CommonEvent a serio (attr_accessor id/name/trigger/switch_id/list).
#      Como o NullStub vinha do const_missing (nunca foi const_set), basta const_set.
#   2) Re-embrulhar $data_common_events: para cada NullStub, criar um RPG::CommonEvent
#      real e copiar os ivars (@list ja' tem a lista de RPG::EventCommand REAIS, pois
#      essa classe existe). Muto o array EM SI (dce[i]=real) p/ nao partir referencias.
#  Depois disto, .list devolve a lista verdadeira -> o interpretador corre tudo
#  nativamente. (O CEFIX antigo passa a nao disparar, pois .list deixa de ser nil.)
#  Read-only sobre os DADOS do jogo no disco; so' corrige objetos ja' em memoria.
# =============================================================================
begin
  if defined?(Scene_Map) && Scene_Map.method_defined?(:update) && !$__cewrap_installed
    $__cewrap_installed = true
    class Scene_Map
      alias_method :__cewrap_orig_update, :update
      def update(*a)
        unless $__cewrap_done
          $__cewrap_done = true
          begin
            # (1) garantir a classe RPG::CommonEvent a serio
            real_klass = nil
            begin
              ::RPG rescue (Object.const_set(:RPG, Module.new))
              is_real = false
              begin
                is_real = (::RPG.const_defined?(:CommonEvent, false)) &&
                          (::RPG.const_get(:CommonEvent).to_s != 'NDS::NullStub') &&
                          (::RPG.const_get(:CommonEvent).instance_methods.include?(:list))
              rescue; end
              if is_real
                real_klass = ::RPG.const_get(:CommonEvent)
              else
                k = ::Class.new do
                  attr_accessor :id, :name, :trigger, :switch_id, :list
                end
                (::RPG.send(:remove_const, :CommonEvent) rescue nil) if (::RPG.const_defined?(:CommonEvent, false) rescue false)
                ::RPG.const_set(:CommonEvent, k)
                real_klass = k
                (MKXPDebug.log("[CEWRAP] RPG::CommonEvent definida a serio (antes era NullStub via const_missing)") rescue nil)
              end
            rescue => _e1
              (MKXPDebug.log("[CEWRAP] def classe falhou: #{_e1.class}: #{_e1.message.to_s[0,90]}") rescue nil)
            end
            # (2) re-embrulhar $data_common_events
            begin
              dce = ($data_common_events rescue nil)
              if dce && real_klass
                wrapped = 0; already = 0; semlista = 0
                i = 0
                while i < dce.size
                  ce = dce[i]
                  if ce
                    if ce.is_a?(real_klass)
                      already += 1
                    else
                      lst = (ce.instance_variable_get(:@list) rescue nil)
                      real = real_klass.new
                      (real.id        = (ce.instance_variable_get(:@id)        rescue nil)) rescue nil
                      (real.name      = (ce.instance_variable_get(:@name)      rescue nil)) rescue nil
                      (real.trigger   = (ce.instance_variable_get(:@trigger)   rescue nil)) rescue nil
                      (real.switch_id = (ce.instance_variable_get(:@switch_id) rescue nil)) rescue nil
                      (real.list      = lst) rescue nil
                      dce[i] = real
                      wrapped += 1
                      semlista += 1 if !(lst.is_a?(Array) && lst.size > 0)
                    end
                  end
                  i += 1
                end
                (MKXPDebug.log("[CEWRAP] re-embrulhados #{wrapped} CommonEvents (ja'-ok=#{already}, sem-lista=#{semlista}). .list passa a funcionar -> estante e todos os CEs corrigidos. CEFIX deixa de ser preciso.") rescue nil)
              else
                (MKXPDebug.log("[CEWRAP] sem $data_common_events (#{dce ? 'sim' : 'nil'}) ou classe (#{real_klass ? 'sim' : 'nil'}) -> nao re-embrulhei") rescue nil)
              end
            rescue => _e2
              (MKXPDebug.log("[CEWRAP] re-embrulhar falhou: #{_e2.class}: #{_e2.message.to_s[0,90]}") rescue nil)
            end
          rescue => _e
            (MKXPDebug.log("[CEWRAP] falhou: #{_e.class}: #{_e.message.to_s[0,100]}") rescue nil)
          end
        end
        __cewrap_orig_update(*a)
      end
    end
  end
rescue => _e
  (MKXPDebug.log("[CEWRAP] instalacao falhou: #{_e.class}: #{_e.message}") rescue nil)
end

# =============================================================================
#  PBANIMFIX  —  causas-raiz REAIS do Bug 1 e do Bug 2 (achadas a ler o log todo)
# -----------------------------------------------------------------------------
#  Fonte (Scripts.rxdata, descomprimido com a toolchain):
#
#   AnimatedBitmap :: def pbGetAnimation(name,hue=0)
#     return AnimatedBitmap.new("Graphics/Animations/" + name, hue).deanimate
#   -> "..." + name REBENTA com "Integer cannot be converted to String" quando os
#      efeitos de campo (Marin's Footprints / Water bubles / Overworld Shadows EX)
#      passam um ID (Integer) em vez de um nome. Esse crash, apanhado pelo FXGUARD,
#      abortava o update do sprite -> player desaparecia/congelava nas ESCADAS (Bug 1).
#      Fix: + name.to_s (no-op quando ja' e' String; coage o Integer).
#
#   Utilities :: def pbExclaim(event,id=...,tinting=false)
#     sprite = $scene.spriteset.addUserAnimation(id, x, y, tinting, 2)
#     while !sprite.disposed?            # espera a animacao terminar
#       Graphics.update; Input.update; pbUpdateSceneMap
#     end
#   -> na 2a visita a' Map43, sem RAM (OOM dos tilesets gigantes), o bitmap da
#      animacao falha e o sprite NUNCA se dispoe -> while infinito -> jogo preso,
#      player nao anda apos sair de casa (Bug 2 = BLOQUEIO EFETIVO, idx=89 code=355).
#      Fix: cap de seguranca no loop (~150 frames) -> dispose forcado e sai.
#
#  Ambos vivem aqui no probe (Ruby), nunca se mexe no Scripts.rxdata (read-only).
#  Redefinicao fiel ao original + a correcao minima. Idempotente (re-load seguro).
# =============================================================================
begin
  if defined?(AnimatedBitmap)
    Object.class_eval do
      def pbGetAnimation(name, hue = 0)
        AnimatedBitmap.new("Graphics/Animations/" + name.to_s, hue).deanimate
      end
    end
    (MKXPDebug.log("[PBANIMFIX] pbGetAnimation seguro (name.to_s -> nao rebenta com Integer) instalado") rescue nil)
  else
    (MKXPDebug.log("[PBANIMFIX] AnimatedBitmap ainda nao definida -> pbGetAnimation nao reescrito") rescue nil)
  end
rescue => _e
  (MKXPDebug.log("[PBANIMFIX] pbGetAnimation falhou: #{_e.class}: #{_e.message}") rescue nil)
end

begin
  Object.class_eval do
    def pbExclaim(event, id = (defined?(Settings) ? Settings::EXCLAMATION_ANIMATION_ID : 1), tinting = false)
      sprite = nil
      if event.is_a?(Array)
        done = []
        for i in event
          unless done.include?(i.id)
            sprite = ($scene.spriteset.addUserAnimation(id, i.x, i.y, tinting, 2) rescue nil)
            done.push(i.id)
          end
        end
      else
        sprite = ($scene.spriteset.addUserAnimation(id, event.x, event.y, tinting, 2) rescue nil)
      end
      guard = 0
      while sprite && !(sprite.disposed? rescue true)
        (Graphics.update rescue nil)
        (Input.update rescue nil)
        (pbUpdateSceneMap rescue nil)
        guard += 1
        if guard > 150  # ~5s @30fps: a animacao nao vai terminar -> sair em vez de pendurar
          (MKXPDebug.log("[PBANIMFIX] pbExclaim: animacao nao terminou em #{guard}f -> dispose forcado (anti-freeze Bug 2)") rescue nil)
          (sprite.dispose rescue nil)
          break
        end
      end
    end
  end
  $__pbanimfix_done = true
  (MKXPDebug.log("[PBANIMFIX] pbExclaim com cap anti-hang (#{150}f) instalado") rescue nil)
rescue => _e
  (MKXPDebug.log("[PBANIMFIX] pbExclaim falhou: #{_e.class}: #{_e.message}") rescue nil)
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
          $__tscache_max = 1   # FIX FREEZE: 1 (nao 2). Ao entrar numa area nova, a
          #   eviccao drena ATE 0 os tilesets nao-activos ANTES de descodificar o
          #   novo (com 2, um gigante antigo ficava retido durante o decode do novo
          #   -> 2 gigantes simultaneos -> OOM -> magenta -> preto). Mapas LIGADOS
          #   com tilesets diferentes continuam ambos em cache (ambos activos via
          #   MapFactory -> nao sao expulsos), logo nao ha thrash no costura.
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
                # ── PLACEHOLDER: se a compactada deste mapa JA esta' em $__tsc_cache, NAO
                # decodificar a fonte gigante (~15MB, ~2.9s de freeze). Devolver 32x32; o
                # CustomTilemap#tileset= deteta a cache e usa a compactada. So' se aplica ao
                # tileset do MAPA ATUAL (nome igual) -> nunca devolve placeholder a quem
                # precise mesmo de decodificar (1a entrada ou outro mapa).
                begin
                  gmc = ($game_map rescue nil)
                  if gmc && $__tsc_cache && $__tsc_cache[(gmc.map_id rescue -1)] &&
                     (nm == (gmc.tileset_name.to_s rescue "\u0000"))
                    (MKXPDebug.log("[TSCACHE] PLACEHOLDER '#{nm}' (compactada em cache map=#{gmc.map_id} -> sem re-decode 15MB)") rescue nil)
                    return (BitmapWrapper.new(32, 32) rescue Bitmap.new(32, 32))
                  end
                rescue; end
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
                    # 1a escolha: um tileset que NENHUM mapa activo use
                    $__tscache_lru.each { |k| (victim = k; break) unless active[k] }
                    # 2a escolha: se todos os cacheados estao "activos", expulsar uma FONTE
                    # GIGANTE (>8192px). Os mapas gigantes activos usam a versao COMPACTADA
                    # ($__tsc_cache[mid][:bmp]); a fonte ja' NAO e' referenciada pelo tilemap
                    # vivo -> libertar os ~15MB e' SEGURO (sem UAF) e desbloqueia o tileset
                    # gigante SEGUINTE (ex: Route conectado ao Town, que nao cabem juntos).
                    if victim.nil?
                      $__tscache_lru.each do |k|
                        gob = $__tscache[k]
                        if gob && !(gob.disposed? rescue true) && (gob.height rescue 0) > 8192
                          victim = k
                          (MKXPDebug.log("[TSCACHE] EVICT-GIGANTE-ATIVO '#{k}' (#{(gob.width rescue '?')}x#{(gob.height rescue '?')}) -- mapa activo usa a compactada, fonte livre") rescue nil)
                          break
                        end
                      end
                    end
                    break if victim.nil?   # nem gigantes livres -> NAO expulsar (seguro)
                    $__tscache_lru.delete(victim)
                    ob = $__tscache.delete(victim)
                    begin
                      (ob.never_dispose = false) rescue nil
                      rc0 = (ob.refcount rescue '?')
                      d0 = (ob.disposed? rescue '?')
                      ob.dispose   # so' liberta de facto se refcount<=0 (ninguem vivo o usa)
                      d1 = (ob.disposed? rescue '?')
                      (MKXPDebug.log("[TSCACHE] EVICT '#{victim}' refcount=#{rc0} disp(#{d0}->#{d1}) | activos=#{active.keys.inspect} (se disp=true e um tilemap o usa -> preto)") rescue nil)
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
          begin
            (MKXPDebug.log("[PERF-RESTO] player=#{(($__pr_player||0.0)/120.0*10).to_i/10.0} screen=#{(($__pr_screen||0.0)/120.0*10).to_i/10.0} interp=#{(($__pr_interp||0.0)/120.0*10).to_i/10.0} ms (do 'resto'; o que sobrar = system+overhead)") rescue nil)
            $__pr_player = 0.0; $__pr_screen = 0.0; $__pr_interp = 0.0
          rescue; end
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
#  PERF-SHOULDUPDATE — should_update? otimizado (GENERICO, behavior-preserving)
# -----------------------------------------------------------------------------
#  O Game_Event#should_update?(true) corre por CADA evento, CADA frame (mesmo os
#  off-screen que vai cull). O original: (a) corre a regex name[/update/i] todo o
#  frame (o nome NUNCA muda) e (b) computa screen_x E screen_y_ground DUAS vezes
#  cada (4 calculos de posicao subpixel por evento/frame). Aqui: cacheia a regex
#  (1x por evento) e computa cada posicao 1x. MESMA logica, MESMOS resultados ->
#  zero risco. Poupa ~40-50% do overhead de culling. Vale p/ qualquer jogo RMXP.
# =============================================================================
begin
  if defined?(Game_Event) && Game_Event.method_defined?(:should_update?) && !$__perf_su
    $__perf_su = true
    class Game_Event
      def should_update?(recalc = false)
        return @to_update if !recalc
        return true if @trigger && (@trigger == 3 || @trigger == 4)
        return true if @move_route_forcing
        if @__su_name_upd.nil?   # cache da regex (nome nunca muda)
          @__su_name_upd = ((@event.name[/update/i] rescue nil) ? true : false)
        end
        return true if @__su_name_upd
        ss = @sprite_size
        return true if ss.nil?   # seguranca: tamanho ainda desconhecido -> atualizar
        range = 2
        gw = Graphics.width; gh = Graphics.height
        tw = Game_Map::TILE_WIDTH; th = Game_Map::TILE_HEIGHT
        sx = self.screen_x       # 1x (era 2x)
        hw = ss[0] / 2
        return false if sx - hw > gw + range * tw
        return false if sx + hw < -range * tw
        sy = self.screen_y_ground  # 1x (era 2x)
        return false if sy - ss[1] > gh + range * th
        return false if sy < -range * th
        true
      end
    end
    (MKXPDebug.log("[PERF-SHOULDUPDATE] should_update? otimizado (posicao 1x, regex cacheada) -- generico") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[PERF-SHOULDUPDATE] falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  PERF-RESTO — cronometrar as 3 partes do "resto" (so' MEDICAO, sem alterar nada)
# -----------------------------------------------------------------------------
#  O FPSPROBE mostra "resto" ~22-38ms mas nao diz de QUE. Estes wrappers medem
#  Game_Player#update / Game_Screen#update / Interpreter#update separadamente. O
#  proximo log dira' qual atacar (provavel: jogador+seguidores OU interpretador).
# =============================================================================
begin
  if !$__perf_resto
    $__perf_resto = true
    $__pr_player = 0.0; $__pr_screen = 0.0; $__pr_interp = 0.0
    if defined?(Game_Player) && Game_Player.method_defined?(:update)
      class Game_Player
        alias_method :__pr_orig_update, :update
        def update(*a)
          __t = (__mkxp_ms rescue 0.0); __r = __pr_orig_update(*a)
          $__pr_player += ((__mkxp_ms rescue 0.0) - __t); __r
        end
      end
    end
    if defined?(Game_Screen) && Game_Screen.method_defined?(:update)
      class Game_Screen
        alias_method :__pr_orig_update, :update
        def update(*a)
          __t = (__mkxp_ms rescue 0.0); __r = __pr_orig_update(*a)
          $__pr_screen += ((__mkxp_ms rescue 0.0) - __t); __r
        end
      end
    end
    _ic = (defined?(Interpreter) ? Interpreter : (defined?(Game_Interpreter) ? Game_Interpreter : nil))
    if _ic && _ic.method_defined?(:update)
      _ic.class_eval do
        alias_method :__pr_orig_update, :update unless method_defined?(:__pr_orig_update)
        def update(*a)
          __t = (__mkxp_ms rescue 0.0); __r = __pr_orig_update(*a)
          $__pr_interp += ((__mkxp_ms rescue 0.0) - __t); __r
        end
      end
    end
    (MKXPDebug.log("[PERF-RESTO] wrappers instalados (player/screen/interpreter) -- so' medicao") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[PERF-RESTO] falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  TRANSFERFIX — elimina os 2 crashes da transicao de mapa (o "congela depois da
#                intro" com o mapa so' num fragmento no canto)
# -----------------------------------------------------------------------------
#  O log da rota (transition #6) mostra 2 crashes durante o transfer que abortam
#  o update INICIAL do spriteset; o tilemap novo nunca recebe ox/oy -> renderiza
#  no offset do mapa anterior (fragmento no canto) e o jogo parece congelado:
#    CRASH 1  Spriteset_Map#update base (_animationSprite_update) rebenta num '+'
#             de coordenadas -- quirk numerico do mruby (MRB_INT32+WORD_BOXING)
#             com display_x num estado transitorio -- ANTES de @tilemap.update.
#    CRASH 2  Spriteset_Global#update original faz @picture_sprites.each{ s.update }
#             e bate num Sprite_Picture NIL/disposto durante o transfer.
#  Os wrappers C++ apanham, mas fazem recuperacao disruptiva (Graphics.transition)
#  e NAO reposicionam o tilemap -> render fica partido. Aqui prevenimos os crashes
#  nas camadas INTERNAS (Ruby, hot-load, SEM recompilar) para que NUNCA cheguem ao
#  C++ e o transfer complete limpo:
#    (1) Spriteset_Global#update: loop de imagens a prova de nil (sem mexer no
#        array -> indices preservados).
#    (2) Spriteset_Map _animationSprite_update: se o update base rebentar, FORCAR
#        @tilemap.ox/oy (de display_x, .round -> Integer) + @tilemap.update, para o
#        mapa novo ficar no sitio certo; o frame seguinte ja' corre tudo normal.
#  NOTA: o FOLLOWGUARD nao apanhava isto (esta' no DependentEventSprites; o crash 2
#  e' ANTES de @followingpkmn_sprites.update). NAO e' regressao do V17.
# =============================================================================
begin
  if defined?(Spriteset_Global) && Spriteset_Global.method_defined?(:__followingpkmn__update) && !$__sgpicfix
    $__sgpicfix = true
    class Spriteset_Global
      alias_method :__sgpicfix_orig, :__followingpkmn__update
      def __followingpkmn__update(*a)
        # CHAMAR o original: no intro/jogo normal corre INTEIRO e as imagens desenham
        # na mesma (o Sprite_Picture#update ja' e' nil-safe p/ o @sprite interno).
        __sgpicfix_orig(*a)
      rescue => _e
        # o original rebentou -> um ELEMENTO nil no @picture_sprites durante o transfer
        # (sprite.update chamado num nil). Refazer o update a prova de nil. ATENCAO: NAO
        # usar disposed? -- Sprite_Picture NAO tem esse metodo (rebentaria e saltaria
        # TODAS as imagens -> ecra preto; era a regressao). So' guardar contra nil.
        begin
          (@playersprite.update rescue nil) if @playersprite
          (@picture_sprites || []).each { |s| (s.update rescue nil) if s } if @picture_sprites.is_a?(Array)
          (@timer_sprite.update rescue nil) if @timer_sprite
        rescue; end
        if (($__tfx_sg_errs ||= 0) < 10)
          $__tfx_sg_errs += 1
          (MKXPDebug.log("[TRANSFERFIX] Spriteset_Global#update protegido (elemento nil no transfer): #{_e.class}: #{_e.message.to_s[0,80].gsub(/\s+/,' ')}") rescue nil)
        end
        nil
      end
    end
    (MKXPDebug.log("[TRANSFERFIX] Spriteset_Global#update a prova de imagem-nil instalado (crash 2 do transfer eliminado)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[TRANSFERFIX] Spriteset_Global falhou: #{e.class}: #{e.message}") rescue nil)
end

begin
  if defined?(Spriteset_Map) && Spriteset_Map.method_defined?(:_animationSprite_update) && !$__animposfix
    $__animposfix = true
    class Spriteset_Map
      alias_method :__animposfix_orig, :_animationSprite_update
      def _animationSprite_update(*a)
        __animposfix_orig(*a)
      rescue => _e
        # o update base rebentou (provavelmente o '+' de coordenadas no transfer):
        # forcar o tilemap para a posicao certa para o mapa novo NAO ficar no canto.
        begin
          tm  = @tilemap
          map = @map
          if tm && map && !(tm.disposed? rescue true)
            xs = (Game_Map::X_SUBPIXELS rescue 4)
            ys = (Game_Map::Y_SUBPIXELS rescue 4)
            nx = ((map.display_x.to_f / xs).round rescue nil)
            ny = ((map.display_y.to_f / ys).round rescue nil)
            (tm.ox = nx) if nx
            (tm.oy = ny) if ny
            (tm.update rescue nil)
          end
        rescue; end
        if (($__tfx_sm_errs ||= 0) < 20)
          $__tfx_sm_errs += 1
          (MKXPDebug.log("[TRANSFERFIX] Spriteset_Map#update base rebentou (#{_e.class}: #{_e.message.to_s[0,80].gsub(/\s+/,' ')}) -> tilemap reposicionado (crash 1 do transfer contido)") rescue nil)
        end
        nil
      end
    end
    (MKXPDebug.log("[TRANSFERFIX] Spriteset_Map#update base com reposicionamento de tilemap em crash instalado (crash 1 do transfer contido)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[TRANSFERFIX] Spriteset_Map falhou: #{e.class}: #{e.message}") rescue nil)
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
  # GENERICO (V33): DESATIVADO. Este hook bridava Input::B(12)->tecla 14 para o
  # binario ANTIGO (KEY_B->RMXP_X=14). O binario atual ja' mapeia KEY_B->RMXP_B=12
  # e o C++ tem trigger?/press?/repeat? corretos. Manter trigger? NATIVO/generico.
  if defined?(Input) && !$__fix_cancelar
    $__fix_cancelar = true
    (MKXPDebug.log("[FIX-CANCELAR] DESATIVADO (input generico) -- B->12 nativo no C++, sem bridge Ruby") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[FIX-CANCELAR] falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  SHADOWZ — diagnostico da ordem-Z da sombra OW (Sprite_OWShadow) vs jogador
# -----------------------------------------------------------------------------
#  Utilizador: "a sombra do NPC nao fica por baixo de mim". A sombra define
#  `@sprite.z = @rsprite.z - 1` (dono.z - 1; plugin ~15540). Quando o jogador
#  esta' ATRAS do NPC, player.z < NPC.z e a sombra do NPC (=NPC.z-1) pode ficar
#  ACIMA de player.z -> desenha por cima do jogador. Para CORRIGIR sem adivinhar
#  (e arriscar regressao tipo-GC), logamos os z REAIS quando a sombra do NPC
#  esta' a <=2 tiles do jogador. Throttle ~120 frames por evento (nao spamma).
#  So' loga; NAO altera rendering -> zero risco.
begin
  if defined?(Sprite_OWShadow) && !$__shadowz_installed
    $__shadowz_installed = true
    class Sprite_OWShadow
      unless method_defined?(:__shadowz_orig_update)
        alias_method :__shadowz_orig_update, :update
        def update(*a, &b)
          __shadowz_orig_update(*a, &b)
          begin
            ev = @event; pl = $game_player
            if ev && @sprite && @rsprite
              aot = (ev.instance_variable_get(:@always_on_top) rescue nil)
              # FIX always_on_top: screen_z devolve 999 p/ eventos always_on_top
              # (base 9465) -> sombra = 998, SEMPRE por cima de tudo, incl. o
              # player. Uma sombra de CHAO nunca deve ser always-on-top. Repo'-la
              # no z de chao REAL do dono (screen_y_ground) p/ encaixar normal
              # com os personagens. So' toca em donos always_on_top -> seguro.
              if aot
                gz = (ev.screen_y_ground rescue nil)
                @sprite.z = (gz - 1) if gz
              end
              # FIX sombra do PLAYER (escadas): a sombra do proprio jogador deve
              # ficar SEMPRE logo por baixo dele (z = player.screen_z - 1). Nas
              # escadas o z default ficava errado (a sombra desaparecia atras do
              # degrau, em vez de ficar sob o player). So' toca na sombra do
              # PROPRIO player. Flag $MKXP_SHADOW_PLAYER_FIX (ON por defeito).
              if pl && ev.equal?(pl) && ($MKXP_SHADOW_PLAYER_FIX != false)
                pz = (pl.screen_z rescue nil)
                if pz
                  @sprite.z = pz - 1
                  # A sombra estava a ser desenhada ACIMA dos pes (anchor oy~14,
                  # quase no fundo do bitmap -> a elipse fica toda acima do ponto
                  # dos pes e o CORPO do player, z=208, tapa-a). Centra-la
                  # verticalmente no ponto dos pes (oy = metade da altura): a
                  # metade de baixo cai no chao, fora do corpo -> visivel.
                  # Flag $MKXP_SHADOW_PLAYER_POS (ON por defeito).
                  if $MKXP_SHADOW_PLAYER_POS != false
                    sbb = (@sprite.bitmap rescue nil)
                    @sprite.oy = (sbb.height / 2) if sbb && !(sbb.disposed? rescue true)
                  end
                  $__shadowpl_t ||= 0
                  if $__shadowpl_t <= 0
                    $__shadowpl_t = 120
                    sb = (@sprite.bitmap rescue nil)
                    sbd = sb.nil? ? "nil" : ((sb.disposed? rescue true) ? "DISP" : "#{sb.width}x#{sb.height}")
                    rb = (@rsprite.bitmap rescue nil)
                    rbd = rb.nil? ? "nil" : ((rb.disposed? rescue true) ? "DISP" : "#{rb.width}x#{rb.height}")
                    (MKXPDebug.log("[SHADOWZ-PL] SOMBRA: z=#{@sprite.z} xy=(#{(@sprite.x rescue '?')},#{(@sprite.y rescue '?')}) " \
                      "oxoy=(#{(@sprite.ox rescue '?')},#{(@sprite.oy rescue '?')}) bmp=#{sbd} op=#{(@sprite.opacity rescue '?')} vis=#{(@sprite.visible rescue '?')} | " \
                      "PLAYER-SPR: z=#{(@rsprite.z rescue '?')} xy=(#{(@rsprite.x rescue '?')},#{(@rsprite.y rescue '?')}) " \
                      "oxoy=(#{(@rsprite.ox rescue '?')},#{(@rsprite.oy rescue '?')}) bmp=#{rbd} | screen_xy=(#{(pl.screen_x rescue '?')},#{(pl.screen_y rescue '?')})") rescue nil)
                  else
                    $__shadowpl_t -= 1
                  end
                end
              end
              # LOG diagnostico (so' quando perto do player). syg = screen_y_ground
              # = a parte posicional do z; se syg_npc>syg_pl o NPC esta' A' FRENTE
              # (sombra por cima e' CORRETO); se syg_npc<syg_pl esta' ATRAS (bug).
              if pl && ev != pl && (ev.x - pl.x).abs <= 2 && (ev.y - pl.y).abs <= 2
                $__shadowz_t ||= {}
                k = (ev.id rescue ev.object_id)
                if ($__shadowz_t[k] ||= 0) <= 0
                  $__shadowz_t[k] = 120
                  pz   = (pl.screen_z rescue -1)
                  sygn = (ev.screen_y_ground rescue -1)
                  sygp = (pl.screen_y_ground rescue -1)
                  rel  = (sygn > sygp) ? "NPC-A-FRENTE(sombra-por-cima-ok)" : "NPC-ATRAS(sombra-devia-estar-sob)"
                  (MKXPDebug.log("[SHADOWZ] ev##{k} aot=#{aot ? 1 : 0} sombra.z=#{@sprite.z} npc.z=#{@rsprite.z} player.z~#{pz} syg_npc=#{sygn} syg_pl=#{sygp} -> #{rel}") rescue nil)
                else
                  $__shadowz_t[k] -= 1
                end
              end
            end
          rescue
          end
        end
      end
    end
    (MKXPDebug.log("[SHADOWZ] hook instalado em Sprite_OWShadow#update (fix always_on_top + loga frente/tras vs jogador)") rescue nil)
  else
    (MKXPDebug.log("[SHADOWZ] Sprite_OWShadow ausente no load -> hook NAO instalado (verei e adio p/ 1o frame)") rescue nil) unless $__shadowz_installed
  end
rescue => e
  (MKXPDebug.log("[SHADOWZ] falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  OW-SHADOW-NPC — fazer as sombras dos NPCs SEGUIREM o evento (V34, bug #4)
# -----------------------------------------------------------------------------
#  "Sombras dos NPCs nao seguem (so' o player)". O player tem sombra a seguir
#  porque o seu sprite (spritesetGlobal.playersprite) esta' SEMPRE no centro do
#  ecra' -> a sombra (=@rsprite.x) tambem. Os NPCs movem-se e a sombra original
#  usa @rsprite.x (posicao do SPRITE), que pode dessincronizar do evento (ordem
#  de update / throttle / culling). Correcao: para EVENTOS (NUNCA o player),
#  repor @sprite.x/y pela posicao AUTORITATIVA do evento (screen_x/screen_y),
#  sempre fresca. Excluir o player e' de proposito -> imune a' regressao do
#  player-invisivel (essa veio de hooks que tocavam no PLAYER). Loga (throttle)
#  a discrepancia rsprite.x vs screen_x p/ confirmar a causa no proximo log.
begin
  if defined?(Sprite_OWShadow) && !$__owshadow_npc_fix
    $__owshadow_npc_fix = true
    module OWShadowNPCFollow
      def update(*a, &b)
        super(*a, &b)
        begin
          ev = @event
          return if ev.nil? || ev.equal?($game_player)          # NUNCA o player
          return if (ev.respond_to?(:jumping?) && (ev.jumping? rescue false))  # jump_sprite trata
          s = @sprite
          return if s.nil? || (s.disposed? rescue true)
          sx = (ev.screen_x rescue nil); sy = (ev.screen_y rescue nil)
          rx = (@rsprite.x rescue nil)
          if sx && rx && (sx - rx).abs >= 4   # DIAG: revela dessincronizacao
            $__owsd_t ||= {}; k = (ev.id rescue ev.object_id)
            if ($__owsd_t[k] ||= 0) <= 0
              $__owsd_t[k] = 150
              (MKXPDebug.log("[OW-SHADOW-NPC] ev##{k} DESSINC: rsprite.x=#{rx} vs screen_x=#{sx} -> corrigido") rescue nil)
            else
              $__owsd_t[k] -= 1
            end
          end
          s.x = sx if sx
          s.y = sy if sy
        rescue
        end
      end
    end
    Sprite_OWShadow.prepend(OWShadowNPCFollow)
    (MKXPDebug.log("[OW-SHADOW-NPC] ATIVO (V34) -- sombras de NPCs seguem por screen_x/y; player intocado") rescue nil)
  end
rescue => _owe
  (MKXPDebug.log("[OW-SHADOW-NPC] falhou: #{_owe.class}: #{_owe.message}") rescue nil)
end

# =============================================================================
#  MENUCLOSE — fechar o menu de pausa Voltseon de forma ROBUSTA (imune ao latch)
# -----------------------------------------------------------------------------
#  RAIZ (provada): input_binding.cpp usa um latch de trigger SINGLE-CONSUME
#  (inp_trigger: se s_trigger_latch[k], devolve true E poe-no a false -> consome
#  na leitura). No loop do menu Voltseon corre tambem pbUpdateSceneMap (o mapa
#  POR BAIXO do menu) + updates de componentes, que leem trigger? tambem; com o
#  FIX-CANCELAR2 a fazer trigger?(12) consumir o latch da tecla 14, o flanco do
#  B/Y e' DRENADO antes de o check do menu o ver. (No cartao NAO ha mapa por
#  baixo -> o unico leitor apanha o latch -> o cartao fecha.) Por isso o B fecha
#  o cartao mas NAO o menu, e trigger?(USE=13) funciona no menu (abre a Bag) mas
#  trigger?(BACK=12)/trigger?(ACTION=11) nao.
#  SOLUCAO: detetar o B com Input.press? (le s_pressed, NAO consome o latch) +
#  deteccao de flanco propria numa ivar do componente (@__mc_b_prev, que reinicia
#  a cada abertura porque o componente e' recriado). No 1o flanco de B, poe
#  @menu.shouldExit=true (o @menu do componente E' a cena, via startComponent
#  (..,self)) e retorna -> a cena sai do loop. press?(14) cobre o B na binary
#  atual (B->RMXP_X=14); press?(12) cobre a recompilada (B->RMXP_B=12). Imune ao
#  latch -> fecha SEMPRE com B. (O Y continua toggle: o mapa abre o menu no
#  ACTION; usa o B para fechar.)
begin
  # DESATIVADO em V17: substituido pelo MENU-V17 (handler completo, acima no
  # per_frame). Como o prepend do MENU-V17 NAO chama super, este update por alias
  # nunca seria chamado de qualquer forma; alem disso fazia um scan press?(0..25)
  # caro a cada frame. Mantido o codigo por referencia, mas nao instala.
  if false && defined?(VoltseonsPauseMenu) && !$__menuclose_installed
    $__menuclose_installed = true
    class VoltseonsPauseMenu
      unless method_defined?(:__mc_orig_update)
        alias_method :__mc_orig_update, :update
        def update(*a, &b)
          # DIAGNOSTICO: (1) prova que o override CORRE no loop do menu; (2) revela
          # que tecla e' o B (scan press? 0..25) -- "B detetado" nunca disparou,
          # so' assim sei se e' tecla errada ou se o #update nem e' chamado.
          begin
            $__mc_calls = ($__mc_calls || 0) + 1
            if $__mc_calls <= 3
              (MKXPDebug.log("[MENUCLOSE-DIAG] VoltseonsPauseMenu#update CHAMADO (##{$__mc_calls}) -- override ativo no loop") rescue nil)
            end
            pressed = []
            (0..25).each { |k| pressed << k if (Input.press?(k) rescue false) }
            if !pressed.empty? && pressed != @__mc_last_pressed
              @__mc_last_pressed = pressed
              (MKXPDebug.log("[MENUCLOSE-DIAG] teclas premidas no menu: #{pressed.inspect}") rescue nil)
            end
          rescue
          end
          # B = 14 (X na binary atual) OU 12 (B recompilada) OU 11 (ACTION). NAO
          # incluir 13 (USE/confirmar) -- fecharia ao confirmar.
          held = ((Input.press?(14) rescue false) || (Input.press?(12) rescue false) ||
                  (Input.press?(11) rescue false))
          if held && !@__mc_b_prev
            @__mc_b_prev = true
            if @menu && (@menu.respond_to?(:shouldExit=) rescue false)
              @menu.shouldExit = true
              (MKXPDebug.log("[MENUCLOSE] B detetado (press? flanco) -> @menu.shouldExit=true (menu vai fechar)") rescue nil)
            else
              (MKXPDebug.log("[MENUCLOSE] B detetado mas @menu sem shouldExit= (@menu=#{(@menu.class rescue '?')}) -- nao fechou") rescue nil)
            end
            return
          end
          @__mc_b_prev = held
          __mc_orig_update(*a, &b)
        end
      end
    end
    (MKXPDebug.log("[MENUCLOSE] override instalado em VoltseonsPauseMenu#update (fecha com B via press?+flanco, imune ao latch)") rescue nil)
  else
    (MKXPDebug.log("[MENUCLOSE] VoltseonsPauseMenu ausente no load -> override NAO instalado") rescue nil) unless $__menuclose_installed
  end
rescue => e
  (MKXPDebug.log("[MENUCLOSE] falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  TILETHROTTLE — quarto a 20fps: autotiles animados re-blittados TODOS os frames
# -----------------------------------------------------------------------------
#  O CustomTilemap (pokemon_tilemap_fix.rb) re-renderiza os autotiles animados
#  (agua, etc.) a cada chamada de refreshLayer0(true). Mas a animacao so' avanca
#  quando (Graphics.frame_count / Animated_Autotiles_Frames) muda -- e
#  Animated_Autotiles_Frames = 5*frame_rate/30 = 5 @30fps. Logo o frame de
#  animacao muda 1 vez em cada 5; re-blittar nos outros 4 e' trabalho perdido
#  (mesma imagem). Isto domina o quarto (mapa hasanim=true).
#  FIX: envolver refreshLayer0 e SALTAR o re-blit quando NEM o indice de animacao
#  NEM a camara (@ox/@oy) mudaram desde o ultimo -> o layer0 ja' tem o conteudo
#  certo. So' atua no caso animado+ja-refrescado; o caso sem-animacao ja' e'
#  saltado pelo proprio CustomTilemap (return true se @prioautotiles vazio).
#  Conservador (so' salta o REDUNDANTE) e desligavel: $MKXP_TILE_THROTTLE=false.
begin
  if defined?(CustomTilemap) && !$__tilethrottle_installed
    $__tilethrottle_installed = true
    class CustomTilemap
      unless method_defined?(:__tt_orig_refreshLayer0)
        alias_method :__tt_orig_refreshLayer0, :refreshLayer0
        def refreshLayer0(autotiles = false)
          if autotiles && @fullyrefreshedautos && $MKXP_TILE_THROTTLE != false
            _aff = (defined?(Animated_Autotiles_Frames) ? Animated_Autotiles_Frames : 5)
            _aff = 1 if !_aff || _aff < 1
            _af  = Graphics.frame_count / _aff
            if @__tt_af == _af && @__tt_ox == @ox && @__tt_oy == @oy
              return true   # animacao e camara iguais -> nada a redesenhar
            end
            @__tt_af = _af; @__tt_ox = @ox; @__tt_oy = @oy
          end
          __tt_orig_refreshLayer0(autotiles)
        end
      end
    end
    (MKXPDebug.log("[TILETHROTTLE] override instalado em CustomTilemap#refreshLayer0 (salta re-blit de autotiles quando animacao+camara iguais; Animated_Autotiles_Frames=#{defined?(Animated_Autotiles_Frames) ? Animated_Autotiles_Frames : '?'}; desligar com $MKXP_TILE_THROTTLE=false)") rescue nil)
  else
    (MKXPDebug.log("[TILETHROTTLE] CustomTilemap ausente no load -> override NAO instalado") rescue nil) unless $__tilethrottle_installed
  end
rescue => e
  (MKXPDebug.log("[TILETHROTTLE] falhou: #{e.class}: #{e.message}") rescue nil)
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
          begin
            bt = (e.backtrace || [])
            if bt.empty?
              (MKXPDebug.log("[MENUGUARD]   (sem backtrace disponivel)") rescue nil)
            else
              bt[0, 10].each_with_index { |ln, i| (MKXPDebug.log("[MENUGUARD]   bt[#{i}] #{ln}") rescue nil) }
            end
          rescue; end
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
#  MENUCOMP-GUARD — blinda o refresh dos componentes do Voltseon Pause Menu
# -----------------------------------------------------------------------------
#  O log mostra call_menu a rebentar SEMPRE com
#    TypeError: Object cannot be converted to Integer
#  dentro de pbRefresh -> @components.each{|c| c.refresh}. Um dos HUD
#  (SafariHud/BugContestHud/PokemonPartyHud/DateAndTimeHud/NewQuestHud) lanca a
#  excecao; o MENUGUARD apanha-a mas ABORTA o menu inteiro -> "nao aparece nada"
#  + fundo laranja. Aqui envolvemos o refresh de CADA componente: se um falhar e'
#  IGNORADO e LOGADO (classe + backtrace) e o resto do menu desenha na mesma.
#  Mesma guarda (__mkxp_orig_refresh_guard) que a versao no binding -> nunca
#  embrulha duas vezes. Podes testar o menu trocando SO' este ficheiro (sem
#  recompilar): a linha [MENUCOMP] dira' exatamente QUAL componente rebenta.
# =============================================================================
begin
  %w[SafariHud BugContestHud PokemonPartyHud DateAndTimeHud NewQuestHud].each do |cn|
    next unless Object.const_defined?(cn.to_sym)
    k = Object.const_get(cn.to_sym)
    next unless k.is_a?(Class) && k.method_defined?(:refresh)
    next if k.method_defined?(:__mkxp_orig_refresh_guard)
    k.send(:alias_method, :__mkxp_orig_refresh_guard, :refresh)
    k.send(:define_method, :refresh) do |*a, &b|
      begin
        __mkxp_orig_refresh_guard(*a, &b)
      rescue => re
        $__mkxp_menucomp_warned ||= {}
        unless $__mkxp_menucomp_warned[self.class.name]
          $__mkxp_menucomp_warned[self.class.name] = true
          (MKXPDebug.log("[MENUCOMP] #{self.class}#refresh rebentou: #{re.class}: #{re.message} -> componente ignorado, menu continua") rescue nil)
          (re.backtrace || [])[0, 8].each_with_index { |ln, i| (MKXPDebug.log("[MENUCOMP]   bt[#{i}] #{ln}") rescue nil) }
        end
      end
    end
  end
  (MKXPDebug.log("[MENUCOMP] guard de refresh instalado nos componentes do pause menu") rescue nil)
rescue => e
  (MKXPDebug.log("[MENUCOMP] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
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
          # Comparacao campo-a-campo (SEM alocar array -> zero pressao de GC, ao
          # contrario da versao anterior que criava 1 array por evento por frame).
          sx = (c.screen_x rescue 0); sy = (c.screen_y rescue 0)
          pt = (c.pattern rescue 0);  dr = (c.direction rescue 2)
          op = (c.opacity rescue 255); ti = (c.tile_id rescue 0)
          cn = (c.character_name rescue ""); tr = (c.transparent rescue false)
          now = (Graphics.frame_count rescue 0)
          if @__pst_fc && (now - @__pst_fc) < 16 &&
             sx == @__pst_sx && sy == @__pst_sy && pt == @__pst_pt &&
             dr == @__pst_dr && op == @__pst_op && ti == @__pst_ti &&
             cn == @__pst_cn && tr == @__pst_tr
            return   # estado de desenho identico ao ultimo -> salta (~21 setters)
          end
          @__pst_sx = sx; @__pst_sy = sy; @__pst_pt = pt; @__pst_dr = dr
          @__pst_op = op; @__pst_ti = ti; @__pst_cn = cn; @__pst_tr = tr
          @__pst_fc = now
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
              # marcos: 120(~4s), 360(~12s), 900(~30s) -> distinguir LENTO de INFINITO
              if @__stuck == 120 || @__stuck == 360 || @__stuck == 900
                cmd  = (lst[idx] rescue nil)
                code = (cmd.code rescue '?')
                par  = (cmd.parameters.inspect[0,90] rescue '?')
                secs = (@__stuck / 30)
                (MKXPDebug.log("[INTERP-STUCK] map=#{($game_map.map_id rescue '?')} idx=#{idx} code=#{code} params=#{par} (preso ~#{secs}s)") rescue nil)
                if @__stuck == 120
                  # 1a vez: estado do jogador + comandos a' volta (contexto do cutscene)
                  begin
                    mw   = ($game_temp.message_window_showing rescue '?')
                    mrw  = (@move_route_waiting rescue '?')
                    mrf  = ($game_player.move_route_forcing rescue '?')
                    anim = ($game_player.animation_id rescue '?')
                    (MKXPDebug.log("[INTERP-STUCK]   estado: msg_window=#{mw} move_route_wait=#{mrw} player_mrf=#{mrf} player_anim=#{anim}") rescue nil)
                  rescue; end
                  begin
                    (([idx-2,0].max)..(idx+3)).each do |j|
                      c = (lst[j] rescue nil)
                      next unless c
                      cc = (c.code rescue '?')
                      cp = (c.parameters.inspect[0,70] rescue '?')
                      mk = (j == idx) ? "  <== PRESO AQUI" : ""
                      (MKXPDebug.log("[INTERP-STUCK]   ctx[#{j}] code=#{cc} #{cp}#{mk}") rescue nil)
                    end
                  rescue; end
                end
                if @__stuck == 900
                  (MKXPDebug.log("[INTERP-STUCK]   >>> VEREDITO: >30s no MESMO comando = BLOQUEIO EFETIVO (nao e' lentidao) -- este comando nunca termina.") rescue nil)
                end
              end
            else
              # mudou de comando: se estava preso ha' muito, DESBLOQUEOU (era lentidao)
              if (@__stuck || 0) >= 120
                (MKXPDebug.log("[INTERP-STUCK] DESBLOQUEOU idx #{@__last_idx} apos ~#{((@__stuck||0)/30)}s -> avancou p/ idx #{idx} (era LENTIDAO, nao bloqueio)") rescue nil)
              end
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

# ============================================================================
# [VOIDFIX] (anexado v7) -- corrige "sprites no void": Sprite_Characters cujo
# src_rect ficou na folha 4x4 inteira (ex: 256x256) em vez de um frame 64x64.
# O FRAMEDUMP mostrou: #049 Sprite_Character src=[0,0,256,256] dst=(224,144 256x256)
# com o anchor o=(32,64) (correto p/ frame 64x64) -> o recorte nao pegou neste
# sprite (provavelmente self.src_rect.set devolve copia no port, OU um crash
# interno antes da linha do recorte foi engolido pelo FXGUARD).
# Solucao: hook OUTERMOST que, depois de correr toda a cadeia interna, FORCA
# self.src_rect = Rect.new(frame) (ASSIGNMENT, nao .set) quando deteta folha
# inteira por recortar. So atua quando src_rect.width == bitmap.width (=folha
# completa) e a folha tem >=128px de largura divisivel por 4 (= sheet 4-col).
# O sprite do jogador (ja recortado por FIX-GRELHA7) tem src_rect.width != bitmap.width
# -> VOIDFIX nao lhe toca. Sprites de 1 frame pequenos (<128) tambem sao ignorados.
# ============================================================================
begin
  if defined?(Sprite_Character) && Sprite_Character.method_defined?(:update) && !$__voidfix
    $__voidfix = true
    class Sprite_Character
      alias_method :__voidfix_orig_update, :update
      def update(*a, &b)
        r = (__voidfix_orig_update(*a, &b) rescue nil)
        begin
          bmp = self.bitmap
          if bmp && (@tile_id.to_i == 0) && @character
            sr = self.src_rect
            bw = (bmp.width rescue 0)
            if sr && bw >= 128 && (bw % 4 == 0) && ((sr.width rescue 0) == bw)
              bh = (bmp.height rescue 0)
              cw = (@cw && @cw > 0) ? @cw : (bw / 4)
              chh = (@ch && @ch > 0) ? @ch : (bh / 4)
              if cw > 0 && chh > 0 && cw < bw
                pat = (@character.pattern rescue 0)
                pat = 0 unless pat.is_a?(Integer) && pat >= 0 && pat <= 3
                dir = (@character.direction rescue 2)
                dir = 2 unless dir.is_a?(Integer)
                row = (dir - 2) / 2
                row = 0 if row < 0 || row > 3
                self.src_rect = Rect.new(pat * cw, row * chh, cw, chh)
                unless $__voidfix_logged
                  $__voidfix_logged = true
                  (MKXPDebug.log("[VOIDFIX] folha inteira detetada em '#{@character_name}' (#{bw}x#{bh}) -> recortado p/ frame #{cw}x#{chh} via assign (pat=#{pat} dir=#{dir})") rescue nil)
                end
              end
            end
          end
        rescue => e
          unless $__voidfix_err
            $__voidfix_err = true
            (MKXPDebug.log("[VOIDFIX] erro: #{e.class}: #{e.message}") rescue nil)
          end
        end
        r
      end
    end
    (MKXPDebug.log("[VOIDFIX] hook Sprite_Character#update instalado (outermost; forca recorte de folha inteira)") rescue nil)
  else
    (MKXPDebug.log("[VOIDFIX] Sprite_Character ausente ou ja' instalado -- ignorado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[VOIDFIX] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
end

# ============================================================================
# [WSKINFIX] (anexado v7) -- corrige "ecra rosa": a caixa do nome (ex: "Auntie")
# pede a windowskin 'Graphics/Windowskins/speech black' que NAO existe no jogo
# -> o port cria fallback magenta 32x32 -> esticado = mancha rosa.
# A camada de windowskins vem dos patches embebidos (corre DEPOIS desta probe,
# t~3.89s), por isso instalamos o redirect ao 1o frame (deferido via Scene_Map#update),
# garantindo que ja existe. Duas redes:
#   (1) RPG::Cache.windowskin: se a bitmap devolvida for 32x32 (=fallback magenta),
#       troca por 'speech se 1' (que carrega bem). Uma windowskin legitima nunca e' 32x32.
#   (2) pbResolveBitmap: se um path de Windowskins/ resolver a nil, devolve 'speech se 1'.
# So atua em casos falhados -> nao pode partir windowskins que funcionam.
# ============================================================================
begin
  if defined?(Scene_Map) && Scene_Map.method_defined?(:update) && !$__wskinfix_defer
    $__wskinfix_defer = true
    class Scene_Map
      alias_method :__wskinfix_defer_orig_update, :update
      def update(*a, &b)
        unless $__wskinfix
          $__wskinfix = true
          begin
            installed = []
            # (1) RPG::Cache.windowskin -> troca fallback magenta 32x32 por 'speech se 1'
            # ISOLADO: se o alias_method falhar (windowskin via method_missing, NAO aliasavel),
            # nao pode abortar os hooks (2)/(3) abaixo (pbResolveBitmap + PlayerSprites preview).
            begin
            if defined?(RPG) && defined?(RPG::Cache) && (RPG::Cache.respond_to?(:windowskin) rescue false) &&
               (RPG::Cache.singleton_class.instance_method(:windowskin) rescue nil)
              RPG::Cache.singleton_class.class_eval do
                unless method_defined?(:__wskinfix_orig_windowskin)
                  alias_method :__wskinfix_orig_windowskin, :windowskin
                  def windowskin(filename, *rest)
                    bmp = (__wskinfix_orig_windowskin(filename, *rest) rescue nil)
                    begin
                      if bmp && (bmp.width rescue 0) == 32 && (bmp.height rescue 0) == 32
                        alt = (__wskinfix_orig_windowskin("speech se 1", *rest) rescue nil)
                        if alt && (alt.width rescue 0) != 32
                          unless $__wskinfix_hit
                            $__wskinfix_hit = true
                            (MKXPDebug.log("[WSKINFIX] windowskin em falta '#{filename}' (fallback 32x32) -> trocada por 'speech se 1'") rescue nil)
                          end
                          return alt
                        end
                      end
                    rescue; end
                    bmp
                  end
                end
              end
              installed << "RPG::Cache.windowskin"
            end
            rescue => _we
              (MKXPDebug.log("[WSKINFIX] hook windowskin SALTADO (#{_we.class}: #{_we.message.to_s[0,60]}) -- pbResolveBitmap/PlayerSprites continuam") rescue nil)
            end
            # (2) pbResolveBitmap -> redireciona Windowskins/ em falta  [ISOLADO]
            begin
            if defined?(pbResolveBitmap)
              Object.class_eval do
                unless private_method_defined?(:__wskinfix_orig_prb) || method_defined?(:__wskinfix_orig_prb)
                  alias_method :__wskinfix_orig_prb, :pbResolveBitmap
                  def pbResolveBitmap(filename, *rest)
                    res = (__wskinfix_orig_prb(filename, *rest) rescue nil)
                    begin
                      if (res.nil? || res == false || res.to_s.strip.empty?) && filename.to_s =~ /Windowskins/i
                        alt = (__wskinfix_orig_prb("Graphics/Windowskins/speech se 1", *rest) rescue nil)
                        return alt if alt && !alt.to_s.strip.empty?
                      end
                      # ── PREVIEW DO ROUPEIRO (CE#173 ClothesChangeUI) ──────────────
                      # O menu pede 'Graphics/Pictures/PlayerSprites/<numero>' mas os
                      # ficheiros sao 'POKEMONTRAINER_<Nome>[_<N>]'. Redireciona para o
                      # sprite real do jogador. Loga trainer_type/outfit p/ afinar.
                      if (res.nil? || res == false || res.to_s.strip.empty?) && filename.to_s =~ /PlayerSprites\//i
                        tt = ($Trainer.trainer_type.to_s rescue nil)
                        of = ($Trainer.outfit rescue 0).to_i
                        prt = filename.to_s.split("PlayerSprites/")
                        req = (prt.length > 1) ? prt[-1].to_s : ""
                        if tt && !tt.empty?
                          cands = ["Graphics/Pictures/PlayerSprites/#{tt}"]
                          cands << "Graphics/Pictures/PlayerSprites/#{tt}_#{req}" if req =~ /\A[0-9]+\z/ && req != "0"
                          cands << "Graphics/Pictures/PlayerSprites/#{tt}_#{of}" if of > 0
                          hit = nil
                          cands.each do |cand|
                            alt = (__wskinfix_orig_prb(cand, *rest) rescue nil)
                            if alt && !alt.to_s.strip.empty?; hit = [cand, alt]; break; end
                          end
                          $__psdbg_n = ($__psdbg_n || 0)
                          if $__psdbg_n < 8
                            $__psdbg_n += 1
                            if hit
                              (MKXPDebug.log("[PSDBG] redirect '#{filename}' -> '#{hit[0]}' (tt=#{tt} outfit=#{of} req=#{req})") rescue nil)
                            else
                              (MKXPDebug.log("[PSDBG] SEM-FICHEIRO '#{filename}' tt=#{tt} outfit=#{of} req=#{req} tentou=#{cands.inspect}") rescue nil)
                            end
                          end
                          return hit[1] if hit
                        end
                      end
                    rescue; end
                    res
                  end
                end
              end
              installed << "pbResolveBitmap"
            end
            rescue => _pe2
              (MKXPDebug.log("[WSKINFIX] hook pbResolveBitmap SALTADO (#{_pe2.class}) -- PlayerSprites continua") rescue nil)
            end
            # ── REDIRECT PlayerSprites/<numero> no CARREGADOR CENTRAL (preview do roupeiro) ──
            # AnimatedBitmap.new (usado por IconSprite#setBitmap) NAO passa pelo pbResolveBitmap
            # acima -- vai por RPG::Cache.load_bitmap. Aqui apanhamos 'PlayerSprites/<numero>'
            # e redirecionamos para o sprite real do jogador (POKEMONTRAINER_<Nome>[_N]).
            begin
            if defined?(RPG) && defined?(RPG::Cache) && (RPG::Cache.respond_to?(:load_bitmap) rescue false)
              RPG::Cache.singleton_class.class_eval do
                unless method_defined?(:__ps_orig_lb)
                  alias_method :__ps_orig_lb, :load_bitmap
                  def load_bitmap(folder_name, filename, hue = 0)
                    begin
                      # combina folder+filename: a chamada pode vir dividida
                      # (folder='Graphics/Pictures/PlayerSprites/' filename='0')
                      fn = "#{folder_name}#{filename}"
                      idx = fn.index("PlayerSprites/")
                      if idx
                        after = fn[(idx + 14)..-1].to_s
                        if after.length > 0 && after =~ /\A[0-9]+\z/
                          tt = ($Trainer.trainer_type.to_s rescue nil)
                          of = ($Trainer.outfit rescue 0).to_i
                          num = after.to_i
                          $__pscache_n = ($__pscache_n || 0) + 1
                          if tt && !tt.empty?
                            base = "Graphics/Pictures/PlayerSprites/"
                            cands = []
                            cands << "#{base}#{tt}_#{num}" if num > 0
                            cands << "#{base}#{tt}_#{of}" if of > 0
                            cands << "#{base}#{tt}"
                            hitc = nil
                            cands.each { |cd| if (pbResolveBitmap(cd) rescue nil); hitc = cd; break; end }
                            if hitc
                              (MKXPDebug.log("[PSCACHE] redirect '#{fn}' -> '#{hitc}' (tt=#{tt} of=#{of} num=#{num})") rescue nil) if $__pscache_n <= 8
                              return __ps_orig_lb("", hitc, hue)
                            else
                              (MKXPDebug.log("[PSCACHE] SEM-FICHEIRO '#{fn}' tt=#{tt} of=#{of} num=#{num} tentou=#{cands.inspect}") rescue nil) if $__pscache_n <= 8
                            end
                          else
                            (MKXPDebug.log("[PSCACHE] '#{fn}' visto mas $Trainer.trainer_type=#{tt.inspect} (treinador ausente)") rescue nil) if $__pscache_n <= 8
                          end
                        end
                      end
                    rescue; end
                    __ps_orig_lb(folder_name, filename, hue)
                  end
                end
              end
              installed << "RPG::Cache.load_bitmap(PlayerSprites)"
            end
            rescue => _pe3
              (MKXPDebug.log("[WSKINFIX] hook PlayerSprites SALTADO (#{_pe3.class}: #{_pe3.message.to_s[0,60]})") rescue nil)
            end
            if installed.empty?
              (MKXPDebug.log("[WSKINFIX] nada para instalar no 1o frame (RPG::Cache.windowskin/pbResolveBitmap ausentes)") rescue nil)
            else
              (MKXPDebug.log("[WSKINFIX] redirect de windowskins em falta -> 'speech se 1' instalado no 1o frame (#{installed.join('+')})") rescue nil)
            end
          rescue => e
            (MKXPDebug.log("[WSKINFIX] instalacao no 1o frame falhou: #{e.class}: #{e.message}") rescue nil)
          end
        end
        __wskinfix_defer_orig_update(*a, &b)
      end
    end
    (MKXPDebug.log("[WSKINFIX] defer-installer registado em Scene_Map#update") rescue nil)
  else
    (MKXPDebug.log("[WSKINFIX] Scene_Map ausente ou ja' instalado -- ignorado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[WSKINFIX] defer registo falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  SUPER-DEBUG — sondas DIRIGIDAS p/ apanhar varios bugs num so' log
# -----------------------------------------------------------------------------
#  Liga/desliga: $MKXP_SUPERDEBUG = false desliga tudo o que esta' aqui.
#  Filosofia: ZERO log por-frame as cegas (floodava o cartao e mudava o timing
#  -> Heisenbug). Cada sonda dispara so' no contexto certo e limita-se a N
#  linhas, p/ o log ser GRANDE mas legivel e nao alterar os bugs.
# =============================================================================
$MKXP_SUPERDEBUG = true if $MKXP_SUPERDEBUG.nil?

if $MKXP_SUPERDEBUG
  # ---- TREE-PROBE (bug 1: arvore invisivel a meio, Map043) ------------------
  #  So' nos mapas de tileset GIGANTE (>4096px alto = o Gen4 256x31040). Para
  #  tiles de Y-alto no tileset regista id, Y calculado e ALPHA central do bitmap
  #  EXTRAIDO. Se tiles de Y alto saem alpha=0, a extracao no tileset alto falha
  #  -> e' a causa do topo invisivel. Max 80 linhas.
  begin
    if defined?(CustomTilemap) && CustomTilemap.method_defined?(:getRegularTile) &&
       !CustomTilemap.method_defined?(:__treeprobe_orig_getRegularTile)
      class CustomTilemap
        alias_method :__treeprobe_orig_getRegularTile, :getRegularTile
        def getRegularTile(sprite, id)
          r = __treeprobe_orig_getRegularTile(sprite, id)
          begin
            ts = (@tileset rescue nil)
            tall = (ts && ts.respond_to?(:height) && ts.height > 4096) rescue false
            if tall
              $__treeprobe_count ||= 0
              ty = (id.is_a?(Integer) && id >= 384) ? ((id - 384) / 8) * 32 : -1
              if $__treeprobe_count < 80 && (ty > 1024 || $__treeprobe_count < 24)
                $__treeprobe_count += 1
                bm = (sprite.bitmap rescue nil)
                bw = (bm ? (bm.width rescue -1) : -1)
                bh = (bm ? (bm.height rescue -1) : -1)
                ca = -9
                if bm && bw > 0 && bh > 0
                  px = (bm.get_pixel(bw / 2, bh / 2) rescue nil)
                  ca = (px ? (px.alpha rescue -3) : -2)
                end
                (MKXPDebug.log("[TREE-PROBE] id=#{id} tileY=#{ty} tilesetH=#{ts.height} bmp=#{bw}x#{bh} alpha_centro=#{ca}") rescue nil)
              end
            end
          rescue
          end
          r
        end
      end
      (MKXPDebug.log("[TREE-PROBE] instalado (so' tilesets >4096px, max 80 linhas)") rescue nil)
    end
  rescue => e
    (MKXPDebug.log("[TREE-PROBE] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
  end

  # ---- CE-TRACE (bug 3: outfit -- CE#34 trava antes do picker CE#212) --------
  #  Regista CADA comando que os interpretadores do CE#34 e do CE#212 executam
  #  (identificados por @list ser a lista do Common Event, em cache). Mostra ate'
  #  onde o CE#34 corre e se o CE#212 alguma vez arranca. Max 70 + 50 linhas.
  begin
    if defined?(Interpreter) && Interpreter.method_defined?(:execute_command) &&
       !Interpreter.method_defined?(:__cetrace_orig_execute_command)
      class Interpreter
        alias_method :__cetrace_orig_execute_command, :execute_command
        def execute_command
          begin
            lst = @list
            dce = $data_common_events
            if lst && dce
              if $__ce_lists.nil?
                $__ce_lists = {}
                [27, 34, 212].each { |n| $__ce_lists[n] = ((dce[n].list rescue nil)) }
              end
              cmd = (lst[@index] rescue nil)
              if cmd
                code = (cmd.code rescue '?')
                p0 = (cmd.parameters[0] rescue nil)
                if $__ce_lists[27] && lst.equal?($__ce_lists[27])
                  $__ce27_n ||= 0
                  if $__ce27_n < 90
                    $__ce27_n += 1
                    (MKXPDebug.log("[CE-TRACE] CE#27  idx=#{@index} code=#{code} p0=#{p0.inspect}") rescue nil)
                  end
                elsif $__ce_lists[34] && lst.equal?($__ce_lists[34])
                  $__ce34_n ||= 0
                  if $__ce34_n < 70
                    $__ce34_n += 1
                    (MKXPDebug.log("[CE-TRACE] CE#34  idx=#{@index} code=#{code} p0=#{p0.inspect}") rescue nil)
                  end
                elsif $__ce_lists[212] && lst.equal?($__ce_lists[212])
                  $__ce212_n ||= 0
                  if $__ce212_n < 50
                    $__ce212_n += 1
                    (MKXPDebug.log("[CE-TRACE] CE#212 idx=#{@index} code=#{code} p0=#{p0.inspect}") rescue nil)
                  end
                end
              end
            end
          rescue
          end
          __cetrace_orig_execute_command
        end
      end
      (MKXPDebug.log("[CE-TRACE] instalado (CE#34 max70, CE#212 max50)") rescue nil)
    end
  rescue => e
    (MKXPDebug.log("[CE-TRACE] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
  end

  (MKXPDebug.log("[SUPER-DEBUG] sondas dirigidas ativas (TREE-PROBE + CE-TRACE + FXGUARD bt + MENUCOMP bt)") rescue nil)
end

# =====================================================================================
# HOUSEFIX (bug #1: casas/arvores invisiveis -- tiles de PRIORIDADE da coluna 1 do
# tileset compactado). PROVA (ATDBG): no compactado em cache, a coluna 1 (x>=256) esta
# CHEIA (comp=256@(256,0)), mas a extracao do motor (getRegularTile -> blitVWrappedPixels
# ramo wrap) devolve um tile VAZIO (opq=0). get_pixel le a coluna 1 corretamente (e' o que
# o comp= usa). Logo extraimos os tiles de WRAP por copia get_pixel/set_pixel a partir do
# compactado AUTORITATIVO ($__tsc_cache[mid][:bmp]) -- robusto quer @tileset!=cache, quer
# o blt-da-coluna-1 falhe. So' intercepta tiles de wrap ((id-384)>>3*32 >= TILESET_HEIGHT);
# a coluna 0 continua a usar o caminho original (blt direto, comprovadamente correto).
# Custo: ~1024 get_pixel por tile de col-1, UMA vez por carga de mapa (cacheado em
# @regularTileInfo). Nao corre por frame.
# =====================================================================================
begin
  if defined?(CustomTilemap) && CustomTilemap.method_defined?(:getRegularTile) &&
     !CustomTilemap.method_defined?(:__housefix_orig_getRegularTile)
    class CustomTilemap
      alias_method :__housefix_orig_getRegularTile, :getRegularTile
      def getRegularTile(sprite, id)
        begin
          if id.is_a?(Integer) && id >= 384
            th = (VWrap::TILESET_HEIGHT rescue 1024) || 1024
            th = 1024 if th <= 0
            k  = id - 384
            row_y = (k >> 3) * 32
            if row_y >= th                       # tile de WRAP (coluna >= 1)
              mid = ($game_map ? ($game_map.map_id rescue -1) : -1)
              tc  = (defined?($__tsc_cache) && $__tsc_cache) ? $__tsc_cache[mid] : nil
              cb  = (tc && tc[:bmp]) ? tc[:bmp] : (@tileset rescue nil)
              if cb && !(cb.disposed? rescue true)
                @regularTileInfo ||= {}
                bmp = @regularTileInfo[id]
                if !(bmp && !(bmp.disposed? rescue true))
                  tw  = ((@tileSrcWidth  rescue 32) || 32)
                  thg = ((@tileSrcHeight rescue 32) || 32)
                  dw  = ((@tileWidth  rescue 32) || 32)
                  dh  = ((@tileHeight rescue 32) || 32)
                  bmp = Bitmap.new(dw, dh)
                  col = row_y / th
                  sxs = col * 256 + (k & 7) * 32
                  sys = row_y % th
                  cw  = (cb.width  rescue 0)
                  ch  = (cb.height rescue 0)
                  yy = 0
                  while yy < thg && yy < dh
                    xx = 0
                    while xx < tw && xx < dw
                      px = sxs + xx; py = sys + yy
                      if px < cw && py < ch
                        c = (cb.get_pixel(px, py) rescue nil)
                        (bmp.set_pixel(xx, yy, c) if c && (c.alpha rescue 0) > 0) rescue nil
                      end
                      xx += 1
                    end
                    yy += 1
                  end
                  @regularTileInfo[id] = bmp
                  $__housefix_n ||= 0
                  if $__housefix_n < 12
                    $__housefix_n += 1
                    (MKXPDebug.log("[HOUSEFIX] id=#{id} k=#{k} col=#{col} <-(#{sxs},#{sys}) cb=#{cw}x#{ch} dst=#{dw}x#{dh} (copia CPU col-1)") rescue nil)
                  end
                end
                sprite.bitmap = bmp if (sprite.bitmap rescue nil) != bmp
                return
              end
            end
          end
        rescue => _hf
          ($__housefix_err ||= 0
           if $__housefix_err < 6
             $__housefix_err += 1
             (MKXPDebug.log("[HOUSEFIX] erro id=#{id}: #{_hf.class}: #{_hf.message}") rescue nil)
           end) rescue nil
        end
        __housefix_orig_getRegularTile(sprite, id)
      end
    end
    (MKXPDebug.log("[HOUSEFIX] instalado -- extracao CPU dos tiles col-1/wrap a partir do compactado em cache (resolve casas/arvores invisiveis)") rescue nil)
  else
    (MKXPDebug.log("[HOUSEFIX] SKIP (CustomTilemap/getRegularTile ausente ou ja' instalado)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[HOUSEFIX] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
end
