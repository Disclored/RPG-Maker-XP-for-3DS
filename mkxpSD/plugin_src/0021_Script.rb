#===============================================================================
# * Single Screen Day-Care Checker Item - by FL (Credits will be apreciated)
#===============================================================================
#
# This script is for Pokémon Essentials. It makes a single screen Day-Care 
# Checker (like in DPP Pokétch) activated by item. This displays the pokémon
# sprites, names, levels, genders and if they generated an egg.
#
#== INSTALLATION ===============================================================
#
# To this script works, put it above main OR convert into a plugin. Put a 
# 512x384 background in "Graphics/Pictures/day_care_checker_bg" and add into
# PBS\items.txt:
#
# In Essentials version 20 or above:
#
#  [DAYCARESIGHT]
#  Name = Day-Care Sight
#  NamePlural = Day-Care Sights
#  Pocket = 8
#  Price = 0
#  FieldUse = Direct
#  Flags = KeyItem
#  Description = A visor that can be use to see Pokémon in Day-Care to monitor their growth.
#
# In v16.2-v19.1:
#
#  631,DAYCARESIGHT,Day-Care Sight,Day-Care Sights,8,0,"A visor that can be use to see Pokémon in Day-Care to monitor their growth.",2,0,6
#
# In v15 or below:
#
#  631,DAYCARESIGHT,DayCare Sight,8,0,"A visor that can be use to see Pokémon in Day-Care to monitor their growth.",2,0,6
#
#===============================================================================

