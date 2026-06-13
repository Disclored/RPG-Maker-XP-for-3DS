#==============================================================================#
#                            Enhanced Jukebox                                  #
#                                by Marin                                      #
#                   Commissioned by Derxwna Kapsyla                            #
#                       Graphics by Derxwna Kapsyla                            #
#     Additional sssets from The Gensou Ningyou Enbu Asset Collection Pack     #
#==============================================================================#
#                              Dependencies                                    #
#                                                                              #
#                     - Marin's Scripting Utilities:                           #
#                       https://reliccastle.com/r/165/                         #
#                     - OggDecoder:                                            #
#                       Bundled with this plugin.                              #
#==============================================================================#
#                       Credit is always appreciated                           #
#==============================================================================#

class EnhancedJukebox
  # Some constants to more easily reconfigure the UI if you have a different screenstyle, or
  # other wishes for how some the elements interact.

  # The base color of all the drawn text throughout the jukebox UI.
  BASECOLOR = Color.new(80, 80, 80)

  # The shadow color of all the drawn text throughout the jukebox UI.
  SHADOWCOLOR = Color.new(160, 160, 160)

  # The number of items that is drawn on the screen at once.
  ITEMS_DRAWN = 8

  # Means that the screen will start scrolling on the x-th item from the top or bottom.
  # Setting it to 1 means it scrolls when you're on the first or last item,
  # 2 means it starts on the second or second-to-last item, etc.
  SCROLL_FROM = 2

  # Displayed whenever a track has not yet been encountered, and would thus be considered a spoiler.
  # Determined by the SHOWIFVARIABLE, SHOWIFSWITCH, and SHOWIFLOCATION tags in the file metadata.
  SPOILER_TEXT = "?????"

  # Displayed before the title of the currently playing song. This text is not scrolled.
  # To not show any text in front of the title, leave this as "".
  NOW_PLAYING_TEXT = "Playing: "


  # All about scrolling delays, frequencies, and increments.

  # The number of pixels to scroll by each frame when scrolling the page in the song screen.
  PAGE_SCROLL_INCREMENT = 3

  # Wait this number of frames before the currently playing song text starts scrolling.
  TEXT_SCROLL_DELAY = 80

  # This is the frame interval in between two scroll updates.
  TEXT_SCROLL_FREQUENCY = 1

  # This is the number of pixels to scroll the text by whenever a scroll update happens.
  TEXT_SCROLL_INCREMENT = 1

  # Thus you could say the text will scroll at a speed of TEXT_SCROLL_INCREMENT (px) / TEXT_SCROLL_FREQUENCY (frames), i.e. pixels per frame

  def initialize
    showBlk
    @path = "Graphics/Pictures/Enhanced Jukebox/"
    @viewport = Viewport.new(0, 0, Graphics.width, Graphics.height)
    @viewport.z = 99999
    @sprites = SpriteHash.new
    @sprites["bg"] = Sprite.new(@viewport)
    @sprites["bg"].bitmap = Bitmap.new(@path + "bg")
    @sprites["playingtext"] = TextSprite.new(@viewport)
    if NOW_PLAYING_TEXT != ""
      @sprites["playingtext"].draw(
          [NOW_PLAYING_TEXT, 26, 330, 0, BASECOLOR, SHADOWCOLOR]
      )
      size = @sprites["playingtext"].bitmap.text_size(NOW_PLAYING_TEXT)
      x = 26 + size.width + 8
      w = Graphics.width - x - 20
      @scroll = ScrollingMusicText.new(TEXT_SCROLL_DELAY, x, 336, w, 32)
    else
      @scroll = ScrollingMusicText.new(TEXT_SCROLL_DELAY, 20, 336, 472, 32)
    end
    @fe = FileExplorer.new
    @songscreen = nil
    @brk = false
    update_song
    hideBlk
    main
  end

  def update_song
    name = $PokemonTemp.defaultBGM&.name || "Default Music"
    name = name.split("/")[-1]
    name = name.split(".")[0..-2].join(".") if name.include?(".")
    @scroll.text = name
  end

  def update
    Graphics.update
    Input.update
    @sprites.update
    @fe.update
    @songscreen&.update
    @scroll.update(TEXT_SCROLL_FREQUENCY, TEXT_SCROLL_INCREMENT)
  end

  def main
    loop do
      update
      break if @brk
      if @songscreen.nil?
        ret = @fe.main
        if ret == false
          @brk = true
        elsif ret.nil?
          update_song
        elsif ret.is_a?(Array)
          if ret[1] == :folder
            # Change folders
          elsif ret[1] == :file
            # Show song screen
            @fe.hide
            @songscreen = SongScreen.new(@fe.folder + "/" + (ret[3] || ret[0]), ret[2])
            update_song
          end
        end
      else
        if !@songscreen.main
          # Close song screen and go back to file explorer
          @songscreen.dispose
          @songscreen = nil
          @fe.show
        end
      end
    end
    dispose
  end

  def dispose
    showBlk
    @fe.dispose
    @scroll.dispose
    @sprites.dispose
    @viewport.dispose
    hideBlk
  end

  class FileExplorer
    attr_reader :folder

    def initialize
      @path = "Graphics/Pictures/Enhanced Jukebox/"
      @viewport = Viewport.new(16, 52, 480, 264)
      @viewport.z = 99999
      @arrowvp = Viewport.new(16, 52, 480, 264)
      @arrowvp.z = 100000
      @sprites = SpriteHash.new
      @sprites["bg"] = Sprite.new(@viewport)
      @sprites["bg"].bitmap = Bitmap.new(@path + "list_bg")
      @sprites["text"] = TextSprite.new(@viewport)
      @sprites["sel"] = Sprite.new(@viewport)
      @sprites["sel"].bitmap = Bitmap.new("Graphics/Pictures/selarrow")
      @sprites["sel"].x = 10
      @sprites["sel"].y = 4
      @sprites["icons"] = SpriteHash.new
      @sprites["up"] = Sprite.new(@arrowvp)
      @sprites["up"].bitmap = Bitmap.new("Graphics/Pictures/uparrow")
      @sprites["up"].src_rect.width = 28
      @sprites["up"].visible = false
      @sprites["up"].x = @viewport.rect.width - @sprites["up"].src_rect.width - 8
      @sprites["down"] = Sprite.new(@arrowvp)
      @sprites["down"].bitmap = Bitmap.new("Graphics/Pictures/downarrow")
      @sprites["down"].src_rect.width = 28
      @sprites["down"].visible = false
      @sprites["down"].x = @sprites["up"].x
      @sprites["down"].y = @viewport.rect.height - @sprites["down"].src_rect.height
      @items = []
      @scrolling_items = []
      @top = 0
      @sel = 0
      open_directory("Audio")
      @sprites["down"].visible = can_scroll_down?
    end

    def hide
      @sprites.visible = false
    end

    def show
      @sprites.visible = true
    end

    def open_directory(folder)
      @top = 0
      @sel = 0
      update_selection
      @folder = folder
      @items = []
      @items << ["Stop Jukebox", :stop]
      @items << ["Return to Parent Folder", :parent] if @folder != "Audio"
      Dir.get_dirs(@folder, false).each do |folder|
        @items << [folder.gsub(/\\/, "/").split("/")[-1], :folder]
      end
      categories = {}
      Dir.get_files(@folder, false).each do |file|
        next if !file.end_with?(".ogg")
        decoder = OggDecoder.new(file, true)
        metadata = decoder.comment_header
        next if metadata.has?("DONOTSHOW") && metadata.get_one("DONOTSHOW").downcase == "true"
        file = file.gsub(/\\/, "/").split("/")[-1]
        category = ""
        if metadata.has?("SONGTYPE")
          category = metadata.get_one("SONGTYPE")
        end
        categories[category] ||= []
        categories[category] << [file, :file, metadata]
      end
      keys = categories.keys
      if keys.include?("Other")
        keys.delete("Other")
        keys << "Other"
      end
      for key in keys
        category = categories[key]
        @items << ["--- " + key + " ---", :category] if key != ""
        items = category.map do |file, type, metadata|
          spoiler = false
          # Determine if this track contains spoilers based on the SHOWIFSWITCH tag
          if metadata.has?("SHOWIFSWITCH")
            conditions = metadata.get("SHOWIFSWITCH")
            spoiler ||= conditions.any? do |line|
              next line.split(",").all? do |c|
                if c.include?("|")
                  # Format SWITCH,BOOLEAN
                  switch = c.split("|")[0].to_i
                  value = c.split("|")[1].downcase == "true"
                  next $game_switches[switch] != value
                else
                  # Format SWITCH
                  switch = c.to_i
                  next !$game_switches[switch]
                end
              end
            end
          end
          # Determine if this track contains spoilers based on the SHOWIFVARIABLE tag
          if metadata.has?("SHOWIFVARIABLE")
            conditions = metadata.get("SHOWIFVARIABLE")
            spoiler ||= conditions.any? do |line|
              next line.split(",").all? do |c|
                variable = c.split("|")[0].to_i
                value = c.split("|")[1].to_i
                next $game_variables[variable] < value
              end
            end
          end
          # Determine if this track contains spoilers based on the SHOWIFLOCATION tag
          if metadata.has?("SHOWIFLOCATION")
            conditions = metadata.get("SHOWIFLOCATION")
            spoiler ||= conditions.any? do |c|
              mapids = c.split(",").map(&:to_i)
              next mapids.none? { |id| $PokemonGlobal.visitedMaps[id] }
            end
          end
          # If this track is a spoiler
          if spoiler
            next [EnhancedJukebox::SPOILER_TEXT, type, metadata, file]
          else
            next [file, type, metadata]
          end
        end
        # Ensure all spoilers are at the bottom of their category
        items.sort! do |a, b|
          if a[3]
            if b[3]
              next a[3] <=> b[3]
            else
              next 1
            end
          else
            if b[3]
              next -1
            else
              next a[0] <=> b[0]
            end
          end
        end
        @items.concat(items)
      end
      draw_items
      @sprites["down"].visible = can_scroll_down?
      @sprites["up"].visible = can_scroll_up?
    end

    def draw_items
      @sprites["text"].clear
      @sprites["icons"].dispose
      @scrolling_items.each(&:dispose)
      @scrolling_items = []
      for i in @top...(@top + EnhancedJukebox::ITEMS_DRAWN)
        item = @items[i]
        next if item.nil?
        x = 30
        if item[1] == :stop || item[1] == :folder || item[1] == :file
          x += 40
          @sprites["icons"][i - @top] = Sprite.new(@viewport)
          @sprites["icons"][i - @top].bitmap = Bitmap.new(@path + "icons")
          @sprites["icons"][i - @top].src_rect.width = 32
          @sprites["icons"][i - @top].src_rect.x = 32 * [:folder, :stop, :file].index(item[1])
          @sprites["icons"][i - @top].x = 30
          @sprites["icons"][i - @top].y = 4 + 32 * (i - @top)
        end
        text = item[0].include?(".") ? item[0].split(".")[0..-2].join(".") : item[0]
        width = @sprites["text"].bitmap.text_size(text).width
        if width >= @viewport.rect.width - x
          s = HScrollSprite.new(TEXT_SCROLL_DELAY, x + 16, 56 + 32 * (i - @top), @viewport.rect.width - x - 4, 32)
          s.looping = false
          s.text = text
          @scrolling_items << s
        else
          @sprites["text"].draw(
              [text, x, -2 + 32 * (i - @top), 0, EnhancedJukebox::BASECOLOR, EnhancedJukebox::SHADOWCOLOR]
          )
        end
      end
    end

    def update
      @sprites.update
      @scrolling_items.each { |s| s.update(TEXT_SCROLL_FREQUENCY, TEXT_SCROLL_INCREMENT) }
      @i ||= 0
      if @i % 3 == 0
        @sprites["up"].src_rect.x = (@sprites["up"].src_rect.x + @sprites["up"].src_rect.width) % @sprites["up"].bitmap.width
        @sprites["down"].src_rect.x = (@sprites["down"].src_rect.x + @sprites["down"].src_rect.width) % @sprites["down"].bitmap.width
      end
      @i += 1
    end

    def update_selection
      @sprites["sel"].y = 4 + 32 * @sel
    end

    def to_parent_dir
      open_directory(@folder.split("/")[0..-2].join("/"))
    end

    def clicked
      idx = @top + @sel
      item = @items[idx]
      case item[1]
      when :stop
        $game_system.setDefaultBGM(nil)
        return nil
      when :folder
        open_directory(@folder + "/" + item[0])
      when :parent
        to_parent_dir
      when :file
        if item[3] # Contains spoilers
          # Dirty fix to make sure the down arrow won't overlap the message box
          oldvis = @sprites["down"].visible
          @sprites["down"].visible = false
          if pbConfirmMessage("This track may contain spoilers for content you haven't seen yet. Proceed?")
            @sprites["down"].visible = oldvis
            return item
          end
          @sprites["down"].visible = oldvis
        else
          return item
        end
      end
      return true
    end

    def can_scroll_down?
      return @top + ITEMS_DRAWN < @items.size
    end

    def can_scroll_up?
      return @top > 0
    end

    def main
      if Input.trigger?(Input::B)
        if @folder != "Audio"
          pbPlayCancelSE
          to_parent_dir
        else
          pbPlayCloseMenuSE
          return false
        end
      end
      if Input.trigger?(Input::C)
        pbPlayDecisionSE
        return clicked
      end
      oldsel = @sel
      if Input.repeat?(Input::DOWN)
        if @sel >= ITEMS_DRAWN - SCROLL_FROM && @top + ITEMS_DRAWN < @items.size
          @top += 1
          draw_items
          @sprites["up"].visible = true
          @sprites["down"].visible = can_scroll_down?
          pbPlayCursorSE
        elsif @top + @sel < @items.size - 1
          @sel += 1
        end
      end
      if Input.repeat?(Input::UP)
        if @top > 0 && @sel <= SCROLL_FROM - 1
          @top -= 1
          draw_items
          @sprites["up"].visible = can_scroll_up?
          @sprites["down"].visible = true
          pbPlayCursorSE
        elsif @sel > 0
          @sel -= 1
        end
      end
      if Input.repeat?(Input::JUMPDOWN)
        if @sel < ITEMS_DRAWN - SCROLL_FROM
          if @top + ITEMS_DRAWN == @items.size
            @sel = ITEMS_DRAWN - 1
          else
            @sel = [@items.size - 1, ITEMS_DRAWN - SCROLL_FROM].min
          end
        elsif @top + @sel < @items.size - 1
          diff = @items.size - (@top + ITEMS_DRAWN)
          @top += [ITEMS_DRAWN, diff].min
          if diff != 0
            draw_items
            @sprites["up"].visible = true
            @sprites["down"].visible = can_scroll_down?
            pbPlayCursorSE
            @sel = ITEMS_DRAWN - 1 if @top + ITEMS_DRAWN == @items.size
          else
            @sel = ITEMS_DRAWN - 1
          end
        end
      end
      if Input.repeat?(Input::JUMPUP)
        if @sel >= SCROLL_FROM
          @sel = @top == 0 ? 0 : SCROLL_FROM - 1
        elsif @top > 0
          @top -= [ITEMS_DRAWN, @top].min
          draw_items
          @sprites["up"].visible = can_scroll_up?
          @sprites["down"].visible = can_scroll_down?
          @sel = 0 if @top == 0
        else
          @sel = 0
        end
      end
      if @sel != oldsel
        update_selection
        pbPlayCursorSE
      end
      return true
    end

    def dispose
      @scrolling_items.each(&:dispose)
      @scrolling_items = []
      @sprites.dispose
      @viewport.dispose
    end

    raise "ITEMS_DRAWN must be at least 1." if EnhancedJukebox::ITEMS_DRAWN < 1
    raise "SCROLL_FROM must be at least 1." if EnhancedJukebox::SCROLL_FROM < 1
  end

  class SongScreen
    def initialize(filename, metadata)
      @filename = filename
      @metadata = metadata
      @path = "Graphics/Pictures/Enhanced Jukebox/"
      @viewport = Viewport.new(16, 52, 480, 264)
      @viewport.z = 99999
      @sprites = SpriteHash.new
      @sprites["bg"] = Sprite.new(@viewport)
      @sprites["bg"].bitmap = Bitmap.new(@path + "list_bg")
      @sprites["up"] = Sprite.new(@viewport)
      @sprites["up"].bitmap = Bitmap.new("Graphics/Pictures/uparrow")
      @sprites["up"].src_rect.width = 28
      @sprites["up"].visible = false
      @sprites["up"].x = @viewport.rect.width - @sprites["up"].src_rect.width - 8
      @sprites["up"].z = 1
      @sprites["down"] = Sprite.new(@viewport)
      @sprites["down"].bitmap = Bitmap.new("Graphics/Pictures/downarrow")
      @sprites["down"].src_rect.width = 28
      @sprites["down"].visible = false
      @sprites["down"].x = @sprites["up"].x
      @sprites["down"].y = @viewport.rect.height - @sprites["down"].src_rect.height
      @sprites["down"].z = 1
      @sprites["text"] = TextSprite.new(@viewport)
      @sprites["text"].bitmap = Bitmap.new(410, 1)
      @sprites["text"].x = 30
      @sprites["text"].y = -2
      pbSetSystemFont(@sprites["text"].bitmap)
      @items = []
      @items << [:header, "Song Name"]
      name = @filename.split("/")[-1]
      name = @filename.include?(".") ? @filename.split(".")[0..-2].join(".") : @filename
      @items << [:bullet, @metadata.get_one("TITLE")&.strip || name]
      if @metadata.has?("ARTIST")
        @items << [:header, "Artist"]
        @metadata.get("ARTIST").each { |artist| @items << [:bullet, artist.strip] }
      end
      if @metadata.has?("ALBUM")
        @items << [:header, "Album"]
        @items << [:bullet, @metadata.get("ALBUM").join(", ")]
      end
      if @metadata.has?("SOURCE")
        @items << [:header, "Source"]
        @metadata.get("SOURCE").each { |source| @items << [:bullet, source.strip] }
      end
      if @metadata.has?("LOCATIONS")
        @items << [:header, "Locations"]
        drew_any = false
        @metadata.get("LOCATIONS").each do |line|
          line.split(",").each do |location|
            begin
              if location.include?("|")
                name, type, id, value = location.split("|")
                type = type.downcase
                id = id.to_i
                show = true
                if type == "switch"
                  value = value.downcase == "true"
                  show = $game_switches[id] == value
                elsif type == "variable"
                  value = value.to_i
                  show = $game_variables[id] >= value
                elsif type == "area"
                  show = $PokemonGlobal.visitedMaps[id]
                end
                if show
                  @items << [:bullet, name.strip]
                  drew_any = true
                end
              else
                @items << [:bullet, location.strip]
                drew_any = true
              end
            rescue => e
              @items << [:bullet, "Error"]
              echoln "Invalid format in the LOCATIONS metadata tag.\nFilename: #{@filename}"
            end
          end
        end
        @items << [:bullet, "Unknown"] if !drew_any
      end
      if @metadata.has?("COMMENTS")
        @items << [:header, "Description"]
        @items << [:bullet, @metadata.get("COMMENTS").join("\n")]
      end
      @items << [:header, "Filename"]
      @items << [:bullet, @filename.split("/")[-1]]
      process_items
      draw_items
      $game_system.setDefaultBGM(@filename)
      @sprites["down"].visible = can_scroll_down?
    end

    def process_items
      @fields = []
      height = 0
      @items.each do |type, text|
        if type == :header
          @fields << text
          height += 32
        elsif type == :bullet
          chunks = getLineBrokenChunks(@sprites["text"].bitmap, text, 410, nil, true)
          @fields << chunks
          height += chunks.map { |c| c[2] + c[4] }.max # y + height
        end
      end
      @sprites["text"].bitmap.dispose
      @sprites["text"].bitmap = Bitmap.new(443, height + 4)
      pbSetSystemFont(@sprites["text"].bitmap)
    end

    def draw_items
      current_y = 0
      @fields.each do |field|
        if field.is_a?(String)
          @sprites["text"].draw(
              [field, 0, current_y, 0, EnhancedJukebox::BASECOLOR, EnhancedJukebox::SHADOWCOLOR]
          )
          current_y += 32
        else
          @sprites["text"].draw(
              ["*", 14, current_y, 0, EnhancedJukebox::BASECOLOR, EnhancedJukebox::SHADOWCOLOR]
          )
          field.each do |text, x, y, width, height, color|
            # [text, x, y, width, height, color]
            @sprites["text"].draw(
                [text, 33 + x, current_y + y, 0, EnhancedJukebox::BASECOLOR, EnhancedJukebox::SHADOWCOLOR]
            )
          end
          current_y += field.map { |c| c[2] + c[4] }.max # y + height
        end
      end
    end

    def update
      @sprites.update
      @i ||= 0
      if @i % 3 == 0
        @sprites["up"].src_rect.x = (@sprites["up"].src_rect.x + @sprites["up"].src_rect.width) % @sprites["up"].bitmap.width
        @sprites["down"].src_rect.x = (@sprites["down"].src_rect.x + @sprites["down"].src_rect.width) % @sprites["down"].bitmap.width
      end
      @i += 1
    end

    def can_scroll_down?
      return @sprites["text"].oy + @viewport.rect.height < @sprites["text"].bitmap.height + 4
    end

    def can_scroll_up?
      return @sprites["text"].oy > 0
    end

    def main
      if Input.trigger?(Input::B)
        pbPlayCancelSE
        return false
      end
      if Input.press?(Input::DOWN) && can_scroll_down?
        @sprites["text"].oy += EnhancedJukebox::PAGE_SCROLL_INCREMENT
        @sprites["up"].visible = true
        @sprites["down"].visible = can_scroll_down?
      end
      if Input.press?(Input::UP) && can_scroll_up?
        @sprites["text"].oy -= EnhancedJukebox::PAGE_SCROLL_INCREMENT
        @sprites["down"].visible = true
        @sprites["up"].visible = can_scroll_up?
      end
      if Input.repeat?(Input::JUMPDOWN) && can_scroll_down?
        # To ensure you end up at the same spot as you would if you were to scroll manually,
        # this adds the couple pixels you might overshoot the max bitmap height in case of
        # a non-perfect division
        maxoy = (@sprites["text"].bitmap.height - @viewport.rect.height) / EnhancedJukebox::PAGE_SCROLL_INCREMENT.to_f
        maxoy = (maxoy * EnhancedJukebox::PAGE_SCROLL_INCREMENT.to_f).ceil
        @sprites["text"].oy = maxoy + 4
        @sprites["down"].visible = false
        @sprites["up"].visible = can_scroll_up?
      end
      if Input.repeat?(Input::JUMPUP) && can_scroll_up?
        @sprites["text"].oy = 0
        @sprites["down"].visible = can_scroll_down?
        @sprites["up"].visible = false
      end
      return true
    end

    def dispose
      @sprites.dispose
      @viewport.dispose
    end
  end
