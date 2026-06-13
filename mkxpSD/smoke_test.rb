# =============================================================================
# SMOKE TEST — mkxp-3DS
# -----------------------------------------------------------------------------
# Percorre TUDO de uma vez (imagens, scripts, dados, audio, tiles, animacoes),
# com rescue por item, e grava num LOG SEPARADO: sdmc:/mkxp/smoke_test.log
#
# Objetivo: num so arranque, ver tudo o que esta partido, em vez de descobrir
# bug a bug. Cada teste e' isolado -- um erro nunca para os outros.
#
# COMO USAR:
#   - Carregado automaticamente pelo binding (no Scene_DebugIntro, depois do
#     jogo arrancar e os dados estarem prontos).
#   - Ou a' mao a qualquer momento:  SmokeTest.run_all
#   - O relatorio fica em sdmc:/mkxp/smoke_test.log (separado do log normal).
#
# NOTA HONESTA: o smoke test confirma que cada recurso CARREGA e com que
# dimensoes/posicao. NAO consegue ver se aparece visualmente no sitio certo --
# isso so' tu, a olhar para o ecra, podes confirmar. Mas regista o tamanho e a
# posicao esperada de cada imagem, que e' o maximo possivel sem olhos.
# =============================================================================

module SmokeTest
  @file = nil

  def self.open_log
    return if @file
    begin
      @file = File.open("sdmc:/mkxp/smoke_test.log", "w")
    rescue
      begin; @file = File.open("smoke_test.log", "w"); rescue; @file = nil; end
    end
  end

  def self.log(msg)
    line = "[SMOKE] #{msg}"
    begin
      if @file
        @file.puts(line)
        @file.flush rescue nil
      end
    rescue; end
    # tambem espelha no log normal, para teres tudo num sitio se quiseres
    begin; Probe.log(line) if defined?(Probe); rescue; end
  end

  # tenta um bloco; devolve [ok?, resultado_ou_erro]
  def self.try(label)
    begin
      r = yield
      [true, r]
    rescue Exception => e
      [false, "#{e.class}: #{e.message}"]
    end
  end

  # ---------------------------------------------------------------------------
  # 1. DADOS DO JOGO (GameData) — ve se cada tipo de dados carregou
  # ---------------------------------------------------------------------------
  def self.test_gamedata
    log "=== 1. DADOS (GameData) ==="
    types = %w[Species Move Item Ability Type Nature Trainer TrainerType
               Metadata PlayerMetadata Encounter Evolution Stat Status
               Weather Target Ribbon Habitat Environment BattleWeather]
    ok = 0; fail = 0
    types.each do |t|
      pass, res = try(t) do
        klass = (eval("GameData::#{t}") rescue nil)
        if klass.nil?
          "NAO DEFINIDO"
        elsif klass.respond_to?(:each)
          n = 0
          klass.each { |_| n += 1 }
          "#{n} registos"
        else
          "existe (sem .each)"
        end
      end
      if pass && res.to_s !~ /NAO DEFINIDO/
        ok += 1; log "  OK   GameData::#{t} -> #{res}"
      else
        fail += 1; log "  FALHA GameData::#{t} -> #{res}"
      end
    end
    log "DADOS: #{ok} OK, #{fail} com problema"
    [ok, fail]
  end

  # ---------------------------------------------------------------------------
  # 2. IMAGENS — tenta carregar cada uma e regista tamanho.
  #    Para as do titulo, regista tambem a posicao esperada no ecra.
  # ---------------------------------------------------------------------------
  def self.load_bitmap(path)
    # tenta varias formas de carregar, conforme o que o jogo usa
    bmp = (pbBitmap(path) rescue nil)
    bmp ||= (RPG::Cache.load_bitmap("", path) rescue nil)
    bmp ||= (Bitmap.new(path) rescue nil)
    bmp
  end

  def self.test_images
    log "=== 2. IMAGENS (carregamento + tamanho) ==="
    # [caminho, posicao_esperada_ou_nil]
    imgs = [
      # --- Tela de titulo base (Essentials) ---
      ["Graphics/Titles/title",   "fundo, ecra todo"],
      ["Graphics/Titles/splash1", "splash, centro"],
      ["Graphics/Titles/splash2", "splash, centro"],
      ["Graphics/Titles/splash3", "splash, centro"],
      ["Graphics/Titles/start",   "x=0 y=322 (PRESS ENTER)"],
      # --- Modular Title Screen (Solar Eclipse) ---
      ["Graphics/MODTS/start",                   "PRESS ENTER do MODTS"],
      ["Graphics/MODTS/logo2",                   "logo, logoY:200"],
      ["Graphics/MODTS/Backgrounds/eclipse",     "fundo eclipse, ecra todo"],
      ["Graphics/MODTS/Overlays/static001",      "overlay6"],
      ["Graphics/MODTS/Effects/sparkle",         "effect (sparkle)"],
      # --- Exemplos gerais (mapa/jogo) ---
      ["Graphics/Characters/trchar000",          "sprite de personagem"],
      ["Graphics/Pictures/martialartist",        "imagem generica"],
    ]
    ok = 0; fail = 0
    imgs.each do |path, pos|
      pass, res = try(path) do
        bmp = load_bitmap(path)
        if bmp.nil?
          "nil (nao carregou)"
        else
          w = (bmp.width rescue '?'); h = (bmp.height rescue '?')
          extra = pos ? " | esperado: #{pos}" : ""
          "#{w}x#{h}#{extra}"
        end
      end
      if pass && res.to_s !~ /nil/
        ok += 1; log "  OK   #{path} -> #{res}"
      else
        fail += 1; log "  FALHA #{path} -> #{res}"
      end
    end
    log "IMAGENS: #{ok} OK, #{fail} com problema"
    log "  (nota: 'OK' = carregou. Se aparece no sitio certo, so' a ver o ecra.)"
    [ok, fail]
  end

  # ---------------------------------------------------------------------------
  # 3. CLASSES-CHAVE — existencia das classes do fluxo do jogo
  # ---------------------------------------------------------------------------
  def self.test_classes
    log "=== 3. CLASSES-CHAVE (existencia) ==="
    names = %w[Scene_Intro ModularTitleScreen IntroEventScene Scene_Map
               Game_Map Game_Player Game_Temp Game_System Spriteset_Map
               PokemonLoadScreen PokemonGlobalMetadata Sprite_Character
               MTS_Element_Logo]
    ok = 0; fail = 0
    names.each do |n|
      exists = (Object.const_defined?(n.to_sym) rescue false)
      unless exists
        exists = (eval("defined?(#{n})") ? true : false) rescue false
      end
      if exists
        ok += 1; log "  OK   #{n} -> definida"
      else
        fail += 1; log "  FALHA #{n} -> NAO definida"
      end
    end
    log "CLASSES: #{ok} OK, #{fail} em falta"
    [ok, fail]
  end

  # ---------------------------------------------------------------------------
  # 4. FALAS / TRADUCOES (_INTL) e sistema de mensagens
  # ---------------------------------------------------------------------------
  def self.test_messages
    log "=== 4. FALAS / MENSAGENS ==="
    ok = 0; fail = 0
    pass, res = try("_INTL") do
      s = (_INTL("Hello world") rescue nil)
      s.nil? ? "nil" : "devolveu: #{s}"
    end
    if pass && res !~ /nil/
      ok += 1; log "  OK   _INTL -> #{res}"
    else
      fail += 1; log "  FALHA _INTL -> #{res}"
    end
    [["MessageTypes", "MessageTypes"], ["pbGetMessage", "pbGetMessage"]].each do |label, expr|
      _p, r = try(label) do
        (eval("defined?(#{expr})") ? "definido" : "NAO definido") rescue "erro"
      end
      log "  #{label} -> #{r}"
    end
    log "FALAS: #{ok} OK, #{fail} com problema"
    [ok, fail]
  end

  # ---------------------------------------------------------------------------
  # 5. AUDIO — BGM e SE. NAO toca de verdade (so' verifica que o metodo aceita
  #    sem rebentar). No port atual o audio e' stub, por isso esperamos "aceitou
  #    sem crashar" em vez de som real.
  # ---------------------------------------------------------------------------
  def self.test_audio
    log "=== 5. AUDIO (BGM/SE — verifica que aceita sem crashar) ==="
    ok = 0; fail = 0
    tests = [
      ["pbBGMPlay(title)", lambda { pbBGMPlay("title_md") rescue pbBGMPlay("Title") }],
      ["pbSEPlay(decision)", lambda { pbSEPlay("GUI sel decision") rescue pbSEPlay("decision") }],
      ["pbPlayDecisionSE", lambda { pbPlayDecisionSE }],
      ["pbBGMStop", lambda { pbBGMStop rescue pbBGMFade(0.1) }],
    ]
    tests.each do |label, fn|
      pass, res = try(label) do
        fn.call
        "aceitou (sem crash)"
      end
      if pass
        ok += 1; log "  OK   #{label} -> #{res}"
      else
        fail += 1; log "  FALHA #{label} -> #{res}"
      end
    end
    log "AUDIO: #{ok} OK, #{fail} com problema"
    log "  (nota: 'OK' = nao rebentou. Som real depende da pipeline de audio,"
    log "         que no port atual ainda e' stub.)"
    [ok, fail]
  end

  # ---------------------------------------------------------------------------
  # 6. TILESETS — ve se os tilesets carregam (base do mapa)
  # ---------------------------------------------------------------------------
  def self.test_tilesets
    log "=== 6. TILESETS (base do mapa) ==="
    ok = 0; fail = 0
    pass, res = try("$data_tilesets") do
      ts = ($data_tilesets rescue nil)
      ts ||= (load_data("Data/Tilesets.rxdata") rescue nil)
      if ts.nil?
        "nil"
      else
        n = (ts.compact.size rescue ts.size rescue '?')
        "#{n} tilesets"
      end
    end
    if pass && res !~ /nil/
      ok += 1; log "  OK   tilesets -> #{res}"
    else
      fail += 1; log "  FALHA tilesets -> #{res}"
    end
    # tentar carregar o grafico do 1o tileset
    _p, r2 = try("tileset gfx") do
      ts = ($data_tilesets rescue nil)
      if ts && ts.compact.size > 0
        t = ts.compact[0]
        name = (t.tileset_name rescue '?')
        bmp = load_bitmap("Graphics/Tilesets/#{name}")
        bmp.nil? ? "grafico nao carregou (#{name})" : "grafico OK: #{name} #{bmp.width rescue '?'}x#{bmp.height rescue '?'}"
      else
        "sem tilesets para testar"
      end
    end
    log "  primeiro tileset gfx -> #{r2}"
    log "TILESETS: #{ok} OK, #{fail} com problema"
    [ok, fail]
  end

  # ---------------------------------------------------------------------------
  # 7. ANIMACOES / PARTICULAS — ve se os dados de animacao existem
  # ---------------------------------------------------------------------------
  def self.test_animations
    log "=== 7. ANIMACOES / PARTICULAS ==="
    ok = 0; fail = 0
    pass, res = try("$data_animations") do
      anims = ($data_animations rescue nil)
      anims ||= (load_data("Data/Animations.rxdata") rescue nil)
      if anims.nil?
        "nil (sem animacoes carregadas)"
      else
        n = (anims.compact.size rescue anims.size rescue '?')
        "#{n} animacoes"
      end
    end
    if pass && res !~ /nil/
      ok += 1; log "  OK   animacoes -> #{res}"
    else
      fail += 1; log "  FALHA animacoes -> #{res}"
    end
    # classes de particulas/efeitos do EBDX, se existirem
    %w[Particle_Engine ParticleEffect Sprite_Particle].each do |n|
      ex = (Object.const_defined?(n.to_sym) rescue false)
      log "  classe #{n} -> #{ex ? 'existe' : 'nao existe'}"
    end
    log "ANIMACOES: #{ok} OK, #{fail} com problema"
    [ok, fail]
  end

  # ---------------------------------------------------------------------------
  # CORRER TUDO
  # ---------------------------------------------------------------------------
  def self.run_all
    open_log
    log "##################################################"
    log "#          INICIO DO SMOKE TEST                  #"
    log "##################################################"
    total_ok = 0; total_fail = 0
    secoes = [
      :test_gamedata, :test_images, :test_classes, :test_messages,
      :test_audio, :test_tilesets, :test_animations
    ]
    secoes.each do |m|
      begin
        o, f = send(m)
        total_ok += (o || 0); total_fail += (f || 0)
      rescue Exception => e
        log "SECAO #{m} REBENTOU: #{e.class}: #{e.message}"
        total_fail += 1
      end
      log ""  # linha em branco entre seccoes
    end
    log "##################################################"
    log "#          FIM DO SMOKE TEST                     #"
    log "##################################################"
    log "RESUMO GLOBAL: #{total_ok} OK, #{total_fail} com problema"
    log "Relatorio completo em: sdmc:/mkxp/smoke_test.log"
    begin; @file.flush rescue nil; rescue; end
  end
end
