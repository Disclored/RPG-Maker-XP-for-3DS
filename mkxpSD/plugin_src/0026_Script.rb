class PokemonTrainerCard_Scene
  ITEMS_DRAWN = 6
  ITEM_HEADERS = ["BATTLE","PARAMETERS","BENEFITS","SPECIAL"]
  FRONTIER_PASS_SWITCH = 247

  def pbStartScene
    # General variables
    @cardMode = 0
    # Trainer Card variables
    @front = true
    @sel = 0

    # Create scene
    @viewport = Viewport.new(0,0,Graphics.width,Graphics.height)
    @viewport.z = 99999
    @sprites = {}
    addBackgroundPlane(@sprites,"bg","Trainer Card/bg",@viewport)

    # Base Card graphics
    @sprites["card"] = IconSprite.new(0,-20,@viewport)
    @sprites["knob"] = IconSprite.new(0,0,@viewport)
    pbRefreshCardBitmap

    @sprites["card"].ox = @sprites["card"].bitmap.width / 2
    @sprites["card"].oy = @sprites["card"].bitmap.height / 2
    @sprites["card"].x += @sprites["card"].ox
    @sprites["card"].y += @sprites["card"].oy

    # UI Graphics
    @sprites["overlay"] = BitmapSprite.new(Graphics.width,Graphics.height,@viewport)
    pbSetSystemFont(@sprites["overlay"].bitmap)
    @sprites["elements"] = IconSprite.new(0,0,@viewport)
    @sprites["elements"].setBitmap(sprintf("Graphics/Pictures/Trainer Card/card_overlay"))

    # Trainer Card text
    @sprites["text"] = TextSprite.new(@viewport)

    # Trainer Card Trainer sprite
    @sprites["trainer"] = IconSprite.new(336*0.9,-60*0.1,@viewport)
    @sprites["trainer"].setBitmap(GameData::TrainerType.player_front_sprite_filename($Trainer.trainer_type))
    @sprites["trainer"].x -= (@sprites["trainer"].bitmap.width-128)/(2*1)
    @sprites["trainer"].y -= (@sprites["trainer"].bitmap.height-128)/(1*1)
    @sprites["trainer"].z = 2
    @sprites["trainer"].zoom_x = 2
    @sprites["trainer"].zoom_y = 2

    # Badge Sprites
    badgeX = $Trainer.badge_count > 8 ? 40 : 48
    badgeY = 52
    bXOffset = $Trainer.badge_count > 8 ? 88 : 112
    bYOffset = 160
    @sprites["badge0"] = IconSprite.new(badgeX+bXOffset*0,badgeY+bYOffset*0,@viewport)
    @sprites["badge1"] = IconSprite.new(badgeX+bXOffset*1,badgeY+bYOffset*0,@viewport)
    @sprites["badge2"] = IconSprite.new(badgeX+bXOffset*2,badgeY+bYOffset*0,@viewport)
    @sprites["badge3"] = IconSprite.new(badgeX+bXOffset*3,badgeY+bYOffset*0,@viewport)
    @sprites["badge4"] = IconSprite.new(badgeX+bXOffset*0,badgeY+bYOffset*1,@viewport)
    @sprites["badge5"] = IconSprite.new(badgeX+bXOffset*1,badgeY+bYOffset*1,@viewport)
    @sprites["badge6"] = IconSprite.new(badgeX+bXOffset*2,badgeY+bYOffset*1,@viewport)
    @sprites["badge7"] = IconSprite.new(badgeX+bXOffset*3,badgeY+bYOffset*1,@viewport)
    @sprites["badge8"] = IconSprite.new(badgeX+bXOffset*4,badgeY+bYOffset*0,@viewport)
    @sprites["badge9"] = IconSprite.new(badgeX+bXOffset*4,badgeY+bYOffset*1,@viewport)

    # Frontier Prints
    @sprites["printTower"] = IconSprite.new(184,64,@viewport)
    @sprites["printFactory"] = IconSprite.new(306,154,@viewport)
    @sprites["printArcade"] = IconSprite.new(272,248,@viewport)
    @sprites["printCastle"] = IconSprite.new(94,248,@viewport)
    @sprites["printHall"] = IconSprite.new(60,154,@viewport)

    pbRedrawSide
    pbFadeInAndShow(@sprites) { pbUpdate }
  end

  def pbRefreshCardBitmap
    # Get correct Card graphic
    case @cardMode
    when 0
      stars = ($game_variables[Settings::TRAINER_STARS]).clamp(0,6)
      @sprites["knob"].setBitmap("Graphics/Pictures/Trainer Card/card_knob_#{stars}")
      if @front
        @sprites["card"].setBitmap("Graphics/Pictures/Trainer Card/card_#{stars}")
      else
        @sprites["card"].setBitmap("Graphics/Pictures/Trainer Card/card_back_#{stars}")
      end
    when 1
      @sprites["card"].setBitmap("Graphics/Pictures/Trainer Card/badge_case")
    when 2
      @sprites["card"].setBitmap("Graphics/Pictures/Trainer Card/frontier_pass")
    end
  end

  def pbClearSide
    overlay = @sprites["overlay"].bitmap
    overlay.clear
    text = @sprites["text"].bitmap
    text.clear

    @sprites["trainer"].visible = false
    @sprites["knob"].visible = false

    @sprites["badge0"].visible = false
    @sprites["badge1"].visible = false
    @sprites["badge2"].visible = false
    @sprites["badge3"].visible = false
    @sprites["badge4"].visible = false
    @sprites["badge5"].visible = false
    @sprites["badge6"].visible = false
    @sprites["badge7"].visible = false
    @sprites["badge8"].visible = false
    @sprites["badge9"].visible = false

    @sprites["printTower"].visible = false
    @sprites["printFactory"].visible = false
    @sprites["printArcade"].visible = false
    @sprites["printCastle"].visible = false
    @sprites["printHall"].visible = false
  end

  def pbRedrawSide
    # Get correct Card content graphics
    hasFrontierPass = $game_switches[FRONTIER_PASS_SWITCH]
    case @cardMode
    when 0
      if @front
        pbDrawTrainerCardFront
        @sprites["trainer"].visible = true
        @sprites["knob"].visible = false
      else
        pbDrawItems
        pbDrawListKnob
        @sprites["trainer"].visible = false
        @sprites["knob"].visible = true
      end
      
      if hasFrontierPass
        @sprites["elements"].setBitmap(sprintf("Graphics/Pictures/Trainer Card/card_overlay"))
      else
        @sprites["elements"].setBitmap(sprintf("Graphics/Pictures/Trainer Card/card_overlay_3"))
      end
    when 1
      @sprites["badge0"].setBitmap("Graphics/Pictures/Trainer Card/badge_0")
      @sprites["badge1"].setBitmap("Graphics/Pictures/Trainer Card/badge_1")
      @sprites["badge2"].setBitmap("Graphics/Pictures/Trainer Card/badge_2")
      @sprites["badge3"].setBitmap("Graphics/Pictures/Trainer Card/badge_3")
      @sprites["badge4"].setBitmap("Graphics/Pictures/Trainer Card/badge_4")
      @sprites["badge5"].setBitmap("Graphics/Pictures/Trainer Card/badge_5")
      @sprites["badge6"].setBitmap("Graphics/Pictures/Trainer Card/badge_6")
      @sprites["badge7"].setBitmap("Graphics/Pictures/Trainer Card/badge_7")
      @sprites["badge8"].setBitmap("Graphics/Pictures/Trainer Card/badge_8")
      @sprites["badge9"].setBitmap("Graphics/Pictures/Trainer Card/badge_9")

      @sprites["badge0"].visible = true if $Trainer.badge_count > 0
      @sprites["badge1"].visible = true if $Trainer.badge_count > 1
      @sprites["badge2"].visible = true if $Trainer.badge_count > 2
      @sprites["badge3"].visible = true if $Trainer.badge_count > 3
      @sprites["badge4"].visible = true if $Trainer.badge_count > 4
      @sprites["badge5"].visible = true if $Trainer.badge_count > 5
      @sprites["badge6"].visible = true if $Trainer.badge_count > 6
      @sprites["badge7"].visible = true if $Trainer.badge_count > 7
      @sprites["badge8"].visible = true if $Trainer.badge_count > 8
      @sprites["badge9"].visible = true if $Trainer.badge_count > 9

      if hasFrontierPass
        @sprites["elements"].setBitmap(sprintf("Graphics/Pictures/Trainer Card/card_overlay_1"))
      else
        @sprites["elements"].setBitmap(sprintf("Graphics/Pictures/Trainer Card/card_overlay_4"))
      end
    when 2
      ls = ["none","silver","gold"]
      towTxt = ls[pbGet(174).clamp(0,2)]
      facTxt = ls[pbGet(179).clamp(0,2)]
      arcTxt = ls[pbGet(184).clamp(0,2)]
      casTxt = ls[pbGet(194).clamp(0,2)]
      halTxt = ls[pbGet(189).clamp(0,2)]
      @sprites["printTower"].setBitmap("Graphics/Pictures/Trainer Card/frontier_print_#{towTxt}_tower")
      @sprites["printFactory"].setBitmap("Graphics/Pictures/Trainer Card/frontier_print_#{facTxt}_factory")
      @sprites["printArcade"].setBitmap("Graphics/Pictures/Trainer Card/frontier_print_#{arcTxt}_arcade")
      @sprites["printCastle"].setBitmap("Graphics/Pictures/Trainer Card/frontier_print_#{casTxt}_castle")
      @sprites["printHall"].setBitmap("Graphics/Pictures/Trainer Card/frontier_print_#{halTxt}_hall")

      @sprites["printTower"].visible = true
      @sprites["printFactory"].visible = true
      @sprites["printArcade"].visible = true
      @sprites["printCastle"].visible = true
      @sprites["printHall"].visible = true

      @sprites["elements"].setBitmap(sprintf("Graphics/Pictures/Trainer Card/card_overlay_2"))
    end
  end

  def pbFlipCard
    sprite = @sprites["card"]
    frames = 16
    yOffset = 8
    pbSEPlay("GUI trainer card flip")

    pbClearSide
    sprite.y -= yOffset
    for i in 0...(frames/2)
      sprite.zoom_x = 1.0 * (1.0 - (i+1).to_f/(frames/2))
      pbUpdateSpriteHash(@sprites)
      Graphics.update
    end

    @front = !@front
    pbRefreshCardBitmap

    for i in 0...(frames/2)
      sprite.zoom_x = 1.0 * ((i+1).to_f/(frames/2))
      pbUpdateSpriteHash(@sprites)
      Graphics.update
    end
    sprite.y += yOffset
    pbRedrawSide
  end

  def pbDrawTrainerCardBack
    overlay = @sprites["overlay"].bitmap
    overlay.clear
    pbDrawItems
    pbDrawListKnob
  end

  def pbTrainerCard
    pbSEPlay("GUI trainer card open")
    loop do
      Graphics.update
      Input.update
      pbUpdate

      if Input.trigger?(Input::LEFT)
        pbSwitchScene(@cardMode - 1)
        pbFlipScene(true)
      end
      if Input.trigger?(Input::RIGHT)
        pbSwitchScene(@cardMode + 1)
        pbFlipScene(false)
      end
      if Input.trigger?(Input::BACK)
        pbPlayCloseMenuSE
        break
      end

      if Input.trigger?(Input::ACTION) || Input.trigger?(Input::USE)
        pbFlipCard if @cardMode == 0
      end
      if !@front && @cardMode == 0
        oldsel = @sel
        if Input.repeat?(Input::DOWN)
          @sel = [@sel+1, pbGetAllCatagories.size - ITEMS_DRAWN].min
        end
        if Input.repeat?(Input::UP)
          @sel = [@sel-1, 0].max
        end
        if Input.repeat?(Input::JUMPDOWN)
          @sel = [@sel+ITEMS_DRAWN, pbGetAllCatagories.size - ITEMS_DRAWN].min
        end
        if Input.repeat?(Input::JUMPUP)
          @sel = [@sel-ITEMS_DRAWN, 0].max
        end
        if @sel != oldsel
          pbDrawItems
          pbDrawListKnob
          pbSEPlay("GUI trainer card sel")
        end
      end
    end
  end

  def pbSwitchScene(sceneID)
    scenes = 2
    scenes = 3 if $game_switches[FRONTIER_PASS_SWITCH]
    newScene = sceneID % scenes
    @cardMode = newScene
  end

  def pbFlipScene(goLeft)
    sprite = @sprites["card"]
    frames = 8
    graphicOffset = 512
    startX = sprite.x
    startX2 = sprite.x + graphicOffset
    endX = sprite.x - graphicOffset
    if goLeft == true
      startX2 = sprite.x - graphicOffset
      endX = sprite.x + graphicOffset
    end
    pbSEPlay("GUI naming tab swap start")

    pbClearSide
    for i in 0...frames
      t = (i + 1).to_f / frames
      sprite.x = startX + (endX - startX) * t
      pbUpdateSpriteHash(@sprites)
      Graphics.update
    end
    sprite.x = endX

    pbRefreshCardBitmap

    for i in 0...frames
      t = (i + 1).to_f / frames
      sprite.x = startX2 + (startX - startX2) * t
      pbUpdateSpriteHash(@sprites)
      Graphics.update
    end
    sprite.x = startX
    pbRedrawSide
    pbSEPlay("GUI naming tab swap end")
  end

  ##############################################################################################################
  # Trainer Card Backside Methods
  ##############################################################################################################

  def pbGetAllCatagories
    return pbDrawBattle + pbDrawParam + pbDrawBenefits + pbDrawSpecial
  end

  def pbDrawItems
    baseColor   = pbBaseTextColor
    shadowColor = pbShadowTextColor
    titleX = 48
    titleY = 32
    offsetY = 32
    contentX = 460
    contentY = titleY
    headerX = 254
    pbDisposeSprite(@sprites, "text")
    @sprites["text"] = TextSprite.new(@viewport)
    @sprites["text"].draw(["Rank",titleX,titleY+(offsetY*0),0,baseColor,shadowColor])
    @sprites["text"].draw([$Trainer.difficulty.to_s,contentX,contentY+(offsetY*0),1,baseColor,shadowColor])
    i = 0
    p = 0
    elements = pbGetAllCatagories
    for i in @sel...(@sel + PokemonTrainerCard_Scene::ITEMS_DRAWN)
      elementText = elements[i].to_s
      newX = titleX
      newY = titleY
      textAlign = 0
      newOffsetY = offsetY*(p+2)
      if ITEM_HEADERS.include?(elementText)
        newX = headerX
        textAlign = 2
        @sprites["text"].draw([elementText,newX,newY+(newOffsetY),textAlign,baseColor,shadowColor])
      elsif elements[i] != nil
        @sprites["text"].draw([elementText,newX,newY+(newOffsetY),textAlign,baseColor,shadowColor])
        elementText = AnswerDictionary(elements[i]).to_s
        newX = contentX
        newY = contentY
        textAlign = 1
        @sprites["text"].draw([elementText,newX,newY+(newOffsetY),textAlign,baseColor,shadowColor])
      end
      i+=1
      p+=1
    end
  end

  def pbDrawBattle
    header = ITEM_HEADERS[0]
    array = ["Player Strength","Opponent Strength","Banned Items",
             "Opponent Level","Opponent Skill"]
    s = [header]
    i = 0
    loop do
      s << array[i]
      i += 1
      break if i >= array.length
    end
    return s
  end

  def pbDrawParam
    header = ITEM_HEADERS[1]
    array = ["Player IVs","Opponent IVs","Player EVs","Opponent EVs",
            "Exp. Modifier","Money Modifier","Capture Modifier"]
    s = [header]
    i = 0
    loop do
      s << array[i]
      i += 1
      break if i >= array.length
    end
    return s
  end

  def pbDrawBenefits
    header = ITEM_HEADERS[2]
    array = ["Pokémon Affection","Exp. All","Recycle Items",
	     "Box Link","Free Doctors"]
    s = [header]
    i = 0
    loop do
      s << array[i]
      i += 1
      break if i >= array.length
    end
    return s
  end

  def pbDrawSpecial
    header = ITEM_HEADERS[3]
    array = ["Nuzlocke","Randomizer","Inverse Battles",
	     "Level Cap","Little Cup","Enhanced Bosses"]
    s = [header]
    i = 0
    loop do
      s << array[i]
      i += 1
      break if i >= array.length
    end
    return s
  end

  def pbDrawListKnob
    knobX = 476
    knobY = 92
    knobEndY = knobY + 112
    step = (pbGetAllCatagories.size - ITEMS_DRAWN)
    @sprites["knob"].x = knobX
    newY = knobY + knobEndY * (@sel.to_f/step.to_f)
    @sprites["knob"].y = newY.clamp(knobY,knobY + knobEndY)
  end

  def ProperString(input)
    string = input.to_s
    if string.capitalize == "True"
      return "On"
    elsif string.capitalize == "False"
      return "Off"
    else
      return string.capitalize
    end
  end

  def pbGetDictionary
    dictionary = { "Nuzlocke" => pbGetNuzlocke, "Randomizer" => pbGetRandomizer, "Inverse Battles" => pbGetInverse, 
		   "Little Cup" => pbGetLittleCup, "Enhanced Bosses" => pbGetHarderBosses, "Opponent Level" => pbGetEnemyLevel,
		   "Pokémon Affection" => pbGetAffectionM, "Exp. All" => pbGetExpShare, "Recycle Items" => pbGetKeepItems,
		   "Box Link" => pbGetBoxLink, "Free Doctors" => pbGetFreeDoctors,
		   "Player Strength" => pbGetPlayerStrength, "Opponent Strength" => pbGetOpponentStrength, 
		   "Banned Items" => pbGetBannedItems, "Exp. Modifier" => pbGetEXPModifier, "Money Modifier" => pbGetMoneyModifier,
		   "Opponent Skill" => pbGetEnemyAI, "Player IVs" => pbGetPlayerIVs,  "Opponent IVs" => pbGetEnemyIVs, 
		   "Player EVs" => pbGetEVGain, "Opponent EVs" => pbGetEnemyEVs, "Level Cap" => pbGetLevelCap, "Capture Modifier" => pbGetCaptureRate}
    return dictionary
  end

  def AnswerDictionary(key)
    return pbGetDictionary.fetch(key)
  end

  def pbGetNuzlocke