end

class HScrollContainer
  attr_reader :looping

  def initialize(cooldown, x, y, width, height)
    @viewport = Viewport.new(x, y, width, height)
    @viewport.z = 99999
    @sprites = {}
    @cooldown = cooldown
    @ox = 0
    @i = -@cooldown
    @disposed = false
    @looping = true
    @mod = 1
  end

  def update_width
    @maxchildwidth = 0
    @sprites.each_value do |sprite|
      w = sprite.x
      w += sprite.bitmap.width if sprite.bitmap
      @maxchildwidth = w if w > @maxchildwidth
    end
  end

  def looping=(value)
    @looping = value
    @i = -@cooldown
    @viewport.ox = @ox = 0
  end

  def update(frame_interval, amount)
    @sprites.each_value(&:update)
    if @i >= 0 && @i % frame_interval == 0 && @maxchildwidth > @viewport.rect.width
      if @looping
        @ox = @ox + amount
        if @ox >= @maxchildwidth
          @ox = -@viewport.rect.width
        elsif @ox == 0
          @i = -@cooldown
        end
        @viewport.ox = @ox
      else
        @ox = @ox + amount * @mod
        if @ox <= 0
          @ox = 0
          @mod *= -1
          @i = -@cooldown
        elsif @ox >= @maxchildwidth - @viewport.rect.width
          @ox = @maxchildwidth - @viewport.rect.width
          @mod *= -1
          @i = -@cooldown
        end
        @viewport.ox = @ox
      end
    end
    @i += 1
  end

  def disposed?
    return @disposed
  end

  def dispose
    @sprites.each_value(&:dispose)
    @viewport.dispose
    @disposed = true
  end
