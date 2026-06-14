#===============================================================================
# * Field moves defined by species - by Lexor
#===============================================================================
# 
# This script make the field moves (cut, fly, ...) usable by simply check
# if the Pokemon can learn that move instead of check the moveset
# 
# NOTE: You must insert add the TM/HM moves like softboiled and teleport
#       in pokemon.txt in the TutorMove field if you want to make them usable
# 
# If you wanna add a new field move, after defined in Overworld_FieldMoves their
# code, add in the returned array in getFieldMoves a new element:
#   
#   GameData::Move.get(:"INTERNAL MOVE NAME")
#
#===============================================================================

def getFieldMoves()
  moveList = [
          GameData::Move.get(:TELEPORT),
          GameData::Move.get(:SOFTBOILED),
          GameData::Move.get(:MILKDRINK),
          GameData::Move.get(:SWEETSCENT)]
          
  moveList.push(GameData::Move.get(:DIG)) if $PokemonBag.pbHasItem?(:TM28)
  moveList.push(GameData::Move.get(:CUT)) if $PokemonBag.pbHasItem?(:TM95)
  moveList.push(GameData::Move.get(:FLY)) if $PokemonBag.pbHasItem?(:TM96)
  moveList.push(GameData::Move.get(:SURF)) if $PokemonBag.pbHasItem?(:TM97)
  moveList.push(GameData::Move.get(:STRENGTH)) if $PokemonBag.pbHasItem?(:TM98)
  moveList.push(GameData::Move.get(:ROCKSMASH)) if $PokemonBag.pbHasItem?(:TM99)
  
  return moveList
end