#    condition = $game_switches[35]
    condition = $PokemonGlobal.isNuzlocke
    if condition
      return "On"
    else
      return "Off"
    end
  end

  def pbGetRandomizer
    if $PokemonGlobal.isRandomizer
      return "On"
    else
      return "Off"
    end
  end

  def pbGetInverse
    s = $game_switches[Settings::INVERSE_BATTLE]
    return ProperString(s)
  end

  def pbGetLevelCap
    if $game_switches[Settings::FORCED_LEVELCAP]
      cap = Settings::LEVELCAPS[$Trainer.badge_count.clamp(0,Settings::LEVELCAPS.size)]
      lvlMod = $game_variables[Settings::OPPONENT_LEVEL_MOD]
      if lvlMod !=0
        mod = lvlMod
        if $game_switches[Settings::RISING_LEVEL] && lvlMod > 0
          i = $Trainer.badge_count.clamp(0,Settings::RISINGLEVELS.size-1)
          mod += Settings::RISINGLEVELS[i]
        end
        cap += mod
      end
      cap = Settings::MAXIMUM_LEVEL if $game_switches[12] == true
      return cap.clamp(0,Settings::MAXIMUM_LEVEL)
    else
      return "Off"
    end
  end

  def pbGetLittleCup
    s = $game_switches[Settings::LITTLE_CUP]
    return ProperString(s)
  end

  def pbGetHarderBosses
    s = $game_switches[Settings::HARDER_BOSSES]
    return ProperString(s)
  end

  def pbGetPlayerStrength
    val = $game_variables[Settings::PLAYER_DAMAGE_OUPUT]
    if val >= 1
      s = "Max"
    elsif val <= -1
      s = "Min"
    else
      s = "Standard"
    end
    return s
  end

  def pbGetOpponentStrength
    val = $game_variables[Settings::ENEMY_DAMAGE_OUPUT]
    if val >= 1
      s = "Max"
    elsif val <= -1
      s = "Min"
    else
      s = "Standard"
    end
    return s
  end

  def pbGetBannedItems
    s = ""
    i = 0
    if $game_switches[Settings::BAN_RECOVERY]
      s += "Recovery"
      i += 1
    end
    if $game_switches[Settings::BAN_REVIVAL]
      if i > 0
        s += "/"
      end
      s += "Revival"
      i += 1
    end
    if $game_switches[Settings::BAN_XITEMS]
      if i > 0
        s += "/"
      end
      s += "X Items"
      i += 1
    end
    if i >= 3
      s = "All"
    elsif i < 1
      s = "None"
    end
    return s
  end

  def pbGetEXPModifier
    v = ($game_variables[Settings::EXP_MODIFIER].to_f).floor(1)
    s = v.to_s + "x"
    return s
  end

  def pbGetMoneyModifier
    v = ($game_variables[Settings::MONEY_MODIFIER].to_f).floor(1)
    s = v.to_s + "x"
    return s
  end

  def pbGetCaptureRate
    v = ($game_variables[Settings::CATCH_MODIFIER].to_f).floor(1)
    s = v.to_s + "x"
    return s
  end

  def pbGetEnemyAI
    s = "Standard"
    s = "Bad" if $game_variables[Settings::TRAINER_AI] >= 0
    s = "Ok" if $game_variables[Settings::TRAINER_AI] >= 1
    s = "Good" if $game_variables[Settings::TRAINER_AI] >= 32
    s = "Great" if $game_variables[Settings::TRAINER_AI] >= 48
    s = "Excellent" if $game_variables[Settings::TRAINER_AI] >= 100
    return s
  end

  def pbGetPlayerIVs
    s = ""
    if $game_variables[Settings::PLAYER_IVS] < 0
      s = "Random"
    else
      s = $game_variables[Settings::PLAYER_IVS].to_s
    end
    return s
  end

  def pbGetEnemyIVs
    s = ""
    if $game_variables[Settings::OPPONENT_IVS] < 0
      s = "Random"
    else
      s = $game_variables[Settings::OPPONENT_IVS].to_s
    end
    return s
  end

  def pbGetEVGain
    s = !$game_switches[Settings::NO_EVS]
    return ProperString(s)
  end

  def pbGetEnemyEVs
    s = ""
    if $game_variables[Settings::OPPONENT_EVS] < 1
      s = "None"
    elsif $game_variables[Settings::OPPONENT_EVS] > 1
      s = "Specialised"
    else
      s = "Balanced"
    end
    return s
  end

  def pbGetEnemyLevel
    s = ""
    lvlMod = $game_variables[Settings::OPPONENT_LEVEL_MOD]
    if lvlMod != 0
      if lvlMod > 0
        # The "-" is already handled by the variable itself
        s = "+"
      end
      if $game_switches[Settings::RISING_LEVEL] && $game_variables[Settings::OPPONENT_LEVEL_MOD] > 0
        # If Rising Level is turned on, take that into account for Level Modifer
        i = $Trainer.badge_count.clamp(0,Settings::RISINGLEVELS.size-1)
        lvlMod += Settings::RISINGLEVELS[i]
      end
        s += lvlMod.to_s
    else
      s = "Standard"
    end
    return s
  end

  def pbGetAffectionM
    s = $game_switches[Settings::AFFECTION_EFFECTS]
    return ProperString(s)
  end

  def pbGetExpShare
    s = $game_switches[Settings::EXP_ALL]
    return ProperString(s)
  end

  def pbGetKeepItems
    s = $game_switches[Settings::KEEP_ITEMS]
    return ProperString(s)
  end

  def pbGetBoxLink
    s = $game_switches[Settings::BOX_LINK]
    return ProperString(s)
  end

  def pbGetFreeDoctors
    s = $game_switches[Settings::FREE_DOCTORS]
    return ProperString(s)
  end
end