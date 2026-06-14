#===============================================================================
# * Near-Universal TMs - by FL (Credits will be apreciated)
#===============================================================================
#
# This script is for Pokémon Essentials. It makes all pokémon, except a few 
# specific ones, learn the near universal moves as TM/TR/HM/Tutor, so it's not
# necessary to add the move to the PBS learnset.
#
#== INSTALLATION ===============================================================
#
# Put it above main OR convert into a plugin. No need to add/remove anything
# from PBS.
#
#===============================================================================

NEAR_UNIVERSAL_TUTOR_MOVES = [
  :ATTRACT,:BIDE,:CAPTIVATE,:CONFIDE,:CURSE,:DOUBLETEAM,:DOUBLEEDGE,:ENDURE,
  :FACADE,:FRUSTRATION,:HEADBUTT,:HELPINGHAND,:HIDDENPOWER,:MIMIC,:NATURALGIFT,
  :PROTECT,:RAGE,:REST,:RETURN,:ROUND,:SECRETPOWER,:SLEEPTALK,:SNORE,
  :SUBSTITUTE,:SWAGGER,:TAKEDOWN,:TERABLAST,:TOXIC
]

# Ignores forms
NEAR_INCOMPATIBLE_TUTOR_SPECIES = [
  :CATERPIE,:METAPOD,:WEEDLE,:KAKUNA,:MAGIKARP,:DITTO,:UNOWN,:WOBBUFFET,
  :SMEARGLE,:WURMPLE,:SILCOON,:CASCOON,:WYNAUT,:BELDUM,:KRICKETOT,:BURMY,
  :COMBEE,:TYNAMO,:SCATTERBUG,:SPEWPA,:COSMOG,:COSMOEM,:BLIPBUG,:APPLIN,
  :DITTO
]

NEAR_COMPATIBLE_TUTOR_SPECIES = [
  :MEW,:SMEARGLE,:ARCEUS
]

class Pokemon
  alias :_compatible_with_move_FL_near :compatible_with_move?
  def compatible_with_move?(move_id)
    if (NEAR_COMPATIBLE_TUTOR_SPECIES.include?(@species))
      return true
    end
    if (NEAR_UNIVERSAL_TUTOR_MOVES.include?(move_id) && !NEAR_INCOMPATIBLE_TUTOR_SPECIES.include?(@species))
      return true
    end
    return _compatible_with_move_FL_near(move_id)
  end
end