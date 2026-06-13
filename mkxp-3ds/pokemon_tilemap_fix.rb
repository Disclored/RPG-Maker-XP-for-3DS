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
        return true
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

      return false if @usedsprites

      @firsttime = false
      @oxLayer0 = (@ox - (width  >> 2)).floor
      @oyLayer0 = (@oy - (height >> 2)).floor

      if @layer0clip
        @layer0.ox = 0
        @layer0.oy = 0
        @layer0.src_rect.set(width >> 2, height >> 2,
                             @viewport.rect.width, @viewport.rect.height)
      else
        @layer0.ox = width  >> 2
        @layer0.oy = height >> 2
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
                  # prioid == 0 → desenhar
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
      true
    end

  end # class CustomTilemap

  puts "[VWrap 3DS] CustomTilemap patched. Max tileset height: #{VWrap::MAX_TEX_SIZE_BOOSTED}px"

end # if $MKXP && CustomTilemap
