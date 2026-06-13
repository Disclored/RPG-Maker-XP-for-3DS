class PokemonSummary_Scene
  def drawPageThree
    overlay = @sprites["overlay"].bitmap
    base = Color.new(248,248,248)
    shadow = Color.new(104,104,104)
    baseUp = Color.new(224,152,144)
    shadowUp = Color.new(248,56,32)
    baseDown = Color.new(136,168,208)
    shadowDown = Color.new(24,112,216)
    # Determine which stats are boosted and lowered by the Pokémon's nature
    statstring = {}
    statbase = {}
    statshadows = {}
    ivbase = {}
    ivshadows = {}
    evbase = {}
    evshadows = {}
    GameData::Stat.each_main do |s|
      statstring[s.id] = ""
      statbase[s.id] = base
      statshadows[s.id] = shadow
      # IV Colours
      if @pokemon.iv[s.id] >= Pokemon::IV_STAT_LIMIT
        ivbase[s.id] = baseUp
        ivshadows[s.id] = shadowUp
      else
        ivbase[s.id] = base
        ivshadows[s.id] = shadow
      end
      # EV Colours
      if @pokemon.ev[s.id] >= Pokemon::EV_STAT_LIMIT || @pokemon.ev.values.sum >= Pokemon::EV_LIMIT
        evbase[s.id] = baseUp
        evshadows[s.id] = shadowUp
      else
        evbase[s.id] = base
        evshadows[s.id] = shadow
      end
    end
    if !@pokemon.shadowPokemon? || @pokemon.heartStage > 3
      @pokemon.nature_for_stats.stat_changes.each do |change|
        # Neutral
        if change[1] > 0
          # Increase
          statstring[change[0]] = "+"
          statbase[change[0]] = baseUp
          statshadows[change[0]] = shadowUp
        elsif change[1] < 0
          # Decrease
          statstring[change[0]] = "-"
          statbase[change[0]] = baseDown
          statshadows[change[0]] = shadowDown
        end
      end
    end
    # Write various bits of text
    textpos = [
       [_INTL("HP" + statstring[:HP].to_s),243,70,2,statbase[:HP],statshadows[:HP]],
       [sprintf("%d/%d",@pokemon.hp,@pokemon.totalhp),382,70,1,base,shadow],
	   [sprintf("%d",@pokemon.iv[:HP]),444,70,1,ivbase[:HP],ivshadows[:HP]],
	   [sprintf("%d",@pokemon.ev[:HP]),500,70,1,evbase[:HP],evshadows[:HP]],
       [_INTL("Attack" + statstring[:ATTACK].to_s),225,114,0,statbase[:ATTACK],statshadows[:ATTACK]],
       [sprintf("%d",@pokemon.attack),382,114,1,base,shadow],
	   [sprintf("%d",@pokemon.iv[:ATTACK]),444,114,1,ivbase[:ATTACK],ivshadows[:ATTACK]],
	   [sprintf("%d",@pokemon.ev[:ATTACK]),500,114,1,evbase[:ATTACK],evshadows[:ATTACK]],
       [_INTL("Defense" + statstring[:DEFENSE].to_s),225,146,0,statbase[:DEFENSE],statshadows[:DEFENSE]],
       [sprintf("%d",@pokemon.defense),382,146,1,base,shadow],
	   [sprintf("%d",@pokemon.iv[:DEFENSE]),444,146,1,ivbase[:DEFENSE],ivshadows[:DEFENSE]],
	   [sprintf("%d",@pokemon.ev[:DEFENSE]),500,146,1,evbase[:DEFENSE],evshadows[:DEFENSE]],
       [_INTL("Sp. Atk" + statstring[:SPECIAL_ATTACK].to_s),225,178,0,statbase[:SPECIAL_ATTACK],statshadows[:SPECIAL_ATTACK]],
       [sprintf("%d",@pokemon.spatk),382,178,1,base,shadow],
	   [sprintf("%d",@pokemon.iv[:SPECIAL_ATTACK]),444,178,1,ivbase[:SPECIAL_ATTACK],ivshadows[:SPECIAL_ATTACK]],
	   [sprintf("%d",@pokemon.ev[:SPECIAL_ATTACK]),500,178,1,evbase[:SPECIAL_ATTACK],evshadows[:SPECIAL_ATTACK]],
       [_INTL("Sp. Def" + statstring[:SPECIAL_DEFENSE].to_s),225,210,0,statbase[:SPECIAL_DEFENSE],statshadows[:SPECIAL_DEFENSE]],
       [sprintf("%d",@pokemon.spdef),382,210,1,base,shadow],
	   [sprintf("%d",@pokemon.iv[:SPECIAL_DEFENSE]),444,210,1,ivbase[:SPECIAL_DEFENSE],ivshadows[:SPECIAL_DEFENSE]],
	   [sprintf("%d",@pokemon.ev[:SPECIAL_DEFENSE]),500,210,1,evbase[:SPECIAL_DEFENSE],evshadows[:SPECIAL_DEFENSE]],
       [_INTL("Speed" + statstring[:SPEED].to_s),225,242,0,statbase[:SPEED],statshadows[:SPEED]],
       [sprintf("%d",@pokemon.speed),382,242,1,base,shadow],
	   [sprintf("%d",@pokemon.iv[:SPEED]),444,242,1,ivbase[:SPEED],ivshadows[:SPEED]],
	   [sprintf("%d",@pokemon.ev[:SPEED]),500,242,1,evbase[:SPEED],evshadows[:SPEED]],
       [_INTL("Ability"),224,278,0,base,shadow]
    ]
    # Draw ability name and description
    ability = @pokemon.ability
    if ability
      textpos.push([ability.name,322,278,0,base,shadow])
      # Temp font size change
      oldName = overlay.font.name
      oldSize = overlay.font.size
      pbSetSmallFont(overlay)
      drawTextEx(overlay,224,320,282,2,ability.description,base,shadow)
      overlay.font.name = oldName
      overlay.font.size = oldSize
    end
    # Draw all text
    pbDrawTextPositions(overlay,textpos)
    # Draw HP bar
    if @pokemon.hp>0
      w = @pokemon.hp*96*1.0/@pokemon.totalhp
      w = 1 if w<1
      w = ((w/2).round)*2
      hpzone = 0
      hpzone = 1 if @pokemon.hp<=(@pokemon.totalhp/2).floor
      hpzone = 2 if @pokemon.hp<=(@pokemon.totalhp/4).floor
      imagepos = [
         ["Graphics/Pictures/Summary/overlay_hp",339,111,0,hpzone*6,w,6]
      ]
      pbDrawImagePositions(overlay,imagepos)
    end
  end
end