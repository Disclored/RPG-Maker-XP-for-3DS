# pokemon_tilemap_fix.rb  —  mruby 3.2 / 3DS port
#
# Portado de mkxp-z-apple-mobile (iOS) para mruby 3.2 sem stdlib,
# sem Regexp nativo, sem Float#ceil com argumento, sem define_method
# com blocos que capturam locals externos (mruby tem closures mais
# restritas que MRI).
#
# O que faz:
#   Pokemon Essentials usa CustomTilemap (Ruby puro) que carrega o
#   tileset inteiro como um único Bitmap. No 3DS o limite de textura
#   GPU é 1024 px. Tilesets de PE têm normalmente 4000-8000 px de
#   altura → create_texture falha silenciosamente → mapa preto.
#
#   VWrap reparte o tileset em múltiplas colunas de 1024 px,
#   mantendo a largura total dentro do limite GPU. O lookup de tiles
#   é redirecionado para as coordenadas correctas no bitmap repartido.
#
# Limite efectivo após VWrap:
#   GPU 1024 → suporta tilesets até 1024 * (1024/256) = 4096 px tall
#   (256 é a largura padrão de um tileset RGSS1)

$DONTREFRESHAUTOTILES = false unless defined?($DONTREFRESHAUTOTILES)

module VWrap
  # Bitmap.max_size devolve 1024 no binding 3DS.
  # Usar 1024 directamente como fallback seguro caso max_size falhe.
  MAX_TEX_SIZE = begin
    Bitmap.max_size
  rescue
    1024
  end

  TILESET_WIDTH  = 256   # largura fixa do tileset RGSS1/PE
  # Altura máxima por coluna: múltiplo de 32 <= MAX_TEX_SIZE
  TILESET_HEIGHT = MAX_TEX_SIZE - (MAX_TEX_SIZE % 32)
  # Altura total suportada após repartição em colunas
  MAX_TEX_SIZE_BOOSTED = (MAX_TEX_SIZE * MAX_TEX_SIZE) / TILESET_WIDTH

  # Clamp sem Float (mruby tem Comparable#clamp mas só com range em
  # algumas builds — usar forma explícita para garantir)
  def self.clamp(val, min_v, max_v)
    val = max_v if val > max_v
    val = min_v if val < min_v
    val
  end

  # Divide um bitmap alto em múltiplas colunas de TILESET_HEIGHT px.
  # Devolve o bitmap original se já couber numa textura.
  def self.makeVWrappedTileset(originalbmp)
    w = originalbmp.width
    h = originalbmp.height

    # Só processar tilesets com a largura padrão que excedam o limite
    return originalbmp unless w == TILESET_WIDTH && h > TILESET_HEIGHT

    # Número de colunas necessárias (ceil sem Float#ceil com arg)
    columns = (h + TILESET_HEIGHT - 1) / TILESET_HEIGHT

    if columns * TILESET_WIDTH > MAX_TEX_SIZE
      raise "Tileset demasiado alto para 3DS!\n" \
            "ALTURA: #{h}px\n" \
            "LIMITE GPU: #{MAX_TEX_SIZE}px\n" \
            "LIMITE BOOSTED: #{MAX_TEX_SIZE_BOOSTED}px"
    end

    bmp = Bitmap.new(TILESET_WIDTH * columns, TILESET_HEIGHT)
    remainder = h % TILESET_HEIGHT
    remainder = TILESET_HEIGHT if remainder == 0

    columns.times do |col|
      src_h = (col + 1 == columns) ? remainder : TILESET_HEIGHT
      srcrect = Rect.new(0, col * TILESET_HEIGHT, w, src_h)
      bmp.blt(col * TILESET_WIDTH, 0, originalbmp, srcrect)
    end

    bmp
  end

  # Copia pixels do bitmap repartido para o destino,
  # traduzindo coordenadas srcrect do espaço original para o repartido.
  def self.blitVWrappedPixels(dest_x, dest_y, dest, src, srcrect)
    # Origem 4444 (tileset gigante não-embrulhado): coordenadas originais válidas
    # -> blt direto (o motor lê 4444 na origem). Sem tradução de colunas.
    if src.respond_to?(:native_4444?) && src.native_4444?
      dest.blt(dest_x, dest_y, src, srcrect)
      return
    end
    # Se o rect cabe inteiramente na primeira coluna, blit directo
    if srcrect.y + srcrect.height <= TILESET_HEIGHT
      dest.blt(dest_x, dest_y, src, srcrect)
      return
    end

    # Traduzir coordenadas para o bitmap repartido
    sx = clamp(srcrect.x, 0, TILESET_WIDTH)
    sw = clamp(srcrect.width, 0, TILESET_WIDTH - sx)
    col   = srcrect.y / TILESET_HEIGHT
    src_x = col * TILESET_WIDTH + sx
    src_y = srcrect.y % TILESET_HEIGHT

    dest.blt(dest_x, dest_y, src, Rect.new(src_x, src_y, sw, srcrect.height))
  end
