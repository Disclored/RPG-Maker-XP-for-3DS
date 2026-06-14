#===============================================================================
# ■ Fly Animation by KleinStudio
# http://pokemonfangames.com
#===============================================================================
# bo4p5687 (update)
#===============================================================================
BIRD_ANIMATION_TIME = 10
HiddenMoveHandlers::UseMove.add(:FLY,proc { |move,pokemon|
  if !$PokemonTemp.flydata
    pbMessage(_INTL("Can't use that here."))
    next false
  end
  if !pbHiddenMoveAnimation(pokemon)
    pbMessage(_INTL("{1} used {2}!",pokemon.name,GameData::Move.get(move).name))
  end
  pbFlyAnimation
  pbCancelVehicles
  pbFadeOutIn {
    $game_temp.player_new_map_id    = $PokemonTemp.flydata[0]
    $game_temp.player_new_x         = $PokemonTemp.flydata[1]
    $game_temp.player_new_y         = $PokemonTemp.flydata[2]
    $game_temp.player_new_direction = 2
    $PokemonTemp.flydata = nil
    $scene.transfer_player
    $game_map.autoplay
    $game_map.refresh
    
  }
  pbFlyAnimation(false)
  pbEraseEscapePoint
  next true
})
class Game_Character
  def setOpacity(value)
    @opacity = value
  end
end
def pbFlyAnimation(landing=true)
  if landing
    $game_player.turn_left
    pbSEPlay("flybird")
  end
	width  = Settings::SCREEN_WIDTH
	height = Settings::SCREEN_HEIGHT
  @flybird = Sprite.new
  @flybird.bitmap = RPG::Cache.picture("flybird")
  @flybird.ox = @flybird.bitmap.width/2
  @flybird.oy = @flybird.bitmap.height/2
  @flybird.x  = width + @flybird.bitmap.width
  @flybird.y  = height/4
  loop do
    pbUpdateSceneMap
    if @flybird.x > (width / 2 + 10)
      @flybird.x -= (width + @flybird.bitmap.width - width / 2).div BIRD_ANIMATION_TIME
      @flybird.y -= (height / 4 - height / 2).div BIRD_ANIMATION_TIME
    elsif @flybird.x <= (width / 2 + 10) && @flybird.x >= 0
      @flybird.x -= (width + @flybird.bitmap.width - width / 2).div BIRD_ANIMATION_TIME
      @flybird.y += (height / 4 - height / 2).div BIRD_ANIMATION_TIME
      $game_player.setOpacity(landing ? 0 : 255)
    else
      break
    end
    Graphics.update
  end
  @flybird.dispose
  @flybird = nil
end
