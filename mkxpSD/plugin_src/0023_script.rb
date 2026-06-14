#==============================================================================#
#                                   Quicksave                                  #
#                                    by Marin                                  #
#==============================================================================#
#         Saves the game with a little animation upon pressing F8.             #
#==============================================================================#
#                    Please give credit when using this.                       #
#==============================================================================#

class Scene_Map
  alias quicksave_update update
  def update
    quicksave_update
    if Input.trigger?(Input::AUX1) && !$game_player.moving? && @mode.nil? && $Trainer.save_slot && $game_switches[65] && !pbMapInterpreterRunning? && !$game_switches[Settings::DISABLE_SAVING]
      Game.save
      pbSEPlay("GUI save choice",100)
      @mode = 0
      @vp = Viewport.new(0,0,Graphics.width,Graphics.height)
      @vp.z = 100000
      @disk = Sprite.new(@vp)
      @disk.bitmap = RPG::Cache.load_bitmap("Graphics/Pictures/", "saveDisk")
      @disk.x, @disk.y = 8, 8
      @disk.opacity = 0
      @arrow = Sprite.new(@vp)
      @arrow.bitmap = RPG::Cache.load_bitmap("Graphics/Pictures/", "saveArrow")
      @arrow.x, @arrow.y = 8, -4
      @arrow.opacity = 0
    end
    if @mode == 0
      @disk.opacity += 64
      @mode = 2 if @disk.opacity >= 128
    end
#    if @mode == 1
#      @arrow.opacity += 32
#      @mode = 2 if @arrow.opacity >= 128
#    end
#    if @mode == 2
#      @arrow.y += 1
#      @mode = 3 if @arrow.y >= 22
#    end
    if @mode == 2
#      @arrow.opacity -= 32
      @disk.opacity -= 16
      if @disk.opacity <= 0
#        @arrow.dispose
        @disk.dispose
        @vp.dispose
        @mode = nil
      end
    end
  end
end