end

class HScrollSprite < HScrollContainer
  def initialize(cooldown, x, y, width, height)
    super
    @sprites["text"] = TextSprite.new(@viewport)
  end

  def text=(value)
    @text = value
    @i = -@cooldown
    @viewport.ox = @ox = 0
    pbSetSystemFont(@sprites["text"].bitmap)
    size = @sprites["text"].bitmap.text_size(@text)
    @sprites["text"].bitmap&.dispose
    @sprites["text"].bitmap = Bitmap.new(size.width, @viewport.rect.height)
    pbSetSystemFont(@sprites["text"].bitmap)
    @sprites["text"].draw(
        [@text, 0, -6, 0, EnhancedJukebox::BASECOLOR, EnhancedJukebox::SHADOWCOLOR]
    )
    update_width
  end
end

class ScrollingMusicText < HScrollSprite
  def initialize(cooldown, x, y, width, height)
    super
    @path = "Graphics/Pictures/Enhanced Jukebox/"
    @sprites["text"].x = 36
    @sprites["icon"] = Sprite.new(@viewport)
    @sprites["icon"].bitmap = Bitmap.new(@path + "icons")
    @sprites["icon"].src_rect.width = 32
    @sprites["icon"].src_rect.x = 2 * @sprites["icon"].src_rect.width
  end
