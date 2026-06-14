MOVETUTOR=34
BANVAR=35

BANNED_TUTOR_MOVES = [[],[
  :VOLTTACKLE,:PAYDAY,:SHELLSIDEARM,:BARRAGE,:BONEMERANG,:SHADOWBONE,:STRANGESTEAM,:SOFTBOILED,:CAMOUFLAGE,
  :LOVELYKISS,:RAGINGBULL,:SPLASH,:CONVERSION,:CONVERSION2,:FREEZINGGLARE,:THUNDEROUSKICK,:FIERYWRATH,:PSYSTRIKE,
  :INFERNALPARADE,:TOXICTHREAD,:CHILLYRECEPTION,:EERIESPELL,:HIDDENPOWER,:TWINBEAM,:HYPERDRILL,:BARBBARRAGE,:PSYSHIELDBASH,
  :SKETCH,:TRIPLEKICK,:MILKDRINK,:AEROBLAST,:SACREDFIRE,:ASSIST,:TAILGLOW,:SPORE,:MISTBALL,
  :LUSTERPURGE,:ORIGINPULSE,:PRECIPICEBLADES,:DRAGONASCENT,:DOOMDESIRE,:PSYCHOBOOST,:DEFENDORDER,:ATTACKORDER,:CHATTER,
  :MYSTICALPOWER,:ROAROFTIME,:SPACIALREND,:MAGMASTORM,:CRUSHGRIP,:SHADOWFORCE,:LUNARDANCE,:LUNARBLESSING,:TAKEHEART,
  :HEARTSWAP,:DARKVOID,:SEEDFLARE,:JUDGMENT,:SEARINGSHOT,:VCREATE,:CEASELESSEDGE,:VICTORYDANCE,:NIGHTDAZE,
  :BITTERMALICE,:GEARGRIND,:SHIFTGEAR,:WATERSHURIKEN,:SNAPTRAP,:ESPERWING,:FIERYDANCE,:SACREDSWORD,:BLEAKWINDSTORM,
  :WILDBOLTSTORM,:FUSIONFLARE,:BLUEFLARE,:FUSIONBOLT,:BOLTSTRIKE,:SANDSEARSTORM,:GLACIATE,:ICEBURN,:FREEZESHOCK,
  :SECRETSWORD,:RELICSONG,:TECHNOBLAST,:MATBLOCK,:LIGHTOFRUIN,:KINGSSHIELD,:FLYINGPRESS,:SHELTER,:FAIRYLOCK,
  :GEOMANCY,:OBLIVIONWING,:LANDSWRATH,:THOUSANDARROWS,:THOUSANDWAVES,:COREENFORCER,:DIAMONDSTORM,:HYPERSPACEHOLE,:HYPERSPACEFURY,
  :STEAMERUPTION,:SPIRITSHACKLE,:TRIPLEARROWS,:DARKESTLARIAT,:SPARKLINGARIA,:BEAKBLAST,:REVELATIONDANCE,:POLLENPUFF,:BANEFULBUNKER,
  :TROPKICK,:FLORALHEALING,:INSTRUCT,:SHOREUP,:PURIFY,:MULTIATTACK,:SHELLTRAP,:ZINGZAP,:ANCHORSHOT,
  :CLANGINGSCALES,:CLANGOROUSSOUL,:NATURESMADNESS,:SUNSTEELSTRIKE,:MOONGEISTBEAM,:PRISMATICLASER,:PHOTONGEYSER,:SPECTRALTHIEF,:MINDBLOWN,
  :PLASMAFISTS,:DOUBLEIRONBASH,:DRUMBEATING,:COURTCHANGE,:PYROBALL,:SNIPESHOT,:STUFFCHEEKS,:JAWLOCK,:TARSHOT,
  :GRAVAPPLE,:APPLEACID,:OVERDRIVE,:OCTOLOCK,:TEATIME,:MAGICPOWDER,:FALSESURRENDER,:SPIRITBREAK,:OBSTRUCT,
  :METEORASSAULT,:DECORATE,:NORETREAT,:AURAWHEEL,:DRAGONDARTS,:BEHEMOTHBLADE,:BEHEMOTHBASH,:DYNAMAXCANNON,:WICKEDBLOW,
  :SURGINGSTRIKES,:JUNGLEHEALING,:THUNDERCAGE,:DRAGONENERGY,:GLACIALLANCE,:ASTRALBARRAGE,:STONEAXE,:BLOODMOON,:DIRECLAW,
  :SPRINGTIDESTORM,:FLOWERTRICK,:TORCHSONG,:AQUASTEP,:SILKTRAP,:DOUBLESHOCK,:POPULATIONBOMB,:TERRAINPULSE,:SALTCURE,
  :ARMORCANNON,:BITTERBLADE,:DOODLE,:SPICYEXTRACT,:SPEEDSWAP,:LUMINACRASH,:GIGATONHAMMER,:TRIPLEDIVE,:JETPUNCH,
  :SPINOUT,:WICKEDTORQUE,:BLAZINGTORQUE,:NOXIOUSTORQUE,:MAGICALTORQUE,:COMBATTORQUE,:MORTALSPIN,:FILLETAWAY,:ORDERUP,
  :RAGEFIST,:KOWTOWCLEAVE,:STEELROLLER,:GLAIVERUSH,:MAKEITRAIN,:RUINATION,:COLLISIONCOURSE,:ELECTRODRIFT,:HYDROSTEAM,
  :PSYBLADE,:SYRUPBOMB,:MATCHAGOTCHA,:IVYCUDGEL,:ELECTROSHOT,:FICKLEBEAM,:BURNINGBULWARK,:THUNDERCLAP,
  :MIGHTYCLEAVE,:TACHYONCUTTER,:TERASTARSTORM,:MALIGNANTCHAIN,:QUIVERDANCE,:TERABLAST,:SHELLSMASH,:LASTRESPECTS,:CRAFTYSHIELD,
  :ETERNABEAM,:LIFEDEW,:MIMIC,:MINDBLOWN,:MIRRORCOAT,:MIRRORMOVE,:REVIVALBLESSING,:SHEDTAIL,:SPIKYSHIELD,
  :SPOTLIGHT,:STRUGGLE,:TIDYUP,:TRANSFORM,:WIDEGUARD,:CELEBRATE,:HOLDHANDS,:SPLASH,:HAPPYHOUR,:GUILLOTINE,:HORNDRILL,:FISSURE,:SHEERCOLD,
  :REFLECTTYPE
],[],[]]

