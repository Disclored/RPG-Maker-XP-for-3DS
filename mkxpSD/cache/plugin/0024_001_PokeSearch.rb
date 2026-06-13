#-------------------------------------------------------------------------------
# PokéSearch v1.0
# Find your perfect encounter with style 😎
#-------------------------------------------------------------------------------
#
# Based on UI-Encounters by ThatWelshOne
# 
#-------------------------------------------------------------------------------
#
# Call the UI with: vPokeSearch
#
#-------------------------------------------------------------------------------

def vPokeSearch
  scene = PokeSearch_Scene.new
  screen = PokeSearch_Screen.new(scene)
  screen.pbStartScreen
end

class PokeSearch_Scene
  SELECTEDPOKEMON      = 229
  SELECTEDEFFECT       = 230
  SELECTEDBASE         = 231
  SELECTEDLEVEL        = 232

  ITEMTEXTBASECOLOR    = Color.new(224,232,232)
  ITEMTEXTSHADOWCOLOR  = Color.new(64,64,64)

  ONLYONENCOUNTERTILE = true # Whether you can only search while on an encounter tile (grass, surfing, in a cave)

  COMBO_TEXT_X         = Graphics.width / 4
  COMBO_TEXT_Y         = 148

  # Initializes Scene
  def initialize
    @viewport = Viewport.new(0, 0, Graphics.width, Graphics.height)
    @viewport.z = 99999
    @sprites = {}
    mapid = $game_map.map_id
    @encounter_data = GameData::Encounter.get(mapid, $PokemonGlobal.encounter_version)
    if @encounter_data
        @encounter_tables = Marshal.load(Marshal.dump(@encounter_data.types))
        @max_enc, @eLength = getMaxEncounters(@encounter_tables)
    else
        @max_enc, @eLength = [1, 1]
    end
    @index_hor = 1
    @index_ver = 0
    @current_key = 0
    @current_mon = nil
    @current_berry = nil
    @current_repel = nil
    @average_level = 1
    @disposed = false
  end

  # draw scene elements
  def pbStartScene
    addBackgroundPlane(@sprites,"bg","PokeSearch/bg",@viewport)
    @sprites["elements"] = IconSprite.new(0,0,@viewport)
    @sprites["elements"].setBitmap(sprintf("Graphics/Pictures/PokeSearch/elements"))
    @sprites["overlay"] = BitmapSprite.new(Graphics.width,Graphics.height,@viewport)
    pbSetSystemFont(@sprites["overlay"].bitmap)
    @sprites["sel_berry"] = IconSprite.new(0,0,@viewport)
    @sprites["sel_berry"].setBitmap(sprintf("Graphics/Pictures/PokeSearch/sel_small"))
    @sprites["sel_berry"].x = 96
    @sprites["sel_berry"].y = 96
    @sprites["sel_berry"].visible = false
    @sprites["sel_repel"] = IconSprite.new(0,0,@viewport)
    @sprites["sel_repel"].setBitmap(sprintf("Graphics/Pictures/PokeSearch/sel_small"))
    @sprites["sel_repel"].x = 360
    @sprites["sel_repel"].y = 96
    @sprites["sel_repel"].visible = false
    @sprites["sel_pokemon"] = IconSprite.new(0,0,@viewport)
    @sprites["sel_pokemon"].setBitmap(sprintf("Graphics/Pictures/PokeSearch/sel_medium"))
    @sprites["sel_pokemon"].x = 166
    @sprites["sel_pokemon"].y = 102
    @sprites["sel_pokemon"].visible = true
    @sprites["sel_search"] = IconSprite.new(0,0,@viewport)
    @sprites["sel_search"].setBitmap(sprintf("Graphics/Pictures/PokeSearch/sel_large_search"))
    @sprites["sel_search"].x = 84
    @sprites["sel_search"].y = 318
    @sprites["sel_search"].visible = false
    @sprites["sel_cancel"] = IconSprite.new(0,0,@viewport)
    @sprites["sel_cancel"].setBitmap(sprintf("Graphics/Pictures/PokeSearch/sel_large_cancel"))
    @sprites["sel_cancel"].x = 276
    @sprites["sel_cancel"].y = 318
    @sprites["sel_cancel"].visible = false
    @sprites["berry_icon"] = ItemIconSprite.new(124,124,nil,@viewport)
    @sprites["berry_text"] = Window_UnformattedTextPokemon.newWithSize("",26, 160, 236, 174, @viewport)
    @sprites["berry_text"].baseColor   = ITEMTEXTBASECOLOR
    @sprites["berry_text"].shadowColor = ITEMTEXTSHADOWCOLOR
    @sprites["berry_text"].visible     = true
    @sprites["berry_text"].windowskin  = nil
    @sprites["berry_text"].text = "Add an incense to gain an extra effect."
    @sprites["repel_icon"] = ItemIconSprite.new(388,124,nil,@viewport)
    @sprites["repel_text"] = Window_UnformattedTextPokemon.newWithSize("",262, 160, 236, 174, @viewport)
    @sprites["repel_text"].baseColor   = ITEMTEXTBASECOLOR
    @sprites["repel_text"].shadowColor = ITEMTEXTSHADOWCOLOR
    @sprites["repel_text"].visible     = true
    @sprites["repel_text"].windowskin  = nil
    @sprites["repel_text"].text = "Use a Honey pot to attract wild Pokémon. (Required)"

