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
# [MKXP-EARLY b] guard corrigido: no mruby defined?(Const) NAO devolve "constant"
# (devolve "expression") -> a comparacao antiga era FALSA para sempre.
if $MKXP == true && Object.const_defined?(:CustomTilemap)

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
        @__vw_anim_gate = nil   # [TILEANIM-GATE] tileset novo -> reavaliar animacao
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
      @__vw_anim_gate = nil   # [TILEANIM-GATE] tileset novo -> reavaliar animacao
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
      # [VWALIVE] sonda 1x: prova que ESTE refreshLayer0 (VWrap) e' o ativo
      unless $__vw_alive
        $__vw_alive = true
        MKXPDebug.log("[VWALIVE] refreshLayer0 do VWrap ATIVO (1a chamada)") rescue nil
      end
      return true if autotiles && !shown?
      ($__vwp ||= Hash.new(0))
      # [VWCALL] censo de caminhos 1x/600 (independente do full -> nao falha nunca)
      $__vwp[:call] = ($__vwp[:call] || 0) + 1
      $__vwp[:c_auto] = ($__vwp[:c_auto] || 0) + 1 if autotiles
      if ($__vwp[:call] % 600) == 1
        MKXPDebug.log("[VWCALL] call=#{$__vwp[:call]} (auto=#{$__vwp[:c_auto] || 0}) | full=#{$__vwp[:full]} incr=#{$__vwp[:incr]} adopt=#{$__vwp[:adopt]} fast=#{$__vwp[:fast]} small=#{$__vwp[:small]} why[zero=#{$__vwp[:i_zero]} dxbig=#{$__vwp[:i_dxbig]} prevnil=#{$__vwp[:i_prevnil]} buf=#{$__vwp[:i_buf]} ts=#{$__vwp[:i_ts]} tschg=#{$__vwp[:i_tschg]} go=#{$__vwp[:i_go]}] auto_path=#{$__vwp[:auto]} used=#{$__vwp[:used]} incrfail=#{$__vwp[:incrfail]}") rescue nil
      end
      # [L0CACHE 2026-07-05] ADOCAO do buffer composto: os tilemaps dos VIZINHOS
      # sao destruidos/recriados a cada cruzamento de fronteira (full=241 vs
      # incr=20 no censo!). O dispose (wrap abaixo) resgata o buffer p/
      # $__vw_l0c[map_data.__id__]; aqui, o tilemap NOVO do MESMO mapa adota-o
      # (mesmo Game_Map->mesmo data; mesmo tileset compactado persistente) ->
      # zero full na recriacao; o INCR/fast cobrem o delta de camara.
      # Gate $MKXP_L0CACHE=false reverte.
      # [VWFULL-VALID 2026-07-11] guards novos no adopt: (a) so' adotar buffer
      # cujo compose foi PROVADO ok (:ok — sem isto um buffer preto de um full
      # falhado era cacheado e re-adotado = falha transitoria virava permanente);
      # (b) nunca adotar um husk (pixels=NULL apos strip/OOM) — __has_pixels?
      # e' o predicado nativo novo; em build antigo o stub devolve 0 (truthy) e
      # o guard passa como antes.
      if $MKXP_L0CACHE != false && @firsttime && !autotiles &&
         ($__vw_l0c ||= {}) && (_lc = $__vw_l0c[@map_data.__id__]) &&
         _lc[:ok] &&
         _lc[:ts].equal?(@tileset) && !(_lc[:bmp].disposed? rescue true) &&
         ((_lc[:bmp].__has_pixels? != false) rescue true) &&
         _lc[:bmp].width == @layer0.bitmap.width &&
         _lc[:bmp].height == @layer0.bitmap.height
        (@layer0.bitmap.dispose rescue nil)
        @layer0.bitmap = _lc[:bmp]
        @oxLayer0 = _lc[:oxL]
        @oyLayer0 = _lc[:oyL]
        @__vw_small_done = _lc[:small]
        @__vw_prev_oxL = _lc[:oxL]
        @__vw_prev_oyL = _lc[:oyL]
        @__vw_prev_buf = _lc[:bmp]
        @__vw_prev_ts  = @tileset
        @firsttime = false
        @tilesetchanged = false
        @__vw_composed_ok = true
        $__vw_l0c.delete(@map_data.__id__)
        $__vwp[:adopt] += 1
      end

      # [L0RECLAIM 2026-07-11] layer0 novo SEM PIXELS: na travessia de fronteira
      # o Bitmap.new(832x704) do tilemap NOVO corre ANTES do dispose do antigo
      # -> os 2 slots eternos podem estar ambos ocupados (1 no tilemap velho
      # ainda vivo + 1 estacionado no $__vw_l0c) e a alocacao normal esta' rota
      # (malloc residente + linear fragmentado) -> pixels=NULL -> compose
      # falharia em silencio p/ SEMPRE. Recuperacao: evictar o cache (liberta o
      # slot estacionado) e recriar o bitmap. Em build antigo __has_pixels? e'
      # stub 0 (truthy) -> "== false" falha -> bloco nunca corre (seguro).
      begin
        if !autotiles && @layer0 && @layer0.bitmap &&
           (@layer0.bitmap.__has_pixels? == false)
          if $__vw_l0c && !$__vw_l0c.empty?
            $__vw_l0c.each_value { |_e| (_e[:bmp].dispose rescue nil) if _e && _e[:bmp] }
            $__vw_l0c.clear
          end
          _w0 = @layer0.bitmap.width; _h0 = @layer0.bitmap.height
          (@layer0.bitmap.dispose rescue nil)
          @layer0.bitmap = Bitmap.new(_w0, _h0)
          @firsttime = true          # o buffer novo esta' vazio: forcar full
          @__vw_prev_oxL = nil       # invalida INCR/fast (referiam o buffer morto)
          @__vw_prev_buf = nil
          @__vw_small_done = false
          $__l0rec = ($__l0rec || 0) + 1
          if $__l0rec <= 12
            MKXPDebug.log("[L0RECLAIM] ##{$__l0rec} layer0 estava SEM PIXELS -> cache evictado + bitmap #{_w0}x#{_h0} recriado (px=#{(@layer0.bitmap.__has_pixels? rescue '?')})") rescue nil
          end
        end
      rescue Exception
      end

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
        (MKXPDebug.log("[VWPATH] full=#{$__vwp[:full]} incr=#{$__vwp[:incr]} fast=#{$__vwp[:fast]} small=#{$__vwp[:small]} auto=#{$__vwp[:auto]} used=#{$__vwp[:used]} | firsttime=#{@firsttime.inspect} usedspr=#{@usedsprites.inspect} tschg=#{(@tilesetchanged rescue '?').inspect} small_done=#{@__vw_small_done.inspect} map=#{(@map_data.xsize rescue '?')}x#{(@map_data.ysize rescue '?')} buf=#{@layer0.bitmap.width}x#{@layer0.bitmap.height} ox=#{@ox.to_i} oxL=#{@oxLayer0.to_i}") rescue nil)
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

        unless $__vw_ap_said2
          $__vw_ap_said2 = true
          (MKXPDebug.log("[AUTOPASS] passe autotiles: fully=#{@fullyrefreshedautos.inspect} ap_ok=#{$__vw_ap_ok.inspect} ct_ok=#{$__vw_ct_ok.inspect}") rescue nil)
        end
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
          $__vw_ap_ok = (Bitmap.instance_methods.include?(:compose_autopass) rescue false) if $__vw_ap_ok.nil?
          _dat_a = ($__vw_ct_ok || $__vw_ap_ok) ? (mapdata.data rescue nil) : nil
          _prio_ap = _dat_a ? (@priorities.data rescue nil) : nil
          unless $__vw_ap_said
            $__vw_ap_said = true
            (MKXPDebug.log("[AUTOPASS] ap_ok=#{$__vw_ap_ok.inspect} ct_ok=#{$__vw_ct_ok.inspect} dat=#{!_dat_a.nil?} prio=#{!_prio_ap.nil?} -> caminho=#{(_dat_a && _prio_ap && $__vw_ap_ok) ? 'NATIVO' : (_dat_a ? 'AUTOBATCH' : 'ORIGINAL')}") rescue nil)
          end
          if _dat_a && _prio_ap && $__vw_ap_ok
            # ── [PERF-AUTOPASS 2026-07-05] o passe INTEIRO numa chamada nativa ──
            # A construcao dos quads (celulas x z0-2 x ~10 ops Ruby) saiu p/ C++.
            # Aqui so': celulas planas (cache por retangulo), PRE-criacao dos
            # bitmaps de autotile em falta, e 1 travessia nativa.
            _xy2 = xsize * ysize
            _ts4b = ((@tileset.respond_to?(:native_4444?) && @tileset.native_4444?) rescue false)
            if @__vw_ap_rect != @priorect
              _c = (@__vw_ap_cells ||= []); _c.clear
              _ids = {}
              @priorectautos.each do |t|
                _ax = t[0]; _ay = t[1]
                _c << _ax << _ay
                next if _ax < 0 || _ay < 0 || _ax >= xsize || _ay >= ysize
                z = 0
                while z < zsize
                  id = _dat_a[z * _xy2 + _ay * xsize + _ax] || 0
                  _ids[id] = true if id >= 48 && id < 384 && (_prio_ap[id] || 0) == 0
                  z += 1
                end
              end
              @__vw_ap_ids  = _ids.keys
              @__vw_ap_rect = @priorect.dup
            end
            # PRE-criar autotile-bitmaps em falta (o C++ NAO cria, so compoe);
            # apos cada tick de animacao o @autotileInfo foi limpo -> recria os
            # ~5-20 ids distintos (mesmo custo lazy do caminho atual)
            @__vw_ap_ids.each do |id|
              next if @autotileInfo[id]
              anim = autotileFrame(id)
              next if anim < 0
              _tb = Bitmap.new(twidth, theight)
              bltAutotile(_tb, 0, 0, id, anim)
              @autotileInfo[id] = _tb
            end
            _pr = (@__vw_ap_params ||= []); _pr.clear
            _pr << xsize << ysize << zsize << twidth << theight <<
                   @oxLayer0 << @oyLayer0 << (_ts4b ? 1 : 0) <<
                   VWrap::TILESET_HEIGHT << VWrap::TILESET_WIDTH <<
                   @tileSrcWidth << @tileSrcHeight
            _miss = (@__vw_ap_miss ||= []); _miss.clear
            overallcount += bitmap.compose_autopass(@tileset, @__vw_ap_cells,
                              _dat_a, _prio_ap, @autotileInfo, _pr, _miss)
            unless _miss.empty?
              _miss.each do |id|
                next if @autotileInfo[id]
                anim = autotileFrame(id)
                next if anim < 0
                _tb = Bitmap.new(twidth, theight)
                bltAutotile(_tb, 0, 0, id, anim)
                @autotileInfo[id] = _tb
              end
              @__vw_ap_ids |= _miss
              bitmap.compose_autopass(@tileset, @__vw_ap_cells, _dat_a,
                                      _prio_ap, @autotileInfo, _pr)
            end
          elsif _dat_a
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
      # [TILE-INCR 2026-07-05] recompose INCREMENTAL na fronteira: em vez de
      # clear+~600 celulas x3 camadas (~15-30ms), shift do buffer retido
      # (memmove) + compor SO' as bandas expostas (+1 celula de margem
      # anti-seam) via compose_autopass (fill+draw POR CELULA -> idempotente).
      # Gate $MKXP_TILE_INCR=false reverte.
      $__vw_incr_ok = ((Bitmap.instance_methods.include?(:shift_pixels) &&
                        Bitmap.instance_methods.include?(:compose_autopass)) rescue false) if $__vw_incr_ok.nil?
      # [INCRWHY] nomear o guard que impede o incremental (12 incr vs 145 fulls!)
      begin
        _iw = if $MKXP_TILE_INCR == false then :i_off
              elsif !$__vw_incr_ok then :i_nook
              elsif _vw_small then :i_small
              elsif (@tilesetchanged rescue false) then :i_tschg
              elsif @usedsprites then :i_used
              elsif !@__vw_prev_oxL then :i_prevnil
              elsif !@__vw_prev_buf.equal?(bitmap) then :i_buf
              elsif !@__vw_prev_ts.equal?(@tileset) then :i_ts
              else
                _dxq = @oxLayer0 - @__vw_prev_oxL
                _dyq = @oyLayer0 - @__vw_prev_oyL
                if _dxq == 0 && _dyq == 0 then :i_zero
                elsif _dxq.abs >= width - twidth || _dyq.abs >= height - theight then :i_dxbig
                else :i_go
                end
              end
        $__vwp[_iw] += 1
      rescue Exception
      end
      _did_incr = false
      if $MKXP_TILE_INCR != false && $__vw_incr_ok && !_vw_small &&
         !(@tilesetchanged rescue false) && !@usedsprites &&
         @__vw_prev_oxL && @__vw_prev_buf.equal?(bitmap) &&
         @__vw_prev_ts.equal?(@tileset)
        _dxp = @oxLayer0 - @__vw_prev_oxL
        _dyp = @oyLayer0 - @__vw_prev_oyL
        if (_dxp != 0 || _dyp != 0) && _dxp.abs < width - twidth && _dyp.abs < height - theight
          _dat_i = (mapdata.data rescue nil)
          _pri_i = (@priorities.data rescue nil)
          if _dat_i && _pri_i
            bitmap.shift_pixels(-_dxp, -_dyp)
            # bandas expostas em px do referencial NOVO, +1 celula p/ dentro
            # (a celula da borda antiga estava cortada pelo limite do buffer)
            _bx0 = nil; _bx1 = nil; _by0 = nil; _by1 = nil
            if _dxp > 0 then _bx0 = width - _dxp - twidth; _bx1 = width
            elsif _dxp < 0 then _bx0 = 0; _bx1 = -_dxp + twidth end
            if _dyp > 0 then _by0 = height - _dyp - theight; _by1 = height
            elsif _dyp < 0 then _by0 = 0; _by1 = -_dyp + theight end
            _cells = (@__vw_ic ||= []); _cells.clear
            _ids_i = {}
            _xs0 = @oxLayer0 / twidth;  _xs0 = 0 if _xs0 < 0
            _ys0 = @oyLayer0 / theight; _ys0 = 0 if _ys0 < 0
            _xs1 = _xs0 + width / twidth + 1;  _xs1 = xsize - 1 if _xs1 >= xsize
            _ys1 = _ys0 + height / theight + 1; _ys1 = ysize - 1 if _ys1 >= ysize
            _xy_i = xsize * ysize
            _y = _ys0
            while _y <= _ys1
              _py = _y * theight - @oyLayer0
              _iny = _by0 && _py + theight > _by0 && _py < _by1
              _x = _xs0
              while _x <= _xs1
                _px = _x * twidth - @oxLayer0
                if _iny || (_bx0 && _px + twidth > _bx0 && _px < _bx1)
                  _cells << _x << _y
                  z = 0
                  while z < zsize
                    _idc = _dat_i[z * _xy_i + _y * xsize + _x] || 0
                    _ids_i[_idc] = true if _idc >= 48 && _idc < 384 && (_pri_i[_idc] || 0) == 0
                    z += 1
                  end
                end
                _x += 1
              end
              _y += 1
            end
            _ids_i.each_key do |_ai|
              next if @autotileInfo[_ai]
              _an = autotileFrame(_ai)
              next if _an < 0
              _tb3 = Bitmap.new(twidth, theight)
              bltAutotile(_tb3, 0, 0, _ai, _an)
              @autotileInfo[_ai] = _tb3
            end
            _ts4i = ((@tileset.respond_to?(:native_4444?) && @tileset.native_4444?) rescue false)
            _pri2 = (@__vw_ip ||= []); _pri2.clear
            _pri2 << xsize << ysize << zsize << twidth << theight <<
                     @oxLayer0 << @oyLayer0 << (_ts4i ? 1 : 0) <<
                     VWrap::TILESET_HEIGHT << VWrap::TILESET_WIDTH <<
                     @tileSrcWidth << @tileSrcHeight
            _miss_i = (@__vw_im ||= []); _miss_i.clear
            _rc_i = bitmap.compose_autopass(@tileset, _cells, _dat_i, _pri_i,
                                            @autotileInfo, _pri2, _miss_i)
            unless _miss_i.empty?
              _miss_i.each do |_ai|
                next if @autotileInfo[_ai]
                _an = autotileFrame(_ai)
                next if _an < 0
                _tb3 = Bitmap.new(twidth, theight)
                bltAutotile(_tb3, 0, 0, _ai, _an)
                @autotileInfo[_ai] = _tb3
              end
              bitmap.compose_autopass(@tileset, _cells, _dat_i, _pri_i,
                                      @autotileInfo, _pri2)
            end
            # celulas por desenhar + retorno 0 = falha interna silenciosa do
            # passe -> NAO validar o incremental; cai no clear+full abaixo
            if _cells.empty? || (_rc_i.is_a?(Integer) && _rc_i > 0)
              _did_incr = true
              $__vwp[:incr] += 1
            else
              $__vwp[:incrfail] += 1
            end
          end
        end
      end
      if _did_incr
        @__vw_prev_oxL = @oxLayer0
        @__vw_prev_oyL = @oyLayer0
        Graphics.frame_reset
        return true
      end
      bitmap.clear
      @__vw_prev_oxL = @oxLayer0
      @__vw_prev_oyL = @oyLayer0
      @__vw_prev_buf = bitmap
      @__vw_prev_ts  = @tileset
      # [TSCHG-CLEAR] o flag minusculo e' set pelo tileset= do VWrap e NUNCA era
      # limpo -> small path e TILE-INCR nunca ativavam (small=0, incr=0 no censo).
      # So' o VWrap le este nome (o 075 usa @tilesetChanged camelCase) -> seguro
      # limpa'-lo aqui: o full-recompose que se segue absorve a mudanca de tileset.
      @tilesetchanged = false
      # [VWFULL-VALID] o full deixou de ser descartavel: qualquer compose_tiles
      # que devolva nil (guard C++: fonte/destino sem pixels) marca a passagem
      # como FALHADA e o bloco no fim re-arma @firsttime + invalida o estado.
      _vw_full_ok = true

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
            unless quads.empty?
              # [VWFULL-VALID] nil = guard C++ (fonte/destino sem pixels)
              _vw_full_ok = false if bitmap.compose_tiles(@tileset, quads).nil?
            end
            if @__vw_faq
              @__vw_faq.each do |_aid, _q|
                next if _q.empty?
                _tb2 = @autotileInfo[_aid]
                _vw_full_ok = false if _tb2 && bitmap.compose_tiles(_tb2, _q).nil?
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
      # [VWFULL-VALID 2026-07-11] veredito do full: antes, o full consumia
      # @firsttime e cometia @__vw_prev_* ANTES de saber se o compose escreveu
      # um unico pixel; com o guard C++ a devolver nil em silencio, UMA falha
      # = chao preto PERMANENTE (o fast path nunca re-corre o full) e o
      # L0CACHE ainda resgatava/re-adotava o buffer preto. Agora: falha ->
      # re-arma @firsttime + invalida prev (sem INCR, sem fast, sem resgate)
      # -> o proximo refresh re-tenta o full do zero, ja com a fonte
      # possivelmente recarregada ([NCACHE-RETRY]/[L0RECLAIM]).
      # [COMPOSE-RET-GATE 2026-07-13] o motor de 5-Jul devolve nil no compose_tiles
      # MESMO em SUCESSO -> o VWFULL-VALID (usa .nil?) julgava TODO o full falhado ->
      # re-armava @firsttime -> full-redraw eterno (incr=0) -> 40-60ms/frame -> 10 FPS.
      # Neste motor o mapa renderiza bem (nil nao distingue sucesso de falha), por isso
      # NAO re-armar por defeito. $MKXP_VWFULL_REARM=true reativa (p/ motor de 12-Jul).
      if _vw_full_ok || !$MKXP_VWFULL_REARM
        @__vw_composed_ok = true
      else
        @firsttime = true
        @__vw_prev_oxL = nil
        @__vw_prev_oyL = nil
        @__vw_prev_buf = nil
        @__vw_prev_ts  = nil
        @__vw_small_done = false
        @__vw_composed_ok = false
        $__vwp[:fullfail2] = ($__vwp[:fullfail2] || 0) + 1
        if $__vwp[:fullfail2] <= 3 || ($__vwp[:fullfail2] % 120) == 0
          MKXPDebug.log("[VWFULL-FAIL] ##{$__vwp[:fullfail2]} compose devolveu nil -> firsttime re-armado, prev invalidado (razao no [COMPOSE-NIL] do motor)") rescue nil
        end
      end
      true
    end

    # [TONEGUARD 2026-07-05] o update do 075 compara `@oldtone != @tone` — em
    # mruby a comparacao de Tone/Color e' por IDENTIDADE (sem == de valor) ->
    # TRUE todos os frames -> re-aplica tone E color a TODOS os @tiles e
    # @autosprites (200-270 setters/frame x3 tilemaps = os 5ms/chamada do
    # SSPROF). Corpo fiel ao 075 com guard por COMPONENTES (so' aplica quando o
    # VALOR muda; day/night tint continua a funcionar). $MKXP_TONEGUARD=false
    # repoe a comparacao antiga.
    def update
      # [EPOCH-TOUCH 2026-07-11] o tileset do mapa ATIVO conta como "lido agora"
      # TODOS os frames: os caminhos fast/small do refreshLayer0 nao compoem
      # nada -> sem isto o epoch estagnava e o GIANT-LRU despojava o tileset EM
      # USO quando um gigante vizinho carregava (compose seguinte saia no guard
      # em silencio = chao preto). rescue: build antigo sem __touch.
      (@tileset.__touch if @tileset) rescue nil
      if @haveGraphicsWH
        @graphicsWidth  = Graphics.width
        @graphicsHeight = Graphics.height
      end
      _tn = @tone
      _tchg = ($MKXP_TONEGUARD == false) ? (@oldtone != _tn) :
              (@__vwu_tr != _tn.red || @__vwu_tg != _tn.green ||
               @__vwu_tb != _tn.blue || @__vwu_tgy != _tn.gray)
      if _tchg
        @layer0.tone = _tn
        @flash.tone  = _tn if @flash
        for sprite in @autosprites
          sprite.tone = _tn if sprite.is_a?(Sprite)
        end
        for sprite in @tiles
          sprite.tone = _tn if sprite.is_a?(Sprite)
        end
        @oldtone = _tn.clone
        @__vwu_tr = _tn.red; @__vwu_tg = _tn.green
        @__vwu_tb = _tn.blue; @__vwu_tgy = _tn.gray
      end
      _cl = @color
      _cchg = ($MKXP_TONEGUARD == false) ? (@oldcolor != _cl) :
              (@__vwu_cr != _cl.red || @__vwu_cg != _cl.green ||
               @__vwu_cb != _cl.blue || @__vwu_ca != _cl.alpha)
      if _cchg
        @layer0.color = _cl
        @flash.color  = _cl if @flash
        for sprite in @autosprites
          sprite.color = _cl if sprite.is_a?(Sprite)
        end
        for sprite in @tiles
          sprite.color = _cl if sprite.is_a?(Sprite)
        end
        @oldcolor = _cl.clone
        @__vwu_cr = _cl.red; @__vwu_cg = _cl.green
        @__vwu_cb = _cl.blue; @__vwu_ca = _cl.alpha
      end
      if @autotiles.changed
        refresh_autotiles
        repaintAutotiles
      end
      refresh_flash if @flashChanged
      refresh_tileset if @tilesetChanged
      @flash.opacity = FlashOpacity[(Graphics.frame_count / 2) % 6] if @flash
      mustrefresh = (@oldOx != @ox || @oldOy != @oy || @tilesetChanged || @autotiles.changed)
      if @viewport.ox != @oldViewportOx || @viewport.oy != @oldViewportOy
        mustrefresh = true
        @oldViewportOx = @viewport.ox
        @oldViewportOy = @viewport.oy
      end
      refresh if mustrefresh
      if (Graphics.frame_count % Animated_Autotiles_Frames) == 0 || @nowshown
        # [TILEANIM-GATE 2026-07-11] mapas SEM autotiles animados (todos os
        # framecount<=1) nao pagam o repaint periodico (~cada 8 frames): era
        # trabalho Ruby constante em TODOS os tilemaps vivos (atual+vizinhos),
        # mesmo em interiores sem agua. Calculado 1x por tilemap; tileset= do
        # VWrap repoe @__vw_anim_gate=nil. Default true em qualquer duvida.
        if @__vw_anim_gate.nil?
          begin
            _fc = @framecount
            @__vw_anim_gate = _fc.is_a?(Array) ? !!_fc.find { |f| f && f > 1 } : true
          rescue Exception
            @__vw_anim_gate = true
          end
        end
        if @__vw_anim_gate || @nowshown
          repaintAutotiles
          refresh(true)
        end
      end
      @nowshown          = false
      @autotiles.changed = false
      @tilesetChanged    = false
    end

    # [L0CACHE] resgate do buffer composto ANTES do dispose original o matar:
    # troca-o por um dummy 4x4 (o original faz @layer0.bitmap.dispose explicito).
    # Cap de 4 buffers (~4MB cada) — o mais antigo e' disposto ao exceder.
    begin
      alias_method :__vw_l0c_dispose, :dispose
      def dispose
        begin
          # [L0DBG] adopt=0 no censo -> saber se o dispose sequer corre e porque
          # o resgate falha (raro: so' em transicoes)
          $__l0d = ($__l0d || 0) + 1
          if $__l0d <= 8
            MKXPDebug.log("[L0DBG] dispose ##{$__l0d}: disposed?=#{(disposed? rescue '?')} layer0=#{!@layer0.nil?} l0disp=#{(@layer0 ? @layer0.disposed? : '?') rescue '?'} bmp=#{(@layer0 && @layer0.bitmap ? true : false) rescue '?'} prev=#{!@__vw_prev_oxL.nil?} md=#{!@map_data.nil?}") rescue nil
          end
          # [VWFULL-VALID] so' resgatar buffers com compose PROVADO ok e com
          # pixels vivos — um buffer preto (full falhado) ou husked nunca mais
          # entra no cache (era a "lavagem" que tornava a falha permanente).
          if $MKXP_L0CACHE != false && !disposed? && @layer0 && !@layer0.disposed? &&
             @layer0.bitmap && !(@layer0.bitmap.disposed? rescue true) &&
             @__vw_prev_oxL && @map_data && @__vw_composed_ok &&
             ((@layer0.bitmap.__has_pixels? != false) rescue true)
            ($__vw_l0c ||= {})[@map_data.__id__] = {
              :bmp => @layer0.bitmap, :oxL => @__vw_prev_oxL, :oyL => @__vw_prev_oyL,
              :ts => @tileset, :small => @__vw_small_done, :ok => true
            }
            # [ATLAS-HANDOFF 2026-07-11] o buffer estacionado NAO precisa de
            # textura (so' de pixels); levar a textura com ele prendia a POSSE
            # do ATLAS-ETERNO 1024x1024 -> o layer0 do mapa NOVO caia no
            # C3D_TexInit de 4MB que falha sempre = chao preto no 2o mapa
            # (provado: TEXDIAG 832x704 FALHOU->CHAO_PRETO em loop no log 18:4X).
            # Largar aqui: o atlas passa ao tilemap novo; re-adopt re-textura.
            (@layer0.bitmap.__drop_texture rescue nil)
            @layer0.bitmap = Bitmap.new(4, 4)
            # [v2] cap 1 (era 4): 4 buffers = +9MB de LINEAR retido — o SUPERDBG
            # apanhou linear_livre=2MB/TEX=24MB. 1 buffer cobre o vai-e-vem comum
            # (casa<->rua) e custa 2.3MB no maximo.
            while $__vw_l0c.size > 1
              _k0 = $__vw_l0c.keys.first
              _e0 = $__vw_l0c.delete(_k0)
              (_e0[:bmp].dispose rescue nil) if _e0
            end
          end
        rescue Exception
        end
        __vw_l0c_dispose
      end
    rescue Exception
    end

  end # class CustomTilemap

  puts "[VWrap 3DS] CustomTilemap patched. Max tileset height: #{VWrap::MAX_TEX_SIZE_BOOSTED}px"

end # if $MKXP && CustomTilemap
