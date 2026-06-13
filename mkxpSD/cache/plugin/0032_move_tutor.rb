class MoveRelearnerScreen
MOVETUTOR=34
  def eggMoves(pkmn)
    babyspecies=pkmn.species
    babyspecies = GameData::Species.get(babyspecies).get_baby_species(false, nil, nil)
    eggmoves=GameData::Species.get_species_form(babyspecies, pkmn.form).egg_moves
    return eggmoves
  end
  
  def getMoveList
    return species_data.moves
  end
  
  def tutorMoves(pkmn)
    return pkmn.species_data.tutor_moves
  end
  
  def hackmoves
    moves=[]
	GameData::Move.each { |i| moves.push(i.id) }
	return moves
  end

  def stabmoves(pkmn)
    smoves=[]
	GameData::Move.each { |i| smoves.push(i.id) if (i.type==pkmn.type1 || i.type==pkmn.type2) }
	return smoves
  end

	
  def pbGetRelearnableMoves(pkmn)
    return [] if !pkmn || pkmn.egg? || pkmn.shadowPokemon?
    # Get Level Up moves PKMN doesn't know yet, but can learn.
    moves = []
    pkmn.getMoveList.each do |m|
      next if m[0] > pkmn.level || pkmn.hasMove?(m[1])
      moves.push(m[1]) if !moves.include?(m[1])
    end
    # Get PKMN first moves (like level 1 moves)
    tmoves = []
    if pkmn.first_moves
      for i in pkmn.first_moves
        tmoves.push(i) if !pkmn.hasMove?(i) && !moves.include?(i)
      end
    end

    # Move Relearner
    if $game_variables[MOVETUTOR]==0
      moves = tmoves + moves
    end

    # Teaches Egg Moves
    if $game_variables[MOVETUTOR]==1
      moves = []
      eggmoves=eggMoves(pkmn)
      for i in eggmoves
        moves.push(i) if !pkmn.hasMove?(i) && !moves.include?(i)
      end
      moves = eggmoves
    end

    # Teaches Tutor moves and TMs
    if $game_variables[MOVETUTOR]==2
      moves = []
      tutormoves= tutorMoves(pkmn)
	  for i in tutormoves
        moves.push(i) if !pkmn.hasMove?(i) && !moves.include?(i)
      end
      moves = tutormoves
    end

    # Teaches every move in the game
    if $game_variables[MOVETUTOR]==3
      moves = []
      hmoves = hackmoves
      for i in hmoves
        moves.push(i) if !pkmn.hasMove?(i) && !moves.include?(i)
      end
      moves = hmoves
    end

    # Teaches every move that is the same type
    if $game_variables[MOVETUTOR]==4
      moves = []  
      smoves = stabmoves(pkmn)
      for i in smoves
        moves.push(i) if !pkmn.hasMove?(i) && !moves.include?(i) && !BANNED_TUTOR_MOVES[$game_variables[BANVAR]].include?(i)
      end
    end

    moves.sort! { |a, b| a.downcase <=> b.downcase } #sort moves alphabetically
    return moves | []   # remove duplicates
  end
  
end


 