#    @sprites["combo_text"] = Window_UnformattedTextPokemon.newWithSize("",150, 124, 236, 100, @viewport)
#    @sprites["combo_text"].baseColor   = ITEMTEXTBASECOLOR
#    @sprites["combo_text"].shadowColor = ITEMTEXTSHADOWCOLOR
#    @sprites["combo_text"].visible     = true
#    @sprites["combo_text"].windowskin  = nil
#    combo = $game_variables[Settings::DEXNAV_CHAIN].clamp(0,999)
#    @sprites["combo_text"].text = "Search Combo: #{combo}"

    # Load presets
    @current_mon = nil
    @current_berry = nil
    @current_repel = nil
    @current_mon = nil
    @average_level = 1

    @current_berry = $game_variables[SELECTEDEFFECT] if ($game_variables[SELECTEDEFFECT] != nil && $game_variables[SELECTEDEFFECT] != 0) && $PokemonBag.pbHasItem?($game_variables[SELECTEDEFFECT])
    @current_repel = $game_variables[SELECTEDBASE] if ($game_variables[SELECTEDBASE] != nil && $game_variables[SELECTEDBASE] != 0) && $PokemonBag.pbHasItem?($game_variables[SELECTEDBASE])

    enc_data = getEncData
    if [356].include?($game_map.map_id) # Friend Safari
      enc_data = $game_variables[Settings::SAFARI_ENCOUNTERS]
      if enc_data.is_a?(Array) && enc_data.length > 1
        if enc_data.include?($game_variables[SELECTEDPOKEMON])
          @current_mon = $game_variables[SELECTEDPOKEMON]
        end
      end
    else
      if enc_data != nil && $game_variables[SELECTEDPOKEMON] != nil && $game_variables[SELECTEDPOKEMON] != 0
        if enc_data[0].include?($game_variables[SELECTEDPOKEMON])
          @current_mon = $game_variables[SELECTEDPOKEMON]
        end
      end
    end

    if ($game_variables[SELECTEDLEVEL] != nil && $game_variables[SELECTEDLEVEL] > 0)
      @average_level = $game_variables[SELECTEDLEVEL] 
      @average_level = 1 if @average_level < 1
    end

    hasEffect = @current_berry != nil ? true : false
    hasHoney = @current_repel != nil ? true : false
    hasPokemon = @current_mon != nil ? true : false

    @sprites["berry_icon"].visible = hasEffect
    @sprites["repel_icon"].visible = hasHoney
    @sprites["berry_icon"].item = @current_berry
    @sprites["repel_icon"].item = @current_repel
    @sprites["berry_text"].text = description(@current_berry) if hasEffect
    @sprites["repel_text"].text = description(@current_repel) if hasHoney
    if hasPokemon
      # Draw Pokemon
      overlay = @sprites["overlay"].bitmap
      overlay.clear
      pkmnName = GameData::Species.get(@current_mon).name
      pbDrawTextPositions(overlay,[[pkmnName,Graphics.width/2,100,2,ITEMTEXTBASECOLOR,ITEMTEXTSHADOWCOLOR]])
      # Set cursor default to Search
      if hasHoney
        @index_ver = 1
        @index_hor = 0
        drawPresent
      else
        @index_ver = 0
        @index_hor = 2
        drawPresent
      end
    else
      $game_variables[Settings::DEXNAV_CHAIN] = 0
    end

    # Catch combo text
    @sprites["combo_text"] = BitmapSprite.new(Graphics.width,Graphics.height,@viewport)
    pbSetSmallFont(@sprites["combo_text"].bitmap)
    combo_text = "Search Combo: #{$game_variables[Settings::DEXNAV_CHAIN].clamp(0,999)}"
    pbDrawOutlineText(@sprites["combo_text"].bitmap, COMBO_TEXT_X, COMBO_TEXT_Y, width=200, height=32, 
    combo_text, ITEMTEXTBASECOLOR, ITEMTEXTSHADOWCOLOR, align=1)

    pbFadeInAndShow(@sprites) { pbUpdate }
  end

  # input controls
  def pbPokeSearch
    loop do
      Graphics.update
      Input.update
      pbUpdate
      if @disposed
        break
      else
        if Input.trigger?(Input::RIGHT) && @index_hor < 2
          pbPlayCursorSE
          @index_hor += 1
          drawPresent
        elsif Input.trigger?(Input::LEFT) && @index_hor !=0
          pbPlayCursorSE
          @index_hor -= 1
          drawPresent
        elsif Input.trigger?(Input::DOWN) && @index_ver < 1
          pbPlayCursorSE
          @index_ver += 1
          drawPresent
        elsif Input.trigger?(Input::UP) && @index_ver !=0
          pbPlayCursorSE
          @index_ver -= 1
          drawPresent
        elsif Input.trigger?(Input::USE)
          pbPlayCursorSE
          case @index_ver # please ignore this awful code
          when 0
            case @index_hor
            when 0
              selectBerry
            when 1
              selectMon
            when 2
              selectRepel
            end
          when 1
            case @index_hor
            when 0
              startSearch
            when 1
              pbPlayCloseMenuSE
              break
            when 2
              pbPlayCloseMenuSE
              break
            end
          end
        elsif Input.trigger?(Input::BACK)
          pbPlayCloseMenuSE
          break
        end
      end
    end
  end

  # selecting the correct berry
  def selectBerry
    pbFadeOutIn {
      scene = PokemonBag_Scene.new
      screen = PokemonBagScreen.new(scene,$PokemonBag)
      berry = screen.pbChooseItemScreen(Proc.new { |item| GameData::Item.get(item) == :FULLINCENSE || GameData::Item.get(item) == :LAXINCENSE || GameData::Item.get(item) == :LUCKINCENSE || GameData::Item.get(item) == :ODDINCENSE || GameData::Item.get(item) == :PUREINCENSE || GameData::Item.get(item) == :ROCKINCENSE || GameData::Item.get(item) == :ROSEINCENSE || GameData::Item.get(item) == :SEAINCENSE || GameData::Item.get(item) == :WAVEINCENSE })
      @sprites["berry_icon"].item = berry
      if berry
        @sprites["berry_text"].text = description(berry)
        @sprites["berry_icon"].visible = true
      else
        @sprites["berry_text"].text = "Add an incense to gain an extra effect."
        @sprites["berry_icon"].visible = false
      end
      @current_berry = berry
      $game_variables[SELECTEDEFFECT] = berry
    }
  end

  # returns the correct description
  def description(item)
    case item
    when :HONEY
      return _INTL("Due to the Honey, an encounter is guaranteed.")
    when :ODDINCENSE
      return _INTL("{1} increases the odds of hidden ability encounters.",GameData::Item.get(item).name)
    when :SEAINCENSE, :WAVEINCENSE
      return _INTL("{1} increase the odds of Pokérus encounters.",GameData::Item.get(item).name)
    when :ROSEINCENSE
      return _INTL("{1} increases the odds of encounters holding items.",GameData::Item.get(item).name)
    when :ROCKINCENSE
      return _INTL("{1} increases the IVs of encounters.",GameData::Item.get(item).name)
    when :FULLINCENSE, :LAXINCENSE
      return _INTL("{1} lowers the level of encounters.",GameData::Item.get(item).name)
    when :LUCKINCENSE
      return _INTL("{1} increases the odds of shiny encounters.",GameData::Item.get(item).name)
    when :PUREINCENSE
      return _INTL("{1} increases the level of encounters.",GameData::Item.get(item).name_plural)
    else
      return _INTL("No item has been identified.")
    end
  end

  # selecting mons based on the encounter table
  def selectMon
    commands = []
    mons = []
    if getEncData != nil
      command_list = getEncData[0]
      @average_level = getEncData[1]
      $game_variables[SELECTEDLEVEL] = getEncData[1]
    end
    if command_list != nil
      command_list.each { |mon| mons.push(mon)}
      mons.each { |thismon| commands.push(GameData::Species.get(thismon).name)}
    else
      pbMessage("Failed to find Pokémon...")
      return
    end
    safariMaps = [356]
    safariList = $game_variables[Settings::SAFARI_ENCOUNTERS]
    if safariMaps.include?($game_map.map_id)
      # Friend Safari
      if safariList.is_a?(Array) && safariList.length > 1 &&
        commands = []
        mons = []
        safariList.each { |mon| mons.push(mon)}
        mons.each { |thismon| commands.push(GameData::Species.get(thismon).name)}
      else
        pbMessage("Failed to find Pokémon...")
        return
      end
    end
    commands.push("Cancel")
    command = pbShowCommands(nil,commands,commands.length)
    @current_mon = nil
    overlay = @sprites["overlay"].bitmap
    overlay.clear
    if commands[command] != "Cancel"
      pbDrawTextPositions(overlay,[[commands[command],Graphics.width/2,100,2,ITEMTEXTBASECOLOR,ITEMTEXTSHADOWCOLOR]])
      @current_mon = mons[command]
      $game_variables[SELECTEDPOKEMON] = mons[command]
      # Reset Chain
      @sprites["combo_text"].bitmap.clear
      $game_variables[Settings::DEXNAV_CHAIN] = 0
      combo_text = "Search Combo: #{$game_variables[Settings::DEXNAV_CHAIN].clamp(0,999)}"
      pbDrawOutlineText(@sprites["combo_text"].bitmap, COMBO_TEXT_X, COMBO_TEXT_Y, width=200, height=32, 
        combo_text, ITEMTEXTBASECOLOR, ITEMTEXTSHADOWCOLOR, align=1)
    end
  end

  # selecting repels
  def selectRepel
    pbFadeOutIn {
      scene = PokemonBag_Scene.new
      screen = PokemonBagScreen.new(scene,$PokemonBag)
      repel = screen.pbChooseItemScreen(Proc.new { |item| GameData::Item.get(item) == :HONEY})
      @sprites["repel_icon"].item = repel
      if repel
        @sprites["repel_text"].text = description(repel)
        @sprites["repel_icon"].visible = true
      else
        @sprites["repel_text"].text = "Use a Honey pot to attract wild Pokémon. (Required)"
        @sprites["repel_icon"].visible = false
      end
      @current_repel = repel
      $game_variables[SELECTEDBASE] = repel
    }
  end

  # checks all of the current parameters and initiates a battle if successful
  def startSearch
    if @current_mon == nil
      pbPlayBuzzerSE()
      pbMessage("Select a Pokémon to attract.")
      return
    end
    if @current_repel == nil
      pbPlayBuzzerSE()
      pbMessage("Add a honey pot to use.")
      return
    end
    if !$PokemonEncounters.encounter_possible_here? && ONLYONENCOUNTERTILE
      pbPlayBuzzerSE()
      pbMessage("Can only scan when standing in an area where you can get encounters.")
      return
    end
    level = @average_level + rand(-2..2)
    if @current_berry == :FULLINCENSE || @current_berry == :LAXINCENSE
      level = [level-3,1].max
    elsif @current_berry == :PUREINCENSE
      level = [level+3,100].min
    end
    $PokemonSystem.current_berry = @current_berry
    $PokemonSystem.pokesearch_encounter = true
    odds = rand(0..100) < getRepelOdds
    if @current_repel != nil
      $PokemonBag.pbDeleteItem(@current_repel)
      @current_repel = nil
    end
    if @current_berry != nil