end

class PokemonTemp
  attr_accessor :defaultBGM
end

# Overwrite the defaultBGM property to allow for all folders, rather
# than only look inside the Audio/BGM folder, and move it to $PokemonTemp.
class Game_System
  def bgm_play_internal(bgm, position)
    @bgm_position = position if !@bgm_paused
    if bgm
      if ($PokemonTemp.nil? || $PokemonTemp.defaultBGM.nil? || bgm.name != $PokemonTemp.defaultBGM.name)
        @playing_bgm = bgm.clone
      end
    else
      @playing_bgm = nil
    end
    try_bgm = true
    if $PokemonTemp && $PokemonTemp.defaultBGM
      if FileTest.audio_exist?("Audio/BGM/" + $PokemonTemp.defaultBGM.name)
        bgm_play_internal2("Audio/BGM/" + $PokemonTemp.defaultBGM.name,
            $PokemonTemp.defaultBGM.volume, $PokemonTemp.defaultBGM.pitch, @bgm_position)
        try_bgm = false
      elsif FileTest.audio_exist?("Audio/" + $PokemonTemp.defaultBGM.name)
        bgm_play_internal2("Audio/" + $PokemonTemp.defaultBGM.name,
            $PokemonTemp.defaultBGM.volume, $PokemonTemp.defaultBGM.pitch, @bgm_position)
        try_bgm = false
      elsif FileTest.audio_exist?($PokemonTemp.defaultBGM.name)
        bgm_play_internal2($PokemonTemp.defaultBGM.name,
            $PokemonTemp.defaultBGM.volume, $PokemonTemp.defaultBGM.pitch, @bgm_position)
        try_bgm = false
      end
    end
    if try_bgm && bgm && bgm.name != ""
      if FileTest.audio_exist?("Audio/BGM/" + bgm.name)
        bgm_play_internal2("Audio/BGM/" + bgm.name,
            bgm.volume, bgm.pitch, @bgm_position)
      elsif FileTest.audio_exist?("Audio/" + bgm.name)
        bgm_play_internal2("Audio/" + bgm.name,
            bgm.volume, bgm.pitch, @bgm_position)
      elsif FileTest.audio_exist?(bgm.name)
        bgm_play_internal2(bgm.name,
            bgm.volume, bgm.pitch, @bgm_position)
      else
        @playing_bgm = nil
        Audio.bgm_stop
      end
    end
    Graphics.frame_reset
  end

  def setDefaultBGM(bgm, volume = 80, pitch = 100)
    bgm = RPG::AudioFile.new(bgm, volume, pitch) if bgm.is_a?(String)
    if bgm && bgm.name != ""
      $PokemonTemp.defaultBGM = bgm.clone
      self.bgm_play(bgm)
    else
      $PokemonTemp.defaultBGM = nil
      self.bgm_play(@playing_bgm)
    end
  end

  def bgm_stop # :nodoc:
    @bgm_position = 0 if !@bgm_paused
    @playing_bgm  = nil
    Audio.bgm_stop if !$PokemonTemp.defaultBGM
  end

  def bgm_fade(time) # :nodoc:
    @bgm_position = 0 if !@bgm_paused
    @playing_bgm = nil
    Audio.bgm_fade((time*1000).floor) if !$PokemonTemp.defaultBGM
  end
