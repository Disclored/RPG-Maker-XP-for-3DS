# Don't touch these lines
module GameData
  class Metadata
		def self.limit_quantity_player = (SCHEMA.size - 8)
	end
end

def pbChangePlayer(id)
  return false if id < 0 || id >= GameData::Metadata.limit_quantity_player
  meta = GameData::Metadata.get_player(id)
  return false if !meta
  $Trainer.character_ID = id
  $Trainer.trainer_type = meta[0]
  $game_player.character_name = meta[1]
end

module GenderPickSelection

	class Show

		# If SET_NAME is true, you need to add name of bitmap in NAME_OF_BITMAP
		# If not, it will use name in metadata, first name.
		# Ex: PlayerA = POKEMONTRAINER_Red,... -> Name of bitmap need to set POKEMONTRAINER_Red
		SET_NAME = false
		NAME_OF_BITMAP = [
			# Example
			"AvatarA", # Name of first Avatar: Player A
			"AvatarB", # Name of second Avatar: Player B
			"AvatarC", # Name of third Avatar: Player C
			"AvatarD", # Name of fourth Avatar: Player D
			"AvatarE", # Name of fifth Avatar: Player E
			"AvatarF", # Name of sixth Avatar: Player F
			"AvatarG", # Name of seventh Avatar: Player G
			"AvatarH"  # Name of eighth Avatar: Player H
		]

		def initialize
			@sprites = {}
			# Viewport
			@viewport = Viewport.new(0, 0, Graphics.width, Graphics.height)
			@viewport.z = 99999
			# Value
			quantity = GenderPickSelection.quant_registered_player
			@realquant = quantity > 10 ? 10 : quantity
			@tabAmount = 4
			@player = []
			begin
				quantity.times { |i|
					if SET_NAME
						@player << NAME_OF_BITMAP[i]
					else
						meta = GameData::Metadata.get_player(i)
						@player << meta[0]
					end
				}
			rescue
				p "Error: set @player"
				Kernel.exit!
			end
			@tab = []
			@bg = 0
			@oldbg = 0
			@startnum = 0
			@position = 0
			@currentTab = 0
			@exit = false
		end

		def set_player(id) = pbChangePlayer(id)

	end

	def self.quant_registered_player
		i = 0
		loop do
			meta = GameData::Metadata.get_player(i)
			!meta ? (break) : (i+=1)
		end
		return i
	end

	def self.show
		s = Show.new
		s.show
		s.endScene
	end

end