#      $PokemonBag.pbDeleteItem(@current_berry)
      @current_berry = nil
    end
    pbEndScene
    usingDexNav = 288
    $game_switches[usingDexNav] = true
    waitingMessage = "\\ts[3]\\se[GUI dexnav search]...............\\. \\se[GUI dexnav search]...............\\. "
    finalMessage = ""
    for i in 0..rand(1,2); finalMessage += waitingMessage; end
    pbMessage(_INTL("{1}\\wtnp[2]", finalMessage))
    if odds
      pbMessage("\\se[Exclaim]Pokémon found!\\wt[20]\\^")
      pbWait(20)
      pbWildBattle(@current_mon, level)
    else
      pbMessage("No Pokémon appeared.")
    end
    $PokemonSystem.current_berry = nil
    $PokemonSystem.pokesearch_encounter = false
    # Increase Chain
    $game_variables[Settings::DEXNAV_CHAIN] += 1
    $game_switches[usingDexNav] = false
  end

  # in percentages
  def getRepelOdds
    case @current_repel
    when :HONEY
      return 101
    end
    return 10
  end

  # update UI based on current status
  # thanks to ThatWelshOne
  def drawPresent
    @sprites["sel_berry"].visible = false
    @sprites["sel_pokemon"].visible = false
    @sprites["sel_repel"].visible = false
    @sprites["sel_search"].visible = false
    @sprites["sel_cancel"].visible = false
    case @index_ver
    when 0
      case @index_hor
      when 0
        @sprites["sel_berry"].visible = true
      when 1
        @sprites["sel_pokemon"].visible = true
      when 2
        @sprites["sel_repel"].visible = true
      end
    when 1
      case @index_hor
      when 0
        @sprites["sel_search"].visible = true
      when 1
        @sprites["sel_cancel"].visible = true
      when 2
        @sprites["sel_cancel"].visible = true
        @index_hor = 1
      end
    end
  end

  # get encounter data
  # again thanks to ThatWelshOne
  # Thanks to wrigty12 for bugfix
  def getEncData
    return nil if @encounter_tables.nil?
    return nil if $PokemonEncounters.encounter_type.nil? #TDW try to get the correct encounter table for your tile
    correctKey = @encounter_tables.keys.each_index.select{|i| @encounter_tables.keys[i] == $PokemonEncounters.encounter_type}
    return nil if correctKey.nil?
    #currKey = @encounter_tables.keys[@current_key]
    currKey = @encounter_tables.keys[correctKey[0]]
    arr = []
    min_levels = 0
    max_levels = 0
    enc_array = []
    @encounter_tables[currKey].each { |s| arr.push( s[1] ); min_levels+= s[2]; max_levels += s[3] }
    GameData::Species.each { |s| enc_array.push(s.id) if arr.include?(s.id) && $Trainer.seen?(s) } # From Maruno
    enc_array.uniq!
    average_level = ((min_levels+max_levels)/2)/arr.length
    return enc_array, average_level
  end

  # get max encounters
  # again again thanks to ThatWelshOne
  def getMaxEncounters(data)
    keys = data.keys
    a = []
    for key in keys
      b = []
      arr = data[key]
      for i in 0...arr.length
        b.push( arr[i][1] )
      end
      a.push(b.uniq.length)
    end
    return a.max, keys.length
  end

  def pbUpdate
    pbUpdateSpriteHash(@sprites)
    
    if @sprites["bg"]
      @sprites["bg"].ox+=1
      @sprites["bg"].oy-=0
    end
  end

  def pbEndScene
    pbFadeOutAndHide(@sprites) { pbUpdate }
    pbDisposeSpriteHash(@sprites)
    @disposed = true
    @viewport.dispose
  end
end

class PokeSearch_Screen
  def initialize(scene)
    @scene = scene
  end

  def pbStartScreen
    @scene.pbStartScene
    @scene.pbPokeSearch
    @scene.pbEndScene
  end
end

class PokemonSystem
  attr_writer :current_berry
  attr_writer :pokesearch_encounter
  
	def current_berry
		return @current_berry
  end

  def pokesearch_encounter
		return @pokesearch_encounter
  end
end