end

class PokemonPokegearScreen
  def pbStartScreen
    commands = []
    cmdMap     = -1
    cmdPhone   = -1
    cmdJukebox = -1
    cmdQuest = -1
    cmdEggCheck = -1
    cmdEncounters = -1
    cmdSearch = -1
    encounter_data = GameData::Encounter.get($game_map.map_id, $PokemonGlobal.encounter_version)
    commands[cmdMap = commands.length]     = ["map", _INTL("Map"), lock=false]
    if true
      phoneLock = $PokemonGlobal.phoneNumbers && $PokemonGlobal.phoneNumbers.length > 0
      commands[cmdPhone = commands.length] = ["phone", _INTL("Phone"), !phoneLock]
    end
    commands[cmdJukebox = commands.length] = ["jukebox", _INTL("Music"), lock=false]
    if true 
      commands[cmdQuest = commands.length] = ["quest", _INTL("Tasks"), lock=!hasAnyQuests?]
    end
    if $game_switches[Settings::PHONEAPP_EGG]
      commands[cmdEggCheck = commands.length] = ["egg", _INTL("Day-Care"), lock=false]
    end
    if $Trainer.has_pokedex && $game_switches[Settings::PHONEAPP_HABITAT]
      habitLock = encounter_data && $game_map.map_id != 356
      commands[cmdEncounters = commands.length] = ["encounters", _INTL("Habitat"), !habitLock]
    end
    if $Trainer.has_pokedex && $game_switches[Settings::PHONEAPP_NAV]
      navLock = $PokemonEncounters.encounter_possible_here? && encounter_data
      commands[cmdSearch = commands.length] = ["search", _INTL("DexNav"), !navLock]
    end
    @scene.pbStartScene(commands)
    loop do
      cmd = @scene.pbScene
      if cmd < 0
        break
      elsif cmdMap >= 0 && cmd == cmdMap
        pbShowMap(-1,false)
      elsif cmdPhone >= 0 && cmd == cmdPhone
        pbFadeOutIn { PokemonPhoneScene.new.start }
      elsif cmdJukebox >= 0 && cmd == cmdJukebox
        EnhancedJukebox.new
      elsif cmdEggCheck >= 0 && cmd == cmdEggCheck
        DayCareChecker.display
      elsif cmdEncounters >= 0 && cmd == cmdEncounters
        pbFadeOutIn {
          scene = EncounterList_Scene.new
          screen = EncounterList_Screen.new(scene)
          screen.pbStartScreen
        }
      elsif cmdSearch >= 0 && cmd == cmdSearch
        pbFadeOutIn {
          scene = PokeSearch_Scene.new
          screen = PokeSearch_Screen.new(scene)
          screen.pbStartScreen
        }
      elsif cmdQuest>=0 && cmd == cmdQuest
        pbFadeOutIn {
          pbViewQuests
        }
      end
    end
    @scene.pbEndScene
  end
end
