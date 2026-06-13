# If you want to increase quantity of players, you need to do like this example.
# This action changes limit of players in metadata but you need to add new player in file PBS.
# You need to add in PBS, 'trainertypes'. This file sets gender of player and set new player if you want.
# In this example, I add 2 player: playerK and playerL
# If you don't want to add, just add =begin below this line and add =end above line 'Add above this line'

module GameData
  class Metadata

		class << self
			alias add_new_editor_prop editor_properties
			alias add_new_get_player get_player
		end
		alias add_new_init initialize
		alias add_new_property_from_string property_from_string

		# Add new player, here
		attr_reader :player_K
		attr_reader :player_L
		attr_reader :player_M
		attr_reader :player_N

		def self.add_schema_metadata(name=nil)
			return if name.nil?
			hash = SCHEMA
			size = hash.size
			hash[name] = [size+1, "esssssss", :TrainerType]
			const_set(:SCHEMA, hash)
		end

		def self.editor_properties
			ret = add_new_editor_prop
			ret << ["PlayerK", PlayerProperty, _INTL("Specifies player K.")]
			ret << ["PlayerL", PlayerProperty, _INTL("Specifies player L.")]
			ret << ["PlayerK", PlayerProperty, _INTL("Specifies player M.")]
			ret << ["PlayerL", PlayerProperty, _INTL("Specifies player N.")]
			return ret
		end

		def self.get_player(id)
			case id
			when 8 then return self.get.player_K
			when 9 then return self.get.player_L
			when 10 then return self.get.player_M
			when 11 then return self.get.player_N
			else return add_new_get_player(id)
			end
		end

		def initialize(hash)
			add_new_init(hash)
			@player_K = hash[:player_K]
			@player_L = hash[:player_L]
			@player_M = hash[:player_M]
			@player_N = hash[:player_N]
		end

		def property_from_string(str)
			case str
			when "PlayerK" then return @player_K
			when "PlayerL" then return @player_L
			when "PlayerM" then return @player_M
			when "PlayerN" then return @player_N
			else return add_new_property_from_string(str)
			end
		end

	end
end

GameData::Metadata.add_schema_metadata("PlayerK")
GameData::Metadata.add_schema_metadata("PlayerL")
GameData::Metadata.add_schema_metadata("PlayerM")
GameData::Metadata.add_schema_metadata("PlayerN")

def pbEditMetadata(map_id = 0)
  mapinfos = pbLoadMapInfos
  data = []
  if map_id == 0   # Global metadata
    map_name = _INTL("Global Metadata")
    metadata = GameData::Metadata.get
    properties = GameData::Metadata.editor_properties
  else   # Map metadata
    map_name = mapinfos[map_id].name
    metadata = GameData::MapMetadata.try_get(map_id)
    metadata = GameData::Metadata.new({}) if !metadata
    properties = GameData::MapMetadata.editor_properties
  end
  properties.each do |property|
    data.push(metadata.property_from_string(property[0]))
  end
  if pbPropertyList(map_name, data, properties, true)
    if map_id == 0   # Global metadata
      # Construct metadata hash
      metadata_hash = {
        :id                 => map_id,
        :home               => data[0],
        :wild_battle_BGM    => data[1],
        :trainer_battle_BGM => data[2],
        :wild_victory_ME    => data[3],
        :trainer_victory_ME => data[4],
        :wild_capture_ME    => data[5],
        :surf_BGM           => data[6],
        :bicycle_BGM        => data[7],
        :player_A           => data[8],
        :player_B           => data[9],
        :player_C           => data[10],
        :player_D           => data[11],
        :player_E           => data[12],
        :player_F           => data[13],
        :player_G           => data[14],
        :player_H           => data[15],

				#-----------------------------------------------------------------------------------
				# Add new
				#-----------------------------------------------------------------------------------
				:player_K           => data[16],
        			:player_L           => data[17],
        			:player_M           => data[18],
        			:player_N           => data[19]


      }
      # Add metadata's data to records
      GameData::Metadata.register(metadata_hash)
      GameData::Metadata.save
    else   # Map metadata
      # Construct metadata hash
      metadata_hash = {
        :id                   => map_id,
        :outdoor_map          => data[0],
        :announce_location    => data[1],
        :can_bicycle          => data[2],
        :always_bicycle       => data[3],
        :teleport_destination => data[4],
        :weather              => data[5],
        :town_map_position    => data[6],
        :dive_map_id          => data[7],
        :dark_map             => data[8],
        :safari_map           => data[9],
        :snap_edges           => data[10],
        :random_dungeon       => data[11],
        :battle_background    => data[12],
        :wild_battle_BGM      => data[13],
        :trainer_battle_BGM   => data[14],
        :wild_victory_ME      => data[15],
        :trainer_victory_ME   => data[16],
        :wild_capture_ME      => data[17],
        :town_map_size        => data[18],
        :battle_environment   => data[19]
      }
      # Add metadata's data to records
      GameData::MapMetadata.register(metadata_hash)
      GameData::MapMetadata.save
    end
    Compiler.write_metadata
  end
