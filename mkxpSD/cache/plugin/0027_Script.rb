#=================================================================================
# PE Terrain Step Sounds v19.1
# Version 1.2
# by Enurta and Ikaro
#---------------------------------------------------------------------------------
# Create nice aesthetics with terrain noise. As you walk across the ground, this
# will play a step sound to add a little bit of unique sparkle to your game.
# 
#
# Features:
# Specific Sound for Each Terrain and Tileset
# Specify Volume and Pitch
# Round Robin code breaks repetition (no more 'machine gun' steps!)
#
# Instructions:
# Setup the config below as you desire, it's fairly self explanatory, more 
# instructions with each config section.
#=================================================================================   
module PETS
Tag = []
Tileset  = []
StepEnabled = ["Trainer", "NPC", "step"] # Events MUST contain one of these strings in order to produce step sounds.
#=================================================================================
# Enter in sounds for each terrain tag
# Goes from 0-15 for base Pokémon Essentials. Terrain Tag 1 won't be used as it is not used for normal walking space.
# Each terrain type is in the array below.
#
# You can specify the sound file, the volume, and pitch of the file.
# Tag[2] = ["Filename",volume,pitch]
# Filename - Replace with the name of the file that you want to use
# Volume - 0-100; higher is louder
# Pitch - 50-150; lower is deeper
# You can repeat the above three as many times as desired to make use of the Round Robin code.
# If volume and pitch are not specified they will default to 100 for both.
# OGG files seem to throw an error. Working on figuring out why.
#=================================================================================
Tag[0] = [] # Nothing
Tag[2] = [] # Grass
Tag[3] = ["se_step_run_dirt",100,100,"se_step_run_dirt",100,120,"se_step_dirt",100,80] # Sand
Tag[4] = [] # Rock
Tag[5] = [] # Deep Water
Tag[6] = [] # Still Water
Tag[7] = [] # Water
Tag[8] = [] # Waterfall
Tag[9] = [] # Waterrfal Crest
Tag[10] = [] # Tall Grass
Tag[11] = [] # Underwater Grass
Tag[12] = [] # Ice
Tag[13] = [] # Neutral
Tag[14] = [] # Sooty Grass
Tag[15] = [] # Bridge
Tag[16] = ["se_step_puddle",100,100,"se_step_puddle",100,120,"se_step_puddle",100,80] # Puddle
Tag[17] = ["se_step_deepsand",80,100,"se_step_deepsand",80,90,"se_step_deepsand",80,80] # DeepSand
Tag[18] = [] # StairLeft
Tag[19] = [] # StairRight
Tag[20] = ["se_step_snow",80,100,"se_step_snow",80,120,"se_snow",80,80] # Snow
Tag[21] = ["se_step_mud",100,100,"se_step_mud",100,120,"se_step_mud",100,80] # Marsh
Tag[25] = ["se_step_puddle",100,100,"se_step_puddle",100,120,"se_step_puddle",100,80] # Beach
# With tilesets, you can set specific sounds for each tileset so you don't
# have the same sounds everywhere. Add a new line and put
# Tileset[tileset id] = []
# Then for each terrain tag add
# Tileset[tileset id][terrain id] = "sound file"
# If a sound doesn't exist for a tileset, it will play the default sound,
# and if a default doesn't exist, no sound is played at all.
end
#=================================================================================
# Game Map
#=================================================================================
class Game_Map
  attr_accessor :map
end
#=================================================================================
# Round Robin (Random step sound)
#=================================================================================
def rr # laziest def on the planet
	@rr = 0
end
#=================================================================================
# Event that triggers the sound
#=================================================================================
Events.onStepTakenFieldMovement += proc { |_sender,e|
	event = e[0] # Get the event affected by field movement
	if $scene.is_a?(Scene_Map) && event==$game_player && !$PokemonGlobal.bicycle
		step_sound = PETS::Tag[$game_map.terrain_tag(event.x,event.y).id_number] #.id_number gets the terrain tag's number instead of a string.
		if PETS::Tileset[$game_map.map.tileset_id] != nil # Prevents crashing
		unless PETS::Tileset[$game_map.map.tileset_id][$game_map.terrain_tag(event.x,event.y).id_number] == nil
			step_sound = PETS::Tileset[$game_map.map.tileset_id][$game_map.terrain_tag(event.x,event.y).id_number]
			end
		end
	if @rr==0 # Do random step sounds
		sound = step_sound[0]
		volume = step_sound[1]
		pitch = step_sound[2] # These numbers refer to the Tags above. Like so: Tag[0] = [0,1,2]
		pbSEPlay(sound,volume,pitch)
		@rr = 1
	else # if rr==1 # Increase Round Robin repetitions
		sound = step_sound[3]
		volume = step_sound[4]
		pitch = step_sound[5] # Following with the above, you can tack on as many sounds as you like: Tag[0] = [0,1,2,3,4,5]
		pbSEPlay(sound,volume,pitch)
		@rr = 0 # Set to 2 if you add more RR repetitions
	end
  end
}
#=================================================================================
# For non-player events, trigger the sound as well
#=================================================================================
def name # Code won't work without this
	name = ""
end

Events.onStepTakenFieldMovement += proc { |_sender, e|
	event = e[0]
	@event = e[0] # Get the non-player event.
	if $scene.is_a?(Scene_Map) && PETS::StepEnabled.any?{|e| @event.name[/#{e}/i]} # Checks the event name for requisite strings. If strings are not present, do nothing
		step_sound = PETS::Tag[$game_map.terrain_tag(event.x,event.y).id_number]
		if PETS::Tileset[$game_map.map.tileset_id] != nil
		unless PETS::Tileset[$game_map.map.tileset_id][$game_map.terrain_tag(event.x,event.y).id_number] == nil
			step_sound = PETS::Tileset[$game_map.map.tileset_id][$game_map.terrain_tag(event.x,event.y).id_number]
			end
	end
	if @rr==0 # Remember that any changes you make here should be duped onto the player's step code too. HOWEVER, you can use this seperated code to give NPCs quieter footsteps.
		sound = step_sound[0]
		volume = step_sound[1]
		pitch = step_sound[2]
		pbSEPlay(sound,volume,pitch)
		@rr = 1
	else
		sound = step_sound[3]
		volume = step_sound[4]
		pitch = step_sound[5]
		pbSEPlay(sound,volume,pitch)
		@rr = 0
	end
  end
}