module DayCareChecker
  # Item ID
  ITEM = :DAYCARESIGHT

  # When true and there are only one pokémon, it is always on the left.
  FORCE_LEFT = false

  # When true, only an egg shadow will be show.
  EGG_SHADOW = false

  def self.display
    pbFadeOutIn(99999){
      scene=Scene.new
      screen=Screen.new(scene)
      screen.start_screen
    }
  end

  class Scene
    def start_scene
      @sprites={}
      @viewport=Viewport.new(0,0,Graphics.width,Graphics.height)
      @viewport.z=99999
      @pokemon_array = [Bridge.day_care_pokemon(0),Bridge.day_care_pokemon(1)]
      sprite_y = Graphics.height/2+16
      if FORCE_LEFT && !@pokemon_array[0] && @pokemon_array[1]
        @pokemon_array.delete_at(0)
      end
      text_positions=[]
      @sprites["overlay"]=Sprite.new(@viewport)
      @sprites["overlay"].bitmap=BitmapWrapper.new(Graphics.width,Graphics.height)
      @sprites["overlay"].z = 1
      pbSetSystemFont(@sprites["overlay"].bitmap)
      addBackgroundPlane(@sprites,"bg","DayCareChecker/bg",@viewport)
      @sprites["elements"]=IconSprite.new(0,0,@viewport)
      @sprites["elements"].setBitmap("Graphics/Pictures/DayCareChecker/overlay")
      @sprites["elements"].x = (
        Graphics.width-@sprites["elements"].bitmap.width
      )/2
      @sprites["elements"].y = (
        Graphics.height-@sprites["elements"].bitmap.height
      )/2
      for i in 0...2
        draw_pokemon(i, sprite_y, text_positions) if @pokemon_array[i]
      end
      draw_egg(sprite_y) if Bridge.day_care_has_egg?
      if !text_positions.empty?
        Bridge.draw_text_positions(@sprites["overlay"].bitmap,text_positions)
      end
      if @pokemon_array[0] && @pokemon_array[1]
        @sprites["compa_text"] = BitmapSprite.new(Graphics.width,Graphics.height,@viewport)
        pbSetSystemFont(@sprites["compa_text"].bitmap)
        i = pbDayCareGetCompat
        comp_rating = [0,20,50,70][i]
        comp_rating = [0,40,80,88][i] if GameData::Item.exists?(:OVALCHARM) && $PokemonBag.pbHasItem?(:OVALCHARM)
        compatability = "Compatability: #{comp_rating}%"
        drawFormattedTextEx(@sprites["compa_text"].bitmap, 24, 286, width=200, 
        compatability, Color.new(224,232,232), Color.new(64,64,64), 32)
      end
      pbFadeInAndShow(@sprites) { update }
    end

    def draw_pokemon(i,y,text_positions)
      @sprites["pokemon#{i}"]=PokemonSprite.new(@viewport)
      @sprites["pokemon#{i}"].setPokemonBitmap(@pokemon_array[i])
      @sprites["pokemon#{i}"].mirror=true if i==0
      set_center_offset(@sprites["pokemon#{i}"])
      @sprites["pokemon#{i}"].x = i==0 ? 104 : Graphics.width-104
      @sprites["pokemon#{i}"].y = y
      assing_pokemon_text(
        i, _INTL("{1}\nLv{2}",@pokemon_array[i].name,@pokemon_array[i].level), 
        @pokemon_array[i].gender, 20, 64, text_positions
      )
    end

    def assing_pokemon_text(i, text, gender, x_padding, y, text_positions)
      text_positions.push([
        text, text_x(i, gender, x_padding),
        y, i==1, Color.new(248,248,248),Color.new(72, 80, 88)
      ])
      if @pokemon_array[i].gender != 2
        gender_text_size = @sprites["overlay"].bitmap.text_size(text).width
        text_positions.push([
          gender==0 ? "♂" : "♀", 
          i==0 ? (x_padding+gender_text_size+8) : Graphics.width - x_padding, 
          y, i==1,
          gender==0 ? Color.new(0,128,248) : Color.new(248,24,24),
          Color.new(72, 80, 88)
        ])
      end
      eggGroup = @pokemon_array[i].species_data.egg_groups
      eggText = eggGroup[0].to_s
      eggText += ", " + eggGroup[1].to_s if eggGroup.length > 1
      text_positions.push([
        eggText, i==0 ? text_x(i, gender, x_padding) : Graphics.width - x_padding,
        y+28, i==1, Color.new(248,248,248),Color.new(72, 80, 88)
      ])
    end

    def text_x(index, gender, padding)
      ret = 0
      if index==0
        ret = padding
      else
        ret = Graphics.width - padding
        ret-=20 if gender != 2
      end
      return ret
    end

    def draw_egg(y)
      @sprites["egg"]=IconSprite.new(Graphics.width/2,y+48,@viewport)
      @sprites["egg"].setBitmap(Bridge.egg_path)
      set_center_offset(@sprites["egg"])
      @sprites["egg"].zoom_x = 2
      @sprites["egg"].zoom_y = 2
      @sprites["egg"].color=Color.new(0,0,0,255) if EGG_SHADOW
    end

    def set_center_offset(sprite)
      sprite.ox = sprite.bitmap.width/2 if sprite.bitmap
      sprite.oy = sprite.bitmap.height/2 if sprite.bitmap
    end

    def main
      loop do
        Graphics.update
        Input.update
        self.update
        if Input.trigger?(Input::B) || Input.trigger?(Input::C)
          pbPlayCancelSE
          break
        end
      end
    end

    def update
      pbUpdateSpriteHash(@sprites)
      if @sprites["bg"]
        @sprites["bg"].ox-=0
        @sprites["bg"].oy+=1
      end
    end

    def end_scene
      pbFadeOutAndHide(@sprites) { update }
      pbDisposeSpriteHash(@sprites)
      @viewport.dispose
    end
  end

  class Screen
    def initialize(scene)
      @scene=scene
    end

    def start_screen
      @scene.start_scene
      @scene.main
      @scene.end_scene
    end
  end

  # Essentials multiversion layer
  module Bridge
    if defined?(Essentials)
      MAJOR_VERSION = Essentials::VERSION.split(".")[0].to_i
    else
      MAJOR_VERSION = 0
    end

    module_function
    
    def draw_text_positions(bitmap,textpos)
      if MAJOR_VERSION < 20
        for single_text_pos in textpos
          single_text_pos[2]-= MAJOR_VERSION==19 ? 12 : 6
        end
      end
      return pbDrawTextPositions(bitmap,textpos)
    end
    
    def egg_path
       return "Graphics/Battlers/egg" if MAJOR_VERSION < 19
       return "Graphics/Pokemon/Eggs/000"
    end

    def day_care_pokemon(index)
      return $PokemonGlobal.daycare[index][0] if MAJOR_VERSION < 20
      return nil if !$PokemonGlobal.day_care.slots[index]
      return $PokemonGlobal.day_care.slots[index].pokemon
    end

    def day_care_has_egg?
      return Kernel.pbEggGenerated? if MAJOR_VERSION < 20
      return DayCare.egg_generated?
    end
  end
end


ItemHandlers::UseInField.add(DayCareChecker::ITEM,proc { |item|
  DayCareChecker.display
  next 1 # Continue
})

ItemHandlers::UseFromBag.add(DayCareChecker::ITEM,proc { |item|
  DayCareChecker.display
  next 1 # Continue
})