# $MKXP_TRY_CONTINUE = [x, y]  # (OFF)
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
  # >>> MARCADOR DE BUILD UNICO <<< — se NAO vires esta linha exata no log, o 3DS leu
  # um ficheiro ANTIGO (a etiqueta 'V35' era igual em varias versoes e enganava-nos).
  (MKXPDebug.log("[BUILD-ID] >>>>> SOLAR-2026-07-04-V63-PERF <<<<<  (procura esta linha 1o: confirma que carregaste o probe novo)") rescue nil)

  # ─── [PERF 2026-07-04] MODO SILENCIOSO GLOBAL ──────────────────────────────
  # Os bugs que estes diagnosticos cacavam estao FECHADOS (save/arvore/estatua/
  # menu validados). Em quiet, os blocos DIAG por-frame (TF/TCMD/TPIC, FOLLOWDIAG,
  # MENU-INPUT, GILBERT2, ADDDEP, PICSHOW, TMDBG...) nao correm — poupam ~1 log
  # SD/frame + dezenas de allocations/dispatches por frame + hitches de dump.
  # TODOS os FIXES continuam ativos. Reativar diagnosticos: $MKXP_PROBE_QUIET=false.
  $MKXP_PROBE_QUIET = true if $MKXP_PROBE_QUIET.nil?
  (MKXPDebug.log("[PROBE] QUIET=#{$MKXP_PROBE_QUIET} (diagnosticos por-frame #{$MKXP_PROBE_QUIET ? 'OFF' : 'ON'})") rescue nil)
  # [CHARBATCH 2026-07-04] modo VERIFY (2): corre o update ORIGINAL e compara o
  # batch nativo contra ele (zero mudanca de comportamento; loga [CHARBATCH|DIFF]
  # se divergir). Quando um log de gameplay vier limpo, muda-se para 1 (ligado).
  $MKXP_NATIVE_CHAR_BATCH = 1 if $MKXP_NATIVE_CHAR_BATCH.nil?   # LIGADO (verify limpo: tiles+aot cobertos; unico diff = boy_run3, NPC invisivel ja-quebrado no vanilla)   # LIGADO (verify: ~1900 cmp, diffs so' de 1 sprite ja'-quebrado no vanilla)

  # ─── REVERTER O SORT DO MOTOR (ZSORT-REGTIE) ─────────────────────────────
  # REGRESSAO confirmada: o desempate por ordem de criacao no topo (>=99990) do
  # motor PARTE a ordenacao por z local DENTRO de um viewport a vp.z=99999. O
  # ecra de escrever o nome (Window_TextEntry/CharacterEntry, sprites z=100 num
  # viewport 99999) ficou TAPADO por um sprite z=0 do mesmo viewport criado
  # depois -> "o teclado nao aparece". Desligamos o sort novo do motor SEM
  # recompilar (o binario J21 le este global 1x/frame e reverte ao z local).
  # O motor volta a precisar do contorno do probe (FIX-OUTFITZ, reativado abaixo)
  # para o z-order do menu. O fix de motor precisa de outra abordagem (preservar
  # o z local dentro do viewport); fica para reescrita cuidadosa.
  $MKXP_ZSORT_REGTIE = false
  (MKXPDebug.log("[ZREVERT] $MKXP_ZSORT_REGTIE=false -> motor reverte ao desempate por z local (corrige o teclado do nome). FIX-OUTFITZ do probe reativado.") rescue nil)

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
        # [FIX-ROUPEIRO-FUNDO] o fundo solido e' criado MAIS ABAIXO, depois de o
        # @viewport (z=99999) existir -- aqui ainda esta' nil (linha @viewport=nil).
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
        # [FIX-ROUPEIRO-FUNDO] fundo solido OPACO no MESMO viewport das opcoes (z=99999),
        # agora que @viewport ja' existe. z=-1000 = atras da UI mas a' frente do mapa
        # (vp_z=99999 > mapa vp_z=0). Tapa o quarto/moveis. Antes 'outfitBG' nao existia
        # -> Plane bitmap nil -> mapa aparecia atraves. $MKXP_ROUPEIRO_BG=false desliga.
        __ec(655,0,["if $MKXP_ROUPEIRO_BG != false"]),
        __ec(655,0,["  _vp = (@sprites[\"options\"].viewport rescue @viewport)"]),
        __ec(655,0,["  @sprites[\"bg\"] = Sprite.new(_vp)"]),
        __ec(655,0,["  @sprites[\"bg\"].bitmap = Bitmap.new(Graphics.width, Graphics.height)"]),
        __ec(655,0,["  @sprites[\"bg\"].bitmap.fill_rect(0, 0, Graphics.width, Graphics.height, Color.new(16, 20, 32))"]),
        __ec(655,0,["  @sprites[\"bg\"].z = -1000"]),
        __ec(655,0,["  (MKXPDebug.log(\"[FIX-ROUPEIRO-FUNDO] bg solido criado vp.z=#{(_vp.z rescue '?')} z=-1000\") rescue nil)"]),
        __ec(655,0,["end"]),
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

          # ── TILEDIAG: casa invisivel (1x por mapa; o chao renderiza mas a casa
          # nao -> nao e' falha total do tileset). Os tiles da casa (paredes/telhado)
          # vivem em camadas 1/2 e/ou sao tiles de PRIORIDADE (desenhados acima do
          # jogador) e/ou autotiles. Dumpa: contagem de tiles!=0 por camada,
          # autotiles usados, e nº de tiles com prioridade>0 -> aponta QUE parte da
          # renderizacao de tilemap esta a falhar no motor.
          begin
            md2 = m.instance_variable_get(:@map)
            data = (md2.data rescue nil)            # Table 3D [x,y,camada]
            if data
              xs = (data.xsize rescue 0); ys = (data.ysize rescue 0); zs = (data.zsize rescue 0)
              tsid2 = (md2.tileset_id rescue 0)
              prios = ($data_tilesets[tsid2].priorities rescue nil)
              layer_nz = [0,0,0]; prio_tiles = 0; max_tid = 0
              (0...zs).each do |z|
                (0...xs).each do |x|
                  (0...ys).each do |y|
                    tid = (data[x,y,z] rescue 0)
                    next if !tid || tid == 0
                    layer_nz[z] += 1 if z < 3
                    max_tid = tid if tid > max_tid
                    pr = (prios[tid] rescue 0) || 0
                    prio_tiles += 1 if pr > 0
                  end
                end
              end
              auto = ($data_tilesets[tsid2].autotile_names rescue [])
              auto_s = (auto.each_with_index.map { |n,i| (n.nil? || n.to_s.empty?) ? nil : "#{i+1}:#{n}" }.compact.join(',') rescue '?')
              w("[TILEDIAG] map=#{mid} data=#{xs}x#{ys}x#{zs} tiles!=0/camada=#{layer_nz.inspect} prioridade(>0)=#{prio_tiles} max_tile_id=#{max_tid}")
              w("[TILEDIAG] map=#{mid} autotiles=[#{auto_s}] (se a casa usa um autotile/camada que o motor nao desenha -> invisivel)")
            else
              w("[TILEDIAG] map=#{mid} sem md.data acessivel")
            end
          rescue => e
            w("[TILEDIAG|ERR] #{e.class}: #{e.message}")
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
            # NOTA mruby: `Module.new do def m;end end` NAO define o metodo no
            # modulo (o `def` num bloco vai para o escopo lexical, nao para o
            # modulo) -> o MenuCloseFix ficava VAZIO e o prepend era no-op (por
            # isso nunca apareciam linhas [MENU-V17] update#N no log). define_method
            # define SEMPRE no modulo recetor. (return -> next: em bloco de
            # define_method o `return` pode rebentar; `next valor` sai do metodo.)
            _mcf_mod = Module.new
            _mcf_mod.send(:define_method, :update) do |*a, &b|
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
                  next
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
            Object.const_set(:MenuCloseFix, _mcf_mod)
          end
          # ── V34: RE-ATIVAR o prepend ──────────────────────────────────────
          # A remocao do hack (V33) NAO resolveu: o menu CONTINUA a nao fechar com
          # B (log: Bag bg.png recarrega varias vezes = continua a entrar). A hipotese
          # de que o trigger?(BACK) original + trigger? nativo bastava estava ERRADA --
          # ou o B nao mapeia para BACK no binario, ou Input::BACK!=12, ou o trigger?
          # nao da' flanco limpo aqui. O MenuCloseFix fecha com FLANCO PROPRIO derivado
          # de press? (fiavel -- o d-pad prova-o) para 12 OU 14 (B em qualquer
          # mapeamento) e loga 'premidas' p/ revelar o codigo real do B no proximo log.
          # ── V37: APLICADOR RE-CHAMAVEL (resolve o redef que nao persiste) ──
          # PROVADO em DOIS logs: o V36 (alias+redef) instalou em t=18 mas o update
          # ORIGINAL correu na abertura (t=290/t=339) -- selecionou a Bag, ZERO linhas
          # 'premidas'. O padrao alias+redef FUNCIONA em mruby (testado isoladamente) e
          # o SPRITEPROF (mesma tecnica, instalado no LOAD do probe) interceta sempre;
          # logo algo desfaz o redef do menu entre t=18 (bloco per-frame) e a abertura.
          # SOLUCAO: guardar o TRUE original UMA vez (alias estavel, nunca re-aliasado)
          # e expor um lambda $__apply_mcf que (re)define update -> fecho + true original.
          # Chamado aqui E re-chamado no MENUGUARD mesmo antes do menu abrir (just-in-time),
          # por isso mesmo que o redef seja desfeito, e' restaurado a tempo. Sem re-alias =
          # sem recursao. Fecho por flanco de press? (B=12, ACTION=11, 14=binario antigo;
          # press? le s_pressed, imune ao dreno do latch). Delega o resto ao true original.
          # ── V38: PREPEND (robusto) — substitui as tentativas V17/V33-V37 ───────
          # O log PROVA que o define_method/alias num bloco per-frame NUNCA pegou:
          # owner=0 e ZERO linhas [MENU-CLOSE] -> o redef nao substitui o update
          # realmente chamado (a navegacao corre = o update ORIGINAL corre). O
          # prepend de um Module e' o mecanismo MAIS fiavel em mruby (testado:
          # prepend+super+modulo anonimo funciona) -- o metodo do modulo corre
          # SEMPRE antes do da classe e super delega ao original (navegacao
          # intacta). Fecho por FLANCO PROPRIO de press?(11/12/14): press? le
          # s_pressed (estado mantido, NAO consumido), imune ao modelo de flanco
          # do trigger? (que aqui falha -- a Bag fecha com B, o menu de pausa
          # nao). ADITIVO: sem flanco de B -> super. Loga as 8 primeiras chamadas
          # (Input::BACK real, press(12), trigger(12)) -> fecha E revela a causa.
          if defined?(VoltseonsPauseMenu) && !$__menu_cf_prepended
            $__menu_cf_prepended = true
            $__menuv17_applied = true   # desativa o V37 (define_method); so' o prepend
            begin
              _cf = Module.new do
                def update(*a, &b)
                  @__cf_held ||= {}
                  _closed = false
                  [11, 12, 14].each do |k|
                    _pr = (Input.press?(k) rescue false)
                    _closed = true if _pr && !@__cf_held[k] && @__cf_inited
                    @__cf_held[k] = _pr
                  end
                  @__cf_inited = true
                  # [SELFTEST|MENU] auto-deteta "B nao fecha": se @menu.shouldExit ja'
                  # foi posto mas esta update CONTINUA a correr, o loop da cena nao o
                  # honra. Conta frames com shouldExit=true; flag aos 30 (~0.5s).
                  begin
                    if (@menu && (@menu.shouldExit rescue false))
                      @__cf_exit_frames = (@__cf_exit_frames || 0) + 1
                      if @__cf_exit_frames == 30
                        (MKXPDebug.log("[SELFTEST|MENU] shouldExit=true ha 30 frames mas o menu CONTINUA a atualizar -> B nao fecha (menu amarelo preso)") rescue nil)
                      end
                    else
                      @__cf_exit_frames = 0
                    end
                  rescue; end
                  if (@__cf_dbg ||= 0) < 8
                    @__cf_dbg += 1
                    _bk = (Input::BACK rescue '?'); _ac = (Input::ACTION rescue '?')
                    _pb = (Input.press?(12) rescue '?'); _tb = (Input.trigger?(12) rescue '?')
                    (MKXPDebug.log("[MENU-CF] upd##{@__cf_dbg} BACK=#{_bk} ACTION=#{_ac} press(12)=#{_pb} trig(12)=#{_tb} fecha=#{_closed}") rescue nil)
                  end
                  if _closed
                    ((@menu.shouldExit = true) if @menu && (@menu.respond_to?(:shouldExit=) rescue false)) rescue nil
                    ($PokemonTemp.last_menu_selection = @currentSelection rescue nil)
                    (MKXPDebug.log("[MENU-CF] FLANCO B (press? 11/12/14) -> shouldExit=true") rescue nil)
                    return
                  end
                  # [MENUICONDBG 2026-07-02] "icons somem com as setas": ao premir
                  # LEFT(4)/RIGHT(6), loga o estado dos sprites 'iconN' (x/vis/zoom/bmp)
                  # ANTES de super (que anima). Diz se ficam fora do ecra, invisiveis,
                  # ou sem bitmap apos a navegacao. Cap 24.
                  begin
                    _nav = ((Input.press?(4) rescue false) || (Input.press?(6) rescue false))
                    if _nav && (($__micon_n ||= 0) < 24) && @sprites.is_a?(Hash)
                      $__micon_n += 1
                      _ic = @sprites.keys.select { |k| k.to_s[/icon/] }.sort
                      _st = _ic.first(8).map { |k| _s = @sprites[k]; "#{k}:x=#{(_s.x rescue '?')}v#{(_s.visible ? 1 : 0) rescue '?'}z#{(_s.zoom_x rescue '?')}b#{(_s.bitmap ? 1 : 0) rescue '?'}" }.join(' ')
                      (MKXPDebug.log("[MENUICONDBG] nav sel=#{(@currentSelection rescue '?')} disp=#{(@displayIndexes.length rescue '?')} | #{_st}") rescue nil)
                    end
                  rescue; end
                  super
                end
              end
              VoltseonsPauseMenu.prepend(_cf)
              (MKXPDebug.log("[MENU-CF] prepend instalado (ancestors=#{(VoltseonsPauseMenu.ancestors[0,3].map(&:to_s).inspect rescue '?')})") rescue nil)
            rescue => _pe
              (MKXPDebug.log("[MENU-CF] prepend FALHOU: #{_pe.class}: #{_pe.message}") rescue nil)
            end
          end
          if defined?(VoltseonsPauseMenu) && !$__menuv17_applied
            $__menuv17_applied = true
            begin
              VoltseonsPauseMenu.class_eval do
                unless method_defined?(:__mcf_true_orig_update)
                  alias_method :__mcf_true_orig_update, :update   # UMA vez, estavel
                end
              end
              $__apply_mcf = lambda do
                next unless Object.const_defined?(:VoltseonsPauseMenu)
                VoltseonsPauseMenu.class_eval do
                  define_method(:update) do |*a, &b|
                    @__mcf_held ||= {}
                    begin
                      $__mcf_dbg = 0 if !@__mcf_inited
                      $__mcf_dbg = ($__mcf_dbg || 0)
                      if $__mcf_dbg < 10
                        $__mcf_dbg += 1
                        _pr = (0..25).select { |kk| (Input.press?(kk) rescue false) }
                        (MKXPDebug.log("[MENU-CLOSE] update##{$__mcf_dbg} premidas=#{_pr.inspect}") rescue nil)
                      end
                    rescue; end
                    _close = false
                    [11, 12, 14].each do |k|
                      _pr = (Input.press?(k) rescue false)
                      _close = true if _pr && !@__mcf_held[k] && @__mcf_inited
                      @__mcf_held[k] = _pr
                    end
                    @__mcf_inited = true
                    if _close
                      ((@menu.shouldExit = true) if @menu && (@menu.respond_to?(:shouldExit=) rescue false)) rescue nil
                      ($PokemonTemp.last_menu_selection = @currentSelection rescue nil)
                      (MKXPDebug.log("[MENU-CLOSE] flanco de cancelar (press? 11/12/14) -> shouldExit=true") rescue nil)
                      next
                    end
                    __mcf_true_orig_update(*a, &b)
                  end
                end
              end
              $__apply_mcf.call
              _owner = (VoltseonsPauseMenu.instance_method(:update).owner rescue '?')
              (MKXPDebug.log("[MENU-V17] V37 (aplicador re-chamavel) -- update redefinido AGORA (owner=#{_owner}); sera' re-aplicado no MENUGUARD just-in-time") rescue nil)
            rescue => _pe
              (MKXPDebug.log("[MENU-V17] V37 install FALHOU: #{_pe.class}: #{_pe.message}") rescue nil)
            end
          end
        rescue => _e
          (MKXPDebug.log("[MENU-V17] falhou: #{_e.class}: #{_e.message}") rescue nil)
        end
        # MENU-CLOSE-INPUT: instalar o hook de Input.trigger? assim que o Input estiver pronto
        # (no LOAD nao estava). Idempotente ($__mcf_input_hooked). E' a abordagem que de facto
        # fecha o menu (a classe VoltseonsPauseMenu nao e' intercetavel; o Input e').
        (($__install_mci.call if $__install_mci && !$__mcf_input_hooked) rescue nil)

        # [MENU-INPUT] super-debug do input do menu: cada frame com menu aberto, regista
        # o estado fisico (premidas) + o que o menu vai LER (trigger?12/11/13, press?4/6,
        # dir4). Revela se o B(12)/setas chegam ao menu. Capado a 300 linhas.
        if $MKXP_PROBE_QUIET != true && ($__menu_open rescue false) && (($__mnudbg ||= 0) < 300)
          $__mnudbg += 1
          _held = (0..18).select { |k| (Input.press?(k) rescue false) }
          (MKXPDebug.log("[MENU-INPUT] f=#{(Graphics.frame_count rescue -1)} premidas=#{_held.inspect} " \
            "trg12=#{(Input.trigger?(12) rescue '?')} trg11=#{(Input.trigger?(11) rescue '?')} " \
            "trg13=#{(Input.trigger?(13) rescue '?')} prsL4=#{(Input.press?(4) rescue '?')} " \
            "prsR6=#{(Input.press?(6) rescue '?')} dir4=#{(Input.dir4 rescue '?')} latch=#{$__mcf_close_latch ? 1 : 0}") rescue nil)
        end

        # [GILBERT2] o pbRegisterPartner CORREU mas o pbAddDependency2 seguinte NAO -> o
        # evento para entre os dois. Hook global (lazy) a ambos + pbAddDependency: loga
        # ENTRADA/SAIDA/REBENTOU + dependentEvents.size. Mostra ONDE o seguidor falha.
        unless $__gilbert2_done || $MKXP_PROBE_QUIET
          begin
            if (Object.method_defined?(:pbAddDependency2) || Object.private_method_defined?(:pbAddDependency2) rescue false)
              [:pbRegisterPartner, :pbAddDependency2, :pbAddDependency].each do |_gm|
                _has = ((Object.method_defined?(_gm) || Object.private_method_defined?(_gm)) rescue false)
                if _has && !(Object.method_defined?("__o_#{_gm}") rescue true)
                  Object.send(:alias_method, "__o_#{_gm}", _gm)
                  Object.send(:define_method, _gm) do |*ga|
                    (MKXPDebug.log("[GILBERT2] #{_gm} ENTROU args=#{ga.inspect[0,90]} dep=#{($PokemonGlobal.dependentEvents.size rescue -1)}") rescue nil)
                    begin
                      _gr = send("__o_#{_gm}", *ga)
                      (MKXPDebug.log("[GILBERT2] #{_gm} SAIU dep=#{($PokemonGlobal.dependentEvents.size rescue -1)}") rescue nil)
                      _gr
                    rescue => _ge
                      (MKXPDebug.log("[GILBERT2] #{_gm} REBENTOU: #{_ge.class}: #{_ge.message.to_s[0,110]}") rescue nil)
                      raise
                    end
                  end
                end
              end
              $__gilbert2_done = true
              (MKXPDebug.log("[GILBERT2] hooks globais instalados (pbRegisterPartner/pbAddDependency2/pbAddDependency)") rescue nil)
            end
          rescue; end
        end

        # ═══ DIAGNOSTICO: [PICSHOW] (intro) + [EBDXPROF] (FPS) ═══════════════════
        # Instaladores por-frame idempotentes (as classes/metodos do jogo so' existem
        # em runtime). [PICSHOW] isola a foto do jogador em falta na intro (loga o NOME
        # RESOLVIDO de cada Picture + estado do $Trainer -> apanha 'PlayerSprites/0').
        # [EBDXPROF] isola QUAL efeito EBDX do Sprite_Character custa os 228ms.
        unless $__picshow_done || $MKXP_PROBE_QUIET
          begin
            if Object.const_defined?(:Game_Picture) && Game_Picture.method_defined?(:show) &&
               !Game_Picture.method_defined?(:__ps_orig_show)
              Game_Picture.send(:alias_method, :__ps_orig_show, :show)
              Game_Picture.send(:define_method, :show) do |*a|
                begin
                  _nm = (a[0].to_s rescue '')
                  _last = (_nm.split('/').last.to_s rescue '')
                  _num = (_last.length > 0 && _last == _last.to_i.to_s)
                  if _nm.include?('PlayerSprites') || _nm.include?('intro') || _num
                    (MKXPDebug.log("[PICSHOW] pic='#{_nm}' trainer_type=#{($Trainer.trainer_type.inspect rescue '?')} outfit=#{($Trainer.outfit rescue '?')}#{_num ? ' <- NUMERICO=provavel magenta' : ''}") rescue nil)
                  end
                rescue; end
                __ps_orig_show(*a)
              end
              (MKXPDebug.log("[PICSHOW] hook Game_Picture#show instalado") rescue nil)
              $__picshow_done = true
            end
          rescue; end
        end
        unless $__ebdxprof_done
          begin
            $__ebdx_acc ||= Hash.new(0.0)
            _en = 0
            # [EBDXTHROTTLE] SEGURO: throttla/cronometra os EFEITOS SEPARADOS, NAO a cadeia
            # base do Sprite_Character#update (que e' 'alias X_update update' -> saltar
            # congelaria o sprite, bug de antes). Alvos reais (lidos dos plugins):
            #  - Sprite_Character#update_footsteps  (Marin's Footprints, linha 109)
            #  - Sprite_OWShadow#update             (Overworld Shadows EX, classe propria)
            # 1/3 do ritmo, escalonado por sprite (object_id) -> so' atrasa o efeito
            # cosmetico (sombra/pegada), o update BASE corre sempre. $MKXP_EBDX_THROTTLE=false desliga.
            if (Object.const_defined?(:Sprite_Character) && Sprite_Character.method_defined?(:update_footsteps) rescue false) &&
               !(Sprite_Character.method_defined?(:__ep_fs) rescue true)
              Sprite_Character.send(:alias_method, :__ep_fs, :update_footsteps)
              Sprite_Character.send(:define_method, :update_footsteps) do |*aa|
                if $MKXP_EBDX_THROTTLE != false
                  return nil if (((Graphics.frame_count rescue 0) + (object_id rescue 0)) % 3) != 0
                end
                _t0 = (__mkxp_ms rescue nil); _r = __ep_fs(*aa)
                ($__ebdx_acc[:footsteps] += ((__mkxp_ms rescue _t0) - _t0)) if _t0; _r
              end
              _en += 1
            end
            if (Object.const_defined?(:Sprite_OWShadow) && Sprite_OWShadow.method_defined?(:update) rescue false) &&
               !(Sprite_OWShadow.method_defined?(:__ep_sh) rescue true)
              Sprite_OWShadow.send(:alias_method, :__ep_sh, :update)
              Sprite_OWShadow.send(:define_method, :update) do |*aa|
                if $MKXP_EBDX_THROTTLE != false
                  return nil if (((Graphics.frame_count rescue 0) + (object_id rescue 0)) % 3) != 0
                end
                _t0 = (__mkxp_ms rescue nil); _r = __ep_sh(*aa)
                ($__ebdx_acc[:shadow] += ((__mkxp_ms rescue _t0) - _t0)) if _t0; _r
              end
              _en += 1
            end
            if _en > 0
              (MKXPDebug.log("[EBDXPROF] throttle SEGURO instalado em #{_en} efeitos (footsteps/OWShadow, 1/3 ritmo)") rescue nil)
              $__ebdxprof_done = true
            end
          rescue => _ee2
            (MKXPDebug.log("[EBDXPROF] falhou: #{_ee2.class}: #{_ee2.message}") rescue nil)
          end
        end
        if $__ebdx_acc && !$__ebdx_acc.empty?
          $__ebdx_f = ($__ebdx_f || 0) + 1
          if $__ebdx_f % 240 == 0
            (MKXPDebug.log("[EBDXPROF] 240f acumulado: #{$__ebdx_acc.map{|k,v| "#{k}=#{v.round(0)}ms"}.join(' ')}") rescue nil)
            $__ebdx_acc = Hash.new(0.0)
          end
        end

        # ─── METAFIX: rede de seguranca p/ o magenta da criacao de personagem.
        # GameData::Metadata.get_player(i) devolve o array player_X, e player_X[0]
        # e' o tipo de treinador (simbolo, ex :POKEMONTRAINER_Sorin). O Marshal do
        # mruby por vezes corrompe esse SIMBOLO p/ 0 -> a criacao pede
        # 'PlayerSprites/0' (inexistente) -> magenta. (Mapeamento extraido da
        # metadata.dat real, descodificada em Ruby.) Este override SO' atua quando
        # [0] NAO e' um simbolo (corrompido); se ja' estiver certo, e' no-op total.
        # A criacao corre depois deste install (t>67s no log), por isso protege-a.
        if !$__metafix_done && defined?(GameData) && defined?(GameData::Metadata) &&
           (GameData::Metadata.respond_to?(:get_player) rescue false)
          $__metafix_done = true
          begin
            $__player_tt_map = {
              0 => :POKEMONTRAINER_Sorin,  1 => :POKEMONTRAINER_Soleil,
              2 => :POKEMONTRAINER_Luan,   3 => :POKEMONTRAINER_Lusine,
              4 => :POKEMONTRAINER_Ethan,  5 => :POKEMONTRAINER_Lyra,
              6 => :POKEMONTRAINER_Lucas,  7 => :POKEMONTRAINER_Dawn
            }
            GameData::Metadata.singleton_class.class_eval do
              unless method_defined?(:__metafix_orig_get_player)
                alias_method :__metafix_orig_get_player, :get_player
                def get_player(id)
                  arr = __metafix_orig_get_player(id)
                  if arr.is_a?(Array) && arr[0] && !arr[0].is_a?(Symbol)
                    _c = ($__player_tt_map[id] rescue nil)
                    if _c
                      _old = arr[0]
                      arr[0] = _c
                      if (($__metafix_n ||= 0) < 16)
                        $__metafix_n += 1
                        (MKXPDebug.log("[METAFIX] get_player(#{id})[0] estava #{_old.inspect} (simbolo corrompido pelo Marshal) -> reposto #{_c.inspect}") rescue nil)
                      end
                    end
                  end
                  arr
                end
              end
            end
            (MKXPDebug.log("[METAFIX] override get_player instalado -- repoe o tipo de treinador se o simbolo se corromper (evita PlayerSprites/0 magenta). No-op se ja estiver certo.") rescue nil)
          rescue => _e
            (MKXPDebug.log("[METAFIX] falhou: #{_e.class}: #{_e.message}") rescue nil)
          end
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
                      # [ROUPDIAG] CE#173 = ClothesChangeUI (roupeiro). Agenda dumps Ruby de
                      # sprites (classe/z/vp.z/vis/bmp WxH/cname) p/ identificar os sprites do
                      # quarto (vasos/TV = Sprite_Character com cname) que ficam por cima do
                      # menu E o sprite do outfit em falta (bmp=nil/errado). Off: $MKXP_ROUPDIAG=false.
                      # [PERF 2026-07-02] 1x por SESSAO (era por abertura): cada dump de 65
                      # sprites custava d=1.9-4.3s no 3DS (log 09:2x, o pior hitch do roupeiro).
                      if _id == 173 && $MKXP_ROUPDIAG != false && !$__roup_dumped && (Object.const_defined?(:MAXDIAG) rescue false)
                        $__roup_dumped = true
                        $__roup_dump_left = 1
                        (MAXDIAG.dump_sprites("ROUPEIRO-OPEN-CE173", true) rescue nil)
                        (MKXPDebug.log("[ROUPDIAG] CE#173 (roupeiro) aberto -> dump unico de sprites (1x/sessao)") rescue nil)
                      end
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
                      _rr = __od_execute_script(script)
                      # [ROUPZFIX] o CE#173 (ClothesChangeUI) cria a UI toda com @viewport=nil
                      # e z=0. No 3DS, sprites SEM viewport ordenam pelo PROPRIO z (motor,
                      # sprite_binding_3ds.cpp:165), e os sprites do quarto (viewport z=0) teem
                      # z=48-400 -> a UI (z=0) fica POR BAIXO do quarto (TV/armario) e o preview
                      # do outfit fica soterrado. No RGSS o desempate e' por ordem de criacao (UI
                      # por cima), por isso no Windows funciona. FIX: damos z>=99990 a' UI ->
                      # por cima do mapa (bg atras, preview/opcoes a' frente). $MKXP_ROUPZFIX=false reverte.
                      begin
                        if $MKXP_ROUPZFIX == true && @sprites.is_a?(Hash) &&
                           @sprites.key?("options") && @sprites.key?("player")
                          _zmap = { "bg" => 99990, "player" => 99992, "options" => 99993, "arrow" => 99994 }
                          @sprites.each do |_k, _s|
                            next if _s.nil? || (_s.disposed? rescue true)
                            (_s.z = (_zmap[_k] || 99995)) rescue nil
                          end
                          unless $__roupzfix_logged
                            $__roupzfix_logged = true
                            (MKXPDebug.log("[ROUPZFIX] UI do roupeiro (CE#173) elevada a z>=99990 (nil-viewport -> por cima do quarto/TV). keys=#{(@sprites.keys.inspect rescue '?')}") rescue nil)
                          end
                        end
                      rescue => _ze
                        (MKXPDebug.log("[ROUPZFIX] rebentou: #{_ze.class}: #{_ze.message.to_s[0,80]}") rescue nil)
                      end
                      _rr
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
                    # [PERF 2026-07-04] interior DIAG (GILBERT-ANIM por frame + triptico
                    # FOLLOWDIAG/STATUE/GILDBG/TREEDBG a cada 300f) gated: estatua/seguidor
                    # RESOLVIDOS (GHOST-KILL v3). FIX-FOLLOWKEEP abaixo fica INTACTO.
                    if $MKXP_PROBE_QUIET != true
                    $__fgdiag_n = ($__fgdiag_n || 0) + 1
                    # [SELFTEST|GILBERT-ANIM] amostra o padrao (frame de caminhada) do
                    # seguidor TODOS os frames enquanto ele se MOVE. Se moveu muito mas o
                    # padrao nunca mudou -> ESTATUA (segue sem animar). @sprites[0] = Gilbert.
                    begin
                      _s0 = (@sprites && @sprites[0])
                      _ch = (_s0 && (_s0.character rescue nil))
                      if _ch
                        _mov = ((_ch.moving? rescue false) || ((_ch.respond_to?(:jumping?) rescue false) && (_ch.jumping? rescue false)))
                        _pat = (_ch.pattern rescue (_ch.instance_variable_get(:@pattern) rescue nil))
                        if _mov && _pat
                          $__gil_mv = ($__gil_mv || 0) + 1
                          ($__gil_pats ||= {})[_pat] = true
                        end
                      end
                    rescue; end
                    # [PERF 2026-07-02] 60->300: o Gilbert esta' CONFIRMADO OK (segue/anima/
                    # erased) e cada tríptico FOLLOWDIAG/STATUE/FOLLOWANIM custava d=1.3-2.7s
                    # no 3DS (log 09:2x) -> era das maiores fontes de hitches em cutscene.
                    if ($__fgdiag_n % 300) == 1
                      dep = ($PokemonGlobal.dependentEvents rescue nil)
                      dep = ($game_temp.dependentEvents rescue nil) if dep.nil?
                      pl  = ($game_player rescue nil)
                      plmap = ($game_map.map_id rescue '?')
                      sz  = (dep.respond_to?(:size) ? (dep.size rescue '?') : '?')
                      # data do seguidor = [mapa_ORIGINAL, event_id, mapa_ATUAL, x, y]
                      smp = ''; xfer = 'sem-seguidor'
                      begin
                        if dep.is_a?(Array) && dep[0].is_a?(Array)
                          smp = dep.first(2).map { |e| "[orig=#{(e[0] rescue '?')} ev=#{(e[1] rescue '?')} cur=#{(e[2] rescue '?')} (#{(e[3] rescue '?')},#{(e[4] rescue '?')})]" }.join(' ')
                          _cur = (dep[0][2] rescue nil)
                          xfer = (_cur == plmap) ? 'OK' : "FALHOU (Gilbert no mapa #{_cur}, jogador no #{plmap} -> nao transferiu)"
                        end
                      rescue; end
                      # [SELFTEST] avalia a janela de animacao acumulada
                      _anim = 'parado/sem-amostra'
                      begin
                        if ($__gil_mv || 0) >= 20
                          _np = ($__gil_pats ? $__gil_pats.size : 0)
                          _anim = (_np <= 1) ? "CONGELADA (moveu #{$__gil_mv}f mas padrao sempre o mesmo = ESTATUA)" : "OK (#{_np} padroes a ciclar)"
                        end
                        $__gil_mv = 0; $__gil_pats = {}
                      rescue; end
                      (MKXPDebug.log("[FOLLOWDIAG] seguidores=#{sz} player@(#{(pl.x rescue '?')},#{(pl.y rescue '?')}) map=#{plmap} | TRANSFER=#{xfer} | ANIM=#{_anim} | #{smp}") rescue nil)
                      # [STATUE-DIAG] a estatua = o evento ORIGINAL (#7) nao escondido apos o
                      # addEvent fazer event.erase. Logamos o estado do evento #7 no mapa: @erased,
                      # @character_name (devia ficar "" apos refresh), x/y. Se erased=false -> o
                      # erase nao correu (raiz Ruby/plugin). Se erased=true mas char_name!="" ->
                      # o Game_Event#refresh nao limpou (raiz mruby/motor). Se ambos OK mas o sprite
                      # aparece -> o Sprite_Character do Spriteset_Map nao atualizou (raiz motor render).
                      begin
                        _e7 = ($game_map.events[7] rescue nil)
                        if _e7
                          (MKXPDebug.log("[STATUE-DIAG] ev7: erased?=#{(_e7.instance_variable_get(:@erased) rescue '?')} char_name=#{(_e7.character_name.inspect rescue '?')} tile_id=#{(_e7.instance_variable_get(:@tile_id) rescue '?')} @(#{(_e7.x rescue '?')},#{(_e7.y rescue '?')})") rescue nil)
                        else
                          (MKXPDebug.log("[STATUE-DIAG] ev7: $game_map.events[7]=nil (evento removido do hash)") rescue nil)
                        end
                      rescue => _sde
                        (MKXPDebug.log("[STATUE-DIAG] rebentou: #{_sde.class}: #{_sde.message.to_s[0,60]}") rescue nil)
                      end
                      # [FOLLOWANIM] campos CRUS de animacao do seguidor vivo (raiz da estatua).
                      # v2: amostra o EVENTO real (realEvents), NAO o sprite -- o @sprites[0] vinha
                      # de outra instancia de DependentEventSprites (mapa ligado) e dava nil.
                      # walk_anime=false ou lock_pattern=true -> nunca anima; moving?=false sempre
                      # -> nao esta' a ser movido pelo pbMoveDependentEvents.
                      begin
                        _dep2 = ($PokemonTemp.dependentEvents rescue nil)
                        _re2  = (_dep2.instance_variable_get(:@realEvents) rescue nil)
                        _ch = nil
                        _re2.each { |e| (_ch = e; break) if e } if _re2.is_a?(Array)
                        if _ch
                          (MKXPDebug.log("[FOLLOWANIM] cls=#{(_ch.class rescue '?')} walk_anime=#{(_ch.instance_variable_get(:@walk_anime) rescue '?')} step_anime=#{(_ch.instance_variable_get(:@step_anime) rescue '?')} lock_pattern=#{(_ch.instance_variable_get(:@lock_pattern) rescue '?')} dir=#{(_ch.direction rescue '?')} pat=#{(_ch.pattern rescue (_ch.instance_variable_get(:@pattern) rescue '?'))} opat=#{(_ch.instance_variable_get(:@original_pattern) rescue '?')} stop_count=#{(_ch.instance_variable_get(:@stop_count) rescue '?')} anime_count=#{(_ch.instance_variable_get(:@anime_count) rescue '?')} mrf=#{(_ch.instance_variable_get(:@move_route_forcing) rescue '?')} moving?=#{(_ch.moving? rescue '?')} x=#{(_ch.x rescue '?')} y=#{(_ch.y rescue '?')} char='#{(_ch.character_name rescue '?')}'") rescue nil)
                        else
                          (MKXPDebug.log("[FOLLOWANIM] realEvents vazio/sem evento (seguidor nao construido neste frame)") rescue nil)
                        end
                      rescue => _fae
                        (MKXPDebug.log("[FOLLOWANIM] rebentou: #{_fae.class}: #{_fae.message.to_s[0,80]}") rescue nil)
                      end
                      # [GILDBG 2026-07-02] fecha "2 Gilberts": ha' um evento EV052 no Map043
                      # com charset trainer_RIVAL1 @(31,30) condicionado ao switch 12 (junto a'
                      # fonte da foto). Logamos sw12 + estado do ev52 + TODOS os eventos vivos
                      # com charset RIVAL1 -> distingue "EV052 apareceu (switch)" de "sprite
                      # orfao de rebuild".
                      begin
                        if plmap == 43
                          _sw12 = ($game_switches[12] rescue '?')
                          _e52 = ($game_map.events[52] rescue nil)
                          _riv = []
                          begin
                            ($game_map.events || {}).each do |_id2, _ev2|
                              _riv << "ev#{_id2}@(#{(_ev2.x rescue '?')},#{(_ev2.y rescue '?')})" if ((_ev2.character_name rescue '') == 'trainer_RIVAL1')
                            end
                          rescue; end
                          _e52s = _e52 ? "char=#{(_e52.character_name.inspect rescue '?')} erased=#{(_e52.instance_variable_get(:@erased) rescue '?')} @(#{(_e52.x rescue '?')},#{(_e52.y rescue '?')})" : 'nil'
                          (MKXPDebug.log("[GILDBG] sw12=#{_sw12} ev52: #{_e52s} | eventos RIVAL1 vivos: #{_riv.empty? ? 'nenhum' : _riv.join(' ')}") rescue nil)
                        end
                      rescue; end
                      # [TREEDBG v2 2026-07-02] fecha "arvore cortada" SEM adivinhar coordenadas:
                      # varre o mapa inteiro e encontra as tiles de COPA sozinho = tiles de
                      # camada superior (z=1/2) cuja prioridade ORIGINAL ($data_tilesets, pelo
                      # id ANTIGO) e' >0 (devem desenhar ACIMA do jogador). Para cada uma loga:
                      # id remapeado, id original, prioridade ORIGINAL (o vga) e prioridade
                      # REMAPEADA (gm.@priorities pelo id novo = o que o motor USA). Se
                      # orig>0 mas remap==0 -> a remap perdeu a prioridade (RAIZ: copa cai p/
                      # o chao e fica cortada). Cap 40, 1x/sessao. Tambem conta o total.
                      begin
                        if plmap == 43 && !$__treedbg_done
                          $__treedbg_done = true
                          _gmT = $game_map
                          _dT = (_gmT.data rescue nil)
                          _tsidT = (_gmT.instance_variable_get(:@map).tileset_id rescue (_gmT.tileset_id rescue 28))
                          _origPri = ($data_tilesets[_tsidT].priorities rescue nil)   # por id ANTIGO
                          _remPri  = (_gmT.instance_variable_get(:@priorities) rescue nil) # por id NOVO
                          _cacheT = (defined?($__tsc_cache) && $__tsc_cache) ? $__tsc_cache[43] : nil
                          _usT = _cacheT ? _cacheT[:us] : nil   # us[k] = id antigo do slot 384+k
                          _nlog = 0; _ncanopy = 0; _nlost = 0
                          if _dT && _origPri
                            xs=(_dT.xsize rescue 0); ys=(_dT.ysize rescue 0)
                            (0...ys).each do |_yT|
                              (0...xs).each do |_xT|
                                [1,2].each do |_zT|
                                  _tT = ((_dT[_xT,_yT,_zT] rescue 0) || 0)
                                  next if _tT < 384
                                  _oldT = (_usT && (_usT[_tT-384] rescue nil)) ? _usT[_tT-384] : _tT
                                  _po = ((_origPri[_oldT] rescue 0) || 0)
                                  next if _po == 0    # so' copa (acima do jogador)
                                  _ncanopy += 1
                                  _pr = ((_remPri ? (_remPri[_tT] rescue 0) : 0) || 0)
                                  _nlost += 1 if _pr == 0
                                  if _nlog < 40
                                    _nlog += 1
                                    (MKXPDebug.log("[TREEDBG] (#{_xT},#{_yT}) z#{_zT} tid=#{_tT} old=#{_oldT} priOrig=#{_po} priRemap=#{_pr}#{_pr==0 ? ' <<< PERDEU PRIORIDADE' : ''}") rescue nil)
                                  end
                                end
                              end
                            end
                          end
                          (MKXPDebug.log("[TREEDBG] RESUMO: tiles de copa (priOrig>0)=#{_ncanopy} | perderam prioridade na remap=#{_nlost} | origPri?=#{!_origPri.nil?} remPri?=#{!_remPri.nil?} us?=#{!_usT.nil?}") rescue nil)
                        end
                      rescue => _tde
                        (MKXPDebug.log("[TREEDBG] rebentou: #{_tde.class}: #{_tde.message.to_s[0,70]}") rescue nil)
                      end
                    end
                    end   # fim do gate $MKXP_PROBE_QUIET (DIAG do seguidor)
                  rescue; end
                  # [FIX-FOLLOWKEEP] (bug 5) o seguidor (Gilbert) e' adicionado mas
                  # $PokemonGlobal.dependentEvents cai a 0 (algo limpa-o) -> o sprite
                  # desaparece "depois de falar com ele". Guarda snapshot quando ha
                  # seguidor e RESTAURA-O se for limpo a 0 no MESMO mapa, forcando o
                  # refresh do sprite (bumpa lastUpdate). Corre TODOS os frames.
                  # Reverte com $MKXP_FOLLOWKEEP=false. O log isola:
                  #   "RESTAURADO" -> foi limpo e recuperado (raiz=limpeza espuria)
                  #   "sem snapshot" -> nunca chegou a estar >0 (add+limpeza no mesmo frame)
                  begin
                    if $MKXP_FOLLOWKEEP != false
                      _dg = ($PokemonGlobal.dependentEvents rescue nil)
                      _mp = ($game_map.map_id rescue nil)
                      if _dg.is_a?(Array)
                        if _dg.size > 0
                          $__fk_snap = _dg.map { |e| e.is_a?(Array) ? e.dup : e }
                          $__fk_map  = _mp
                        elsif $__fk_snap && !$__fk_snap.empty? && $__fk_map == _mp
                          $__fk_snap.each { |e| _dg.push(e.is_a?(Array) ? e.dup : e) }
                          _nlu = (($__follow_lu ||= 1000) + 1); $__follow_lu = _nlu
                          ($PokemonGlobal.dependentEvents.instance_variable_set(:@lastUpdate, _nlu)) rescue nil
                          $__fk_restored = ($__fk_restored || 0) + 1
                          if ($__fk_restored <= 3) || ($__fk_restored % 120 == 0)
                            (MKXPDebug.log("[FIX-FOLLOWKEEP] dependentEvents limpo a 0 no mapa #{_mp} -> RESTAURADO #{$__fk_snap.size} (lastUpdate=#{_nlu}, restauros=#{$__fk_restored}) - sprite deve reaparecer") rescue nil)
                          end
                        end
                      end
                    end
                  rescue => _fke
                    if (($__fk_errs ||= 0) < 5)
                      $__fk_errs += 1
                      (MKXPDebug.log("[FIX-FOLLOWKEEP] rebentou: #{_fke.class}: #{_fke.message.to_s[0,90]}") rescue nil)
                    end
                  end
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

        # ─── ADDDEP-DIAG: tracar pbAddDependency2 -> addEvent -> createEvent.
        # No log atual o Giltbert (Map043 ev7: pbAddDependency2(7,"Giltbert",3))
        # NUNCA foi falado -> dependentEvents=0 e' ESPERADO. Quando se falar com
        # ele, este hook revela ONDE a adicao falha: addEvent corre? o event
        # existe? o createEvent rebenta? o push aconteceu (dep_antes->dep_depois)?
        # CE 3 "Giltbert Follower" tem 87 cmds simples (sem Tone/MoveRoute/Audio)
        # -> o @list NAO devia ser nil, mas confirma-se aqui. Em caso de erro,
        # ENGOLE (a adicao ja' falha hoje; engolir evita um CRASH no evento) e
        # loga a causa+backtrace. Uso class_eval{def} (o CEFIX prova que funciona
        # em mruby; o Module.new{def} e' que nao define o metodo).
        # NOTA: obter a CLASSE da instancia em vez de defined?(DependentEvents) -- este
        # motor nao apanha esta constante via defined? (apanha outras, ex GenderPickSelection),
        # e a versao anterior NUNCA instalou. A instancia existe ([VERIFY]: PokemonTemp.dep).
        _adddep_cls = ($__adddep_diag_done || $MKXP_PROBE_QUIET) ? nil : ($PokemonTemp.dependentEvents.class rescue nil)
        if !$__adddep_diag_done && _adddep_cls &&
           (_adddep_cls.method_defined?(:addEvent) rescue false)
          $__adddep_diag_done = true
          begin
            _adddep_cls.class_eval do
              unless method_defined?(:__adddep_orig_addEvent)
                alias_method :__adddep_orig_addEvent, :addEvent
                def addEvent(event, eventName=nil, commonEvent=nil)
                  _before = ($PokemonGlobal.dependentEvents.size rescue -1)
                  _evok = !event.nil?
                  _evid = (event.id rescue '?')
                  _cl = nil
                  if commonEvent
                    begin
                      _ce = Game_CommonEvent.new(commonEvent)
                      _cl = (_ce.list ? _ce.list.size : 'NIL')
                    rescue => _cee
                      _cl = "ERRO:#{_cee.class}"
                    end
                  end
                  (MKXPDebug.log("[ADDDEP] addEvent: event?=#{_evok} id=#{_evid} name=#{eventName.inspect} commonEvent=#{commonEvent.inspect} CE#{commonEvent}.list=#{_cl} dep_antes=#{_before}") rescue nil)
                  begin
                    _r = __adddep_orig_addEvent(event, eventName, commonEvent)
                    _after = ($PokemonGlobal.dependentEvents.size rescue -1)
                    (MKXPDebug.log("[ADDDEP] addEvent OK -> dep_depois=#{_after} (#{_after > _before ? '>>> SEGUIDOR ADICIONADO' : 'NAO adicionou (push falhou ou ja existia)'})") rescue nil)
                    _r
                  rescue => _ae
                    (MKXPDebug.log("[ADDDEP] addEvent REBENTOU: #{_ae.class}: #{_ae.message.to_s[0,160].gsub(/\s+/,' ')} | bt: #{((_ae.backtrace||[])[0,4].join(' <- ') rescue '?')}") rescue nil)
                    nil   # engolir: a adicao ja' falha hoje; evita CRASH. causa fica no log.
                  end
                end
              end
            end
            (MKXPDebug.log("[ADDDEP] diagnostico instalado em DependentEvents#addEvent -- FALA COM O GILTBERT (Gardenia/map43) p/ revelar a falha do seguidor no proximo log") rescue nil)
          rescue => _e
            (MKXPDebug.log("[ADDDEP] instalacao falhou: #{_e.class}: #{_e.message}") rescue nil)
          end
        end

        # ─── FIX-FOLLOWADD: o seguidor (Giltbert, Map043 ev7) nunca e' adicionado.
        # Raiz confirmada na fonte v19.1: o evento corre
        #   pbAddDependency2(7,"Giltbert",3)  ->  addEvent($game_map.events[7],...)
        # e o core tem `return if !event`. Se $game_map.events[7] vier nil no 3DS
        # (lookup de hash por chave inteira que diverge do Windows apos o Marshal),
        # o seguidor e' ENGOLIDO sem erro -> [FOLLOWDIAG] dependentEvents=0 SEMPRE.
        # CORRECAO: reimplementa pbAddDependency2 FIEL ao core e, SO' quando
        # events[id] e' nil, re-resolve o evento por varrimento (por CHAVE ou por
        # .id) -> contorna o hash-miss e o seguidor entra. NO-OP TOTAL quando
        # events[id] funciona (= comportamento core identico). Generico: serve
        # qualquer seguidor, nao so' o Giltbert. Reverte com $MKXP_FOLLOW_ADD_FALLBACK=false.
        # O log [FIX-FOLLOWADD] isola a origem no proximo teste:
        #   via=direto         -> events[id] OK (a falha, se houver, e' a jusante: ver [ADDDEP])
        #   via=scan(chave/.id)-> events[id] era nil mas recuperou == hash-miss CONFIRMADO + corrigido
        #   ev=AINDA-NIL       -> evento 7 ausente do mapa no 3DS (problema de carga, outra raiz)
        #   (linha ausente)    -> pbAddDependency2 nunca correu (o ramo/cutscene do evento nao chega la')
        # NOTA (porque a versao anterior NAO instalou): Object.private_method_defined?
        # rebenta/devolve falso neste mruby p/ defs de topo -> o rescue desligava tudo.
        # [VERIFY] provou que respond_to?(:pbAddDependency2, true)=true E' FIAVEL aqui.
        # DESATIVADO por defeito: o motor ja' carrega o 050 (addEvent real) -> o pbAddDependency2
        # NATIVO funciona como no Windows. Este workaround reimplementava-o e criava um 2o
        # Gilbert (estatua). So' instala se $MKXP_FOLLOW_ADD_FALLBACK==true (opt-in).
        if $MKXP_FOLLOW_ADD_FALLBACK == true && !$__followadd_fix_done &&
           ((respond_to?(:pbAddDependency2, true) rescue false) ||
            (Object.method_defined?(:pbAddDependency2) rescue false) ||
            (Object.private_method_defined?(:pbAddDependency2) rescue false))
          $__followadd_fix_done = true
          begin
            Object.send(:define_method, :pbAddDependency2) do |eventID, eventName, commonEvent|
              _ev   = ($game_map.events[eventID] rescue nil)
              _via  = _ev.nil? ? 'nil' : 'direto'
              if _ev.nil? && $MKXP_FOLLOW_ADD_FALLBACK != false
                begin
                  _evs = ($game_map.events rescue nil)
                  if _evs.respond_to?(:each)
                    _evs.each do |_k, _e|
                      _eo = _e.nil? ? _k : _e   # tolera each que devolva so' valores
                      if (_k == eventID) || ((_eo.id rescue nil) == eventID)
                        _ev = _eo
                        _via = (_k == eventID ? 'scan(chave)' : 'scan(.id)')
                        break
                      end
                    end
                  end
                rescue => _se
                  (MKXPDebug.log("[FIX-FOLLOWADD] scan rebentou: #{_se.class}: #{_se.message.to_s[0,110]}") rescue nil)
                end
              end
              (MKXPDebug.log("[FIX-FOLLOWADD] pbAddDependency2(id=#{eventID}, name=#{eventName.inspect}, ce=#{commonEvent.inspect}) -> ev=#{_ev.nil? ? 'AINDA-NIL(evento ausente no mapa)' : ('OK id=' + (_ev.id rescue '?').to_s)} via=#{_via} | mapEvts=#{(($game_map.events.size) rescue '?')} map=#{($game_map.map_id rescue '?')}") rescue nil)
              # [FIX-FOLLOWADD] DIAGNOSTICO do addEvent: o events[id] ja' vem OK
              # (via=direto), mas dependentEvents fica 0. Medimos a lista ANTES/DEPOIS
              # e apanhamos excecao (com backtrace) -> o proximo log diz se o
              # createEvent rebenta, se "ja existe", ou se cresce mas medimos mal.
              # addEvent e' chamado UMA vez (sem efeitos colaterais extra).
              _gb = ($PokemonGlobal.dependentEvents.size rescue -1)
              _rb = ($PokemonTemp.dependentEvents.instance_variable_get(:@realEvents).size rescue -1)
              _adderr = nil
              begin
                $PokemonTemp.dependentEvents.addEvent(_ev, eventName, commonEvent)
              rescue => _ae
                _adderr = "#{_ae.class}: #{_ae.message.to_s[0,140].gsub(/\s+/,' ')} | bt:#{((_ae.backtrace||[])[0,4].join(' <- ') rescue '?')}"
              end
              _ga = ($PokemonGlobal.dependentEvents.size rescue -1)
              _ra = ($PokemonTemp.dependentEvents.instance_variable_get(:@realEvents).size rescue -1)
              (MKXPDebug.log("[FIX-FOLLOWADD] addEvent: $PokemonGlobal.dep #{_gb}->#{_ga} | @realEvents #{_rb}->#{_ra} | erro=#{_adderr || 'nenhum'}#{(_ga==_gb && _adderr.nil?) ? ' <- SEM ERRO mas NAO cresceu (ja-existe OU createEvent devolveu sem push)' : (_ga>_gb ? ' <- SEGUIDOR ADICIONADO' : '')}") rescue nil)

              # [FIX-FOLLOWKEEP] snapshot NO MOMENTO DO ADD. O update por-frame perdia a
              # janela 1->0 porque a limpeza ($PokemonGlobal.dependentEvents -> 0) acontece
              # DENTRO de um evento (interpretador), antes do proximo update do sprite. Aqui
              # capturamos o que acabou de ser adicionado; o restore (em
              # DependentEventSprites#update) repoe-o se for limpo no mesmo mapa.
              if $MKXP_FOLLOWKEEP != false
                begin
                  _dgnow = ($PokemonGlobal.dependentEvents rescue nil)
                  if _dgnow.is_a?(Array) && _dgnow.size > 0
                    $__fk_snap = _dgnow.map { |e| e.is_a?(Array) ? e.dup : e }
                    $__fk_map  = ($game_map.map_id rescue nil)
                    (MKXPDebug.log("[FIX-FOLLOWKEEP] snapshot capturado NO ADD (#{$__fk_snap.size} no mapa #{$__fk_map}) -> restaura-se se for limpo") rescue nil)
                  end
                rescue => _fks
                  (MKXPDebug.log("[FIX-FOLLOWKEEP] snapshot-no-add rebentou: #{_fks.class}: #{_fks.message.to_s[0,80]}") rescue nil)
                end
              end

              # [DEPGETTER] TESTE DECISIVO (1x): o getter $PokemonGlobal.dependentEvents
              # devolve SEMPRE o mesmo array (push persiste) ou um array FRESCO (push perde-se)?
              unless $__depgetter_tested
                $__depgetter_tested = true
                begin
                  _A = $PokemonGlobal.dependentEvents
                  _oidA = (_A.object_id rescue '?'); _s0 = (_A.size rescue -1)
                  (_A.push([:__probe_stab__]) rescue nil)
                  _B = $PokemonGlobal.dependentEvents
                  _oidB = (_B.object_id rescue '?'); _s1 = (_B.size rescue -1)
                  (MKXPDebug.log("[DEPGETTER] estavel? oid #{_oidA}==#{_oidB}? #{_oidA==_oidB} | size #{_s0}->#{_s1} #{_s1>_s0 ? '(ESTAVEL: push persiste)' : '(INSTAVEL: getter devolve array FRESCO -> e ESTA a raiz do seguidor)'}") rescue nil)
                  # limpeza do marcador de teste
                  (_B.reject!{ |x| x==[:__probe_stab__] } rescue nil)
                  ($PokemonGlobal.dependentEvents = _B) rescue nil
                rescue => _dge
                  (MKXPDebug.log("[DEPGETTER] teste rebentou: #{_dge.class}: #{_dge.message.to_s[0,100]}") rescue nil)
                end
              end

              # [FIX-FOLLOWADD] CORRECAO (segura, so' quando o addEvent normal NAO cresceu):
              # push DIRETO no array persistente + write-back via attr_writer + push ao
              # @realEvents existente + erase do evento estatico. Tudo em rescue (no-op no
              # pior caso). Reverte com $MKXP_FOLLOW_ADD_FALLBACK=false.
              if $MKXP_FOLLOW_ADD_FALLBACK != false && _ga == _gb && _adderr.nil? && _ev
                begin
                  _dep  = $PokemonTemp.dependentEvents
                  _arr  = ($PokemonGlobal.dependentEvents rescue nil); _arr = [] if !_arr.is_a?(Array)
                  _ed   = [($game_map.map_id rescue 0), (_ev.id rescue 0), ($game_map.map_id rescue 0),
                           (_ev.x rescue 0), (_ev.y rescue 0), (_ev.direction rescue 2),
                           ((_ev.character_name || "").dup rescue ""), (_ev.character_hue rescue 0),
                           eventName, commonEvent]
                  _arr.push(_ed)
                  ($PokemonGlobal.dependentEvents = _arr) rescue nil       # write-back persistente
                  # createEvent pode rebentar no mruby ($MapFactory.getMap / Game_CommonEvent.new)
                  # -> ANTES era engolido (rescue nil) e @realEvents ficava 0. Agora regista a causa.
                  _newev = nil; _ceerr = nil
                  begin
                    _newev = _dep.send(:createEvent, _ed)
                  rescue => _cee
                    _ceerr = "#{_cee.class}: #{_cee.message.to_s[0,140].gsub(/\s+/,' ')} | bt:#{((_cee.backtrace||[])[0,4].join(' <- ') rescue '?')}"
                  end
                  # FALLBACK robusto: se o createEvent core rebentou (provavel ponto de
                  # falha mruby = $MapFactory.getMap(map) nil OU Game_CommonEvent.new(ce)),
                  # reconstroi o Game_Event a' mao com cada passo guardado e mapa-atual
                  # como recurso. Assim o seguidor ganha um evento vivo mesmo que o core falhe.
                  if _newev.nil?
                    begin
                      _rpg = RPG::Event.new(_ed[3], _ed[4]); _rpg.id = _ed[1]
                      if _ed[9]
                        _cev = (Game_CommonEvent.new(_ed[9]) rescue nil)
                        (_rpg.pages[0].list = _cev.list if _cev && _cev.respond_to?(:list) && _cev.list) rescue nil
                      end
                      _mp = ($MapFactory.getMap(_ed[2]) rescue nil) || ($game_map rescue nil)
                      _newev = Game_Event.new(_ed[0], _rpg, _mp)
                      (_newev.character_name = _ed[6]) rescue nil
                      (_newev.character_hue  = _ed[7]) rescue nil
                      case _ed[5]
                      when 2 then (_newev.turn_down  rescue nil)
                      when 4 then (_newev.turn_left  rescue nil)
                      when 6 then (_newev.turn_right rescue nil)
                      when 8 then (_newev.turn_up    rescue nil)
                      end
                      (MKXPDebug.log("[FIX-FOLLOWADD] createEvent-FALLBACK construiu o evento a' mao (core falhou) -> #{_newev.nil? ? 'AINDA-NIL' : 'OK'}") rescue nil)
                    rescue => _fbe
                      (MKXPDebug.log("[FIX-FOLLOWADD] createEvent-FALLBACK tambem rebentou: #{_fbe.class}: #{_fbe.message.to_s[0,120].gsub(/\s+/,' ')}") rescue nil)
                    end
                  end
                  _re    = (_dep.instance_variable_get(:@realEvents) rescue nil)
                  (_re.push(_newev) if _re && _newev) rescue nil           # entra na lista viva
                  # CRUCIAL: bump @lastUpdate -> DependentEventSprites#update faz refresh e
                  # CRIA o Sprite_Character do seguidor (sem isto, dados entram mas sprite nao).
                  begin
                    _lu = (_dep.lastUpdate rescue (_dep.instance_variable_get(:@lastUpdate) rescue 0)) || 0
                    _dep.instance_variable_set(:@lastUpdate, _lu + 1)
                  rescue; end
                  (_ev.erase rescue nil)                                   # esconde o evento estatico
                  _gc = ($PokemonGlobal.dependentEvents.size rescue -1)
                  (MKXPDebug.log("[FIX-FOLLOWADD] FORCE write-back: dep=#{_gc} realEvents=#{(_re.size rescue '?')} newev=#{_newev.nil? ? 'NIL' : 'OK'}#{_ceerr ? (' createEvent REBENTOU-> ' + _ceerr) : ''} lastUpdate++=#{(_dep.lastUpdate rescue '?')} #{_gc>_gb ? '<- SEGUIDOR FORCADO (ve se o Giltbert te segue)' : '<- AINDA falhou'}") rescue nil)
                  # ── REBUILD (a CORRECAO real do Gilbert) ──────────────────────────
                  # A instancia DependentEvents viva esta' FROZEN (Marshal no mruby) ->
                  # os push/set acima falham silenciosamente (@realEvents=0, lastUpdate=0).
                  # Mas o $PokemonGlobal.dependentEvents (Array) JA' tem o seguidor e e'
                  # mutavel/estavel, e DependentEvents#initialize reconstroi @realEvents a
                  # partir dele. Ha 'attr_writer :dependentEvents'. Entao criamos uma
                  # instancia NOVA (mutavel, com o seguidor ja' em @realEvents) e
                  # atribuimo-la -> o DependentEventSprites refresca (lastUpdate=-1) e CRIA
                  # o Sprite_Character. Reverte com $MKXP_FOLLOW_REBUILD=false.
                  if $MKXP_FOLLOW_REBUILD != false
                    begin
                      _froz = (_dep.frozen? rescue '?')
                      # initialize chama o createEvent CORE que REBENTA (loop aborta ->
                      # @realEvents=0). Por isso saltamos o initialize (allocate) e
                      # reconstruimos @realEvents A' MAO, com o createEvent robusto +
                      # registo da excecao do core (para finalmente sabermos PORQUE rebenta).
                      _nd = (DependentEvents.allocate rescue DependentEvents.new)
                      _nd.instance_variable_set(:@realEvents, []) rescue nil
                      # @lastUpdate TEM de ser DIFERENTE do que o DependentEventSprites
                      # tem em cache (a instancia antiga usava -1; se a nova tambem for
                      # -1, update() ve -1==-1 -> NUNCA faz refresh -> sprite nunca criado).
                      # Valor unico e crescente garante refresh -> cria o Sprite_Character.
                      $__follow_lu = ($__follow_lu || 1000) + 1
                      _nd.instance_variable_set(:@lastUpdate, $__follow_lu) rescue nil
                      _evs = ($PokemonGlobal.dependentEvents rescue []) || []
                      _built = 0; _cerr = nil
                      _evs.each do |_e2|
                        _made = nil
                        begin
                          _made = _nd.send(:createEvent, _e2)        # core
                        rescue => _ce2
                          _cerr ||= "#{_ce2.class}: #{_ce2.message.to_s[0,90].gsub(/\s+/,' ')} @#{((_ce2.backtrace||[])[0].to_s[0,60] rescue '?')}"
                        end
                        if _made.nil?                                 # fallback robusto inline
                          begin
                            _r = RPG::Event.new(_e2[3], _e2[4]); _r.id = _e2[1]
                            if _e2[9]
                              _c = (Game_CommonEvent.new(_e2[9]) rescue nil)
                              (_r.pages[0].list = _c.list if _c && _c.respond_to?(:list) && _c.list) rescue nil
                            end
                            _m = ($MapFactory.getMap(_e2[2]) rescue nil) || ($game_map rescue nil)
                            _made = Game_Event.new(_e2[0], _r, _m)
                            (_made.character_name = _e2[6]) rescue nil
                            (_made.character_hue  = _e2[7]) rescue nil
                          rescue; _made = nil; end
                        end
                        (_nd.instance_variable_get(:@realEvents).push(_made); _built += 1 if _made) rescue nil
                      end
                      ($PokemonTemp.dependentEvents = _nd) rescue nil
                      _nd2  = ($PokemonTemp.dependentEvents rescue nil)
                      _renew = (_nd2.instance_variable_get(:@realEvents).size rescue -1)
                      (MKXPDebug.log("[FIX-FOLLOWADD] REBUILD-MANUAL: frozen?=#{_froz} | $PokemonGlobal.dep=#{_evs.size} construidos=#{_built} @realEvents-nova=#{_renew} | core createEvent erro=#{_cerr || 'nenhum'} #{_renew.to_i>0 ? '<- SEGUIDOR NA LISTA VIVA (deve aparecer e SEGUIR)' : '<- AINDA 0'}") rescue nil)
                    rescue => _rbe
                      (MKXPDebug.log("[FIX-FOLLOWADD] REBUILD rebentou: #{_rbe.class}: #{_rbe.message.to_s[0,130].gsub(/\s+/,' ')}") rescue nil)
                    end
                  end
                rescue => _fwe
                  (MKXPDebug.log("[FIX-FOLLOWADD] FORCE rebentou: #{_fwe.class}: #{_fwe.message.to_s[0,120]}") rescue nil)
                end
              end
            end
            (MKXPDebug.log("[FIX-FOLLOWADD] instalado (reimplementa pbAddDependency2 + re-resolucao de evento; flag $MKXP_FOLLOW_ADD_FALLBACK ON) -- FALA COM O GILTBERT no proximo teste") rescue nil)
          rescue => _fe
            (MKXPDebug.log("[FIX-FOLLOWADD] instalacao falhou: #{_fe.class}: #{_fe.message}") rescue nil)
          end
        end

        # ─── GILBERT-XFER: o seguidor HUMANO (Game_Event normal, NAO Game_FollowerEvent)
        # nao e' reposicionado ao mudar de mapa. Raiz na fonte: Refresh.rb (transfer_player)
        # e o core pbFollowEventAcrossMaps so' atualizam o mapa-atual (idx2)/posicao para
        # Game_FollowerEvent; o Gilbert fica preso ao mapa antigo -> some no transfer. Aqui,
        # APOS o transfer, reposicionamos os dependentes NAO-FollowerEvent junto do jogador e
        # forcamos walk_anime (anti-estatua). Aditivo, nao mexe no FIX-FOLLOWADD. Off: $MKXP_GILBERT_XFER=false.
        if !$__gilbert_xfer && defined?(Scene_Map) && (Scene_Map.method_defined?(:transfer_player) rescue false)
          $__gilbert_xfer = true
          begin
            Scene_Map.class_eval do
              unless method_defined?(:__gilbertxfer_orig_transfer)
                alias_method :__gilbertxfer_orig_transfer, :transfer_player
                def transfer_player(*a, &b)
                  __gilbertxfer_orig_transfer(*a, &b)
                  return if $MKXP_GILBERT_XFER == false
                  begin
                    _dep = ($PokemonTemp.dependentEvents rescue nil)
                    _arr = ($PokemonGlobal.dependentEvents rescue nil)
                    _re  = (_dep.instance_variable_get(:@realEvents) rescue nil)
                    if _arr.is_a?(Array) && _re.is_a?(Array)
                      _arr.each_index do |i|
                        _ev = _re[i]
                        next if _ev.nil?
                        next if (defined?(Game_FollowerEvent) && _ev.is_a?(Game_FollowerEvent))
                        (_ev.instance_variable_set(:@map, $game_map)) rescue (_ev.map = $game_map rescue nil)
                        (_ev.moveto($game_player.x, $game_player.y)) rescue nil
                        (_ev.direction = $game_player.direction) rescue nil
                        (_ev.walk_anime = true) rescue (_ev.instance_variable_set(:@walk_anime, true) rescue nil)
                        if _arr[i].is_a?(Array)
                          (_arr[i][2] = ($game_map.map_id rescue _arr[i][2])) rescue nil
                          (_arr[i][3] = (_ev.x rescue _arr[i][3])) rescue nil
                          (_arr[i][4] = (_ev.y rescue _arr[i][4])) rescue nil
                        end
                        (MKXPDebug.log("[GILBERT-XFER] dependente #{i} (#{_ev.class}) reposto -> mapa #{($game_map.map_id rescue '?')} @(#{(_ev.x rescue '?')},#{(_ev.y rescue '?')}) dir=#{(_ev.direction rescue '?')}") rescue nil)
                      end
                    end
                  rescue => _ge
                    (MKXPDebug.log("[GILBERT-XFER] runtime falhou: #{_ge.class}: #{_ge.message.to_s[0,120].gsub(/\s+/,' ')}") rescue nil)
                  end
                end
              end
            end
            (MKXPDebug.log("[GILBERT-XFER] hook transfer_player instalado (repoe seguidor humano + walk_anime no transfer)") rescue nil)
          rescue => _ge2
            (MKXPDebug.log("[GILBERT-XFER] instalacao falhou: #{_ge2.class}: #{_ge2.message.to_s[0,120]}") rescue nil)
          end
        end

        # ─── ADDEVENT-TRACE: o addEvent REAL do Essentials e' chamado com evento
        # valido e lista vazia mas NAO cresce, sem erro (teorias freeze/array-fresco/
        # stub TODAS desmentidas pelo log). Envolvemos o addEvent real (alias) e
        # DELEGAMOS p/ o original (comportamento intacto; re-lanca excecoes). Logamos:
        # self vs $PokemonTemp.dependentEvents, validade do evento, character_name,
        # dup-check replicado, EXCECAO escondida (rescue Exception), tamanho antes/depois.
        # Revela a linha exata onde desiste. Off: $MKXP_ADDEVENT_TRACE=false.
        # gate por instance_method (o method_defined?/const_defined? estao PARTIDOS neste
        # mruby -- ADD-ROOT: owner=0, method_defined? devolve false p/ metodos que existem).
        # instance_method funciona (o VERIFY usa-o). Se resolver, a classe+metodo existem.
        if $MKXP_PROBE_QUIET != true && !$__addevent_trace && (begin; DependentEvents.instance_method(:addEvent); true; rescue; false; end)
          $__addevent_trace = true
          begin
            DependentEvents.class_eval do
              unless method_defined?(:__trace_orig_addEvent)
                alias_method :__trace_orig_addEvent, :addEvent
                def addEvent(event, eventName=nil, commonEvent=nil)
                  return __trace_orig_addEvent(event, eventName, commonEvent) if $MKXP_ADDEVENT_TRACE == false
                  _len0 = -1
                  begin
                    _arr  = ($PokemonGlobal.dependentEvents rescue nil)
                    _len0 = (_arr.respond_to?(:length) ? _arr.length : -1)
                    _re0  = (instance_variable_get(:@realEvents).length rescue -1)
                    _dup  = false
                    if _arr.respond_to?(:each_index) && !event.nil?
                      _arr.each_index { |i| _e=_arr[i]; (_dup=true; break) if _e && _e[0]==($game_map.map_id rescue nil) && _e[1]==(event.id rescue nil) }
                    end
                    (MKXPDebug.log("[ADDEVENT-TRACE] ENTROU self_oid=#{(object_id rescue '?')} pt_oid=#{($PokemonTemp.dependentEvents.object_id rescue '?')} | ev_nil=#{event.nil?} id=#{(event.id rescue '?')} char_name=#{(event.character_name.inspect rescue 'RAISE')} | arr_oid=#{(_arr.object_id rescue '?')} len=#{_len0} realEv=#{_re0} dup=#{_dup} | name=#{eventName.inspect} ce=#{commonEvent.inspect}") rescue nil)
                  rescue => _pe
                    (MKXPDebug.log("[ADDEVENT-TRACE] pre-log rebentou: #{_pe.class}: #{_pe.message.to_s[0,90]}") rescue nil)
                  end
                  _r = nil; _excobj = nil
                  begin
                    _r = __trace_orig_addEvent(event, eventName, commonEvent)
                  rescue Exception => _oe
                    _excobj = _oe
                    (MKXPDebug.log("[ADDEVENT-TRACE] original REBENTOU: #{_oe.class}: #{_oe.message.to_s[0,140].gsub(/\s+/,' ')} | bt:#{((_oe.backtrace||[])[0,5].join(' <- ') rescue '?')}") rescue nil)
                  end
                  begin
                    _arr2 = ($PokemonGlobal.dependentEvents rescue nil)
                    _len1 = (_arr2.respond_to?(:length) ? _arr2.length : -1)
                    _re1  = (instance_variable_get(:@realEvents).length rescue -1)
                    (MKXPDebug.log("[ADDEVENT-TRACE] SAIU len=#{_len1} realEv=#{_re1} exc=#{_excobj ? _excobj.class : 'nenhuma'} #{(_len1.to_i > _len0.to_i) ? '<- CRESCEU' : '<- NAO CRESCEU'}") rescue nil)
                  rescue; end
                  raise _excobj if _excobj
                  _r
                end
              end
            end
            (MKXPDebug.log("[ADDEVENT-TRACE] instalado (envolve o addEvent real; revela a raiz do add do seguidor)") rescue nil)
          rescue => _ate
            (MKXPDebug.log("[ADDEVENT-TRACE] instalacao falhou: #{_ate.class}: #{_ate.message}") rescue nil)
          end
        end

        # ─── VERIFY: confirma o ESTADO de TODAS as correcoes (motor OU probe), 1x.
        # Num so' sitio diz se cada fix esta' aplicado, venha do binding_3ds.cpp
        # (compilado) ou do probe. PROCURA esta linha [VERIFY] no proximo log.
        if !$__verify_done && defined?($game_map) && $game_map && (($game_map.map_id rescue 0) > 0)
          $__verify_done = true
          begin
            v = []
            # SEGUIDOR
            if defined?(DependentEvents)
              _ar = (DependentEvents.instance_method(:addEvent).arity rescue 99)
              v << "addEvent.arity=#{_ar}(#{_ar == 1 ? 'STUB-NOOP!!' : 'OK'})"
              v << "eachEvent=#{DependentEvents.method_defined?(:eachEvent)}"
              v << "createEvent=#{DependentEvents.method_defined?(:createEvent)}"
              # [ADD-ROOT] dump decisivo (VERIFY corre sempre): a classe viva e' a CORE (tem
              # initialize/pbEnsureEvent/updateDependentEvents/removeEvent) ou uma CASCA criada
              # por um plugin (o core 050 nao carregou)? Se faltam os metodos core, o addEvent
              # real nunca correu -> por isso dep 0->0. Diz-me onde atacar (motor: porque falha
              # o load do 050; ou probe: repor addEvent core).
              _cm = [:initialize,:pbEnsureEvent,:updateDependentEvents,:removeEvent,:pbMoveDependentEvents,:pbFollowEventAcrossMaps,:getEventByName].map { |m| "#{m}=#{(DependentEvents.method_defined?(m) rescue '?') ? 1 : 0}" }.join(',')
              v << "[ADD-ROOT] core-methods: #{_cm} | im(false)=#{(DependentEvents.instance_methods(false).size rescue '?')} | addEvent_owner=#{(DependentEvents.instance_method(:addEvent).owner rescue '?')}"
            else
              v << "DependentEvents=AUSENTE!!"
            end
            v << "pbAddDependency2=#{(respond_to?(:pbAddDependency2, true) rescue false)}"
            _deg = ($PokemonGlobal.dependentEvents rescue nil)
            v << "PokemonGlobal.dep=#{_deg.class}(#{_deg.is_a?(Array) ? 'Array-OK' : 'ERRADO!!'})"
            v << "PokemonTemp.dep=#{($PokemonTemp.dependentEvents.class rescue '?')}"
            # MENU
            v << "MenuCloseFix=#{Object.const_defined?(:MenuCloseFix)}"
            if defined?(VoltseonsPauseMenu)
              v << "menu_prepended=#{(VoltseonsPauseMenu.ancestors.map(&:to_s).include?('MenuCloseFix') rescue '?')}"
            end
            # MAGENTA
            if defined?(GameData) && defined?(GameData::Metadata)
              v << "metafix=#{(GameData::Metadata.respond_to?(:__metafix_orig_get_player) rescue '?')}"
              _p0 = (GameData::Metadata.get_player(0)[0] rescue '?')
              v << "get_player(0)[0]=#{_p0.inspect}(#{_p0.is_a?(Symbol) ? 'OK' : 'CORROMPIDO'})"
            end
            # PERF
            v << "perf:throttle=#{$__spr_throttle == true},su=#{$__perf_su == true},blank=#{$__perf_blank_on == true},static=#{$__perf_static_on == true}"
            v << "MKXP_PERF=#{$MKXP_PERF.inspect}"
            (MKXPDebug.log("[VERIFY] " + v.join(" | ")) rescue nil)
          rescue => _e
            (MKXPDebug.log("[VERIFY] falhou: #{_e.class}: #{_e.message}") rescue nil)
          end
        end

        # [PERF 2026-07-04] bloco 100% DIAG (TF/TCMD/TPIC/MAPDUMP/TILEDIAG/CENSO/
        # SAVETEST-E2E): 1 log SD por frame + ~10-15 allocations/frame + varrimento
        # de pictures/frame + hitches de dump por mudanca de mapa. Bugs fechados ->
        # gated. Reativar: $MKXP_PROBE_QUIET=false.
        if $MKXP_PROBE_QUIET != true
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
              # [MAPCLASS 2026-07-03] O $game_map VIVO e' Game_Map ou Object? Se Object
              # -> corrupcao acontece no GAMEPLAY (nao no save/load; o marshal ja' foi
              # provado inocente). map_id(metodo) vs @map_id(ivar) confirma. 1x.
              if $__mapclass_done != true
                $__mapclass_done = true
                _gmc = ($game_map.class rescue '?')
                _gmm = ($game_map.map_id rescue 'ERRO')
                _gmi = ($game_map.instance_variable_get(:@map_id) rescue '?')
                (MKXPDebug.log("[MAPCLASS] $game_map VIVO: class=#{_gmc} map_id(metodo)=#{_gmm} @map_id(ivar)=#{_gmi} => #{_gmc.to_s == 'Game_Map' ? 'OK (Game_Map real)' : 'PARTIDO (Object! corrupcao no gameplay)'}") rescue nil)
                # [SAVETEST-E2E 2026-07-03] testar o CAMINHO DE SAVE real com o mapa
                # novo (Game_Map bom): compile_save_hash -> ficheiro -> ler -> classe do
                # @maps[0]. Se voltar Object -> o compile/save mangla (RAIZ). Se Game_Map
                # -> o save e' bom e o save partido do user veio de um load anterior.
                begin
                  if defined?(SaveData) && SaveData.respond_to?(:compile_save_hash) && _gmc.to_s == 'Game_Map'
                    _h = SaveData.compile_save_hash
                    _tf = "sdmc:/mkxp/game/Save/__e2e.bin"
                    File.open(_tf, "wb") { |_f| Marshal.dump(_h, _f) }
                    _b = nil; File.open(_tf, "rb") { |_f| _b = Marshal.load(_f) }
                    (File.delete(_tf) rescue nil)
                    _mf = _b.is_a?(Hash) ? _b[:map_factory] : nil
                    _m0 = _mf ? (_mf.instance_variable_get(:@maps)[0] rescue nil) : nil
                    (MKXPDebug.log("[SAVETEST-E2E] compile->file->load: @maps[0].class=#{(_m0.class rescue '?')} map_id(metodo)=#{(_m0.map_id rescue 'ERRO')} => #{(_m0.class.to_s rescue '') == 'Game_Map' ? 'PASS (save bom; save partido do user veio de load anterior)' : 'FALHOU: o SAVE mangla a classe -> RAIZ!'}") rescue nil)
                  end
                rescue => _ee
                  (MKXPDebug.log("[SAVETEST-E2E] EXCECAO: #{_ee.class}: #{_ee.message.to_s[0,80]}") rescue nil)
                end
              end
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
        end   # fim do gate $MKXP_PROBE_QUIET (bloco TF/TCMD/TPIC)
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
          # [TRY-CONTINUE teleporte tardio 2026-07-03] o teleporte imediato pos-
          # Game.load era desfeito (autorun/cutscene reposiciona). Aqui, ~700
          # frames DEPOIS do mapa estar vivo, movemos o jogador 1x para o alvo
          # ($MKXP_TRY_CONTINUE=[x,y]) -> a camara cobre a arvore -> ARVDBG
          # consegue dar o VEREDITO da fila de desenho. So' diagnostico.
          begin
            if $MKXP_TRY_CONTINUE.is_a?(Array) && !$__tryc_tele_done
              $__tryc_tele_n = ($__tryc_tele_n || 0) + 1
              if $__tryc_tele_n == 700
                $__tryc_tele_done = true
                _tx, _ty = $MKXP_TRY_CONTINUE
                ($game_player.moveto(_tx, _ty) rescue nil)
                ($game_player.center(_tx, _ty) rescue nil)
                (MKXPDebug.log("[TRY-CONTINUE] teleporte tardio p/ (#{_tx},#{_ty}) map=#{($game_map.map_id rescue '?')} feito (f=#{$__tryc_tele_n})") rescue nil)
              end
            end
          rescue; end
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
        # ── DETECTOR DE MAPA PRETO ────────────────────────────────────────────
        # O tilemap ATIVO a renderizar com tileset nil/disposed/placeholder(32x32)
        # = o chao/tiles ficam PRETOS (nada a desenhar). Loga so' QUANDO a condicao
        # acontece, 1x por (mapa,causa) -> identifica EXATAMENTE qual mapa fica preto
        # sem spam. (Causa tipica: tileset evictado e nao-recarregado ao voltar.)
        begin
          sp = (scene.instance_variable_get(:@spriteset) rescue nil)
          tm = (sp ? (sp.instance_variable_get(:@tilemap) rescue nil) : nil)
          if tm
            ts = (tm.tileset rescue (tm.instance_variable_get(:@tileset) rescue nil))
            bad = nil
            if ts.nil?
              bad = 'tileset=NIL'
            elsif (ts.disposed? rescue false)
              bad = 'tileset=DISPOSED'
            elsif ((ts.width rescue 0) <= 64 && (ts.height rescue 0) <= 64)
              bad = "tileset=PLACEHOLDER #{(ts.width rescue '?')}x#{(ts.height rescue '?')}"
            end
            if bad
              if $__sd[:blackmap_last] != [mid, bad]
                $__sd[:blackmap_last] = [mid, bad]
                (MKXPDebug.log("[MAPAPRETO] map=#{mid} '#{tn}' -> #{bad} -> tilemap renderiza PRETO. (tileset evictado/nao-recarregado?)") rescue nil)
              end
            else
              $__sd[:blackmap_last] = nil   # voltou ao normal
            end
          end
        rescue; end
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
      return if $MKXP_MAXDIAG == false   # [FPS] snapshot por-frame silenciado quando MAXDIAG off (default)
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
            # [PERF 2026-07-04] TMDBG gated (quarto preto resolvido); TILEPROF
            # (a medicao abaixo) fica — e' a metrica antes/depois do compose.
            if $MKXP_PROBE_QUIET != true
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
              # [MAPAPRETO2] classificar o CHAO: para algumas celulas visiveis, logar
              # os IDs das 3 camadas (z=0/1/2). id<48 = vazio; 48..383 = AUTOTILE;
              # >=384 = tile regular (blitVWrappedPixels). Diz de uma vez se o chao
              # preto e' autotile (compose original do PE a falhar no 3DS) ou tile
              # regular (remapeamento da compactacao errado). So' 1x por mapa (no
              # bloco @@__tmdbg_seen) e so' no passo regular (auto=false). Seguro:
              # so' le @map_data (Table), tudo rescue. So' p/ diagnostico do preto.
              if !autotiles && md && (md.xsize rescue 0) > 0
                begin
                  _xs = md.xsize; _ys = md.ysize
                  _cx = _xs / 2; _cy = _ys / 2
                  _samples = []
                  [[_cx, _cy], [_cx+1, _cy], [_cx, _cy+1], [1, 1], [_xs-2, _ys-2]].each do |cc|
                    _x, _y = cc[0], cc[1]
                    next if _x < 0 || _y < 0 || _x >= _xs || _y >= _ys
                    _z0 = (md[_x, _y, 0] rescue -1)
                    _z1 = (md[_x, _y, 1] rescue -1)
                    _z2 = (md[_x, _y, 2] rescue -1)
                    _clf = if _z0 < 48 then "vazio" elsif _z0 < 384 then "AUTOTILE" else "regular" end
                    _samples << "(#{_x},#{_y})z=[#{_z0},#{_z1},#{_z2}]#{_clf}"
                  end
                  MKXPDebug.log("[MAPAPRETO2] map=#{mid} chao(z0): #{_samples.join(' ')}") rescue nil
                rescue => _me
                  (MKXPDebug.log("[MAPAPRETO2] falhou: #{_me.class}: #{_me.message}") rescue nil)
                end
              end
            end
            end   # fim do gate $MKXP_PROBE_QUIET (TMDBG)
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
#  PEGUARD  —  ParticleEngine#update rebenta apos transfer (sair de casa)
# -----------------------------------------------------------------------------
#  CAUSA (provada pelo backtrace): footsteps_update -> ... -> ParticleEngine#update
#  (010_ParticleEngine.rb:91 'if particle.event.pe_refresh'). Ao sair da casa, o
#  @effect guarda particulas cujo EVENTO foi destruido no transfer; particle.event
#  da' 'NoMethodError: undefined method event' -> MATA o Spriteset_Map#update TODOS
#  os frames -> os sprites (casa, arvore, NPCs) nao se reposicionam e parecem
#  "seguir o player"; depois a cena do Gilbert crasha. O particle obsoleto NUNCA e'
#  limpo, por isso o crash persiste ~20s. Aqui re-implementamos o update com guarda
#  POR-PARTICULA: a particula obsoleta salta (efeito cosmetico), as boas continuam,
#  e o spriteset volta a atualizar. $MKXP_PEGUARD=false desliga.
# =============================================================================
begin
  if defined?(ParticleEngine) && ParticleEngine.method_defined?(:update) && !$__peguard_installed
    $__peguard_installed = true
    class ParticleEngine
      alias_method :__peguard_orig_update, :update
      def update
        if $MKXP_PEGUARD == false
          return __peguard_orig_update
        end
        begin
          if @firsttime
            @firsttime = false
            (vals = (@map.events.values rescue nil)) and vals.each do |event|
              (remove_effect(event) rescue nil)
              (add_effect(event) rescue nil)
            end
          end
          n = (@effect.length rescue 0)
          i = 0
          while i < n
            particle = @effect[i]
            i += 1
            next if particle.nil?
            begin
              ev = (particle.event rescue nil)   # obsoleto apos transfer -> nil, salta o refresh
              if ev && (ev.pe_refresh rescue false)
                (ev.pe_refresh = false rescue nil)
                np = (realloc_effect(ev, particle) rescue particle)
                @effect[i-1] = np
                particle = np
              end
              (particle.update if particle) rescue nil
            rescue => _pe1
              # particula obsoleta (evento destruido no transfer) -> salta so' esta
              $__peguard_skips = ($__peguard_skips || 0) + 1
              (MKXPDebug.log("[PEGUARD] particula obsoleta saltada (#{_pe1.class}: #{_pe1.message.to_s[0,40]})") rescue nil) if ($__peguard_skips <= 3)
            end
          end
        rescue => _pe2
          (MKXPDebug.log("[PEGUARD] update contido: #{_pe2.class}: #{_pe2.message.to_s[0,60]}") rescue nil) if (($__peguard_outer = ($__peguard_outer || 0) + 1) <= 3)
        end
        nil
      end
    end
    (MKXPDebug.log("[PEGUARD] ParticleEngine#update protegido (particula obsoleta no transfer nao mata o spriteset)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[PEGUARD] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  FIX-OLDRAND  —  '[ERROR] script.rb: undefined method oldrand'
# -----------------------------------------------------------------------------
#  Um script chama 'oldrand' (alias antigo do RNG do RGSS, guardado/reposto por
#  alguns scripts de batalha) que nao existe nesta build -> NoMethodError. Definimos
#  Kernel#oldrand = rand para qualquer chamada em runtime nao rebentar. Inofensivo.
#  $MKXP_OLDRAND_SHIM=false desliga.
# =============================================================================
begin
  if $MKXP_OLDRAND_SHIM != false && !(Kernel.method_defined?(:oldrand) rescue false)
    module Kernel
      def oldrand(*a); rand(*a); end
    end
    (MKXPDebug.log("[FIX-OLDRAND] Kernel#oldrand definido (alias de rand) -- script chamava oldrand inexistente") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[FIX-OLDRAND] falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  FIX-NSYNC  —  sync NATIVO (C++) de posicao para sprites de personagem ESTATICOS
# -----------------------------------------------------------------------------
#  Para um NPC PARADO, os plugins que embrulham Sprite_Character#update (Water
#  bubbles, Marin's Footprints) sao NO-OP (nao surfa -> sem bolha; nao se move ->
#  sem pegada). Logo, quando a assinatura do @character nao mudou, saltamos TODA a
#  cadeia Ruby do update e recalculamos SO' x/y/z em C++ (mkxp_static_sync, formula
#  EXATA da v19.1). Ataca a CAMINHADA: o scroll da camara move ~20 NPCs parados no
#  ecra' sem ~10 dispatches mruby cada.
#
#  Sprites que se MOVEM/animam/saltam, em escadas, tile-based, ou especiais (player,
#  reflexos, follower com @reflection/@surfbase) -> caem no update Ruby COMPLETO
#  (FXGUARD->plugins->vanilla), exatamente como antes. Refresh Ruby forcado a cada
#  30 frames apanha o tom dia/noite (cache de 30s) e qualquer edge case (<=1s).
#
#  INSTALADO DEPOIS do FXGUARD -> wrap MAIS EXTERIOR: estaticos saltam tudo;
#  nao-estaticos passam pela cadeia protegida como antes. Nunca crasha (ler ivar
#  inexistente -> nil -> "mudou" -> Ruby; e begin/rescue a toda a volta).
#  Flag: $MKXP_NATIVE_STATIC_SYNC (default ON; pôr =false reverte instantaneamente).
# =============================================================================
begin
  if defined?(Sprite_Character) && Sprite_Character.method_defined?(:mkxp_static_sync) &&
     Sprite_Character.method_defined?(:update) && !$__nsync_installed
    $__nsync_installed = true
    class Sprite_Character
      alias_method :__nsync_orig_update, :update
      def update(*a)
        if $MKXP_NATIVE_STATIC_SYNC != false
          begin
            if mkxp_static_sync
              # diagnostico leve: razao fast-path vs Ruby (cumulativa), logada ~cada 1800 updates
              $__ns_hit = ($__ns_hit || 0) + 1
              _tot = $__ns_hit + ($__ns_rb || 0)
              if (_tot % 1800) == 0
                (MKXPDebug.log("[FIX-NSYNC] fast-path(C++)=#{$__ns_hit} ruby=#{$__ns_rb || 0} => #{100 * $__ns_hit / _tot}% nativo") rescue nil)
              end
              return
            end
          rescue => _e
            unless $__nsync_err_logged
              (MKXPDebug.log("[FIX-NSYNC] excecao no sync nativo -> fallback Ruby: #{_e.class}: #{_e.message}") rescue nil)
              $__nsync_err_logged = true
            end
          end
        end
        $__ns_rb = ($__ns_rb || 0) + 1
        __nsync_orig_update(*a)
      end
    end
    (MKXPDebug.log("[FIX-NSYNC] sync nativo de sprites estaticos instalado (wrap exterior; flag $MKXP_NATIVE_STATIC_SYNC)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[FIX-NSYNC] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
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
        next if old.nil?   # [FIX-CASA-COL] slot vazio de alinhamento de coluna
        remap[old] = 384 + k
        i = old - 384
        cy = (k >> 3) * 32
        col = cy / th
        dst.blt(col * 256 + (k & 7) * 32, cy % th, src, Rect.new((i & 7) * 32, (i >> 3) * 32, 32, 32))
      end
      # ── RENDER-FIX (casa/arvore invisivel): o blt da fonte GIGANTE (256x31040) em
      # Y-alto falha em SILENCIO p/ alguns tiles -> compactado fica alpha 0 mesmo com
      # a fonte cheia (ATDBG: old=7850/7852 pri=5 -> aMAX=0). O get_pixel da fonte faz
      # unwrap CORRETO (TSC-PIXDBG provou s255 a Y30784). Logo, 2 passagens p/ minimizar
      # syncs de textura: (1) deteta tiles "fonte-cheia mas compactado-vazio"; (2)
      # recopia-os pixel-a-pixel via get_pixel/set_pixel. Custo: ~9 get_pixel/tile na
      # deteccao (surfaces sincronizam 1x) + 1024 so' p/ os que falharam (raros). UMA
      # vez por carga de mapa gigante; nao corre por frame. Desligar: $MKXP_TSC_RECOPY=false.
      begin
        if $MKXP_TSC_RECOPY != false
          _pts = [[16,16],[8,8],[24,24],[8,24],[24,8],[16,4],[4,16],[28,16],[16,28]]
          _fail = []
          _forced1 = 0
          used_sorted.each_with_index do |old, k|
            next if old.nil?   # [FIX-CASA-COL] slot vazio de alinhamento
            i = old - 384
            sxb = (i & 7) * 32; syb = (i >> 3) * 32
            cy = (k >> 3) * 32; col = cy / th
            dx = col * 256 + (k & 7) * 32; dy = cy % th
            if col >= 1 && $MKXP_TSC_COL1_FORCE != false
              # [FIX-CASA-COL1] A COLUNA 1 e' a zona onde o blt da fonte gigante em
              # Y-alto falha de forma ERRATICA (provado: tiles col=1 -> aMAX=0 no
              # render; col=0 -> aMAX=255). A deteccao por 9 pontos perde tiles
              # ESPARSOS (bordas de telhado/arvore) e marca-os como "fonte vazia".
              # Por isso recopiamos SEMPRE toda a col1 via get_pixel/set_pixel -- o
              # caminho CPU que le Y-alto corretamente (TSC-PIXDBG: s255 a Y30784).
              # Aditivo: so' escreve col1; a col0 (que funciona) nunca e' tocada.
              # Tiles de fonte transparente recopiam transparente (inofensivo).
              _fail.push([sxb, syb, dx, dy, old]); _forced1 += 1
            else
              _as = 0; _pts.each { |p| _v = (src.get_pixel(sxb + p[0], syb + p[1]).alpha rescue 0); (_as = _v if _v > _as) }
              next if _as <= 0                       # col0: fonte vazia => tile legitimamente transparente
              _ac = 0; _pts.each { |p| _v = (dst.get_pixel(dx + p[0], dy + p[1]).alpha rescue 0); (_ac = _v if _v > _ac) }
              _fail.push([sxb, syb, dx, dy, old]) if _ac == 0   # blt falhou em silencio
            end
          end
          # [PERF-RECOPY 2026-07-04] o recopy pixel-a-pixel da col1 inteira custava
          # 3-5 SEGUNDOS por tileset grande (115-166 tiles x 2048 dispatches get/
          # set_pixel = 235-340k travessias Ruby->C++ = o grosso do hitch de porta).
          # Agora: 1a tentativa em LOTE nativo — para cada tile, fill transparente
          # (sx=-1) + blt (compose_tiles processa em ordem => semantica REPLACE
          # exata, igual ao set_pixel). Depois RE-verificamos com os 9 pontos: so'
          # os que CONTINUAREM vazios (com fonte nao-vazia) caem no recopy antigo.
          # Se o caminho nativo estiver bom (provavel: a falha col1 era de uma era
          # anterior do blt), o custo cai de 3-5s para ~50ms com correcao IGUAL.
          _nat_ok = (Bitmap.instance_methods.include?(:compose_tiles) rescue false)
          if _nat_ok && _fail.size > 0
            _q = []
            _fail.each { |f| _q << f[2] << f[3] << -1 << 0 << 32 << 32     # limpa (REPLACE)
                              _q << f[2] << f[3] << f[0] << f[1] << 32 << 32 }
            (dst.compose_tiles(src, _q) rescue nil)
            _still = []
            _fail.each do |f|
              _as2 = 0; _pts.each { |p| _v = (src.get_pixel(f[0] + p[0], f[1] + p[1]).alpha rescue 0); (_as2 = _v if _v > _as2) }
              next if _as2 <= 0                    # fonte vazia -> transparente e' correto
              _ac2 = 0; _pts.each { |p| _v = (dst.get_pixel(f[2] + p[0], f[3] + p[1]).alpha rescue 0); (_ac2 = _v if _v > _ac2) }
              _still.push(f) if _ac2 == 0          # nativo falhou mesmo -> pixel a pixel
            end
            (MKXPDebug.log("[TSC-RECOPY] lote nativo: #{_fail.size} tiles em 1 chamada; #{_still.size} precisaram de fallback pixel") rescue nil)
            _fail = _still
          end
          _fail.each do |sxb, syb, dx, dy, _old|
            _yy = 0
            while _yy < 32
              _xx = 0
              while _xx < 32
                _px = (src.get_pixel(sxb + _xx, syb + _yy) rescue nil)
                (dst.set_pixel(dx + _xx, dy + _yy, _px) rescue nil) if _px
                _xx += 1
              end
              _yy += 1
            end
          end
          if _fail.size > 0
            _ids = _fail.first(12).map { |f| f[4] }.join(',')
            (MKXPDebug.log("[TSC-RECOPY] #{_fail.size} tiles re-copiados via get_pixel (col1 forcada=#{_forced1}, restantes=deteccao col0) ids=#{_ids}#{_fail.size > 12 ? '...' : ''}") rescue nil)
          else
            (MKXPDebug.log("[TSC-RECOPY] 0 falhas por pixel (col1 forcada=#{_forced1})") rescue nil)
          end
        end
      rescue => _re
        (MKXPDebug.log("[TSC-RECOPY] erro: #{_re.class}: #{_re.message}") rescue nil)
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
                # ── FIX PAREDES DAS CASAS (bug de render, provado por render proprio) ──
                # As PAREDES (prioridade 0) que calhavam na COLUNA 1 do compactado falhavam
                # silenciosamente: o refreshLayer0 (que compoe a camada 0 = tiles pri-0) le
                # a coluna 1 errado, e o HOUSEFIX so' cobre o getRegularTile (tiles pri>0 =
                # telhados). Por isso os telhados apareciam e as paredes nao. SOLUCAO: ordenar
                # os tiles pri-0 PRIMEIRO (-> coluna 0, onde o refreshLayer0 le bem) e os pri>0
                # por ULTIMO (-> coluna 1, onde o HOUSEFIX ja' trata). Os 204 tiles pri-0 cabem
                # na coluna 0 (max 256). Consistente: remap, data e tabelas pri/pass derivam de 'us'.
                # Fallback seguro: se as prioridades nao estiverem acessiveis, ordena so' por id (=antes).
                _tsid_pr = (gm.instance_variable_get(:@map).tileset_id rescue (gm.tileset_id rescue nil))
                _opr_pr  = (_tsid_pr ? ($data_tilesets[_tsid_pr].priorities rescue nil) : nil)
                us = used.keys.sort_by { |old| [(((_opr_pr && (_opr_pr[old] rescue 0)) || 0) > 0) ? 1 : 0, old] }
                _npr0 = us.count { |old| (((_opr_pr && (_opr_pr[old] rescue 0)) || 0) == 0) }
                (MKXPDebug.log("[TSC-WALLFIX] map=#{mid} ordenado pri-0 primeiro: #{_npr0} pri-0 (col0, max256) + #{us.size - _npr0} pri>0 (col1/HOUSEFIX)") rescue nil)
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
    # ── ARVDBG: VERDADE COMPLETA da copa da arvore (map43) ─────────────────────
    # GROUND TRUTH obtido no host (2026-07-02, leitura direta do Map043.rxdata +
    # PNG): a copa REAL = tiles antigos 8081/8082/8083 (pri=5) na CAMADA z=2 das
    # celulas (36-38,42), EMPILHADOS sobre 8057/8058/8059 (pri=5) na z=1. O PNG
    # TEM arte nos 3 (8081/8083 esparsos: 120/1024 px nas BORDAS -> amostragens
    # de 5/9 pontos interiores dao 0 e ENGANAM). 7850/7852 sao vazios DE ORIGEM
    # (marcadores do mapper) -- aMAX=0 neles e' correto, pista falsa antiga.
    # Este bloco corre UMA vez quando a arvore esta' visivel e despeja SEM
    # amostragem: (a) remap + conteudo real das 6 celulas na compactada
    # (varrimento 32x32 completo), (b) os sprites vivos na fila da copa (y=42)
    # com conteudo/z/src_rect. O proximo log diz exatamente ONDE a copa se perde:
    # sem remap / compactada vazia / sprite ausente / sprite vazio / z errado.
    begin
      if defined?(CustomTilemap) && !$__arvdbg
        $__arvdbg = true
        class CustomTilemap
          unless method_defined?(:__arv_orig_refresh)
            alias_method :__arv_orig_refresh, :refresh
            def refresh(*a)
              _r = __arv_orig_refresh(*a)
              begin
                # [2026-07-03] heartbeat: prova que o hook esta' VIVO (no log anterior
                # o ARVDBG nao deixou rasto nenhum -- nem o install, que e' de load-time
                # e invisivel; isto confirma no 1o refresh de qualquer mapa).
                if $__arvdbg_hb != true
                  $__arvdbg_hb = true
                  (MKXPDebug.log("[ARVDBG] vivo (1o refresh; map=#{($game_map.map_id rescue '?')})") rescue nil)
                end
                if $__arvdbg_done != true &&
                   defined?($__tsc_cache) && $__tsc_cache && $__tsc_cache[43]
                  # [2026-07-03c] O gate por $game_map.map_id falhava: com MapFactory ha'
                  # VARIAS instancias de CustomTilemap e o $game_map pode ser o vizinho.
                  # Identificacao DEFINITIVA da instancia do map43: os DADOS ja' remapeados
                  # — a celula da copa (37,42,z2) tem de conter o nid do 8082.
                  # [2026-07-03d] REAPONTADO a' ARVORE DO UTILIZADOR (a do monumento):
                  # celulas (39-44,19-23), topo=7850/7851/7852 (7850/52 VAZIOS no PNG,
                  # 7851=256px), corpo pri4 = 7857(60px esparso!)/7858(664)/7859(1024)/
                  # 7864(220)/7865(980). A antiga (36-38,42) era outra arvore do mapa.
                  _e_nid = ($__tsc_cache[43][:remap][7851] rescue nil)
                  _mine = false
                  begin
                    _mine = _e_nid && @map_data && @map_data[42, 19, 1] == _e_nid
                  rescue; end
                  _rect_ok = false
                  begin
                    _pr = @priotilesrect
                    _rect_ok = _mine && _pr && _pr[0] <= 40 && _pr[2] >= 43 && _pr[1] <= 20 && _pr[3] >= 22
                  rescue; end
                  # rasto periodico (so' na instancia certa): mostra o rect real ->
                  # o silencio deixa de ser possivel
                  if _mine
                    $__arv_rn = ($__arv_rn || 0) + 1
                    if ($__arv_rn % 240) == 1
                      (MKXPDebug.log("[ARVDBG] rasto: instancia map43 OK (data[42,19,1]=#{_e_nid}); rect=#{(@priotilesrect.inspect rescue '?')} alvo=[<=40,<=20,>=43,>=22]") rescue nil)
                    end
                  end
                  if $__arvdbg_atlas != true || _rect_ok
                    _c = $__tsc_cache[43]; _rm = _c[:remap]; _cb = _c[:bmp]
                    $__arvdbg_done = true if _rect_ok   # veredito final so' com o rect certo
                    if $__arvdbg_atlas != true
                      $__arvdbg_atlas = true   # atlas so' 1x (verdade PNG: 7850=0 7851=256 7852=0 7857=60 7858=664 7859=1024 7864=220 7865=980 px/1024)
                      [7850, 7851, 7852, 7857, 7858, 7859, 7864, 7865].each do |_o|
                        _nid = _rm[_o]
                        if _nid.nil?
                          (MKXPDebug.log("[ARVDBG] old=#{_o}: SEM REMAP (tile NAO esta' na compactada!)") rescue nil)
                          next
                        end
                        _k = _nid - 384; _cy = (_k >> 3) * 32
                        _cxb = (_cy / 1024) * 256 + (_k & 7) * 32; _cyb = _cy % 1024
                        _opq = 0; _amax = 0; _yy = 0
                        while _yy < 32
                          _xx = 0
                          while _xx < 32
                            _aa = (_cb.get_pixel(_cxb + _xx, _cyb + _yy).alpha.to_i rescue 0)
                            _opq += 1 if _aa > 0; _amax = _aa if _aa > _amax
                            _xx += 1
                          end
                          _yy += 1
                        end
                        (MKXPDebug.log("[ARVDBG] old=#{_o} nid=#{_nid} comp@(#{_cxb},#{_cyb}): opq=#{_opq}/1024 aMAX=#{_amax} (PNG real: 8081/8083=120px, 8082=960px, 805x=cheios)") rescue nil)
                      end
                    end
                    # [2026-07-03] em vez de adivinhar coords de ecra: verificar se a
                    # COPA esta' na fila de desenho (@priotilesfast tem [x,y,z,id] das
                    # celulas de prioridade a renderizar). Diz de vez: o renderer
                    # enfileira os tiles da copa (remapeados) ou nem isso?
                    if _rect_ok   # so' loga o VEREDITO (rect a cobrir a copa); interim = ruido
                      _nids = [7850, 7851, 7852, 7857, 7858, 7859, 7864, 7865].map { |_o| _rm[_o] }.compact
                      _hits = []
                      (@priotilesfast || []).each do |_p|
                        _hits.push("(#{_p[0]},#{_p[1]},z#{_p[2]})id#{_p[3]}") if _nids.include?(_p[3])
                      end
                      (MKXPDebug.log("[ARVDBG] VEREDITO copa na fila de desenho: #{_hits.size}/#{_nids.size} -> #{_hits.join(' ')} | priotilesfast total=#{(@priotilesfast || []).size} rect=#{(@priotilesrect.inspect rescue '?')}") rescue nil)
                      (MKXPDebug.log("[ARVDBG] (0/6 = refresh nao enfileira a copa -> raiz no enfileiramento; 6/6 = raiz no sprite/extracao)") rescue nil)
                      # [ARVDBG-GRELHA 2026-07-04] "meio cortada" persiste com fila
                      # 8/8 -> mapa COMPLETO da arvore, 1x: para CADA celula
                      # (38-45)x(18-24) e z0-2: id ja-remapeado, prioridade na
                      # tabela COMPACTA, se esta' na fila, e alpha real no atlas.
                      # Fecha o funil de vez: diz que celulas/camadas faltam e porque.
                      unless $__arv_grid_done
                        $__arv_grid_done = true
                        begin
                          _fastset = {}
                          (@priotilesfast || []).each { |_p| _fastset["#{_p[0]},#{_p[1]},#{_p[3]}"] = true }
                          _pd = (@priorities.data rescue nil)
                          _dd = (@map_data.data rescue nil)
                          _xs = @map_data.xsize; _ys2 = @map_data.ysize
                          (18..24).each do |_gy|
                            _row = []
                            (38..45).each do |_gx|
                              next if _gx >= _xs || _gy >= _ys2
                              _cell = []
                              (0..2).each do |_gz|
                                _id2 = (_dd ? (_dd[_gz * _xs * _ys2 + _gy * _xs + _gx] || 0) : @map_data[_gx, _gy, _gz])
                                next if _id2 == 0
                                _pr2 = (_pd ? (_pd[_id2] || 0) : (@priorities[_id2] rescue '?'))
                                _fl  = _fastset["#{_gx},#{_gy},#{_id2}"] ? "F" : "-"
                                _am2 = -1
                                if _id2 >= 384 && _cb
                                  _k2 = _id2 - 384
                                  _cy2 = (_k2 >> 3) * 32
                                  _cxb2 = (_cy2 / 1024) * 256 + (_k2 & 7) * 32
                                  _cyb2 = _cy2 % 1024
                                  [[16,16],[8,8],[24,24]].each { |_pp| _v2 = (_cb.get_pixel(_cxb2+_pp[0], _cyb2+_pp[1]).alpha.to_i rescue -1); _am2 = _v2 if _v2 > _am2 }
                                end
                                _cell << "z#{_gz}:#{_id2}p#{_pr2}#{_fl}a#{_am2}"
                              end
                              _row << "(#{_gx})#{_cell.join('|')}" unless _cell.empty?
                            end
                            (MKXPDebug.log("[ARVDBG-GRELHA] y=#{_gy}: #{_row.join(' ')}") rescue nil) unless _row.empty?
                          end
                          (MKXPDebug.log("[ARVDBG-GRELHA] legenda: id=remapeado p=prioridade F=na fila a=alphaMax atlas(3pts; -1=n/a)") rescue nil)
                        rescue => _ge
                          (MKXPDebug.log("[ARVDBG-GRELHA] erro: #{_ge.class}: #{_ge.message.to_s[0,70]}") rescue nil)
                        end
                      end
                    end
                  end
                end
              rescue => _ae
                (MKXPDebug.log("[ARVDBG] erro: #{_ae.class}: #{_ae.message.to_s[0, 80]}") rescue nil)
              end
              _r
            end
          end
        end
        (MKXPDebug.log("[ARVDBG] instalado (verdade da copa map43: compactada s/ amostragem + sprites da fila y=42, 1x qd visivel)") rescue nil)
      end
    rescue => _aie
      (MKXPDebug.log("[ARVDBG] install falhou: #{_aie.class}: #{_aie.message}") rescue nil)
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
              # ── [FIX-CASA-COL1-EXTRACT] correcao REAL da casa invisivel ──────────
              # O motor extrai o tile pri>0 para a bitmap 32x32 do sprite com a formula
              # NAO-embrulhada (src_y=(id-384)/8*32). Para a COL1 (id-384>=256) isso da'
              # y>=1024 -> FORA do tileset 1024-alto -> 32x32 VAZIA (telhados/casa
              # invisiveis). Provado pelo ATDBG: comp>0 (compactada TEM o pixel) mas
              # aMAX=0 (sprite vazio) = bug de EXTRACAO. A col1 esta' embrulhada em
              # x=256+, y=0+ na compactada; re-blt a regiao CERTA para a bitmap do
              # sprite. So' col1 (col0 ja' extrai bem). Idempotente; desliga:
              # $MKXP_COL1_EXTRACT_FIX=false.
              begin
                if $MKXP_COL1_EXTRACT_FIX != false && id.is_a?(Integer) && (id - 384) >= 256
                  _mid = ($game_map ? ($game_map.map_id rescue -1) : -1)
                  _tc  = (defined?($__tsc_cache) && $__tsc_cache) ? $__tsc_cache[_mid] : nil
                  _cb  = _tc && _tc[:bmp]
                  _spr = tiles[count]
                  if _cb && !(_cb.disposed? rescue true) && _spr && _spr.bitmap &&
                     !(_spr.bitmap.disposed? rescue true) &&
                     (_spr.bitmap.width rescue 0) == 32 && (_spr.bitmap.height rescue 0) == 32
                    _kk  = id - 384
                    _ccy = (_kk >> 3) * 32
                    _cxb = (_ccy / 1024) * 256 + (_kk & 7) * 32
                    _cyb = _ccy % 1024
                    if _cxb + 32 <= (_cb.width rescue 0) && _cyb + 32 <= (_cb.height rescue 0)
                      # [2026-07-02] CLEAR antes do blt: o blt so' escreve onde a fonte tem
                      # alpha>0 -- se a extracao ERRADA do motor deixou pixels opacos no
                      # bitmap do sprite, ficavam la' por baixo (lixo visivel em tiles
                      # esparsos como as bordas da copa). Limpar primeiro = conteudo exato.
                      (_spr.bitmap.clear rescue nil)
                      _spr.bitmap.blt(0, 0, _cb, Rect.new(_cxb, _cyb, 32, 32))
                      $__col1fix_n = ($__col1fix_n || 0) + 1
                      (MKXPDebug.log("[FIX-CASA-COL1-EXTRACT] id=#{id} col1 re-extraido da posicao embrulhada (#{_cxb},#{_cyb}) -> sprite preenchido") rescue nil) if $__col1fix_n <= 5
                    end
                  end
                end
              rescue => _cfe
                (MKXPDebug.log("[FIX-CASA-COL1-EXTRACT] erro: #{_cfe.class}: #{_cfe.message.to_s[0,80]}") rescue nil) if ($__col1fix_err = ($__col1fix_err || 0) + 1) <= 2
              end
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
    # [BMPDUP->C++] Migrado para o motor (bmp_dup nativo, bitmap_binding_3ds.cpp).
    # Testa se o dup NATIVO ja' faz copia real; se sim, NAO instala o override Ruby
    # (redundante). So' cai no fallback Ruby se o motor nao tiver a correcao (build
    # antigo) -> sem regressao com binarios antigos.
    _native_dup_ok = false
    begin
      _t = Bitmap.new(2, 2); _d = _t.dup
      _native_dup_ok = (((_d.width rescue 0) == 2) && ((_d.height rescue 0) == 2))
      (_d.dispose rescue nil); (_t.dispose rescue nil)
    rescue; _native_dup_ok = false; end
    if _native_dup_ok
      $__bmpdup_done = true
      (MKXPDebug.log("[BMPDUP] dup/clone NATIVO do motor OK (Bitmap.new(2,2).dup -> 2x2) -> override Ruby NAO instalado (migrado p/ o C++)") rescue nil)
    else
    $__bmpdup_done = true
    (MKXPDebug.log("[BMPDUP] dup nativo AUSENTE/partido -> instala fallback Ruby (build do motor sem [FIX-BMPDUP]?)") rescue nil)
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
            MKXPDebug.log("[PROBE-CONFIRM] ===== debug_probe V47 a correr (confirmado no 1o frame) =====")
            MKXPDebug.log("[PROBE-CONFIRM] >>> VERSAO=V62 (defined? removido do MAXDIAG/BAG) BAGREOPEN+ZREVERT | CEWRAP=#{!!$__cewrap_installed} " \
                          "PBANIMFIX=#{!!$__pbanimfix_done} " \
                          "TSCOMPACT_RT=#{!!$__tscompactrt_installed} " \
                          "TILETHROTTLE=#{!!$__tilethrottle_installed} " \
                          "PERF_STATIC=#{!!$__perf_static_on} " \
                          "TRANSFERFIX=#{!!$__sgpicfix && !!$__animposfix} <<<  " \
                          "(se VERSAO!=V47, o motor leu um ficheiro antigo)")
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
          __dt = ((__mkxp_ms rescue 0.0) - __t)
          $__pr_interp += __dt
          # [INTERPCULPRIT] (bug 2) o FPSPROBE provou que o 'interp' domina e dispara a
          # ~200ms. Quando UM update do interpretador e' caro (>15ms), regista o
          # evento/mapa/comando atual -> aponta o evento ou Common Event pesado p/
          # atacar a seguir. So' loga em frames lentos, limitado. $MKXP_INTERPCULPRIT=false desliga.
          # [2026-07-02] Baixado 15->5ms: o PERF-RESTO provou interp=53-82ms ACUMULADO
          # numa janela, mas o INTERPCULPRIT (>15ms/update unico) ficou VAZIO -> o custo
          # esta' espalhado por muitos updates <15ms. A 5ms apanha-os e revela QUAL
          # evento/mapa/CE os gera (o alvo real do spike). $MKXP_INTERPCULPRIT=false desliga.
          if $MKXP_INTERPCULPRIT != false && __dt > 5.0
            $__ic_n = ($__ic_n || 0) + 1
            if $__ic_n <= 40 || ($__ic_n % 30 == 0)
              _eid = (instance_variable_get(:@event_id) rescue '?')
              _mid = (instance_variable_get(:@map_id) rescue '?')
              _idx = (instance_variable_get(:@index) rescue '?')
              _lst = (instance_variable_get(:@list) rescue nil)
              _cmd = (_lst && _idx.is_a?(Integer) && _lst[_idx] ? (_lst[_idx].code rescue '?') : '?')
              (MKXPDebug.log("[INTERPCULPRIT] interp #{__dt.to_i}ms | map=#{_mid} evento=#{_eid} idx=#{_idx} code=#{_cmd} (##{$__ic_n}) <- evento/CE pesado (bug 2)") rescue nil)
            end
          end
          __r
        end
      end
    end
    (MKXPDebug.log("[PERF-RESTO] wrappers instalados (player/screen/interpreter+culprit) -- medicao + culpado do bug 2") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[PERF-RESTO] falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  SAVETIME-FIX — o jogo FECHA no arranque: "comparison of Integer with Time"
# -----------------------------------------------------------------------------
#  CRASH (log 2026-07-02 16:16): Scene_Intro -> closeTitle -> get_newest_save_slot
#  -> `save_time > newest_time` -> ArgumentError "comparison of Integer with Time".
#  RAIZ: este build de mruby NAO tem mruby-time; o motor define uma classe Time
#  Ruby (binding_3ds.cpp ~8854, Comparable, @epoch). O Time#<=> aceita Integer,
#  MAS quando `save_time` deserializa de um slot como Integer PURO (epoch), a
#  comparacao usa Integer#> (que NAO conhece Time) -> rebenta -> Scene_Intro morre
#  -> o jogo fecha. Player#last_time_saved (attr no plugin Auto Multi Save) e' a
#  fonte. Aqui normalizamos o GETTER: se o valor for numerico, devolve Time.at(v)
#  -> a comparacao passa a ser Time#> (Comparable, robusto). So' age quando o
#  valor e' Integer (senao devolve o Time intacto) -> zero mudanca no caso bom.
#  Backwards-compat com saves que guardaram epoch. SEM rebuild (hot-load).
#  $MKXP_SAVETIME_FIX=false desliga.
# =============================================================================
#  NOTA: o FIX PRINCIPAL e' no MOTOR (binding_3ds.cpp, Game.initialize ~2180): corre
#  a t~6.7s com Player ja' definido e ANTES do titulo -> apanha o crash. Este bloco
#  do probe corre a t~3.8s (Player ainda NAO existe -> guard falso -> no-op) e serve
#  so' de rede redundante caso o motor nao tenha sido recompilado. MESMO alias
#  (__mkxp_orig_lts) do motor -> nunca faz double-wrap: quem correr primeiro ganha.
begin
  if $MKXP_SAVETIME_FIX != false && defined?(Player) && Player.method_defined?(:last_time_saved) &&
     !Player.method_defined?(:__mkxp_orig_lts)
    class Player
      alias_method :__mkxp_orig_lts, :last_time_saved
      def last_time_saved
        v = __mkxp_orig_lts
        (v.is_a?(Integer) || v.is_a?(Float)) ? (Time.at(v) rescue v) : v
      end
    end
    (MKXPDebug.log("[SAVETIME-FIX] (probe) Player#last_time_saved coage epoch -> Time") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[SAVETIME-FIX] (probe) falhou: #{e.class}: #{e.message}") rescue nil)
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
#  FIX-CANCELAR — REMOVIDO (migrado para o motor): o input_3ds.cpp ja' mapeia
#  KEY_B -> RMXP_B=12 e tem trigger?/press?/repeat? nativos. O bridge Ruby
#  (B(12)->tecla 14) era so' p/ o binario antigo -> codigo morto, removido.
# =============================================================================

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
            # [FIX-BAGFREEZE] guarda a cena do menu globalmente: quando a bag e'
            # bloqueada (FIX-BAGREOPEN) o loop so' quebra se ALGUEM puser shouldExit
            # no menu. O bloqueio corre em PokemonBagScreen (sem @menu), por isso
            # precisa desta referencia para fechar o menu e parar o congelamento.
            $__active_pause_menu = @menu if @menu && (@menu.respond_to?(:shouldExit=) rescue false)
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
            # CHAVE: comparar a ORIGEM DO BUFFER (@oxLayer0/@oyLayer0), NAO a camara
            # (@ox/@oy). Os autotiles sao blittados DENTRO do @layer0, que faz scroll
            # com a camara via src_rect (fast-path) -- so' precisam de re-blit quando
            # a animacao avanca OU o buffer e' re-renderizado (cruzar borda muda
            # @oxLayer0). A camara muda a CADA frame ao andar, logo comparar @ox
            # re-blitava a camada de chao inteira (832x704) todos os frames -> 50ms.
            if @__tt_af == _af && @__tt_ox == @oxLayer0 && @__tt_oy == @oyLayer0
              return true   # animacao e buffer iguais -> nada a redesenhar
            end
            @__tt_af = _af; @__tt_ox = @oxLayer0; @__tt_oy = @oyLayer0
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
        $__bag_blocked = false   # [FIX-BAGREOPEN] reabriste o menu -> desbloqueia a bag (uso normal)
        (MKXPDebug.log("[MENUGUARD] call_menu ENTROU (menu de pausa a abrir)") rescue nil)
        # Marcar o menu como ABERTO -> o hook de Input.trigger? (MENU-CLOSE-INPUT) passa a
        # fechar em flanco do B fisico. O redef de VoltseonsPauseMenu#update NAO funciona
        # neste mruby (4 tentativas, owner=0); o Input e' intercetavel (SPRITEPROF prova).
        $__menu_open = true
        $__menu_open_frame = (Graphics.frame_count rescue 0)   # p/ a janela anti auto-Bag (MENU-CLOSE-INPUT)
        $__menu_use_released = false   # [AUTOBAG-FIX] (legado v1)
        $__autobag_ph = 0              # [AUTOBAG-FIX v3] reset do contador de fantasmas de USE por abertura
        $__mit = 0                     # reset do log MENU-INPUT por abertura (captura o USE do auto-Bag)
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
          $__menu_open = false
          $__menu_scene_obj = nil   # [MENU-SCENECAP] limpar a referencia da cena fechada
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
#  MENU-SCENECAP — capturar a instancia da CENA do menu p/ fecho direto
# -----------------------------------------------------------------------------
#  PROVADO (5 tentativas, mias + do outro agente): redefinir/prepend
#  VoltseonsPauseMenu#update (o COMPONENTE) NAO pega neste mruby (owner=0, o
#  update redefinido nunca corre) -> a leitura trigger?(Input::BACK=12) do menu
#  nunca acontece -> nao fecha, mesmo com o B detetado (trigger?(12)=true).
#  MAS o MENUCOMP-GUARD prova que envolver OUTROS metodos de plugin FUNCIONA.
#  A CENA (VoltseonsPauseMenu_Scene) tem attr_accessor :shouldExit e o loop
#  (update) verifica-o TODOS os frames (linha 235: if @shouldExit -> pbEndScene
#  -> break). Capturamos a instancia da cena aqui; o hook de trigger?
#  (MENU-CLOSE-INPUT) poe $__menu_scene_obj.shouldExit=true no flanco fisico do
#  B -> o loop fecha LIMPO (pbEndScene dispoe os sprites). ADITIVO: se o wrap nao
#  pegar, $__menu_scene_obj fica nil e o fecho-por-cena e' no-op total (os outros
#  hooks ficam exatamente como estao). [MENU-SCENECAP] no log = pegou.
# =============================================================================
begin
  if defined?(VoltseonsPauseMenu_Scene) &&
     (VoltseonsPauseMenu_Scene.method_defined?(:pbStartScene) rescue false) && !$__menu_scenecap
    $__menu_scenecap = true
    class VoltseonsPauseMenu_Scene
      alias_method :__scenecap_orig_pbStartScene, :pbStartScene
      def pbStartScene(*a, &b)
        $__menu_scene_obj = self
        $__menu_open_fc = ($__menu_fc || 0)   # [BUG5] tick de abertura: a Bag que abre
        $__menu_just_opened = true            # nos 1os ticks e' o press do Y a "sangrar"
        (MKXPDebug.log("[MENU-SCENECAP] cena capturada (#{self.class}); fecho por shouldExit no flanco do B | open_fc=#{$__menu_open_fc}") rescue nil)
        __scenecap_orig_pbStartScene(*a, &b)
      end
    end
    (MKXPDebug.log("[MENU-SCENECAP] wrap pbStartScene instalado") rescue nil)
  else
    (MKXPDebug.log("[MENU-SCENECAP] VoltseonsPauseMenu_Scene#pbStartScene ausente ou ja' instalado") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[MENU-SCENECAP] instalacao falhou: #{e.class}: #{e.message}") rescue nil)
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
                          # ── [FIX-METAFIX-TT] recuperar trainer_type corrompido ──────────
                          # $Trainer.trainer_type vem de GameData::TrainerType.get(sym).id;
                          # o Marshal do mruby corrompe o SIMBOLO -> id=0 -> tt="0" ->
                          # PlayerSprites/0 magenta (intro E roupeiro). O character_ID e'
                          # um Integer (NAO afetado pela corrupcao de simbolos) -> recupera
                          # o simbolo certo pelo mapa do METAFIX (char_ID=0 -> Sorin, etc.).
                          # GERAL: funciona p/ qualquer personagem. SO' atua se tt estiver
                          # corrompido (vazio ou puramente numerico, ex "0"); se ja' for
                          # "POKEMONTRAINER_X", e' no-op total. Validado: char_ID=0,
                          # player=Sorin, map[0]=:POKEMONTRAINER_Sorin.
                          if (tt.nil? || tt.empty? || tt == tt.to_i.to_s) && defined?($__player_tt_map)
                            _cid = ($Trainer.character_ID rescue nil)
                            _sym = ($__player_tt_map[_cid] rescue nil)
                            if _sym
                              (MKXPDebug.log("[FIX-METAFIX-TT] trainer_type corrompido (tt=#{tt.inspect} char_ID=#{_cid.inspect}) -> reposto '#{_sym}'") rescue nil) if $__pscache_n <= 8
                              tt = _sym.to_s
                            end
                          end
                          # ── FIX-PSFALLBACK ──────────────────────────────────────────
                          # Se o tt AINDA estiver invalido (intro: $Trainer ausente ->
                          # character_ID nil -> o FIX-METAFIX-TT acima nao consegue recuperar),
                          # ultimo recurso para evitar 'PlayerSprites/0' magenta: usar o tipo
                          # de jogador POR OMISSAO (get_player(0)[0], que o METAFIX garante ser
                          # um simbolo valido). A sprite 'PlayerSprites/POKEMONTRAINER_Sorin'
                          # (69x69) existe e carrega. So' atua com tt invalido (vazio/numerico);
                          # se ja' for um tipo real, e' no-op total. Flag: $__pscache_fallback.
                          if $__pscache_fallback != false && (tt.nil? || tt.empty? || tt == tt.to_i.to_s)
                            # [FIX-PSFALLBACK] honra a ESCOLHA: usa o id escolhido
                            # ($__picked_player_id, guardado pelo FIX-CHARPICK) em vez do
                            # default Sorin. So' cai no 0 se nao houve escolha. E' isto que
                            # poe o personagem/roupeiro CERTO mesmo com o $Trainer nil na intro.
                            _pick = ($__picked_player_id rescue nil)
                            _src  = (_pick.is_a?(Integer) && _pick >= 0) ? _pick : 0
                            _dflt = (GameData::Metadata.get_player(_src)[0].to_s rescue nil)
                            if _dflt && !_dflt.empty? && _dflt != _dflt.to_i.to_s
                              (MKXPDebug.log("[FIX-PSFALLBACK] tt invalido (#{tt.inspect}) -> id escolhido=#{_pick.inspect} (src=#{_src}) -> '#{_dflt}'") rescue nil) if $__pscache_n <= 8
                              # pbResolveBitmap devolve nil mesmo para ficheiros que EXISTEM neste
                              # motor -> nao da' para confiar nele aqui. __ps_orig_lb e' o load REAL.
                              _b = (__ps_orig_lb("", "Graphics/Pictures/PlayerSprites/#{_dflt}", hue) rescue nil)
                              return _b if _b
                              tt = _dflt
                            end
                          end
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
#  MENU-CLOSE-INPUT — fecho do menu de pausa via Input.trigger? (abordagem nova)
# -----------------------------------------------------------------------------
#  PROVADO em 4 logs: o redef de VoltseonsPauseMenu#update (prepend V35, alias+redef
#  V36, aplicador re-chamavel V37) NUNCA interceta @pauseMenu.update -- o update
#  ORIGINAL corre sempre, o [MENU-CLOSE] premidas nunca aparece, e instance_method
#  (:update).owner devolve 0 (stub do mruby do mkxp-3ds). A classe nao e' intercetavel
#  neste mruby. MAS o modulo Input E' (mesma tecnica do SPRITEPROF, que funciona).
#  O update ORIGINAL fecha em `trigger?(BACK) || trigger?(ACTION)`. Logo, quando o menu
#  esta' aberto ($__menu_open, posto pelo MENUGUARD), faco trigger?(BACK)/trigger?(ACTION)
#  disparar tambem num FLANCO de press? dos codigos do B fisico (11/12/14/15/16) -> o
#  update original fecha mesmo que o B nao mapeie exatamente para BACK ou que o latch
#  esteja drenado. press? le s_pressed (nao consumido). USE(13)/navegacao ficam intactos.
#  O nucleo (trigger? original) corre SEMPRE fora do rescue -> nunca parte o input.
#  Loga os valores reais dos aliases (revela o mapeamento de uma vez). Off: $MKXP_MENUCLOSE_INPUT=false.
# =============================================================================
$MKXP_MENUCLOSE_INPUT = true if $MKXP_MENUCLOSE_INPUT.nil?
# Instalador em PROC (chamado no LOAD e no per_frame). No LOAD o Input ainda nao esta'
# pronto (ha um plugin a mexer nele, ver [FIX-INPUT]); o per_frame instala assim que estiver.
# Usa const_defined? (defined? rebenta dentro de proc no mruby do mkxp-3ds).
$__install_mci = proc do
  next if $__mcf_input_hooked
  next if $MKXP_MENUCLOSE_INPUT == false
  next unless Object.const_defined?(:Input)
  # CAUSA do menu nunca fechar no B (analise): `Input.respond_to?(:trigger?)`
  # devolve FALSE neste mruby mesmo com o metodo chamavel (module_function/
  # singleton) -> este `next` abortava SEMPRE e o hook NUNCA instalava (zero
  # linhas [MENU-CLOSE-INPUT] no log). Verificar diretamente no singleton_class,
  # que e' fiavel; se ainda nao existir, retenta no frame seguinte.
  next unless ((Input.singleton_class.method_defined?(:trigger?) rescue false) ||
               (Input.singleton_class.private_method_defined?(:trigger?) rescue false) ||
               (Input.respond_to?(:trigger?) rescue false))
  # [MENU-FIX] A flag $__mcf_input_hooked NAO e' posta aqui (era posta no topo, antes
  # do install). Causa-raiz do menu nunca fechar: o install no LOAD (t=3.8s) punha a
  # flag, e 107ms depois o FIX-INPUT3 redefinia Input.trigger? e APAGAVA o wrapper ->
  # a flag bloqueava a reinstalacao no per_frame p/ sempre. Agora so' se marca a flag
  # DEPOIS de o alias_method ter sucesso (ver fim do bloco), e so' no per_frame.
  _bk = (Input::BACK rescue '?'); _ac = (Input::ACTION rescue '?'); _us = (Input::USE rescue '?')
  _b = (Input::B rescue '?'); _c = (Input::C rescue '?'); _x = (Input::X rescue '?'); _y = (Input::Y rescue '?')
  (MKXPDebug.log("[MENU-CLOSE-INPUT] aliases reais: BACK=#{_bk} ACTION=#{_ac} USE=#{_us} | B=#{_b} C=#{_c} X=#{_x} Y=#{_y}") rescue nil)
  Input.singleton_class.class_eval do
    unless method_defined?(:__mcf_orig_trigger)
      alias_method :__mcf_orig_trigger, :trigger?
      define_method(:trigger?) do |code|
        _r = __mcf_orig_trigger(code)           # NUCLEO: corre sempre, fora do rescue
        begin
          if $MKXP_MENUCLOSE_INPUT != false && $__menu_open
            _usec = (Input::USE rescue 13)
            _bkc  = (Input::BACK rescue 12); _acc = (Input::ACTION rescue 11)
            _fc   = (Graphics.frame_count rescue 0)
            # [AUTOBAG-FIX v3] a v2 usava janela por Graphics.frame_count, MAS esse contador e'
            # instavel no 3DS (open_fc=128 a t=386s) -> a janela falhava e o fantasma passava.
            # O teste FANTASMA nao precisa de janela: um USE REAL tem SEMPRE press?=true no frame
            # do trigger (o botao esta' fisicamente em baixo); o fantasma do latch stale tem
            # trigger?=true mas press?=false. Suprimimos SO' fantasmas de USE enquanto o menu
            # esta' aberto (cap 8/abertura, reset no MENUGUARD, p/ nunca trancar o USE de vez).
            # USE real (press?=true) passa sempre -> selecionas a Bag de propósito. Mata a
            # auto-Bag -> o menu fecha com B (bag_active deixa de bloquear). $MKXP_AUTOBAG_FIX=false reverte.
            # [AUTOBAG-FIX v4] a v3 (so' fantasma !press) NAO apanhou: no 3DS o botao que ABRE
            # o menu (A) e' o MESMO que CONFIRMA (USE) -> a auto-Bag e' um USE REAL (press?=true),
            # nao um fantasma. Entao suprimimos os PRIMEIROS 5 trigger?(USE) apos abrir o menu,
            # seja real ou fantasma (cap+reset no MENUGUARD). O B (BACK=12) fecha na mesma. Apos
            # os 5, o USE volta a funcionar p/ selecionares. $MKXP_AUTOBAG_FIX=false reverte.
            _pr_use = (Input.press?(_usec) rescue false)
            # [AUTOBAG-FIX v5 2026-07-02] DEFAULT OFF (== true p/ ligar). PROVADO no log
            # 09:2x: a auto-Bag JA' NAO ocorre neste build (0x 'mochila ABERTA' espontanea;
            # menu fecha com B), mas a supressao v4 estava a COMER os 5 primeiros USE
            # REAIS do utilizador (f=6151/6168: press=1 suprimido x5 -> menu ignorava a
            # selecao). O band-aid tornou-se o proprio bug -> desligado por omissao.
            # O [BAGTRACE] do motor continua armado caso a auto-Bag volte.
            if _r && code == _usec && $MKXP_AUTOBAG_FIX == true && (($__autobag_ph ||= 0) < 5)
              _r = false
              $__autobag_ph += 1
              (MKXPDebug.log("[AUTOBAG-FIX v4] USE suprimido apos abrir (##{$__autobag_ph}, press=#{_pr_use}) -- botao abrir=confirmar no 3DS auto-seleciona a Bag; anti auto-Bag") rescue nil)
            elsif code == _bkc
              # [MENU-FIX2] FECHAR: SO' force trigger?(BACK=12). NUNCA o ACTION(11) -- esse
              # e' o Y que ABRE o menu e que o Scene_Map le via pbUpdateSceneMap; antes
              # ROUBAVA o flanco (o log dizia "premidas=[12] -> trigger?(11) forcado") e o
              # menu (que le trigger?(12)) ficava a false -> nao fechava. Agora: LATCH (nao
              # one-shot consumido pelo 1o chamador) -- deteto o flanco fisico de B uma vez
              # por frame e mantenho o latch ate' o trigger?(12) do MENU o consumir.
              if $__mcf_bedge_frame != _fc
                $__mcf_bedge_frame = _fc
                $__mcf_bheld ||= {}
                _pr12 = (Input.press?(12) rescue false)
                if _pr12 && !$__mcf_bheld[12]
                  $__mcf_close_latch = true
                  # [MENU-SCENECAP] fecho da cena no flanco do B. (1) shouldExit (o loop
                  # da cena verifica todos os frames). (2) FECHO FORTE: chamar pbEndScene
                  # DIRETO na cena capturada -> poe @hasTerminated=true e o proprio loop da
                  # cena sai na linha 'return if @hasTerminated' (caminho que o codigo ja'
                  # tem). So' quando a Bag NAO esta' aberta ($__bag_active=false), senao
                  # fechariamos o menu POR BAIXO da Bag aninhada. Flag $MKXP_MENUCLOSE_HARD.
                  (($__menu_scene_obj.shouldExit = true) if $__menu_scene_obj) rescue nil
                  # [FIX-MENUFORCE] (bug 6) a auto-Bag reabre ~30 ticks apos fechar e
                  # prende $__bag_active=true -> o hard-close (que exige !bag_active) e'
                  # saltado e o menu AMARELO nao fecha. Duas redes, cautelosas:
                  #  (1) suprime a reabertura da Bag por 60 ticks apos QUALQUER B no menu.
                  #  (2) se premires B >=2x com a Bag ainda presa, FORCA o fecho (limpa
                  #      bag_active + pbEndScene). So' dispara a' 2a tentativa -> nunca
                  #      fecha por baixo de uma Bag que abriste de proposito. Toggle
                  #      $MKXP_MENUFORCE=false reverte ambas.
                  if $MKXP_MENUFORCE != false
                    $__bag_suppress_until = (($__menu_fc || 0) + 60)
                    $__bag_blocked = true   # bloqueia a auto-reabertura no pbStartScreen ate' reabrires o menu
                  end
                  if $MKXP_MENUCLOSE_HARD != false && $__menu_scene_obj && !$__bag_active
                    begin
                      _term = ($__menu_scene_obj.instance_variable_get(:@hasTerminated) rescue false)
                      if !_term && ($__menu_scene_obj.respond_to?(:pbEndScene) rescue false)
                        $__menu_scene_obj.pbEndScene
                        (MKXPDebug.log("[MENU-CLOSE-HARD] pbEndScene DIRETO na cena (B, bag fechada) -> menu deve fechar agora") rescue nil)
                      end
                    rescue => _mche
                      (MKXPDebug.log("[MENU-CLOSE-HARD] falhou: #{_mche.class}: #{_mche.message}") rescue nil)
                    end
                    $__menu_b_stuck = 0
                  elsif $MKXP_MENUFORCE != false && $__bag_active && $__menu_scene_obj
                    $__menu_b_stuck = ($__menu_b_stuck || 0) + 1
                    if $__menu_b_stuck >= 2
                      begin
                        $__bag_active = false   # destranca a flag presa pela auto-Bag
                        _term = ($__menu_scene_obj.instance_variable_get(:@hasTerminated) rescue false)
                        if !_term && ($__menu_scene_obj.respond_to?(:pbEndScene) rescue false)
                          $__menu_scene_obj.pbEndScene
                        end
                        (MKXPDebug.log("[FIX-MENUFORCE] B #{$__menu_b_stuck}x com bag_active preso -> FORCADO: bag_active=false + pbEndScene (menu amarelo destranca)") rescue nil)
                        $__menu_b_stuck = 0
                      rescue => _mfe
                        (MKXPDebug.log("[FIX-MENUFORCE] falhou: #{_mfe.class}: #{_mfe.message}") rescue nil)
                      end
                    else
                      (MKXPDebug.log("[FIX-MENUFORCE] B com bag_active=true (tentativa #{$__menu_b_stuck}/2) -- preme B outra vez p/ forcar o fecho") rescue nil)
                    end
                  end
                  if (($__mcf_iclog ||= 0) < 10)
                    $__mcf_iclog += 1
                    (MKXPDebug.log("[MENU-CLOSE-INPUT] flanco B(12) -> shouldExit + hard (bag_active=#{!!$__bag_active})") rescue nil)
                  end
                end
                $__mcf_bheld[12] = _pr12
              end
              if $__mcf_close_latch
                _r = true
                $__mcf_close_latch = false   # consome no trigger?(12) do menu -> fecha
              end
            end
          end
        rescue; end
        # [MENU-INPUT] AQUI (dentro do hook trigger?) — corre no menu bloqueante, ao
        # contrario do Scene_Map#update. Mostra o que o menu LE: code + resultado FINAL.
        begin
          if $__menu_open && (($__mit ||= 0) < 20) && (code == 11 || code == 12 || code == 13)
            $__mit += 1
            (MKXPDebug.log("[MENU-INPUT] trigger?(#{code})=#{_r} latch=#{$__mcf_close_latch ? 1 : 0} prs12=#{(Input.press?(12) rescue '?')} prsL4=#{(Input.press?(4) rescue '?')} prsR6=#{(Input.press?(6) rescue '?')}") rescue nil)
          end
        rescue; end
        _r
      end
      (MKXPDebug.log("[MENU-CLOSE-INPUT] hook instalado (B fecha; USE suprimido nos 1os 10f apos abrir = anti auto-Bag; navegacao intacta)") rescue nil)
      $__mcf_input_hooked = true   # [MENU-FIX] flag SO' apos o alias ter sucesso
    end
  end
end
# [MENU-FIX] NAO chamar no LOAD: o FIX-INPUT3 (binding_3ds.cpp ~2706) redefine
# Input.trigger? a t~3.9s e apagaria este wrapper. O per_frame (Scene_Map#update via
# $__install_mci, t>=17s) instala DEPOIS do FIX-INPUT3, sobre o trigger? FINAL ->
# o wrapper sobrevive. A chamada no LOAD foi removida de proposito.

# =============================================================================
#  BAGCLOSE  —  isolar porque o B NAO fecha a mochila (PokemonBag_Scene)
# -----------------------------------------------------------------------------
#  Utilizador (queixa recorrente): "a mochila nao fecha no B".
#  A v19.1 fecha a bag com Input.trigger?(Input::BACK) dentro do loop de
#  pbChooseItem, que chama PokemonBag_Scene#pbUpdate TODOS os frames (linha 134).
#  Hook 100% logging-only (alias + chamada ao original; nao muda nada):
#    - pbStartScene -> "[BAGCLOSE] mochila ABERTA" + ativa o rastreio
#    - pbEndScene   -> "[BAGCLOSE] mochila FECHADA apos N frames | B-visto=...".
#                      Se aparece um ABERTA e NUNCA o FECHADA -> a bag nao fecha.
#    - pbUpdate     -> enquanto aberta, regista o FLANCO do B.
#         * B dispara (BACK=true) mas a bag nao fecha -> logica/loop de fecho.
#         * B NUNCA dispara                            -> mapeamento de input.
#  Flag $MKXP_BAGCLOSE_LOG (ON por defeito). SEM FALHAS: pior caso = no-op.
# =============================================================================
begin
  if defined?(PokemonBag_Scene) && !$__bagclose_installed
    $__bagclose_installed = true
    class PokemonBag_Scene
      if method_defined?(:pbStartScene)
        alias_method :__bc_orig_start, :pbStartScene
        def pbStartScene(*a)
          $__bag_active = true; $__bag_fr = 0; $__bag_bseen = false
          (MKXPDebug.log("[BAGCLOSE] mochila ABERTA (pbStartScene)") rescue nil) if $MKXP_BAGCLOSE_LOG != false
          __bc_orig_start(*a)
        end
      end
      if method_defined?(:pbEndScene)
        alias_method :__bc_orig_end, :pbEndScene
        def pbEndScene(*a)
          (MKXPDebug.log("[BAGCLOSE] mochila FECHADA (pbEndScene) apos #{$__bag_fr} frames | B-visto-no-loop=#{$__bag_bseen}") rescue nil) if $MKXP_BAGCLOSE_LOG != false
          $__bag_active = false
          $__bag_closed_frame = (Graphics.frame_count rescue 0)  # [BAGGUARD] p/ debounce da reabertura
          __bc_orig_end(*a)
        end
      end
      if method_defined?(:pbUpdate)
        alias_method :__bc_orig_upd, :pbUpdate
        def pbUpdate(*a)
          if $__bag_active && $MKXP_BAGCLOSE_LOG != false
            begin
              $__bag_fr = ($__bag_fr || 0) + 1
              b = (Input.trigger?(Input::BACK) rescue false)
              if b
                $__bag_bseen = true
                (MKXPDebug.log("[BAGCLOSE] frame=#{$__bag_fr}: B(BACK) DISPAROU dentro da bag -> devia fechar agora") rescue nil)
              end
            rescue; end
          end
          __bc_orig_upd(*a)
        end
      end
    end
    (MKXPDebug.log("[BAGCLOSE] instalado (hook PokemonBag_Scene; flag $MKXP_BAGCLOSE_LOG)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[BAGCLOSE] install falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  [FIX-BAGREOPEN] BLOQUEIO no pbStartScreen (catch-all). A bag reabre via
#  PokemonBagScreen.new(...).pbStartScreen ~a cada 120 frames mesmo apos o menu
#  fechar (o Voltseon re-dispara a entrada Bag). Bloquear no MenuEntryBag#selected
#  nao chega (o gatilho varia). Aqui, se a bag estiver BLOQUEADA ($__bag_blocked,
#  posto pelo FIX-MENUFORCE ao fechar o menu e limpo ao reabrir o menu), o
#  pbStartScreen SAI logo -> a bag nao abre. Entrada limpa: nao ha' meia-init.
#  $MKXP_MENUFORCE=false desliga.
# =============================================================================
begin
  if (Object.const_defined?(:PokemonBagScreen) rescue false) && !$__bagblock_installed
    $__bagblock_installed = true
    PokemonBagScreen.class_eval do
      if method_defined?(:pbStartScreen) && !method_defined?(:__bagblock_orig_start)
        alias_method :__bagblock_orig_start, :pbStartScreen
        def pbStartScreen(*a, &b)
          if $MKXP_MENUFORCE != false && $__bag_blocked
            $__bagblock_n = ($__bagblock_n || 0) + 1
            # [FIX-BAGFREEZE] quebrar o loop: devolver nil bloqueia a bag mas o menu
            # volta a re-disparar pbStartScreen -> loop -> congela. Forcar shouldExit
            # no menu capturado fa-lo SAIR do loop e fechar de vez.
            begin
              _pm = $__active_pause_menu
              if _pm && (_pm.respond_to?(:shouldExit=) rescue false)
                _pm.shouldExit = true
                (MKXPDebug.log("[FIX-BAGFREEZE] menu forcado a fechar (shouldExit=true) p/ quebrar o loop de auto-reabertura") rescue nil) if $__bagblock_n <= 3
              end
            rescue; end
            (MKXPDebug.log("[FIX-BAGREOPEN] pbStartScreen BLOQUEADO (auto-reabertura ##{$__bagblock_n}; bag bloqueada ate' reabrires o menu) -- menu amarelo fica fechado") rescue nil) if ($__bagblock_n <= 6) || ($__bagblock_n % 60 == 0)
            $__bag_active = false
            return nil
          end
          __bagblock_orig_start(*a, &b)
        end
      end
    end
    (MKXPDebug.log("[FIX-BAGREOPEN] bloqueio de pbStartScreen instalado em PokemonBagScreen") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[FIX-BAGREOPEN] bloqueio pbStartScreen falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  FIX-BAGREOPEN — a mochila reabre sozinha ao tentar fechar (quarto/menu pausa)
# -----------------------------------------------------------------------------
#  RAIZ (provada no log + fonte do plugin): o loop da CENA do menu de pausa
#  (VoltseonsPauseMenu_Scene#update, 002_VoltseonMenu_Main.rb) atualiza os
#  componentes DUAS vezes na MESMA iteracao -- uma dentro de @pauseMenu.update{...}
#  e outra logo a seguir em @components.each{ c.update }. A bag abre BLOQUEANTE
#  (MenuEntryBag#selected -> pbFadeOutIn(99999){ PokemonBagScreen...pbStartScreen }).
#  Quando o B fecha a bag, o SEGUNDO @components.each da MESMA iteracao chama outra
#  vez MenuEntryBag#selected -> a bag REABRE. Ciclo bag-abre/B-fecha/bag-reabre; o
#  utilizador nunca sai ("a mochila nao fecha"). O delta de Graphics.frame_count
#  NAO serve (a) e' 0 -> 0 na MESMA iteracao e (b) frame_count vem 0 no 3DS.
#  CORRECAO (so' no probe; nao mexe no motor nem no loop do jogo): contador PROPRIO
#  $__menu_fc, incrementado 1x por iteracao do loop do menu via alias do GLOBAL
#  pbUpdateSpriteHash (chamado no topo de cada iteracao). pbEndScene da bag grava
#  $__bag_close_menu_fc = $__menu_fc no instante do fecho. A reabertura espuria
#  acontece na MESMA iteracao (sem novo pbUpdateSpriteHash entre o fecho e o 2o
#  @components.each) -> $__menu_fc == $__bag_close_menu_fc -> selected devolve nil
#  e quebra o ciclo. A iteracao SEGUINTE ja' correu pbUpdateSpriteHash (tick+1),
#  por isso uma reabertura LEGITIMA (re-selecionar a bag) passa. Auto-limitado,
#  sem estado residual. Reversivel: $MKXP_BAGREOPEN_FIX=false.
#  Cada hook tem o SEU begin/rescue (um falhar nao aborta os outros). Os aliases
#  sao instalados em REQUIRE (funciona; ver OUTFITZ/BAGCLOSE), mas os LOGS uteis
#  ([tick ATIVO], [bag fechou no tick], [reabertura ESPURIA bloqueada]) correm em
#  RUNTIME -> aparecem mesmo no log (o sink ja' esta' ativo).
# =============================================================================
begin
  # (1) tick por iteracao do menu via pbUpdateSpriteHash (global do Essentials)
  begin
    if (respond_to?(:pbUpdateSpriteHash, true)) && !$__menu_tick_installed
      $__menu_tick_installed = true
      Object.class_eval do
        alias_method :__menutick_orig_pbUpdateSpriteHash, :pbUpdateSpriteHash
        def pbUpdateSpriteHash(*a, &b)
          $__menu_fc = ($__menu_fc || 0) + 1
          unless $__menu_fc_logged
            $__menu_fc_logged = true
            (MKXPDebug.log("[FIX-BAGREOPEN] tick $__menu_fc ATIVO (1a chamada pbUpdateSpriteHash) -- debounce por iteracao pronto") rescue nil)
          end
          __menutick_orig_pbUpdateSpriteHash(*a, &b)
        end
      end
    end
  rescue => e1
    (MKXPDebug.log("[FIX-BAGREOPEN] tick install falhou: #{e1.class}: #{e1.message}") rescue nil)
  end

  # (2) gravar o tick em que a bag FECHA (PokemonBag_Scene#pbEndScene).
  #     Alias proprio (__bagreopen_orig_end), distinto do BAGCLOSE -> encadeiam.
  begin
    _bag = (PokemonBag_Scene rescue nil)
    if _bag && (_bag.method_defined?(:pbEndScene) rescue false) &&
       !(_bag.method_defined?(:__bagreopen_orig_end) rescue true)
      _bag.class_eval do
        alias_method :__bagreopen_orig_end, :pbEndScene
        def pbEndScene(*a, &b)
          $__bag_close_menu_fc = ($__menu_fc || 0)
          (MKXPDebug.log("[FIX-BAGREOPEN] bag fechou no tick #{$__bag_close_menu_fc} (qualquer selected na MESMA iteracao = espurio)") rescue nil)
          __bagreopen_orig_end(*a, &b)
        end
      end
    end
  rescue => e2
    (MKXPDebug.log("[FIX-BAGREOPEN] pbEndScene hook falhou: #{e2.class}: #{e2.message}") rescue nil)
  end

  # (3) bloquear a reabertura ESPURIA na MESMA iteracao do menu.
  begin
    _meb = (MenuEntryBag rescue nil)
    if _meb && (_meb.method_defined?(:selected) rescue false) &&
       !(_meb.method_defined?(:__bagreopen_orig_selected) rescue true)
      _meb.class_eval do
        alias_method :__bagreopen_orig_selected, :selected
        def selected(menu)
          # [BUG5] Bag AUTOMATICA na abertura do menu: o press do Y que abriu o menu
          # "sangra" para dentro e ativa logo a entrada Bag. Bloqueia o selected nos
          # primeiros ticks apos abrir (ai e' sempre espurio; uma escolha deliberada
          # acontece bem depois, ja' com varios ticks de navegacao). Flag partilhada
          # $MKXP_BAGREOPEN_FIX.
          if $MKXP_BAGREOPEN_FIX != false && $__menu_open_fc &&
             (($__menu_fc || 0) - $__menu_open_fc) <= 3
            (MKXPDebug.log("[FIX-BAGREOPEN] Bag AUTOMATICA bloqueada (abriu ha' #{($__menu_fc||0)-$__menu_open_fc} ticks; press do Y a sangrar) -- navega e abre quando quiseres") rescue nil)
            return nil
          end
          # [FIX-BAGREOPEN-V2] (bug 6) ALARGADA: a auto-Bag espuria nao reabre no
          # MESMO tick do fecho, reabre POUCOS ticks depois (log: bag abre 2x; a 2a
          # nunca fecha e o B nao dispara la' dentro -> menu amarelo preso). Bloqueia
          # qualquer selected ate' 12 ticks apos um fecho. A reabertura DELIBERADA
          # (premes B p/ sair, navegas, reabres) acontece bem depois -> nao afetada.
          _since_close = $__bag_close_menu_fc ? (($__menu_fc || 0) - $__bag_close_menu_fc) : 9999
          # [FIX-MENUFORCE] janela de supressao posta pelo B no menu (apanha a reabertura
          # ~30 ticks depois, fora da janela curta do _since_close).
          if $MKXP_MENUFORCE != false && $__bag_suppress_until && ($__menu_fc || 0) <= $__bag_suppress_until
            (MKXPDebug.log("[FIX-BAGREOPEN] reabertura suprimida (janela do B no menu, ate' tick #{$__bag_suppress_until}) -- auto-Bag bloqueada p/ o menu poder fechar") rescue nil)
            return nil
          end
          if $MKXP_BAGREOPEN_FIX != false && $__bag_close_menu_fc && _since_close >= 0 && _since_close <= 12
            (MKXPDebug.log("[FIX-BAGREOPEN] reabertura ESPURIA bloqueada (#{_since_close} ticks apos fechar no tick #{$__bag_close_menu_fc}) -- auto-Bag suprimida; preme B p/ sair do menu") rescue nil)
            return nil
          end
          __bagreopen_orig_selected(menu)
        end
      end
      (MKXPDebug.log("[FIX-BAGREOPEN] debounce instalado em MenuEntryBag#selected (flag $MKXP_BAGREOPEN_FIX)") rescue nil)
    end
  rescue => e3
    (MKXPDebug.log("[FIX-BAGREOPEN] selected hook falhou: #{e3.class}: #{e3.message}") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[FIX-BAGREOPEN] install falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  MAXDIAG — BUILD DE DIAGNOSTICO TOTAL (a pedido: "registar TUDO, performance
#  horrivel OK"). Liga o MAXIMO de registo para cravar tudo num so' log:
#   - [MAXDIAG-SPR] dump de TODOS os sprites vivos (ObjectSpace -- CONFIRMADO no
#     3DS, e' o que o OUTFITZ usa): classe, z, viewport.z, visivel, opacity,
#     xy/ox/oy, dimensoes da bitmap, OBJECT_ID da bitmap e ASSINATURA DE PIXEIS.
#     Para o "sempre o mesmo sprite":
#       * mesmo bmp_id em 2 sprites = partilham LITERALMENTE a bitmap (raiz Ruby
#         ou o motor a devolver o mesmo objeto);
#       * bmp_id diferente mas assinatura igual = mesmo CONTEUDO (cache/textura);
#       * assinaturas diferentes = e' a SELECAO que nao muda o sprite certo.
#   - [MAXDIAG-PIC] todas as Game_Picture#show (numero + args).
#   - [MAXDIAG-SEL] estado da selecao ($Trainer trainer_type/outfit/name) no tick.
#  CHAVE: o ecra de selecao de personagem / roupeiro (GenderPickSelection::Show)
#  NAO corre Scene_Map -> o dump tem de ser conduzido a partir do #update_scene
#  DESSE ecra. Por isso ha' 3 condutores que partilham o registo:
#    (A) Scene_Map#update    -> overworld (e anuncia, no 1o frame, que esta ATIVO)
#    (B) GenderPickSelection::Show#update_scene -> selecao/roupeiro (faz 1 dump
#        IMEDIATO + dumps a cada $MKXP_MAXDIAG_EVERY_GP). Instalado em REQUIRE p/
#        apanhar TAMBEM o ecra de selecao do INTRO (antes de haver Scene_Map).
#    (D) Graphics.update     -> BONUS; singleton C++, pode nao propagar no 3DS,
#        por isso vai ISOLADO no seu rescue -- falhar aqui nao afeta (A)/(B).
#  Contador unico $__md_fc (NAO Graphics.frame_count, que vem 0 no 3DS); com dois
#  condutores a fazer "+=1; dump se %EVERY==0" sai no MAXIMO 1 dump por frame.
#  Flags: $MKXP_MAXDIAG (ON), $MKXP_MAXDIAG_EVERY (default 90), $MKXP_MAXDIAG_EVERY_GP
#  (default 20), $MKXP_MAXDIAG_PIXELS=false p/ saltar get_pixel (assinatura cai p/
#  WxH#object_id, ainda deteta bitmaps PARTILHADAS). Cada hook no SEU begin/rescue.
#  Tudo em rescue: pior caso = no-op. NAO afeta a logica do jogo.
# =============================================================================
begin
  $MKXP_MAXDIAG = false if $MKXP_MAXDIAG.nil?    # [FPS] OFF por defeito: o dump por-frame travava o jogo. $MKXP_MAXDIAG=true reativa p/ diagnostico.
  $MKXP_INTERPCULPRIT = false if $MKXP_INTERPCULPRIT.nil?   # [FPS] OFF: ja' temos o culpado (evento 1, mapa 1). =true p/ reativar.
  # PIXELS OFF por defeito: agora temos cname=<ficheiro> (identidade exata do sprite),
  # logo o get_pixel (caro no 3DS, culpado de grande parte do 1 FPS no ecra de selecao)
  # e' redundante. Liga com $MKXP_MAXDIAG_PIXELS=true se precisares da assinatura de cor.
  $MKXP_MAXDIAG_PIXELS = false if $MKXP_MAXDIAG_PIXELS.nil?

  # Definicao do modulo em REQUIRE (so' metodos; sem log; inofensivo).
  module MAXDIAG
    # assinatura de uma bitmap: "WxH#objid|p1,p2,p3" (pixeis RGBA hex: cantos+centro)
    def self.sig(bmp)
      return 'nil' if bmp.nil?
      return 'disp' if (bmp.disposed? rescue false)
      w = (bmp.width rescue 0).to_i; h = (bmp.height rescue 0).to_i
      oid = (bmp.object_id rescue (bmp.__id__ rescue '?'))
      return "#{w}x#{h}##{oid}|-" if w <= 0 || h <= 0
      # amostragem de pixeis: forte sinal de conteudo, mas get_pixel pode ser
      # caro/inexistente no motor -> desligavel com $MKXP_MAXDIAG_PIXELS=false
      # (fica so' WxH#object_id, que ja' deteta bitmaps PARTILHADAS pelo id).
      return "#{w}x#{h}##{oid}" if $MKXP_MAXDIAG_PIXELS == false
      pts = [[0,0],[w/2,h/2],[w-1,h-1]]
      px = pts.map { |x,y|
        c = (bmp.get_pixel(x,y) rescue nil)
        c ? (format("%02X%02X%02X%02X",(c.red rescue 0).to_i & 255,(c.green rescue 0).to_i & 255,(c.blue rescue 0).to_i & 255,(c.alpha rescue 0).to_i & 255) rescue 'e') : 'x'
      }.join(',')
      "#{w}x#{h}##{oid}|#{px}"
    end

    def self.dump_sprites(tag, force = false)
      return if $MKXP_MAXDIAG == false && !force
      i = 0
      begin
        ObjectSpace.each_object(Sprite) do |s|
          begin
            next if (s.disposed? rescue true)
            vp = (s.viewport rescue nil)
            vz = vp ? (vp.z rescue '?').to_s : 'noVP'
            # IDENTIDADE REAL do sprite de personagem: o FICHEIRO do grafico (vale
            # muito mais que a assinatura de 3 pixeis, que amostra zonas transparentes).
            # @character_name = o que o sprite mostra agora; @character.character_name
            # = valor VIVO do Game_Character (no player ja' com o outfit aplicado).
            _ch = (s.instance_variable_get(:@character) rescue nil)
            _cn = (s.instance_variable_get(:@character_name) rescue nil)
            _clive = (_ch.character_name rescue nil)
            _isp = ((_ch && $game_player && _ch.equal?($game_player)) rescue false)
            _cinfo = ""
            if _cn || _clive
              _cinfo = " cname=#{(_cn.inspect rescue '?')}"
              _cinfo += " live=#{(_clive.inspect rescue '?')}" if _clive && _clive != _cn
            end
            _cinfo += " <<PLAYER" if _isp
            (MKXPDebug.log("[MAXDIAG-SPR] ##{i} #{s.class} z=#{(s.z rescue '?')} vpz=#{vz} vis=#{(s.visible rescue '?')} op=#{(s.opacity rescue '?')} xy=(#{(s.x rescue '?')},#{(s.y rescue '?')}) o=(#{(s.ox rescue '?')},#{(s.oy rescue '?')}) bmp=#{sig((s.bitmap rescue nil))}#{_cinfo}") rescue nil)
            i += 1
          rescue; end
        end
      rescue => e
        (MKXPDebug.log("[MAXDIAG-SPR] each_object(Sprite) falhou: #{e.class}: #{e.message}") rescue nil)
      end
      # estado da selecao de personagem, no mesmo tick
      begin
        if $Trainer
          (MKXPDebug.log("[MAXDIAG-SEL] trainer_type=#{($Trainer.trainer_type.inspect rescue '?')} outfit=#{($Trainer.outfit rescue '?')} name=#{($Trainer.name.inspect rescue '?')}") rescue nil)
        end
      rescue; end
      (MKXPDebug.log("[MAXDIAG-SPR] === fim #{tag}: #{i} sprites vivos ===") rescue nil)
    end

    # tick partilhado pelos condutores de overworld/Graphics: $__md_fc + modulo.
    def self.tick(driver)
      # [ROUPDIAG] dumps Ruby (cname/bmp) enquanto o roupeiro (CE#173) esta' aberto,
      # INDEPENDENTE de $MKXP_MAXDIAG (que esta' off por FPS). Bounded por contador.
      begin
        if ($__roup_dump_left || 0) > 0
          $__roup_dump_left -= 1
          $__md_fc = ($__md_fc || 0) + 1
          dump_sprites("ROUPEIRO #{driver} f=#{$__md_fc}", true)
        end
      rescue; end
      return if $MKXP_MAXDIAG == false
      $__md_fc = ($__md_fc || 0) + 1
      _ev = ($MKXP_MAXDIAG_EVERY || 90)
      dump_sprites("#{driver} f=#{$__md_fc}") if _ev > 0 && ($__md_fc % _ev) == 0
    rescue; end
  end

  # (A) Scene_Map#update -> condutor de overworld + ANUNCIO diferido no 1o frame
  #     (o anuncio corre em runtime -> aparece mesmo no log). `class Scene_Map`
  #     e' permitido aqui (nivel-0, nao e' corpo de metodo).
  begin
    if (Scene_Map rescue nil) && (Scene_Map.method_defined?(:update) rescue false) &&
       !$__maxdiag_scn_installed
      $__maxdiag_scn_installed = true
      class Scene_Map
        alias_method :__md_scn_orig_update, :update
        def update(*a, &b)
          r = __md_scn_orig_update(*a, &b)
          unless $__maxdiag_announced
            $__maxdiag_announced = true
            (MKXPDebug.log("[MAXDIAG] >>> ALCANCADO (1o frame de Scene_Map) — dumps ATIVOS <<<") rescue nil)
            (MKXPDebug.log("[MAXDIAG] ===== BUILD DE DIAGNOSTICO TOTAL ATIVO ($MKXP_MAXDIAG=true) ===== dump a cada #{($MKXP_MAXDIAG_EVERY||90)}f no overworld / #{($MKXP_MAXDIAG_EVERY_GP||2)}f (e 1as 15) no ecra de selecao. Sprite_Character mostra cname=<ficheiro> e <<PLAYER. Baixa $MKXP_MAXDIAG_EVERY p/ mais detalhe. A performance vai cair, e' esperado.") rescue nil)
          end
          (MAXDIAG.tick("scn") rescue nil)
          r
        end
      end
    end
  rescue => eS
    (MKXPDebug.log("[MAXDIAG] Scene_Map hook falhou: #{eS.class}: #{eS.message}") rescue nil)
  end

  # (B) GenderPickSelection::Show#update_scene -> ecra de selecao/roupeiro (a CHAVE
  #     dos dados de personagem/outfit). REQUIRE-time (a classe ja' existe; o
  #     OUTFITZ ja' a apanha) p/ apanhar tambem o ecra de selecao do INTRO. Faz UM
  #     dump imediato no 1o update + dumps a cada EVERY_GP. Encadeia com o OUTFITZ
  #     via alias proprio (__md_orig_us) e guarda contra dupla instalacao.
  begin
    _gp = (GenderPickSelection::Show rescue nil)
    if _gp && (_gp.method_defined?(:update_scene) rescue false) &&
       !(_gp.method_defined?(:__md_orig_us) rescue true)
      _gp.class_eval do
        # ── FIX-CHARPICK: o set_player(@position) chama pbChangePlayer(@position),
        # que faz `return false if id >= limit_quantity_player` (limit=SCHEMA.size-8,
        # que o Marshal do mruby pode encolher) OU o trainer_type vem corrompido ("0")
        # -> character_ID nunca fica definido -> a criacao do sprite cai no default
        # Sorin (player 0). RESULTADO: "escolhe sempre o primeiro". Este hook regista
        # o estado real E forca o commit correto: character_ID=id + trainer_type do
        # mapa do METAFIX, contornando metadata corrompida e o limit. $MKXP_CHARPICK_FIX=false reverte.
        unless method_defined?(:__cf_orig_set_player)
          alias_method :__cf_orig_set_player, :set_player
          def set_player(id)
            _lim = (GameData::Metadata.limit_quantity_player rescue '?')
            _sch = (GameData::Metadata::SCHEMA.size rescue '?')
            _cid0 = ($Trainer.character_ID rescue '?')
            r = (__cf_orig_set_player(id) rescue nil)
            _cid1 = ($Trainer.character_ID rescue '?')
            (MKXPDebug.log("[FIX-CHARPICK] set_player(id=#{id}) | limit_quantity_player=#{_lim} SCHEMA.size=#{_sch} | character_ID #{_cid0.inspect}->#{_cid1.inspect} | pbChangePlayer devolveu #{r.inspect}") rescue nil)
            # [FIX-CHARPICK-V3] RAIZ provada pelo log: o $Trainer e' NIL no momento da
            # escolha (a selecao corre no fluxo de inicio, ANTES de o $Trainer nascer)
            # -> "cannot set instance variable" em NilClass, a escolha perde-se e o
            # $Trainer nasce com o default (Sorin). Solucao: GUARDA o id escolhido e
            # aplica-o quando o $Trainer e' lido pela 1a vez (self-heal no getter
            # trainer_type, que corre logo a seguir: 'trainer = $Trainer.trainer_type').
            # Cauteloso: so' atua quando @character_ID esta' em BRANCO (nil/""/-1) e ha
            # escolha guardada -> nunca mexe num personagem ja' valido. $MKXP_CHARPICK_FIX=false reverte.
            if $MKXP_CHARPICK_FIX != false
              begin
                $__picked_player_id = id    # persiste a escolha ate' o $Trainer nascer
                _map = {0=>:POKEMONTRAINER_Sorin,1=>:POKEMONTRAINER_Soleil,2=>:POKEMONTRAINER_Luan,3=>:POKEMONTRAINER_Lusine,4=>:POKEMONTRAINER_Ethan,5=>:POKEMONTRAINER_Lyra,6=>:POKEMONTRAINER_Lucas,7=>:POKEMONTRAINER_Dawn}
                # se por acaso o $Trainer JA' existir, aplica ja' (caso raro)
                if $Trainer && (($Trainer.character_ID rescue nil) != id)
                  _meta = (GameData::Metadata.get_player(id) rescue nil)
                  _tt = (_meta && _meta[0].is_a?(Symbol)) ? _meta[0] : (_map[id] || (_meta ? _meta[0] : nil))
                  ($Trainer.instance_variable_set(:@character_ID, id)) rescue nil
                  ($Trainer.instance_variable_set(:@trainer_type, _tt)) if _tt rescue nil
                end
                # instala 1x o self-heal no getter trainer_type. set_player corre bem
                # depois do boot -> a redefinicao do plugin (Misc bug fixes) ja' aconteceu,
                # logo embrulhamos a versao FINAL.
                if (Object.const_defined?(:Player) rescue false) && !$__cp3_installed
                  $__cp3_installed = true
                  Player.class_eval do
                    if method_defined?(:trainer_type) && !method_defined?(:__cp3_orig_trainer_type)
                      alias_method :__cp3_orig_trainer_type, :trainer_type
                      def trainer_type
                        if $MKXP_CHARPICK_FIX != false && $__picked_player_id
                          _cid = (@character_ID rescue nil)
                          _blank = _cid.nil? || _cid == "" || _cid == -1 || !(_cid.is_a?(Integer) && _cid >= 0)
                          # [FIX-CHARPICK] Caso 1: character_ID em branco/corrompido.
                          # Caso 2 (bug 6 in-game): character_ID VALIDO mas != escolha
                          # (ex: $Trainer nasceu Sorin=0 por omissao, mas escolheste
                          # Luan=2). So' forca no $Trainer REAL (self.equal?($Trainer))
                          # e e' idempotente: assim que character_ID==escolha, no-op.
                          # character_ID e' Integer -> imune a' corrupcao de simbolos.
                          _wrong = (!_blank && (self.equal?($Trainer) rescue false) &&
                                    _cid.is_a?(Integer) && _cid != $__picked_player_id)
                          if _blank || _wrong
                            @character_ID = $__picked_player_id
                            unless $__cp3_healed
                              $__cp3_healed = true
                              (MKXPDebug.log("[FIX-CHARPICK] self-heal: @character_ID #{_cid.inspect} -> id escolhido #{$__picked_player_id} (personagem certo in-game; blank=#{_blank} wrong=#{_wrong})") rescue nil)
                            end
                          end
                        end
                        __cp3_orig_trainer_type
                      end
                    end
                  end
                  (MKXPDebug.log("[FIX-CHARPICK] self-heal instalado no Player#trainer_type") rescue nil)
                end
                (MKXPDebug.log("[FIX-CHARPICK] id escolhido GUARDADO=#{id} | $Trainer no pick=#{($Trainer.class rescue 'nil')} (nil e' normal: aplica-se na criacao do $Trainer)") rescue nil)
              rescue => _cfe
                (MKXPDebug.log("[FIX-CHARPICK] guardar/self-heal rebentou: #{_cfe.class}: #{_cfe.message.to_s[0,90]}") rescue nil)
              end
            end
            r
          end
        end
        alias_method :__md_orig_us, :update_scene
        def update_scene(*a, &b)
          r = __md_orig_us(*a, &b)
          # [PREVIEWDBG 2026-07-02] estado do sprite do PREVIEW do outfit (o player que
          # "falta"). Loga bitmap WxH, x/y/z, viewport.z, visivel, zoom -> diz se o
          # preview esta' fora do ecra (y+h*zoom>240), tapado (z baixo), invisivel, ou
          # sem bitmap. 1a-8a + a cada 60. So' se o sprite existir.
          begin
            _sp = (@sprites['preview'] rescue nil)
            # [FIX-PREVIEW-Z 2026-07-03] O varrimento periodico do FIX-OUTFITZ so'
            # levantou "2 sprites" e o preview ficou a z=2 (provado no log) -> o
            # each_object nao o apanha (timing/enumeracao). Aqui temos o OBJETO
            # exato na mao: levantar diretamente. Idempotente (guard <500000).
            # O motor ignora viewport.z; z alto poe o preview ACIMA do fundo do menu.
            # [2026-07-03b] +1e6 nao chegou: os TABS/elements do menu tambem sao
            # levantados pelo FIX-OUTFITZ (+1e6 dos z originais 3/4 -> 1000003/4)
            # e ficavam ACIMA do preview (2->1000002) tapando-o (PNGs de origem
            # verificados no host: tem conteudo real). O plugin repoe z=2 a cada
            # update_preview (Show.rb:194) -> este hook corre DEPOIS e ganha.
            # 2_000_000 = acima de TUDO o que o menu levanta.
            if _sp && !(_sp.disposed? rescue true) && ((_sp.z rescue 9_999_999) < 1_500_000)
              _sp.z = 2_000_000
              if $__pvz_n.nil? || $__pvz_n < 3
                $__pvz_n = ($__pvz_n || 0) + 1
                (MKXPDebug.log("[FIX-PREVIEW-Z] preview -> z=2000000 (acima dos tabs levantados a 100000x)") rescue nil)
              end
            end
            # [FIX-PREVIEW-SRC 2026-07-03] RAIZ do sprite mudo: src_rect fica 0x0
            # (provado: bmp cheio + z=2M + op=255 + src=0x0). O fix generico do motor
            # (bitmap= repoe src) nao pegou NESTE objeto -> hotfix no proprio objeto:
            # repor o retangulo ao bitmap inteiro sempre que estiver 0x0. Rect#set e'
            # NATIVO (atualiza a struct C que o render le).
            begin
              _b3 = (_sp ? _sp.bitmap : nil)
              if _sp && _b3 && !(_b3.disposed? rescue true) &&
                 ((_sp.src_rect.width rescue 1) == 0 || (_sp.src_rect.height rescue 1) == 0)
                _sp.src_rect.set(0, 0, _b3.width, _b3.height)
                if $__pvsrc_n.nil? || $__pvsrc_n < 3
                  $__pvsrc_n = ($__pvsrc_n || 0) + 1
                  (MKXPDebug.log("[FIX-PREVIEW-SRC] src_rect 0x0 -> #{_b3.width}x#{_b3.height} (sprite estava mudo)") rescue nil)
                end
              end
            rescue; end
            # [2026-07-03] O z ja' esta' levantado (z=1000002, vis=true) e o user
            # CONTINUA sem ver o preview -> proxima hipotese: o BITMAP esta' VAZIO
            # (a composicao do charset do player falhou). Scan de conteudo 1x.
            if _sp && $__pvdbg_opq != true
              _b2 = (_sp.bitmap rescue nil)
              if _b2 && !(_b2.disposed? rescue true)
                $__pvdbg_opq = true
                _oc = 0; _am = 0; _yy = 0
                _bh = (_b2.height rescue 0); _bw2 = (_b2.width rescue 0)
                while _yy < _bh
                  _xx = 0
                  while _xx < _bw2
                    _aa = (_b2.get_pixel(_xx, _yy).alpha.to_i rescue 0)
                    _oc += 1 if _aa > 0; _am = _aa if _aa > _am
                    _xx += 3
                  end
                  _yy += 3
                end
                (MKXPDebug.log("[PREVIEWDBG] CONTEUDO do bitmap #{_bw2}x#{_bh}: opacos=#{_oc} aMAX=#{_am} -> #{_oc == 0 ? 'VAZIO (a composicao do charset falhou - raiz!)' : 'TEM IMAGEM (problema e de posicao/tapado)'}") rescue nil)
              end
            end
            $__pvdbg_n = ($__pvdbg_n || 0) + 1
            if _sp && ($__pvdbg_n <= 8 || ($__pvdbg_n % 60) == 0)
              _bmp = (_sp.bitmap rescue nil)
              _vpz = (_sp.viewport.z rescue '?')
              # [2026-07-03c] + opacity/src_rect/viewport.rect: com bmp CHEIO (141px),
              # z=2M e vis=true, a unica coisa que ainda esconde um sprite e' opacity=0
              # (fade-in falhado) ou src_rect 0x0. Se opacity=0 -> RAIZ encontrada.
              _op = (_sp.opacity rescue '?'); _srw = (_sp.src_rect.width rescue '?'); _srh = (_sp.src_rect.height rescue '?')
              _vpr = (_sp.viewport ? "#{_sp.viewport.rect.x},#{_sp.viewport.rect.y},#{_sp.viewport.rect.width}x#{_sp.viewport.rect.height}" : "sem-vp") rescue "?"
              (MKXPDebug.log("[PREVIEWDBG] preview: bmp=#{_bmp ? "#{(_bmp.width rescue '?')}x#{(_bmp.height rescue '?')}" : 'NIL'} @(#{(_sp.x rescue '?')},#{(_sp.y rescue '?')}) z=#{(_sp.z rescue '?')} vp.z=#{_vpz} vis=#{(_sp.visible rescue '?')} zoom=#{(_sp.zoom_x rescue '?')} OPACITY=#{_op} src=#{_srw}x#{_srh} vp.rect=(#{_vpr}) disp=#{(_sp.disposed? rescue '?')} tab=#{(@currentTab rescue '?')} pos=#{(@position rescue '?')}") rescue nil)
              # fix preventivo: se opacity==0 com o menu ja' estavel, forca 255 (o
              # fade-in dos menus e' cosmetico; nunca deve DEIXAR o sprite invisivel)
              if _op.is_a?(Integer) && _op == 0
                (_sp.opacity = 255 rescue nil)
                (MKXPDebug.log("[FIX-PREVIEW-OP] opacity 0 -> 255 (fade-in nunca completou)") rescue nil)
              end
            end
          rescue; end
          begin
            if $MKXP_MAXDIAG != false
              $__md_gp_fc = ($__md_gp_fc || 0) + 1
              unless $__md_gp_first
                $__md_gp_first = true
                (MKXPDebug.log("[MAXDIAG] >>> ecra GenderPickSelection ATIVO — e' aqui que vive a selecao de personagem/outfit; cada movimento de cursor faz um dump <<<") rescue nil)
              end
              # RAJADA: dump nas primeiras 15 chamadas (apanha o ecra mesmo que passe
              # depressa) + depois a cada EVERY_GP (default 2 -> denso, p/ ver o preview
              # a mudar quando escolhes outra personagem). $MKXP_MAXDIAG_EVERY_GP=1 = tudo.
              # LENTIDAO: o dump ObjectSpace(~500 sprites) a cada 2 frames ERA a causa
              # direta do 1 FPS no ecra de selecao. Default agora 30 (esparso) + rajada 3.
              # Para dump denso de diagnostico: $MKXP_MAXDIAG_EVERY_GP=2. Para FPS REAL
              # total: $MKXP_MAXDIAG=false (desliga tudo).
              _ev = ($MKXP_MAXDIAG_EVERY_GP || 30)
              if $__md_gp_fc <= 3 || (_ev > 0 && ($__md_gp_fc % _ev) == 0)
                MAXDIAG.dump_sprites("gp f=#{$__md_gp_fc}")
              end
            end
            # [FIX-ROUPEIRO-BLEED] REDE (instalada AQUI, no hook que comprovadamente
            # corre): o A=USE de abertura sangra p/ set_input -> '3-Show.rb:56' @bg=1,
            # que ESCONDE os sprites + dispara o pbConfirmMessage prematuro + faz commit
            # set_player(@position=0). update_scene corre ANTES de set_input no loop, por
            # isso reverter @bg 1->0 aqui (enquanto NAO houve navegacao) impede a
            # confirmacao do frame seguinte e re-mostra os sprites. Assim que navegas
            # (@position/@currentTab mudam) o guard desliga -> a confirmacao real funciona
            # com o indice certo (corrige tambem o 'personagem nao muda'). Generico
            # (intro + roupeiro). Reverte com $MKXP_ROUPEIRO_BLEED_GUARD=false.
            if $MKXP_ROUPEIRO_BLEED_GUARD != false
              @__rb_nav = true if ((@position rescue 0) != (@startnum rescue 0)) || ((@currentTab rescue 0) != 0)
              # DIAGNOSTICO: mostra o estado real (1x no inicio + quando @bg ou @position
              # mudam). Diz se 'escolhe sempre o primeiro' e' BLEED (sem nav, @bg=1 cedo)
              # ou NAVEGACAO-MORTA (carregas LEFT/RIGHT mas @position fica em @startnum).
              _rk = "#{(@bg rescue '?')}|#{(@position rescue '?')}|#{(@currentTab rescue '?')}"
              if _rk != @__rb_lastk
                @__rb_lastk = _rk
                (MKXPDebug.log("[ROUPEIRO-STATE] @bg=#{(@bg rescue '?')} @position=#{(@position rescue '?')} @startnum=#{(@startnum rescue '?')} @currentTab=#{(@currentTab rescue '?')} navegou?=#{!!@__rb_nav}") rescue nil)
              end
              if !@__rb_nav && (@bg rescue nil) == 1
                @bg = 0
                unless @__rb_logged
                  @__rb_logged = true
                  (MKXPDebug.log("[FIX-ROUPEIRO-BLEED] @bg=1 revertido (USE de abertura sangrou, sem navegacao previa) -> roupeiro mostra sprites e personagem NAO faz commit a 0") rescue nil)
                end
              end
            end
          rescue; end
          r
        end
      end
    end
  rescue => eG
    (MKXPDebug.log("[MAXDIAG] GP hook falhou: #{eG.class}: #{eG.message}") rescue nil)
  end

  # ─── FIX-ROUPEIRO-BLEED: REDE DE SEGURANCA Ruby para o bleed do USE de abertura.
  # O A=USE que abre o roupeiro/selecao sangra para set_input -> '3 - Show.rb:56'
  #   @bg = 1 if checkInput(Input::USE)
  # e @bg=1 ESCONDE todos os sprites (update_player when 1) + dispara o
  # pbConfirmMessage prematuro + faz o commit set_player(@position) com @position
  # inicial=0. Isto explica TRES sintomas: "sem sprites no roupeiro", confirmacao
  # sozinha, e personagem-sempre-a-0. Se a correcao de MOTOR (s_edge_consumed) ja'
  # matar o bleed, este guard nunca dispara (NO-OP). Se falhar, salva: reverte um
  # @bg 0->1 que ocorra cedo E SEM navegacao previa (posicao/tab ainda no inicio ->
  # so' pode ser o press de abertura, nao uma confirmacao real, mesmo a 1 FPS).
  # Generico (intro + roupeiro). Reverte com $MKXP_ROUPEIRO_BLEED_GUARD=false.
  begin
    _gpb = (GenderPickSelection::Show rescue nil)
    if _gpb && (_gpb.method_defined?(:set_input) rescue false) &&
       !(_gpb.method_defined?(:__rb_orig_set_input) rescue true)
      _gpb.class_eval do
        alias_method :__rb_orig_set_input, :set_input
        def set_input(*a, &b)
          @__rb_fc = (@__rb_fc || 0) + 1
          _bg0 = (@bg rescue nil)
          r = __rb_orig_set_input(*a, &b)
          begin
            if $MKXP_ROUPEIRO_BLEED_GUARD != false && @__rb_fc <= 6 &&
               _bg0 == 0 && (@bg rescue nil) == 1 &&
               (@position rescue 0) == (@startnum rescue 0) && (@currentTab rescue 0) == 0
              @bg = 0
              (MKXPDebug.log("[FIX-ROUPEIRO-BLEED] @bg 0->1 revertido no frame #{@__rb_fc} (USE de abertura sangrou, sem navegacao previa) -> roupeiro mostra sprites e personagem NAO faz commit a 0") rescue nil)
            end
          rescue; end
          r
        end
      end
      (MKXPDebug.log("[FIX-ROUPEIRO-BLEED] guard instalado em GenderPickSelection::Show#set_input (janela 6f + sem-navegacao; flag $MKXP_ROUPEIRO_BLEED_GUARD)") rescue nil)
    end
  rescue => eRB
    (MKXPDebug.log("[FIX-ROUPEIRO-BLEED] install falhou: #{eRB.class}: #{eRB.message}") rescue nil)
  end

  # (C) Game_Picture#show -> registar TODAS as imagens (intro/roupeiro usam Pictures)
  begin
    _pic = (Game_Picture rescue nil)
    if _pic && (_pic.method_defined?(:show) rescue false) &&
       !(_pic.method_defined?(:__md_orig_show) rescue true)
      _pic.class_eval do
        alias_method :__md_orig_show, :show
        def show(*args)
          (MKXPDebug.log("[MAXDIAG-PIC] show num=#{(@number rescue '?')} args=#{(args[0,8].inspect rescue '?')}") rescue nil) if $MKXP_PROBE_QUIET != true
          __md_orig_show(*args)
        end
      end
    end
  rescue => eP
    (MKXPDebug.log("[MAXDIAG] PIC hook falhou: #{eP.class}: #{eP.message}") rescue nil)
  end

  # (D) BONUS: Graphics.update -> tick partilhado ($__md_fc). Singleton C++: pode
  #     nao propagar o alias no 3DS. ISOLADO no seu rescue -- falhar aqui NAO
  #     afeta (A)/(B)/(C) (foi esta a causa do MAXDIAG mudo no V39: a falha do
  #     Graphics abortava o begin INTEIRO e o Scene_Map nunca instalava).
  begin
    if (Graphics rescue nil) && (Graphics.respond_to?(:update) rescue false) && !$__maxdiag_gfx
      $__maxdiag_gfx = true
      Graphics.singleton_class.class_eval do
        alias_method :__md_orig_gupdate, :update
        def update(*a, &b)
          r = __md_orig_gupdate(*a, &b)
          (MAXDIAG.tick("gfx") rescue nil)
          r
        end
      end
    end
  rescue => eGr
    (MKXPDebug.log("[MAXDIAG] Graphics hook (BONUS, pode falhar sem problema): #{eGr.class}: #{eGr.message}") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[MAXDIAG] install falhou: #{e.class}: #{e.message}") rescue nil)
end

# =============================================================================
#  OUTFITZ  —  isolar os sprites que ficam POR CIMA do menu do roupeiro
# -----------------------------------------------------------------------------
#  Utilizador (queixa recorrente): "sprites ficam por cima do menu de outfit no
#  roupeiro". O menu e' o plugin GenderPickSelection::Show, que cria um Viewport
#  com z=99999 -> no Windows fica no topo de TUDO. Se no 3DS aparecem sprites por
#  cima, ou ha um viewport/sprite com z>=99999, ou o motor NAO respeita o
#  viewport.z (bug de ordenacao do 3DS -- bate certo com o reframing).
#  Na 1a passagem de update_scene (menu ja' montado) varremos os Sprites VIVOS
#  UMA vez (sem custo por-frame) e separamos em dois baldes:
#    - MENU  : vp.z>=99999  (os sprites do proprio menu -- esperado)
#    - FUNDO : vp.z<99999 ou tem @character (overworld -- DEVIAM estar tapados).
#      Se a lista FUNDO contiver o player/NPCs/seguidor visiveis -> e' o motor a
#      ignorar o viewport.z (a causa). Registamos classe + z + vp.z de cada um.
#  Flag $MKXP_OUTFITZ_LOG (ON por defeito). SEM FALHAS: pior caso = no-op.
# =============================================================================
begin
  if defined?(GenderPickSelection) && defined?(GenderPickSelection::Show) && !$__outfitz_installed
    $__outfitz_installed = true
    module GenderPickSelection
      class Show
        if method_defined?(:update_scene)
          alias_method :__oz_orig_us, :update_scene
          def update_scene(*a)
            if $MKXP_OUTFITZ_LOG != false && !$__oz_dumped
              $__oz_dumped = true
              begin
                myvp = (@viewport.z rescue nil)
                (MKXPDebug.log("[OUTFITZ] menu do roupeiro montado | viewport.z=#{myvp} (deve ficar no topo de tudo)") rescue nil)
                menu_n = 0; fundo_n = 0; vis = 0; bg_found = false
                ObjectSpace.each_object(Sprite) do |sp|
                  begin
                    next if (sp.disposed? rescue true)
                    next if !(sp.visible rescue false)
                    vis += 1
                    vp  = (sp.viewport rescue nil)
                    vpz = vp ? (vp.z rescue -1) : nil
                    spz = (sp.z rescue 0)
                    haschar = (sp.instance_variable_defined?(:@character) rescue false)
                    if vpz && vpz >= 99999
                      menu_n += 1   # sprite do proprio menu (esperado)
                      # [SELFTEST|ROUPEIRO] o bg solido do FIX-ROUPEIRO-FUNDO: vp.z>=99999
                      # + z muito negativo + bitmap grande (tapa o ecra). Se existe, o
                      # mapa esta' tapado; se nao, o quarto/moveis aparecem atraves.
                      if spz <= -500 && !haschar
                        _bgw = (sp.bitmap && !(sp.bitmap.disposed? rescue true) ? (sp.bitmap.width rescue 0) : 0) rescue 0
                        bg_found = true if _bgw >= 300
                      end
                    else
                      fundo_n += 1
                      if fundo_n <= 25
                        cn = (sp.class.name rescue "?")
                        bw = ((sp.bitmap && !(sp.bitmap.disposed? rescue true)) ? "#{sp.bitmap.width}x#{sp.bitmap.height}" : "nil") rescue "?"
                        (MKXPDebug.log("[OUTFITZ]   FUNDO-VISIVEL (devia estar tapado): #{cn} z=#{spz} vp.z=#{vpz.nil? ? 'sem-vp' : vpz} char=#{haschar} bmp=#{bw}") rescue nil)
                      end
                    end
                  rescue; end
                end
                (MKXPDebug.log("[SELFTEST|ROUPEIRO] bg-solido=#{bg_found ? 'PRESENTE (cobre o quarto)' : 'AUSENTE (mapa/moveis aparecem atraves)'} | sprites-do-mapa atras=#{fundo_n}") rescue nil)
                (MKXPDebug.log("[OUTFITZ] total visiveis=#{vis} | do-menu(vp.z>=99999)=#{menu_n} | FUNDO-visivel=#{fundo_n} -> se o FUNDO aparece por cima, e' o motor a ignorar viewport.z") rescue nil)
              rescue => e2
                (MKXPDebug.log("[OUTFITZ] scan falhou: #{e2.class}: #{e2.message}") rescue nil)
              end
            end
            # FIX-OUTFITZ: REATIVADO por defeito. O fix de z-order do MOTOR (ZSORT-REGTIE)
            # foi revertido (partia o teclado do nome -- ver [ZREVERT] no topo), por isso o
            # probe volta a tratar do z-order do menu: sobe o z individual dos sprites do menu
            # (vp.z>=99999) +1_000_000, uma vez cada, preservando a ordem interna. Reversivel
            # com $MKXP_OUTFITZ_FIX=false. NAO toca em sprites sem viewport (ex: o teclado do
            # nome esta' a z=100 num viewport, mas a sua janela de fundo z=0 fica intacta pelo
            # desempate por z local do motor revertido).
            if $MKXP_OUTFITZ_FIX != false
              @__oz_fix_n = (@__oz_fix_n || 0) + 1
              # [2026-07-02] Antes: so' os primeiros 20 frames -> o PREVIEW do player
              # (sprite 69x69, vp.z=99999 z=2) e' criado MAIS TARDE (log: t=99s, muito
              # depois do cap a f=20/t=93s) e nunca era levantado -> ficava por baixo do
              # fundo do menu (motor ignora vp.z) = preview invisivel. Agora corre a cada
              # 8 frames SEM cap: idempotente (o guard z<500000 salta os ja' levantados),
              # apanha sprites criados tarde. So' enquanto o menu do roupeiro esta' aberto.
              if (@__oz_fix_n % 8) == 1
                begin
                  _bz = 0
                  ObjectSpace.each_object(Sprite) do |sp|
                    begin
                      next if (sp.disposed? rescue true)
                      _vp  = (sp.viewport rescue nil)
                      _vpz = _vp ? (_vp.z rescue 0) : 0
                      if _vpz >= 99999 && (sp.z rescue 1_000_000) < 500_000
                        sp.z = (sp.z rescue 0) + 1_000_000
                        _bz += 1
                      end
                    rescue; end
                  end
                  (MKXPDebug.log("[FIX-OUTFITZ] subi z de #{_bz} sprites do menu (+1e6) p/ ficarem sobre o fundo (motor ignora vp.z) | f=#{@__oz_fix_n} flag $MKXP_OUTFITZ_FIX") rescue nil) if _bz > 0
                rescue => _oze
                  (MKXPDebug.log("[FIX-OUTFITZ] falhou: #{_oze.class}: #{_oze.message}") rescue nil)
                end
              end
            end
            __oz_orig_us(*a)
          end
        end
      end
    end
    (MKXPDebug.log("[OUTFITZ] instalado (hook GenderPickSelection::Show; scan unico; flag $MKXP_OUTFITZ_LOG)") rescue nil)
  end
rescue => e
  (MKXPDebug.log("[OUTFITZ] install falhou: #{e.class}: #{e.message}") rescue nil)
end

# ─── [INTERPCMD 2026-07-04] perfil de comandos do interpreter (rua: 25-152ms) ──
# O PERF-RESTO provou interpreter pesado na rua com o EVSC ja' ativo -> o custo
# esta' nos COMANDOS (move routes? waits? scripts pesados?). Mede execute_command
# e acumula ms por code; despeja o top a cada ~600 comandos. Custo: 2 __mkxp_ms
# por comando (so' quando ha' interpreter ativo). Desligar: $MKXP_INTERPCMD=false.
begin
  if $MKXP_INTERPCMD != false && Object.const_defined?(:Interpreter) &&
     Interpreter.instance_methods.include?(:execute_command) &&
     !Interpreter.instance_methods.include?(:__ipc_orig_execute_command)
    $__ipc_acc = {}
    $__ipc_n = 0
    class Interpreter
      alias_method :__ipc_orig_execute_command, :execute_command
      def execute_command
        _t0 = (__mkxp_ms rescue nil)
        _r = __ipc_orig_execute_command
        begin
          if _t0
            _dt = (__mkxp_ms rescue _t0) - _t0
            if _dt > 0.05
              _cd = (@list[@index].code rescue -1)
              _a = ($__ipc_acc[_cd] ||= [0.0, 0])
              _a[0] += _dt; _a[1] += 1
            end
            $__ipc_n += 1
            if ($__ipc_n % 600) == 0
              _top = $__ipc_acc.sort_by { |_, _v| -_v[0] }.first(5)
              (MKXPDebug.log("[INTERPCMD] top comandos por ms (acum): " + _top.map { |_c, _v| "code#{_c}=#{_v[0].round(1)}ms(x#{_v[1]})" }.join(' ')) rescue nil)
              $__ipc_acc.clear
            end
          end
        rescue; end
        _r
      end
    end
    (MKXPDebug.log("[INTERPCMD] perfil de execute_command instalado (top a cada 600 comandos)") rescue nil)
  end
rescue => _ipce
  (MKXPDebug.log("[INTERPCMD] falhou: #{_ipce.class}: #{_ipce.message}") rescue nil)
end