#Everything below is just here to check if your pokémons can learn moves 
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
    moves = pkmn.species_data.tutor_moves
    if (pkmn.species_data.type1 == :DRAGON || pkmn.species_data.type2 == :DRAGON)
      moves.push(:DRACOMETEOR)
    end
    if (pkmn.species_data.type1 == :STEEL || pkmn.species_data.type2 == :STEEL)
      moves.push(:STEELBEAM)
    end
    return moves
end
   
def pbGetRelearnableMoves(pkmn)
    return [] if !pkmn || pkmn.egg? || pkmn.shadowPokemon?
    moves = []
    pkmn.getMoveList.each do |m|
      next if m[0] > pkmn.level || pkmn.hasMove?(m[1])
      moves.push(m[1]) if !moves.include?(m[1])
    end
    tmoves = []
    if pkmn.first_moves
      for i in pkmn.first_moves
        tmoves.push(i) if !pkmn.hasMove?(i) && !moves.include?(i)
      end
    end
    movelist = []
    if $game_variables[MOVETUTOR]==0
      movelist = tmoves + moves  
    end
    # Egg Moves
    if $game_variables[MOVETUTOR]==1
      eggmoves=eggMoves(pkmn)
        for i in eggmoves
        movelist.push(i) if !pkmn.hasMove?(i) && !moves.include?(i)
      end
    end
    # Tutor Moves
    if $game_variables[MOVETUTOR]==2
      tutormoves= tutorMoves(pkmn)
      for i in tutormoves
        movelist.push(i) if !pkmn.hasMove?(i) && !moves.include?(i)
      end
    end
    # Hack Moves
    if $game_variables[MOVETUTOR]==3
      hmoves = hackmoves
      for i in hmoves
         movelist.push(i) if !pkmn.hasMove?(i) && !moves.include?(i)
      end
    end
    # STAB Moves
    if $game_variables[MOVETUTOR]==4
      smoves = stabmoves(pkmn)
      for i in smoves
         movelist.push(i) if !pkmn.hasMove?(i) && !movelist.include?(i) && !BANNED_TUTOR_MOVES[$game_variables[BANVAR]].include?(i)
      end
    end
    movelist.sort! { |a, b| a.downcase <=> b.downcase } #sort moves alphabetically
    return movelist | []   # remove duplicates
end
  
def can_learn_move(pkmn)
	return false if pkmn.egg? || pkmn.shadowPokemon?
	return true if $game_variables[MOVETUTOR]==3 || $game_variables[MOVETUTOR]==4
	moves = pbGetRelearnableMoves(pkmn)
echoln(pkmn.to_s + ": " + moves.to_s)
    if moves!=[]
	 return true
	else
	 return false
	end
end 