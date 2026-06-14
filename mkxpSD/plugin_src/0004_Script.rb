# Water bubles script
# KleinStudio, WolfPP
# bo4p5687 (update v.19)

# Fix event comment
def pbEventCommentInput(*args)
  parameters = []
  list = *args[0].list   # Event or event page
  elements = *args[1]    # Number of elements
  trigger = *args[2]     # Trigger
  return nil if list == nil
  return nil unless list.is_a?(Array)
  for item in list
    next unless item.code == 108 || item.code == 408
    if item.parameters[0] == trigger[0]
      start = list.index(item) + 1
      finish = start + elements[0]
      for id in start...finish
        next if !list[id]
        parameters.push(list[id].parameters[0])
      end
      return parameters
    end
  end
  return nil
end

# Set terrain tag
module GameData
  class TerrainTag
		attr_reader :beach

		alias water_buble_init initialize
		def initialize(hash)
			water_buble_init(hash)
			@beach = hash[:beach] || false
		end
	end
end
# Set number of terrain tag, here
GameData::TerrainTag.register({
  :id                     => :Beach,
  :id_number              => 25,
  :beach                  => true
})

module WaterBublesVariables
	# Set pokemon can't have this animations.
	FOLLOWING_NOT_BUBLES = [
		# Examples:
		17
	]

	def self.terrain_beach(event=nil)
		event = $game_player if !event
    return $MapFactory.getTerrainTag(event.map.map_id, event.x, event.y) if $MapFactory
    return $game_map.terrain_tag(event.x, event.y)
  end

	def self.bubles(event=nil)
		return true if $PokemonSystem.performance==1
		if event != $game_player
			return true if event.character_name == "" || event.character_name == "nil" || event.name.include?("/nowater/")
			return true if pbEventCommentInput(event, 0, "NoWater")
			if $Trainer.party.length > 0
				return true if (!($game_map.events[event] && $game_map.events[event].name == "Dependent") && (FOLLOWING_NOT_BUBLES.include?($Trainer.party[0].species) || $PokemonGlobal.bicycle))
			end
		end
		return true if !self.terrain_beach(event).beach
		return false
	end

	class CreateSprite
		attr_reader   :visible
		
		def initialize(sprite, event, viewport=nil)
			@rsprite   = sprite
			@sprite    = Sprite.new(@viewport)
			@event     = event
			@viewport  = viewport   
			@wateranim = false
			@frame  = 0
			@frames = 4
			@totalFrames  = 0
			@currentIndex = 0
			@disposed   = false
			@bitmapFile = RPG::Cache.load_bitmap("Graphics/Pictures/OverworldEffects/", "Water bubles")
			@water = Bitmap.new(@bitmapFile.width, @bitmapFile.height)
			@water.blt(0, 0, @bitmapFile, Rect.new(0, 0, @bitmapFile.width, @bitmapFile.height))
			@cws = @water.height * 2
			@chs = @water.height * 2
			@totalFrames = @water.width / @water.height
			@animationFrames = @totalFrames * @frames
			@loop_points  = [0, @totalFrames]
			@actualBitmap = Bitmap.new(@cws, @chs)
			update_actual_bitmap
			update
		end
	
		def update_actual_bitmap
			@actualBitmap.clear
			@actualBitmap.stretch_blt(Rect.new(0, 0, @cws, @chs), @water, Rect.new(@currentIndex * @cws / 2, 0, @cws / 2, @chs / 2))
		end
	
		def dispose
			return if @disposed
			@actualBitmap.dispose if @actualBitmap && !@actualBitmap.disposed?
			@sprite.dispose if @sprite
			@sprite    = nil
			@disposed  = true
			@wateranim = false
		end
	
		def disposed? = @disposed
	
		def createWaterAnim(x2,y2)
			return if @wateranim
			@sprite.bitmap = @actualBitmap
			@sprite.x = x2
			@sprite.y = y2
			pbDayNightTint(@sprite)
			@wateranim = true
		end
	
		def updateAnim
			return if !@wateranim || (@sprite && @sprite.disposed?)
			@frame += 1
			if @frame >= @frames
				@currentIndex += 1
				@currentIndex  = @loop_points[0] if @currentIndex >= @loop_points[1]
				@frame = 0
			end
			update_actual_bitmap
			@sprite.bitmap = @actualBitmap
		end
	
		def visible=(value)
			@sprite.visible = value if @sprite && !@sprite.disposed?
		end
	
		def update
			return if disposed? || !$scene || !$scene.is_a?(Scene_Map)
			# Just-in-time disposal of sprite
			if WaterBublesVariables.bubles(@event) || @event.jumping?
				@sprite.dispose if @sprite
				@sprite = nil
				return
			end
			# Just-in-time creation of sprite
			@sprite = Sprite.new(@viewport) if !@sprite
			cw = @cws
			ch = @chs
			updateAnim
			@wateranim = false if @sprite && @sprite.disposed?
			x = @rsprite.x - @rsprite.ox
			y = @rsprite.y - @rsprite.oy
			createWaterAnim(x, y)
			@sprite.x       = @rsprite.x
			@sprite.y       = @rsprite.y
			@sprite.ox      = cw / 2
			@sprite.oy      = ch - 4
			@sprite.z       = @rsprite.z
			@sprite.zoom_x  = @rsprite.zoom_x
			@sprite.zoom_y  = @rsprite.zoom_y
			@sprite.tone    = @rsprite.tone
			@sprite.color   = @rsprite.color
			@sprite.opacity = @rsprite.opacity
			@sprite.update
		end
	end
	
end

class Sprite_Character
	alias water_buble_init initialize
	alias water_buble_visible visible=
	alias water_buble_dispose dispose
	alias water_buble_update update

	def initialize(viewport, character=nil)
    @viewport = viewport
    water_buble_init(viewport,character)
  end

	def visible=(value)
    water_buble_visible(value)
		@waterbubblebitmap.visible = value if @waterbubblebitmap
	end

	def dispose
		water_buble_dispose
    @waterbubblebitmap.dispose if @waterbubblebitmap
    @waterbubblebitmap = nil
	end

	def update
		water_buble_update
		@waterbubblebitmap = WaterBublesVariables::CreateSprite.new(self, @character, @viewport) if !@waterbubblebitmap
		@waterbubblebitmap.update
	end
end