end

module Compiler
  module_function
  def compile_metadata(path = "PBS/metadata.txt")
    GameData::Metadata::DATA.clear
    GameData::MapMetadata::DATA.clear
    # Read from PBS file
    File.open(path, "rb") { |f|
      FileLineData.file = path   # For error reporting
      # Read a whole section's lines at once, then run through this code.
      # contents is a hash containing all the XXX=YYY lines in that section, where
      # the keys are the XXX and the values are the YYY (as unprocessed strings).
      pbEachFileSection(f) { |contents, map_id|
        schema = (map_id == 0) ? GameData::Metadata::SCHEMA : GameData::MapMetadata::SCHEMA
        # Go through schema hash of compilable data and compile this section
        for key in schema.keys
          FileLineData.setSection(map_id, key, contents[key])   # For error reporting
          # Skip empty properties, or raise an error if a required property is
          # empty
          if contents[key].nil?
            if map_id == 0 && ["Home", "PlayerA"].include?(key)
              raise _INTL("The entry {1} is required in {2} section 0.", key, path)
            end
            next
          end
          # Compile value for key
          value = pbGetCsvRecord(contents[key], key, schema[key])
          value = nil if value.is_a?(Array) && value.length == 0
          contents[key] = value
        end
        if map_id == 0   # Global metadata
          # Construct metadata hash
          metadata_hash = {
            :id                 => map_id,
            :home               => contents["Home"],
            :wild_battle_BGM    => contents["WildBattleBGM"],
            :trainer_battle_BGM => contents["TrainerBattleBGM"],
            :wild_victory_ME    => contents["WildVictoryME"],
            :trainer_victory_ME => contents["TrainerVictoryME"],
            :wild_capture_ME    => contents["WildCaptureME"],
            :surf_BGM           => contents["SurfBGM"],
            :bicycle_BGM        => contents["BicycleBGM"],
            :player_A           => contents["PlayerA"],
            :player_B           => contents["PlayerB"],
            :player_C           => contents["PlayerC"],
            :player_D           => contents["PlayerD"],
            :player_E           => contents["PlayerE"],
            :player_F           => contents["PlayerF"],
            :player_G           => contents["PlayerG"],
            :player_H           => contents["PlayerH"],

						#-----------------------------------------------------------------------------------
						# Add new
						#-----------------------------------------------------------------------------------
						:player_K           => contents["PlayerK"],
						:player_L           => contents["PlayerL"],
						:player_M           => contents["PlayerM"],
						:player_N           => contents["PlayerN"]

          }
          # Add metadata's data to records
          GameData::Metadata.register(metadata_hash)
        else   # Map metadata
          # Construct metadata hash
          metadata_hash = {
            :id                   => map_id,
            :outdoor_map          => contents["Outdoor"],
            :announce_location    => contents["ShowArea"],
            :can_bicycle          => contents["Bicycle"],
            :always_bicycle       => contents["BicycleAlways"],
            :teleport_destination => contents["HealingSpot"],
            :weather              => contents["Weather"],
            :town_map_position    => contents["MapPosition"],
            :dive_map_id          => contents["DiveMap"],
            :dark_map             => contents["DarkMap"],
            :safari_map           => contents["SafariMap"],
            :snap_edges           => contents["SnapEdges"],
            :random_dungeon       => contents["Dungeon"],
            :battle_background    => contents["BattleBack"],
            :wild_battle_BGM      => contents["WildBattleBGM"],
            :trainer_battle_BGM   => contents["TrainerBattleBGM"],
            :wild_victory_ME      => contents["WildVictoryME"],
            :trainer_victory_ME   => contents["TrainerVictoryME"],
            :wild_capture_ME      => contents["WildCaptureME"],
            :town_map_size        => contents["MapSize"],
            :battle_environment   => contents["Environment"]
          }
          # Add metadata's data to records
          GameData::MapMetadata.register(metadata_hash)
        end
      }
    }
    # Save all data
    GameData::Metadata.save
    GameData::MapMetadata.save
    Graphics.update
  end
end

# Add above this line