end

# Aplicar apenas se: engine mkxp 3DS E jogo tem CustomTilemap (PE)
if $MKXP == true && defined?(CustomTilemap) == "constant"

  class CustomTilemap

    # Override tileset= para repartir se necessário
    def tileset=(value)
      # Mudança de mapa: limpa a cache negativa de loads falhados no motor, para
      # este mapa voltar a TENTAR carregar os seus tilesets de raiz (o mapa
      # anterior pode tê-los marcado como "falhou" por falta de memória que
      # entretanto libertou). Sem isto, um tileset que falhou uma vez ficaria
      # magenta para sempre. rescue nil: motor sem o método (build antigo).
      Bitmap._clear_failed_loads rescue nil
      # Tileset gigante em RGBA4444 (decodificado assim pelo motor 3DS porque o
      # 8888 de 30MB não cabe): usa-se DIRETO, sem embrulho. As coordenadas
      # originais (getRegularTile) já são válidas no bitmap 256xN não-embrulhado,
      # e o blt nativo lê 4444 na origem. Poupa o Bitmap.new de ~31MB do embrulho.
      if value && (value.respond_to?(:native_4444?) && value.native_4444?)
        @tileset = value
        @tilesetchanged = true
        return value
      end
      if value && value.width == VWrap::TILESET_WIDTH &&
         value.height > VWrap::TILESET_HEIGHT
        wrapped = VWrap.makeVWrappedTileset(value)
        # Só dispor o original se foi mesmo substituído
        if wrapped != value
          value.dispose rescue nil
          value = wrapped
        end
      end
      @tileset = value
      @tilesetchanged = true
    end

    # Override getRegularTile para usar coordenadas repartidas
    def getRegularTile(sprite, id)
      bitmap = @regularTileInfo[id]
      unless bitmap
        bitmap = Bitmap.new(@tileWidth, @tileHeight)
        rect = Rect.new(
          ((id - 384) & 7) * @tileSrcWidth,
          ((id - 384) >> 3) * @tileSrcHeight,
          @tileSrcWidth,
          @tileSrcHeight
        )
        VWrap.blitVWrappedPixels(0, 0, bitmap, @tileset, rect)
        @regularTileInfo[id] = bitmap
      end
      sprite.bitmap = bitmap if sprite.bitmap != bitmap
    end

    # Override refreshLayer0 para usar blitVWrappedPixels nos tiles regulares
    # NOTA: apenas os blits de tiles regulares (id >= 384) são substituídos.
    # A lógica de autotiles e prioridades é mantida intacta.
    def refreshLayer0(autotiles = false)
      return true if autotiles && !shown?
      ($__vwp ||= Hash.new(0))

      # [PERF-SMALLMAP 2026-07-04] mapa que CABE INTEIRO no buffer (vizinhos
      # pequenos): compoe-se 1x (oxLayer0=0) e o scroll e' SO' ox/src — sem as
      # guardas de limites (a janela pode sair do mapa na costura; fora =
      # transparente = correto, o vizinho desenha o resto). Antes: full
      # recompose de ~12-19ms TODOS os frames nestes mapas (o grosso do
      # TILEPROF da rua).
      if !autotiles && !@firsttime && !@usedsprites && @__vw_small_done &&
         !(@tilesetchanged rescue false) &&
         @map_data.xsize * @tileWidth <= @layer0.bitmap.width &&
         @map_data.ysize * @tileHeight <= @layer0.bitmap.height
        _pt_x = @ox - @oxLayer0
        _pt_y = @oy - @oyLayer0
        if @layer0clip && @viewport.ox == 0 && @viewport.oy == 0
          @layer0.ox = 0
          @layer0.oy = 0
          @layer0.src_rect.set(_pt_x.round, _pt_y.round,
                               @viewport.rect.width, @viewport.rect.height)
        else
          @layer0.ox = _pt_x.round
          @layer0.oy = _pt_y.round
          @layer0.src_rect.set(0, 0, @layer0.bitmap.width, @layer0.bitmap.height)
        end
        $__vwp[:small] += 1
        return true
      end
      pt_x = @ox - @oxLayer0
      pt_y = @oy - @oyLayer0
      if !autotiles && !@firsttime && !@usedsprites &&
         pt_x >= 0 && pt_x + @viewport.rect.width  <= @layer0.bitmap.width &&
         pt_y >= 0 && pt_y + @viewport.rect.height <= @layer0.bitmap.height
        if @layer0clip && @viewport.ox == 0 && @viewport.oy == 0
          @layer0.ox = 0
          @layer0.oy = 0
          @layer0.src_rect.set(pt_x.round, pt_y.round,
                               @viewport.rect.width, @viewport.rect.height)
        else
          @layer0.ox = pt_x.round
          @layer0.oy = pt_y.round
          @layer0.src_rect.set(0, 0,
                               @layer0.bitmap.width, @layer0.bitmap.height)
        end
        $__vwp[:fast] += 1
        return true
      end

      $__vwp[:full] += 1
      if ($__vwp[:full] % 30) == 1
        (MKXPDebug.log("[VWPATH] full=#{$__vwp[:full]} fast=#{$__vwp[:fast]} small=#{$__vwp[:small]} auto=#{$__vwp[:auto]} used=#{$__vwp[:used]} | firsttime=#{@firsttime.inspect} usedspr=#{@usedsprites.inspect} tschg=#{(@tilesetchanged rescue '?').inspect} small_done=#{@__vw_small_done.inspect} map=#{(@map_data.xsize rescue '?')}x#{(@map_data.ysize rescue '?')} buf=#{@layer0.bitmap.width}x#{@layer0.bitmap.height} ox=#{@ox.to_i} oxL=#{@oxLayer0.to_i}") rescue nil)
      end
      width   = @layer0.bitmap.width
      height  = @layer0.bitmap.height
      bitmap  = @layer0.bitmap
      ysize   = @map_data.ysize
      xsize   = @map_data.xsize
      zsize   = @map_data.zsize
      twidth  = @tileWidth
      theight = @tileHeight
      mapdata = @map_data

      if autotiles
        $__vwp[:auto] += 1
        return true if $DONTREFRESHAUTOTILES
        return true if @fullyrefreshedautos && @prioautotiles.empty?

        x_start = @oxLayer0 / twidth
        x_start = 0 if x_start < 0
        y_start = @oyLayer0 / theight
        y_start = 0 if y_start < 0
        x_end = x_start + width  / twidth  + 1
        y_end = y_start + height / theight + 1
        x_end = xsize if x_end > xsize
        y_end = ysize if y_end > ysize
        return true if x_start >= x_end || y_start >= y_end

        trans    = Color.new(0, 0, 0, 0)
        temprect = Rect.new(0, 0, 0, 0)
        tilerect = Rect.new(0, 0, twidth, theight)
        overallcount = 0
        count = 0

        if @fullyrefreshedautos
          if !@priorect || !@priorectautos ||
             @priorect[0] != x_start || @priorect[1] != y_start ||
             @priorect[2] != x_end   || @priorect[3] != y_end

            # Suporte a @prioautotiles como Array ou Hash (vários forks PE)
            if @prioautotiles.is_a?(Hash)
              @priorectautos = @prioautotiles.keys.select do |key|
                x = key[0]; y = key[1]
                !(x < x_start || x > x_end || y < y_start || y > y_end)
              end
            else
              @priorectautos = @prioautotiles.select do |tile|
                x = tile[0]; y = tile[1]
                !(x < x_start || x > x_end || y < y_start || y > y_end)
              end
            end
            @priorect = [x_start, y_start, x_end, y_end]
          end

          # [PERF-AUTOBATCH 2026-07-04] o passe de autotiles custava 5-13ms/frame
          # na rua (agua/flores): por celula 1 fill_rect + 1-3 blts Ruby->C++.
          # Agora: 1 chamada de fill p/ TODAS as celulas + por CAMADA z, regulares
          # num lote e autotiles agrupados por bitmap-fonte (~10 chamadas total).
          # Ordem preservada: celulas disjuntas dentro da camada; camadas z
          # processadas por ordem com flush entre elas. Fallback: caminho original.
          $__vw_ct_ok = (Bitmap.instance_methods.include?(:compose_tiles) rescue false) if $__vw_ct_ok.nil?
          _dat_a = $__vw_ct_ok ? (mapdata.data rescue nil) : nil
          if _dat_a
            _prio_d = (@priorities.data rescue nil)
            _xy     = xsize * ysize
            _ts4444 = ((@tileset.respond_to?(:native_4444?) && @tileset.native_4444?) rescue false)
            _tsh    = VWrap::TILESET_HEIGHT
            _tsw    = VWrap::TILESET_WIDTH
            _fq = (@__vw_fq ||= []); _fq.clear
            @priorectautos.each do |tile|
              overallcount += 1
              _fq << (tile[0] * twidth - @oxLayer0) << (tile[1] * theight - @oyLayer0) << -1 << 0 << twidth << theight
            end
            bitmap.compose_tiles(@tileset, _fq) unless _fq.empty?
            _aq = (@__vw_aq ||= {})
            z = 0
            while z < zsize
              _rowz = z * _xy
              _rq = (@__vw_rq ||= []); _rq.clear
              _aq.each_value { |_q| _q.clear }
              @priorectautos.each do |tile|
                x = tile[0]; y = tile[1]
                id = _dat_a[_rowz + y * xsize + x] || 0
                next if id < 48
                next unless (_prio_d ? (_prio_d[id] || 0) : @priorities[id]) == 0
                xpos = x * twidth  - @oxLayer0
                ypos = y * theight - @oyLayer0
                if id >= 384
                  sx = ((id - 384) & 7) * @tileSrcWidth
                  sy = ((id - 384) >> 3) * @tileSrcHeight
                  if !_ts4444 && sy + @tileSrcHeight > _tsh
                    _col = sy / _tsh
                    sx = _col * _tsw + sx
                    sy = sy % _tsh
                  end
                  _rq << xpos << ypos << sx << sy << @tileSrcWidth << @tileSrcHeight
                else
                  tilebitmap = @autotileInfo[id]
                  unless tilebitmap
                    anim = autotileFrame(id)
                    next if anim < 0
                    tilebitmap = Bitmap.new(twidth, theight)
                    bltAutotile(tilebitmap, 0, 0, id, anim)
                    @autotileInfo[id] = tilebitmap
                  end
                  (_aq[id] ||= []) << xpos << ypos << 0 << 0 << twidth << theight
                end
              end
              bitmap.compose_tiles(@tileset, _rq) unless _rq.empty?
              _aq.each do |_aid, _q|
                next if _q.empty?
                _tb = @autotileInfo[_aid]
                bitmap.compose_tiles(_tb, _q) if _tb
              end
              z += 1
            end
          else
            # ---- caminho ORIGINAL (fallback) ----
            @priorectautos.each do |tile|
              x = tile[0]; y = tile[1]
              overallcount += 1
              xpos = x * twidth  - @oxLayer0
              ypos = y * theight - @oyLayer0
              bitmap.fill_rect(xpos, ypos, twidth, theight, trans)
              z = 0
              while z < zsize
                id = mapdata[x, y, z]
                z += 1
                next if !id || id < 48
                prioid = @priorities[id]
                next if prioid != 0 || !prioid
                if id >= 384
                  temprect.set(
                    ((id - 384) & 7) * @tileSrcWidth,
                    ((id - 384) >> 3) * @tileSrcHeight,
                    @tileSrcWidth, @tileSrcHeight
                  )
                  VWrap.blitVWrappedPixels(xpos, ypos, bitmap, @tileset, temprect)
                else
                  tilebitmap = @autotileInfo[id]
                  unless tilebitmap
                    anim = autotileFrame(id)
                    next if anim < 0
                    tilebitmap = Bitmap.new(twidth, theight)
                    bltAutotile(tilebitmap, 0, 0, id, anim)
                    @autotileInfo[id] = tilebitmap
                  end
                  bitmap.blt(xpos, ypos, tilebitmap, tilerect)
                end
              end
            end
          end
          Graphics.frame_reset if overallcount > 500
        else
          y_start.upto(y_end) do |y|
            x_start.upto(x_end) do |x|
              haveautotile = false
              z = 0
              while z < zsize
                id = mapdata[x, y, z]
                z += 1
                next if !id || id < 48 || id >= 384
                prioid = @priorities[id]
                next if prioid != 0 || !prioid
                fcount = @framecount[(id / 48) - 1]
                next if !fcount || fcount < 2
                unless haveautotile
                  haveautotile = true
                  overallcount += 1
                  xpos = x * twidth  - @oxLayer0
                  ypos = y * theight - @oyLayer0
                  bitmap.fill_rect(xpos, ypos, twidth, theight, trans) if overallcount <= 2000
                  break
                end
              end
            end
          end
          Graphics.frame_reset
        end
        @usedsprites = false
        return true
      end

      ($__vwp[:used] += 1; return false) if @usedsprites

      @firsttime = false
      # [PERF-RECENTRE 2026-07-04] folga DIRECIONAL: o recentre fixo (width>>2)
      # deixava so' 112px de folga a' frente -> full recompose a cada ~28 frames
      # de scroll continuo. Agora a folga vai para o lado do MOVIMENTO (1/8 atras,
      # o resto a' frente) -> recompose ~2x mais raro. So' muda QUANDO se recompoe;
      # o output visual de cada recompose e' identico.
      # [PERF-SMALLMAP] mapa cabe no buffer -> origem 0 fixa, compoe-se 1x
      _vw_small = (@map_data.xsize * @tileWidth <= width) &&
                  (@map_data.ysize * @tileHeight <= height)
      if _vw_small
        @oxLayer0 = 0
        @oyLayer0 = 0
        @__vw_small_done = true
        _vw_bias_x = @ox.floor   # pt no frame do compose (o clip-branch abaixo usa)
        _vw_bias_y = @oy.floor
      else
      _vw_slack_x = width  - @viewport.rect.width
      _vw_slack_y = height - @viewport.rect.height
      _vw_dx = @ox - (@__vw_prev_ox || @ox); @__vw_prev_ox = @ox
      _vw_dy = @oy - (@__vw_prev_oy || @oy); @__vw_prev_oy = @oy
      _vw_bx = (width  >> 3); _vw_bx = _vw_slack_x if _vw_bx > _vw_slack_x
      _vw_by = (height >> 3); _vw_by = _vw_slack_y if _vw_by > _vw_slack_y
      _vw_bias_x = _vw_dx > 0 ? _vw_bx : (_vw_dx < 0 ? (_vw_slack_x - _vw_bx) : (_vw_slack_x >> 1))
      _vw_bias_y = _vw_dy > 0 ? _vw_by : (_vw_dy < 0 ? (_vw_slack_y - _vw_by) : (_vw_slack_y >> 1))
      _vw_bias_x = 0 if _vw_bias_x < 0
      _vw_bias_y = 0 if _vw_bias_y < 0
      @oxLayer0 = (@ox - _vw_bias_x).floor
      @oyLayer0 = (@oy - _vw_bias_y).floor
      end

      if @layer0clip
        @layer0.ox = 0
        @layer0.oy = 0
        # [PERF-RECENTRE] o offset da janela = bias real (era width>>2 fixo)
        @layer0.src_rect.set(_vw_bias_x, _vw_bias_y,
                             @viewport.rect.width, @viewport.rect.height)
      else
        @layer0.ox = _vw_bias_x
        @layer0.oy = _vw_bias_y
      end
      @layer0.bitmap.clear

      x_start = @oxLayer0 / twidth
      x_start = 0 if x_start < 0
      y_start = @oyLayer0 / theight
      y_start = 0 if y_start < 0
      x_end = x_start + width  / twidth  + 1
      y_end = y_start + height / theight + 1
      x_end = xsize - 1 if x_end >= xsize
      y_end = ysize - 1 if y_end >= ysize

      if x_start <= x_end && y_start <= y_end
        # [PERF-COMPOSE 2026-07-04] o full recompose custava ~250-300ms: ~1863
        # celulas x 2 lookups Table#[] com SPLAT (~3700 Arrays alocados!) +
        # ~600-900 travessias Ruby->C++ de blt (4 interns + 4 iv_get de Rect
        # cada) + respond_to?+native_4444? POR TILE. Agora: acesso .data com
        # indice aritmetico (0 allocations), teste 4444 içado (1x), e os tiles
        # regulares vao em LOTE numa unica chamada nativa Bitmap#compose_tiles
        # por camada z (flush por camada preserva a ordem de desenho EXATA:
        # dentro de uma camada as celulas sao disjuntas; entre camadas o flush
        # garante z0 -> z1 -> z2 como o original). Fallback: motor antigo sem
        # compose_tiles -> caminho original intacto.
        $__vw_ct_ok = (Bitmap.instance_methods.include?(:compose_tiles) rescue false) if $__vw_ct_ok.nil?
        _vw_dat = $__vw_ct_ok ? (mapdata.data rescue nil) : nil
        if _vw_dat
          dat    = _vw_dat
          priot  = @priorities
          prio_d = (priot.data rescue nil)
          xy     = xsize * ysize
          tsw    = @tileSrcWidth
          tsh    = @tileSrcHeight
          ts4444 = ((@tileset.respond_to?(:native_4444?) && @tileset.native_4444?) rescue false)
          tsheig = VWrap::TILESET_HEIGHT
          tswid  = VWrap::TILESET_WIDTH
          afdiv  = (defined?(Animated_Autotiles_Frames) ? Animated_Autotiles_Frames : 8)
          quads  = (@__vw_quads ||= [])
          z = 0
          while z < zsize
            rowz = z * xy
            quads.clear
            y = y_start
            while y <= y_end
              ypos = y * theight - @oyLayer0
              rowy = rowz + y * xsize
              x = x_start
              while x <= x_end
                id = dat[rowy + x] || 0
                if id != 0 && ((prio_d ? (prio_d[id] || 0) : priot[id]) == 0)
                  if id >= 384
                    sx = ((id - 384) & 7) * tsw
                    sy = ((id - 384) >> 3) * tsh
                    # traducao VWrap inline (mesma matematica de blitVWrappedPixels)
                    if !ts4444 && sy + tsh > tsheig
                      col = sy / tsheig
                      sx  = col * tswid + sx
                      sy  = sy % tsheig
                    end
                    quads << (x * twidth - @oxLayer0) << ypos << sx << sy << tsw << tsh
                  elsif id >= 48
                    # [PERF-FULLAUTO 2026-07-04] autotiles do FULL tambem em lote:
                    # os vizinhos pequenos com agua recompunham ~1x/frame a ~19ms
                    # (centenas de bltAutotile por recompose = 5-10ms/frame na rua).
                    # Cache @autotileInfo por id + grupos por fonte + flush por
                    # camada (celulas disjuntas -> ordem exata preservada).
                    _tb = @autotileInfo[id]
                    unless _tb
                      _anim = autotileFrame(id)
                      if _anim >= 0
                        _tb = Bitmap.new(twidth, theight)
                        bltAutotile(_tb, 0, 0, id, _anim)
                        @autotileInfo[id] = _tb
                      end
                    end
                    ((@__vw_faq ||= {})[id] ||= []) << (x * twidth - @oxLayer0) << ypos << 0 << 0 << twidth << theight if _tb
                  end
                end
                x += 1
              end
              y += 1
            end
            bitmap.compose_tiles(@tileset, quads) unless quads.empty?
            if @__vw_faq
              @__vw_faq.each do |_aid, _q|
                next if _q.empty?
                _tb2 = @autotileInfo[_aid]
                bitmap.compose_tiles(_tb2, _q) if _tb2
                _q.clear
              end
            end
            z += 1
          end
          Graphics.frame_reset
        else
          # ---- caminho ORIGINAL (fallback p/ motor sem compose_tiles) ----
          tmprect = Rect.new(0, 0, 0, 0)
          z = 0
          while z < zsize
            y = y_start
            while y <= y_end
              ypos = y * theight - @oyLayer0
              x = x_start
              while x <= x_end
                xpos = x * twidth - @oxLayer0
                id = mapdata[x, y, z]
                if id && id != 0
                  prioid = @priorities[id]
                  if prioid == 0 || prioid
                    unless prioid != 0
                      if id >= 384
                        tmprect.set(
                          ((id - 384) & 7) * @tileSrcWidth,
                          ((id - 384) >> 3) * @tileSrcHeight,
                          @tileSrcWidth, @tileSrcHeight
                        )
                        VWrap.blitVWrappedPixels(xpos, ypos, bitmap, @tileset, tmprect)
                      else
                        frames = @framecount[(id / 48) - 1]
                        frame = if !frames || frames <= 1
                                  0
                                else
                                  (Graphics.frame_count / (defined?(Animated_Autotiles_Frames) ? Animated_Autotiles_Frames : 8)) % frames
                                end
                        bltAutotile(bitmap, xpos, ypos, id, frame)
                      end
                    end
                  end
                end
                x += 1
              end
              y += 1
            end
            z += 1
          end
          Graphics.frame_reset
        end
      end
      true
    end

  end # class CustomTilemap

  puts "[VWrap 3DS] CustomTilemap patched. Max tileset height: #{VWrap::MAX_TEX_SIZE_BOOSTED}px"

end # if $MKXP && CustomTilemap
