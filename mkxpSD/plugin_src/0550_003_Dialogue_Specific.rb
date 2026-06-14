#-------------------------------------------------------------------------------
# These are used to define what the Follower will say when spoken to under
# specific conditions like Status or Weather or Map names
#-------------------------------------------------------------------------------

#-------------------------------------------------------------------------------
# Amie Compatibility
#-------------------------------------------------------------------------------
if defined?(PkmnAR)
  Events.OnTalkToFollower += proc { |_pkmn, _random_val|
    cmd = pbMessage(_INTL("What would you like to do?"), [
      _INTL("Play"),
      _INTL("Talk"),
      _INTL("Cancel")
    ])
    PkmnAR.show if cmd == 0
    next true if [0, 2].include?(cmd)
  }
end
#-------------------------------------------------------------------------------
# Special Dialogue when statused
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  case pkmn.status
  when :POISON
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_POISON)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    pbMessage(_INTL("{1} is shivering with the effects of being poisoned.", pkmn.name))
  when :BURN
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ANGRY)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    pbMessage(_INTL("{1}'s burn looks painful.", pkmn.name))
  when :FROZEN
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ELIPSES)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    pbMessage(_INTL("{1} seems very cold. It's frozen solid!", pkmn.name))
  when :SLEEP
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ELIPSES)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    pbMessage(_INTL("{1} seems really tired.", pkmn.name))
  when :PARALYSIS
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ELIPSES)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    pbMessage(_INTL("{1} is standing still and twitching.", pkmn.name))
  end
  next true if pkmn.status != :NONE
}
#-------------------------------------------------------------------------------
# Special hold item on a map which includes battle in the name
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |_pkmn, _random_val|
  if $game_map.name.include?("Battle")
    # This array can be edited and extended to your hearts content.
    items = [:POKEBALL, :POKEBALL, :POKEBALL, :GREATBALL, :GREATBALL, :ULTRABALL]
    # Choose a random item from the items array, give the player 2 of the item
    # with the message "{1} is holding a round object..."
    next true if FollowingPkmn.item(items.sample, 2, _INTL("{1} is holding a round object..."))
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map ID is Team Sol HQ (Electric Room)
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if pkmn.hasType?(:ELECTRIC) && ($game_map.map_id == 284 || 
($game_map.map_id == 304 && !$game_switches[113])) 
    FollowingPkmn.animation(Settings::EXCLAMATION_ANIMATION_ID)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} unwittingly released a bit of electricity!"),
      _INTL("{1} seems bothered by something..."),
      _INTL("{1} is looking around frantically.")
    ]
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name is Sol Research Lab
# (Not Sol Island, the outside)
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Sol Research Lab") ||
$game_map.name.include?("Team Sol HQ")
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ELIPSES)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} is staring at some sort of machine..."),
      _INTL("{1} seems interested in pressing the buttons."),
      _INTL("{1} seems to want to touch the machinery."),
      _INTL("{1} is touching some kind of switch."),
      _INTL("{1} has a cord in its mouth!"),
      _INTL("{1} seems to want to touch the machinery.")
    ]
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name is Pokemon Lab
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Lab")
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ELIPSES)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} is touching some kind of switch."),
      _INTL("{1} has a cord in its mouth!"),
      _INTL("{1} seems to want to touch the machinery.")
    ]
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name has the players name in it like the
# Player's House
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?($Trainer.name)
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} is sniffing around the room."),
      _INTL("{1} noticed {2}'s aunt is nearby."),
      _INTL("{1} seems to want to settle down at home."),
      _INTL("{1} noticed {2}'s uncle is nearby."),
      _INTL("{1} seems interested in {2}'s uncle."),
      _INTL("{1} seems interested in {2}'s aunt."),
      _INTL("{1} seems excited to be home."),
      _INTL("{1} is touching everything in the room."),
      _INTL("{1} seems interested in Upah.")
    ]
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name has Gate
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Gate")
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} looks a lot calmer."),
      _INTL("{1} stretched their body."),
      _INTL("{1} seems to want to settle down."),
      _INTL("{1} seems relaxed."),
      _INTL("{1} looks a little better just being able relax."),
      _INTL("{1} seems to be completely at ease."),
      _INTL("There's a content expression on {1}'s face."),
      _INTL("{1} is making itself comfortable."),
      _INTL("{1} looks like it wants to take a nap.")
    ]
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name has Pokecenter or Pokemon Center
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Poké Center") ||
     $game_map.name.include?("Pokémon Center")
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} looks happy to see the nurse."),
      _INTL("{1} looks a little better just being in the Pokémon Center."),
      _INTL("{1} seems fascinated by the healing machinery."),
      _INTL("{1} looks like it wants to take a nap."),
      _INTL("{1} chirped a greeting at the nurse."),
      _INTL("{1} is watching {2} with a playful gaze."),
      _INTL("{1} seems to be completely at ease."),
      _INTL("{1} is making itself comfortable."),
      _INTL("There's a content expression on {1}'s face."),
      _INTL("{1} seems interested in the Mystery Man.")
    ]
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name has Yoko Forest
# Specific Pokemon types have different reactions
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Yoko Forest")
    if pkmn.hasType?(:FAIRY)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ELIPSES)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems a bit melancholic."),
        _INTL("{1} closed their eyes.")
      ]
    else
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_MUSIC)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems highly interested in the trees."),
        _INTL("{1} seems to enjoy the buzzing of the bug Pokémon."),
        _INTL("{1} is jumping around restlessly in the forest."),
        _INTL("{1} is wandering around and listening to the different sounds."),
        _INTL("{1} is munching at the grass."),
        _INTL("{1} is wandering around and enjoying the forest scenery."),
        _INTL("{1} is playing around, plucking bits of grass."),
        _INTL("{1} is staring at the light coming through the trees."),
        _INTL("{1} is playing around with a leaf!"),
        _INTL("{1} seems to be listening to the sound of rustling leaves."),
        _INTL("{1} is standing perfectly still and might be imitating a tree..."),
        _INTL("{1} got tangled in the branches and almost fell down!"),
        _INTL("{1} was surprised when it got hit by a branch!")
      ]
    end
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name has Dimensional Forest
# Specific Pokemon have different reactions
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Dimensional Forest")
    if pkmn.isSpecies?(:LEAVANNY) || pkmn.isSpecies?(:GALLADE) || pkmn.isSpecies?(:HATTERENE) || pkmn.isSpecies?(:GARDEVOIR)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ELIPSES)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems a bit melancholic."),
        _INTL("{1} closed their eyes.")
      ]
    else
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_MUSIC)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems highly interested in the trees."),
        _INTL("{1} is jumping around restlessly in the forest."),
        _INTL("{1} is wandering around and listening to the different sounds."),
        _INTL("{1} is munching at the grass."),
        _INTL("{1} is wandering around and enjoying the forest scenery."),
        _INTL("{1} is playing around, plucking bits of grass."),
        _INTL("{1} is staring at the light coming through the trees."),
        _INTL("{1} is playing around with a leaf!"),
        _INTL("{1} seems to be listening to the sound of rustling leaves."),
        _INTL("{1} is standing perfectly still and might be imitating a tree..."),
        _INTL("{1} got tangled in the branches and almost fell down!"),
        _INTL("{1} was surprised when it got hit by a branch!"),
        _INTL("{1} is playing with a light ball.")
      ]
    end
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name is Route 3
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Route 3") && (pkmn.isSpecies?(:MAREEP) || pkmn.isSpecies?(:FLAAFFY) ||
pkmn.isSpecies?(:AMPHAROS))
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_MUSIC)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} seems highly interested in playing in the fields."),
      _INTL("{1} suddenly started rolling in the grass!"),
      _INTL("{1} looks awfully playful."),
    ]
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name is Route 5
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Route 5")
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_MUSIC)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} seems highly interested in the trees."),
      _INTL("{1} seems to enjoy the buzzing of the bug Pokémon."),
      _INTL("{1} is jumping around restlessly in the forest."),
      _INTL("{1} is wandering around and listening to the different sounds."),
      _INTL("{1} is munching at the grass."),
      _INTL("{1} is wandering around and enjoying the forest scenery."),
      _INTL("{1} is playing around, plucking bits of grass."),
      _INTL("{1} is staring at the light coming through the trees."),
      _INTL("{1} is playing around with a leaf!"),
      _INTL("{1} seems to be listening to the sound of rustling leaves."),
      _INTL("{1} is standing perfectly still and might be imitating a tree..."),
      _INTL("{1} got tangled in the branches and almost fell down!"),
      _INTL("{1} was surprised when it got hit by a branch!")
    ]
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name has Gym in it
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Gym") || $game_map.name.include?("SCT") || 
  $game_map.name.include?("Dojo") || $game_map.name.include?("Victory Road") ||
  $game_map.name.include?("Battle")
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ANGRY)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} looks eager to battle!"),
      _INTL("{1} is looking at {2} with a determined gleam in its' eye."),
      _INTL("{1} is trying to intimidate the other trainers."),
      _INTL("{1} trusts {2} to come up with a winning strategy."),
      _INTL("{1} is keeping an eye on the gym leader."),
      _INTL("{1} is ready to pick a fight with someone."),
      _INTL("{1} looks like it might be preparing for a big showdown!"),
      _INTL("{1} wants to show off how strong it is!"),
      _INTL("{1} is...doing warm-up exercises?"),
      _INTL("{1} is growling quietly in contemplation...")
    ]
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name has Beach in it
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Beach")
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} seems to be enjoying the scenery."),
      _INTL("{1} seems to enjoy the sound of the waves moving the sand."),
      _INTL("{1} looks like it wants to swim!"),
      _INTL("{1} can barely look away from the ocean."),
      _INTL("{1} is staring longingly at the water."),
      _INTL("{1} keeps trying to shove {2} towards the water."),
      _INTL("{1} is excited to be looking at the sea!"),
      _INTL("{1} is happily watching the waves!"),
      _INTL("{1} is playing on the sand!"),
      _INTL("{1} is staring at {2}'s footprints in the sand."),
      _INTL("{1} is rolling around in the sand.")
    ]
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name is Route 13 or Route 14
# During night.
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if ($game_map.name.include?("Route 13") || $game_map.name.include?("Route 14")) && PBDayNight.isNight?
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      FollowingPkmn.move_route([
        PBMoveRoute::TurnRight,
        PBMoveRoute::Wait, 4,
        PBMoveRoute::TurnUp,
        PBMoveRoute::Wait, 4,
        PBMoveRoute::TurnLeft,
        PBMoveRoute::Wait, 4,
        PBMoveRoute::TurnDown,
      ])
    messages = [
      _INTL("{1} looks on edge..."),
      _INTL("{1} is looking around frantically..."),
      _INTL("{1} looks ready for something..."),
      _INTL("{1} is sticking close to you..."),
      _INTL("{1} is seemingly expecting something...")
    ]
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message when the weather is Rainy. Pokemon of different types
# have different reactions to the weather.
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if [:Rain, :HeavyRain].include?($game_screen.weather_type)
    if pkmn.hasType?(:FIRE) || pkmn.hasType?(:GROUND) || pkmn.hasType?(:ROCK)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ANGRY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems very upset the weather."),
        _INTL("{1} is shivering..."),
        _INTL("{1} doesn't seem to like being all wet..."),
        _INTL("{1} keeps trying to shake itself dry..."),
        _INTL("{1} moved closer to {2} for comfort."),
        _INTL("{1} is looking up at the sky and scowling."),
        _INTL("{1} seems to be having difficulty moving its body.")
      ]
    elsif pkmn.hasType?(:WATER) || pkmn.hasType?(:GRASS)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems to be enjoying the weather."),
        _INTL("{1} seems to be happy about the rain!"),
        _INTL("{1} seems to be very surprised that it's raining!"),
        _INTL("{1} beamed happily at {2}!"),
        _INTL("{1} is gazing up at the rainclouds."),
        _INTL("Raindrops keep falling on {1}."),
        _INTL("{1} is looking up with its mouth gaping open.")
      ]
    else
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ELIPSES)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} is staring up at the sky."),
        _INTL("{1} looks a bit surprised to see rain."),
        _INTL("{1} keeps trying to shake itself dry."),
        _INTL("The rain doesn't seem to bother {1} much."),
        _INTL("{1} is playing in a puddle!"),
        _INTL("{1} is slipping in the water and almost fell over!")
      ]
    end
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message when the weather is Storm. Pokemon of different types
# have different reactions to the weather.
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if :Storm == $game_screen.weather_type
    if pkmn.hasType?(:ELECTRIC)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} is staring up at the sky."),
        _INTL("The storm seems to be making {1} excited."),
        _INTL("{1} looked up at the sky and shouted loudly!"),
        _INTL("The storm only seems to be energizing {1}!"),
        _INTL("{1} is happily zapping and jumping in circles!"),
        _INTL("The lightning doesn't bother {1} at all.")
      ]
    else
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ELIPSES)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} is staring up at the sky."),
        _INTL("The storm seems to be making {1} a bit nervous."),
        _INTL("The lightning startled {1}!"),
        _INTL("The rain doesn't seem to bother {1} much."),
        _INTL("The weather seems to be putting {1} on edge."),
        _INTL("{1} was startled by the lightning and snuggled up to {2}!")
      ]
    end
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message when the weather is Snowy. Pokemon of different types
# have different reactions to the weather.
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if :Snow == $game_screen.weather_type
    if pkmn.hasType?(:ICE)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} is watching the snow fall."),
        _INTL("{1} is thrilled by the snow!"),
        _INTL("{1} is staring up at the sky with a smile."),
        _INTL("The snow seems to have put {1} in a good mood."),
        _INTL("{1} is cheerful because of the cold!")
      ]
    else
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ELIPSES)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} is watching the snow fall."),
        _INTL("{1} is nipping at the falling snowflakes."),
        _INTL("{1} wants to catch a snowflake in its' mouth."),
        _INTL("{1} is fascinated by the snow."),
        _INTL("{1}'s teeth are chattering!"),
        _INTL("{1} made its body slightly smaller because of the cold...")
      ]
    end
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message when the weather is Blizzard. Pokemon of different types
# have different reactions to the weather.
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if :Blizzard == $game_screen.weather_type
    if pkmn.hasType?(:ICE)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} is watching the hail fall."),
        _INTL("{1} isn't bothered at all by the hail."),
        _INTL("{1} is staring up at the sky with a smile."),
        _INTL("The hail seems to have put {1} in a good mood."),
        _INTL("{1} is gnawing on a piece of hailstone.")
      ]
    else
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ANGRY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} is getting pelted by hail!"),
        _INTL("{1} wants to avoid the hail."),
        _INTL("The hail is hitting {1} painfully."),
        _INTL("{1} looks unhappy."),
        _INTL("{1} is shaking like a leaf!")
      ]
    end
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message when the weather is Sandstorm. Pokemon of different types
# have different reactions to the weather.
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if :Sandstorm == $game_screen.weather_type
    if pkmn.hasType?(:ROCK) || pkmn.hasType?(:GROUND)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} is coated in sand."),
        _INTL("The weather doesn't seem to bother {1} at all!"),
        _INTL("The sand can't slow {1} down!"),
        _INTL("{1} is enjoying the weather.")
      ]
    elsif pkmn.hasType?(:STEEL)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ELIPSES)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} is coated in sand, but doesn't seem to mind."),
        _INTL("{1} seems unbothered by the sandstorm."),
        _INTL("The sand doesn't slow {1} down."),
        _INTL("{1} doesn't seem to mind the weather.")
      ]
    else
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ANGRY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} is covered in sand..."),
        _INTL("{1} spat out a mouthful of sand!"),
        _INTL("{1} is squinting through the sandstorm."),
        _INTL("The sand seems to be bothering {1}.")
      ]
    end
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message when the weather is Sunny. Pokemon of different types
# have different reactions to the weather.
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if :Sun == $game_screen.weather_type
    if pkmn.hasType?(:GRASS)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems pleased to be out in the sunshine."),
        _INTL("{1} is soaking up the sunshine."),
        _INTL("The bright sunlight doesn't seem to bother {1} at all."),
        _INTL("{1} sent a ring-shaped cloud of spores into the air!"),
        _INTL("{1} is stretched out its body and is relaxing in the sunshine."),
        _INTL("{1} is giving off a floral scent.")
      ]
    elsif pkmn.hasType?(:FIRE)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems to be happy about the great weather!"),
        _INTL("The bright sunlight doesn't seem to bother {1} at all."),
        _INTL("{1} looks thrilled by the sunshine!"),
        _INTL("{1} blew out a fireball."),
        _INTL("{1} is breathing out fire!"),
        _INTL("{1} is hot and cheerful!")
      ]
    elsif pkmn.hasType?(:DARK)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ANGRY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} is glaring up at the sky."),
        _INTL("{1} seems personally offended by the sunshine."),
        _INTL("The bright sunshine seems to bothering {1}."),
        _INTL("{1} looks upset for some reason."),
        _INTL("{1} is trying to stay in {2}'s shadow."),
        _INTL("{1} keeps looking for shelter from the sunlight.")
      ]
    else
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ELIPSES)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} is squinting in the bright sunshine."),
        _INTL("{1} is starting to sweat."),
        _INTL("{1} seems a little uncomfortable in this weather."),
        _INTL("{1} looks a little overheated."),
        _INTL("{1} seems very hot..."),
        _INTL("{1} shielded its vision against the sparkling light!")
      ]
    end
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name has Gardenia Academy in it
# Specific Pokemon have different reactions
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Gardenia Academy")
    if pkmn.isSpecies?(:TOGEPI) || pkmn.isSpecies?(:TOGETIC) || pkmn.isSpecies?(:TOGEKISS)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems to want to play outside."),
        _INTL("{1} is waggling about."),
        _INTL("{1} looks at peace."),
        _INTL("{1} seems interested in Professor Xenia.")
      ]
    elsif pkmn.isSpecies?(:MAREEP) || pkmn.isSpecies?(:FLAAFFY) || pkmn.isSpecies?(:AMPHAROS)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems to want to play outside."),
        _INTL("{1} got a bit closer to {2}!"),
        _INTL("{1} is frolicking about."),
        _INTL("{1} seems interested in Professor Xenia.")
      ]
    elsif pkmn.isSpecies?(:TRAPINCH) || pkmn.isSpecies?(:VIBRAVA) || pkmn.isSpecies?(:FLYGON)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems to want to play outside."),
        _INTL("{1} is eager to show its might."),
        _INTL("{1} is stomping about."),
        _INTL("{1} seems interested in Professor Xenia.")
      ]
    elsif pkmn.isSpecies?(:SPHEAL) || pkmn.isSpecies?(:SEALEO) || pkmn.isSpecies?(:WALREIN)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems to want to play outside."),
        _INTL("{1} started clapping."),
        _INTL("{1} is about to roll away!"),
        _INTL("{1} seems interested in Professor Xenia.")
      ]
    elsif pkmn.isSpecies?(:SEWADDLE) || pkmn.isSpecies?(:SWADLOON) || pkmn.isSpecies?(:LEAVANNY)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems to want to play outside."),
        _INTL("{1} suddenly seems a bit shy."),
        _INTL("{1} got a bit closer to {2}!"),
        _INTL("{1} seems interested in Professor Xenia.")
      ]
    else
      FollowingPkmn.animation(Settings::EXCLAMATION_ANIMATION_ID)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems amazed at the scenery!"),
        _INTL("{1} is curious about the other students."),
        _INTL("{1} looks eager to learn.")
      ]
    end
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name has Sol Island in it
# Specific Pokemon have different reactions
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Sol Island")
    if pkmn.isSpecies?(:COBALION) || pkmn.isSpecies?(:TERRAKION) || pkmn.isSpecies?(:VIRIZION) || pkmn.isSpecies?(:KELDEON)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ANGRY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} looks quite angry!"),
        _INTL("{1} is looking around restlessly.")
      ]
    elsif pkmn.isSpecies?(:COSMOG) || pkmn.isSpecies?(:COSMOEM) || pkmn.isSpecies?(:SOLGALEO) || pkmn.isSpecies?(:LUNALA)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ANGRY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} looks appalled."),
        _INTL("{1} is looking around restlessly.")
      ]
    else
      FollowingPkmn.animation(Settings::EXCLAMATION_ANIMATION_ID)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems impressed at the scenery."),
        _INTL("{1} is staring at the Team Sol iconography.")
      ]
    end
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name has Mine or Cave in it
# Specific Pokemon types have different reactions
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if ($game_map.name.include?("Mine") || $game_map.name.include?("Cave")) && pkmn.hasType?(:ROCK)
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} is having fun rolling around!"),
      _INTL("{1} started grinding at the rocks!"),
      _INTL("{1} is having fun hitting rocks together."),
      _INTL("{1} started pounding the ground flat!")
    ]
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name has Volcano in it
# Specific Pokemon types have different reactions
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Volcano")
    if pkmn.hasType?(:FIRE)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} emitted fire and shouted!"),
        _INTL("{1} blew out a couple of fireballs."),
        _INTL("{1} blew out a fireball."),
        _INTL("{1} looks cosy!"),
        _INTL("{1} just looks so very happy!"),
        _INTL("{1} is cheering happily because of the heat!")
      ]
    elsif pkmn.hasType?(:GRASS) || pkmn.hasType?(:ICE) || pkmn.hasType?(:BUG)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_SAD)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems on edge."),
        _INTL("{1} is sticking closer to {2}."),
        _INTL("{1} seems unhappy about the heat...")
      ]
    else
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems to be taking the heat well."),
        _INTL("{1} looked to {2}, as if trying to say it's fine."),
        _INTL("{1} looks a bit parched..."),
        _INTL("{1} is keeping their guard up."),
        _INTL("{1} looks a little overheated."),
        _INTL("{1} seems very hot...")
      ]
    end
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name has Tomb in it
# Specific Pokemon types have different reactions
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Tomb")
    if pkmn.hasType?(:DRAGON)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ANGRY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} suddenly roared!"),
        _INTL("{1} is staring at the walls."),
        _INTL("{1} suddenly barked!"),
        _INTL("{1} suddenly howled!"),
        _INTL("{1} is growling softly.")
      ]
    elsif pkmn.hasType?(:GHOST)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ELIPSES)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} is being a bit quiet."),
        _INTL("{1} stuck close to {2}."),
        _INTL("{1} seems a bit flustered."),
        _INTL("{1} is looking at all the tombs.")
      ]
    else
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ELIPSES)
      messages = [
        _INTL("{1} is being a bit quiet."),
        _INTL("{1} is looking at all the tombs."),
        _INTL("{1} seems a bit on edge.")
      ]
    end
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map ID is Helianthus City (Outside)
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.map_id == 85 && pkmn.isSpecies?(:SOLGALEO)
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ELIPSES)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} looks at the statue and softly growls.")
    ]
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name has Day Care in it
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Day Care") && pkmn.isSpecies?(:INDEEDEE)
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} seems eager to help!"),
      _INTL("{1} suddenly got to work!"),
      _INTL("{1} looks to {2} for instructions."),
      _INTL("{1} is suddenly acting a bit more professional!")
    ]
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name has Library in it
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Library")
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ELIPSES)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} is trying to be as quiet as they can."),
      _INTL("{1} is trying to be quiet."),
      _INTL("{1} is looking at all the books."),
      _INTL("{1} quickly covered their mouth.")
    ]
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name has Pumon or Laplaenta in it
# Specific Pokemon types have different reactions
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Pumon") || $game_map.name.include?("Laplaenta")
    if pkmn.hasType?(:ICE)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} is cheerful that it's become a bit cold!"),
        _INTL("A cold wind suddenly blew! {1} looks happy."),
        _INTL("{1} is cheering happily because of the cold!"),
        _INTL("{1} just looks so very happy!")
      ]
    elsif pkmn.hasType?(:GRASS) || pkmn.hasType?(:DRAGON)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_SAD)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems unhappy about the cold..."),
        _INTL("{1} made its body slightly smaller because of the cold...")
      ]
    elsif pkmn.hasType?(:FIRE)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_SMILE)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} doesn't seem to mind the cold."),
        _INTL("{1} moved closer to {2}. It got a bit warmer!"),
        _INTL("{1}'s flames make the area not as cold."),
        _INTL("{1} is sticking close to {2}.")
      ]
    else
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ELIPSES)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} doesn't seem to mind the cold too much."),
        _INTL("{1} sneezed!"),
        _INTL("{1} got a bit closer to {2} to warm up."),
        _INTL("{1} seems surprised it can see its own breath!"),
        _INTL("{1} seems to be trying to keep itself warm."),
        _INTL("{1} is shivering a little.")
      ]
    end
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name has Praestia in it
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Praestia")
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_SAD)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} seems on edge."),
      _INTL("Whoa! {1} suddenly got startled!"),
      _INTL("{1} is looking around restlessly")
    ]
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name has Mansion in it
# Specific Pokemon types have different reactions
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Mansion")
    if pkmn.hasType?(:GHOST)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} is being very playful."),
        _INTL("{1} is playing with... something"),
        _INTL("{1} swaying their body."),
        _INTL("{1} started snickering.")
      ]
    else
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ELIPSES)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} looks a bit scared."),
        _INTL("{1} is keeping close to {2}.")
      ]
    end
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name has Arcadia in it
# Specific Pokemon have different reactions
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Arcadia") && pkmn.isSpecies?(:SOLGALEO)
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_EXCLAIM)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} shook the ground with its voice!"),
      _INTL("{1} howled loudly!")
    ]
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name is Route 10 or Route 11
# Specific Pokemon types have different reactions
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.map_id == 11 || $game_map.map_id == 12
    if pkmn.hasType?(:WATER)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_MUSIC)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems very happy and is jumping around!"),
        _INTL("{1} seems to like splashing around."),
        _INTL("Whoa! {1} seems to want to play with {2} in the water!"),
        _INTL("{1} seems happy to be all wet!")
      ]
    elsif pkmn.hasType?(:FIRE)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_ANGRY)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems very unhappy to be wet..."),
        _INTL("{1} doesn't seem to like splashing around..."),
        _INTL("{1} is making an unhappy face...")
      ]
    else
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} seems to be enjoying the scenery."),
      _INTL("{1} seems to enjoy the sound of the waves moving the sand."),
      _INTL("{1} looks like it wants to swim!"),
      _INTL("{1} can barely look away from the ocean."),
      _INTL("{1} is staring longingly at the water."),
      _INTL("{1} keeps trying to shove {2} towards the water."),
      _INTL("{1} is excited to be looking at the sea!"),
      _INTL("{1} is happily watching the waves!"),
      _INTL("{1} is playing on the sand!"),
      _INTL("{1} is staring at {2}'s footprints in the sand."),
      _INTL("{1} is rolling around in the sand.")
    ]
    end
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name is Route 17
# Specific Pokemon types have different reactions
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Route 17")
    if pkmn.hasType?(:FAIRY)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_MUSIC)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems very happy and is jumping around!"),
        _INTL("{1} is admiring the area."),
        _INTL("{1} seems interested in the trees.")
      ]
    elsif pkmn.hasType?(:GROUND)
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_MUSIC)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} seems to like playing in the mud!"),
        _INTL("{1} seems content being in the mud.")
      ]
    else
      FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_EXCLAIM)
      pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
      messages = [
        _INTL("{1} is admiring the area."),
        _INTL("{1} seems curious about the trees."),
        _INTL("{1} started coughing! Looks like it inhaled some mist.")
      ]
    end
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name is Towngor City
# Specific Pokemon have different reactions
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Towngor") && (pkmn.isSpecies?(:MUDBRAY) || pkmn.isSpecies?(:MUDSDALE))
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} looks quite flattered!"),
      _INTL("{1} stood up a bit straighter."),
      _INTL("{1} seems like its trying to look impressive."),
      _INTL("{1} happily walks along with you!")
    ]
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name is Junkar City
# Specific Pokemon have different reactions
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.name.include?("Junkar") && (pkmn.isSpecies?(:TRUBBISH) || pkmn.isSpecies?(:GARBODOR))
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} stretched their body."),
      _INTL("{1} seems to want to settle down."),
      _INTL("{1} seems relaxed."),
      _INTL("{1} seems to be completely at ease."),
      _INTL("There's a content expression on {1}'s face."),
      _INTL("{1} is making itself comfortable."),
      _INTL("{1} looks like it wants to take a nap.")
    ]   
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name is the Development Building
# Specific Pokemon have different reactions
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.map_id == 142 && pkmn.isSpecies?(:PETILIL)
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} is looking at Peti with familiarity."),
      _INTL("{1} seems interested in what Peti is doing."),
      _INTL("{1} is wondering what's happening on the computer.")
    ]   
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}
#-------------------------------------------------------------------------------
# Specific message if the map name is the Development Building
# Specific Pokemon have different reactions
#-------------------------------------------------------------------------------
Events.OnTalkToFollower += proc { |pkmn, _random_val|
  if $game_map.map_id == 142 && pkmn.isSpecies?(:MEOWSTIC)
    FollowingPkmn.animation(FollowingPkmn::ANIMATION_EMOTE_HAPPY)
    pbMoveRoute($game_player, [PBMoveRoute::Wait, 20])
    messages = [
      _INTL("{1} is looking at Gelato with familiarity."),
      _INTL("{1} seems interested in what Gelato is doing."),
      _INTL("{1} is wondering what's happening on the computer.")
    ]   
    pbMessage(_INTL(messages.sample, pkmn.name, $Trainer.name))
    next true
  end
}