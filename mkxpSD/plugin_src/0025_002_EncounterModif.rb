# All of the berry effects modifying the encounter
Events.onWildPokemonCreate+=proc {|sender,e|
  if $PokemonSystem.pokesearch_encounter
    pkmn = e[0]
    # Shiny Odds
    shinyRate = 65536 / Settings::SHINY_POKEMON_CHANCE
    odds = shinyRate
    if GameData::Item.exists?(:SHINYCHARM) && $PokemonBag.pbHasItem?(:SHINYCHARM)
      odds = odds / 2
    end
    # IV Bonus
    IV_Bonus = 0
    # Hidden Ability
    HA_Chance = 0
    # Egg Move Chance
    Egg_Chance = 0
    # Chaining
    chain = $game_variables[Settings::DEXNAV_CHAIN].clamp(0,40).to_f / 40
    odds = odds / (1 + chain)
    IV_Bonus += (chain * 4).floor
    HA_Chance += chain * 40
    Egg_Chance += chain * 100
    # Effects
    if $PokemonSystem.current_berry != nil
      case $PokemonSystem.current_berry
      when :ODDINCENSE
        HA_Chance += 10
      when :SEAINCENSE, :WAVEINCENSE
        if (rand(50)==1)
          pkmn.givePokerus
        end
      when :ROSEINCENSE
        items = pkmn.wildHoldItems
        chances = [50,30,20]
        itemrnd = rand(100)
        if (items[0]==items[1] && items[1]==items[2]) || itemrnd<chances[0]
          pkmn.item = items[0]
        elsif itemrnd<(chances[0]+chances[1])
          pkmn.item = items[1]
        elsif itemrnd<(chances[0]+chances[1]+chances[2])
          pkmn.item = items[2]
        end
      when :ROCKINCENSE
        IV_Bonus += 1
      when :LUCKINCENSE
        odds = (odds / 1.25)
      end
    end
    # IV Bonus
    if $game_variables[Settings::PLAYER_IVS] < 0
      buffs = IV_Bonus
      stats = [:HP, :ATTACK, :DEFENSE, :SPECIAL_ATTACK, :SPECIAL_DEFENSE, :SPEED]
      stats = stats.sort{rand()-0.5}
      buffs = stats[0...buffs]
      buffs.each do |stat|
        pkmn.iv[stat] = Pokemon::IV_STAT_LIMIT
      end
    end
    # Give Pokemon Hidden Ability    
    if (rand(100) < HA_Chance)
      pkmn.ability_index = 2
    end
    # Reset Moves
    # Give Egg Move (If applicable)
    pkmn.reset_moves
    if (rand(100) < Egg_Chance)
      babyspecies = GameData::Species.get(pkmn.species).get_baby_species
      eggmoves = GameData::Species.get_species_form(babyspecies,pkmn.form).egg_moves
      if eggmoves.length > 0
        move = eggmoves.sample
        pkmn.learn_move(move); pkmn.add_first_move(move)
      end
    end
    # See if Pokemon is Shiny
    if (rand(odds.to_i)==1)
      pkmn.shiny = true
    end
    # Other stuff
    pkmn.calc_stats
  end
}
