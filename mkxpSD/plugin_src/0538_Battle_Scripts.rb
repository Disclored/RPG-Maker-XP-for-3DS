#===============================================================================
#  Elite Battle: DX
#    by Luka S.J.
# ----------------
#  Battle Scripts
#===============================================================================
module BattleScripts
  # example scripted battle for PIDGEY
  # you can define other scripted battles in here or make your own section
  # with the BattleScripts module for better organization as to not clutter the
  # main EBDX cofiguration script (or keep it here if you want to, your call)
  PIDGEY = {
    "turnStart0" => {
      :text => [
        "Wow! This here Pidgey is among the top percentage of Pidgey.",
        "I have never seen such a strong Pidgey!",
        "Btw, this feature works even during wild battles.",
        "Pretty exciting, right?"
      ],
      :file => "trainer024"
    }
  }
  # to call this battle script run the script from an event jusst before the
  # desired battle:
  #    EliteBattle.set(:nextBattleScript, :PIDGEY)

#===============================================================================
  # GYM LEADERS
#===============================================================================

  #-----------------------------------------------------------------------------
  XIN = {
   "faintedOpp" => "Oh? We might need to be serious, bro.",
   "afterLastOpp" => {
   	:bgm => "Battle Gym Leader FINAL",
   	:text => "It's not looking good... Come on, bro! Just one last push!"
   	},
   "loss" => "Too bad. Have you tried getting a bro of your own?",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Hee-hee! You can't underestimate the power of teamwork like this.")
      end
   end
  }
  #-----------------------------------------------------------------------------
  AMANDA = {
   "turnStart0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @weatherType = :Sandstorm
      else
        @weatherType = :None
      end
    end,
   "faintedOpp" => "This is where it starts to get heated...",
   "afterLastOpp" => {
   	:bgm => "Battle Gym Leader FINAL",
   	:text => "Don't get overconfident now. Let me show you the Telgior way!"
   	},
   "loss" => "Hmm, you were strong, but not strong enough.",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Do you still have a way out of this? Or are we done here?")
      end
   end
  }
  #-----------------------------------------------------------------------------
  CALIX = {
   "turnStart0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @weatherType = :Rain
      else
        @weatherType = :None
      end
    end,
   "faintedOpp" => "Ah! There goes my precious, but we're not finished yet!",
   "afterLastOpp" => {
   	:bgm => "Battle Gym Leader FINAL",
   	:text => "My shine hasn't been dimmed yet!"
   	},
   "loss" => "I understand, my radiance is just too much for you! You're not the first, nor the last!",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Is my shine too much for you, my dear? Worry not! It'll be over soon!")
      end
   end
  }
  #-----------------------------------------------------------------------------
  EMILIA = {
   "turnEnd0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battlers[1].pbRaiseStatStage(:ATTACK, 1, @battlers[1])
      end
    end,
   "turnEnd1"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battlers[1].pbRaiseStatStage(:ATTACK, 1, @battlers[1])
      end
    end,
   "turnEnd2"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battlers[1].pbRaiseStatStage(:ATTACK, 1, @battlers[1])
      end
    end,
   "turnEnd3"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battlers[1].pbRaiseStatStage(:ATTACK, 1, @battlers[1])
      end
    end,
   "turnEnd4"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battlers[1].pbRaiseStatStage(:ATTACK, 1, @battlers[1])
      end
    end,
   "faintedOpp" => "WHAAAAT!! Ok, but I have another Pokémon!!",
   "afterLastOpp" => {
   	:bgm => "Battle Gym Leader FINAL",
   	:text => "Grrrr... My last Pokémon and I, we can still crush you into dirt!!"
   	},
   "loss" => "HAHA!! That was the Emilia Special!! Come again for another taste of being BAD!!",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Yeah!! We're on fire, baby!! Time for our finishing moves!")
      end
   end
  }
  #-----------------------------------------------------------------------------
  EMILIA_1 = {
   "turnEnd0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battlers[1].pbRaiseStatStage(:ATTACK, 1, @battlers[1])
      end
    end,
   "turnEnd1"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battlers[1].pbRaiseStatStage(:ATTACK, 1, @battlers[1])
      end
    end,
   "turnEnd2"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battlers[1].pbRaiseStatStage(:ATTACK, 1, @battlers[1])
      end
    end,
   "turnEnd3"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battlers[1].pbRaiseStatStage(:ATTACK, 1, @battlers[1])
      end
    end,
   "turnEnd4"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battlers[1].pbRaiseStatStage(:ATTACK, 1, @battlers[1])
      end
    end,
   "faintedOpp" => "We're not done yet!! Time to bring out the big guns!",
   "afterLastOpp" => {
   	:bgm => "Battle Gym Leader FINAL",
   	:text => "My last Pokémon and I, this is where we kick your butt!!"
   	},
   "loss" => "That was the Emilia Special!! ... Wait, does this mean I'm the new Solar Monarch??",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("The magma of our souls burns bright! Time for our finishing move!!")
      end
   end
  }
  #-----------------------------------------------------------------------------
  BENJAMIN = {
   "turnEnd0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battlers[1].pbRecoverHP(@battlers[1].totalhp/16)
      end
    end,
   "turnEnd1"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battlers[1].pbRecoverHP(@battlers[1].totalhp/16)
      end
    end,
   "turnEnd2"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battlers[1].pbRecoverHP(@battlers[1].totalhp/16)
      end
    end,
   "turnEnd3"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battlers[1].pbRecoverHP(@battlers[1].totalhp/16)
      end
    end,
   "turnEnd4"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battlers[1].pbRecoverHP(@battlers[1].totalhp/16)
      end
    end,
   "faintedOpp" => "Wow... You're good...!",
   "afterLastOpp" => {
   	:bgm => "Battle Gym Leader FINAL",
   	:text => "My last Pokémon... We refuse to give up!"
   	},
   "loss" => "You did well. But you'll need to do better.",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("A few more blows...and it'll be over!")
      end
   end
  }
  #-----------------------------------------------------------------------------
  PATRIAMA = {
   "turnEnd0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        target = @battlers[0]
        if target.lastRegularMoveUsed != nil
          @battle.pbCommonAnimation("GRUDGE",target)
          target.effects[PBEffects::Disable]     = 5
          target.effects[PBEffects::DisableMove] = target.lastRegularMoveUsed
          @battle.pbDisplay(_INTL("{1}'s {2} was disabled!",target.pbThis,
            GameData::Move.get(target.lastRegularMoveUsed).name))
        end
      end
    end,
   "turnEnd1"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        target = @battlers[0]
        if target.lastRegularMoveUsed != nil
          @battle.pbCommonAnimation("GRUDGE",target)
          target.effects[PBEffects::Disable]     = 5
          target.effects[PBEffects::DisableMove] = target.lastRegularMoveUsed
          @battle.pbDisplay(_INTL("{1}'s {2} was disabled!",target.pbThis,
            GameData::Move.get(target.lastRegularMoveUsed).name))
        end
      end
    end,
   "turnEnd2"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        target = @battlers[0]
        if target.lastRegularMoveUsed != nil
          @battle.pbCommonAnimation("GRUDGE",target)
          target.effects[PBEffects::Disable]     = 5
          target.effects[PBEffects::DisableMove] = target.lastRegularMoveUsed
          @battle.pbDisplay(_INTL("{1}'s {2} was disabled!",target.pbThis,
            GameData::Move.get(target.lastRegularMoveUsed).name))
        end
      end
    end,
   "turnEnd3"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        target = @battlers[0]
        if target.lastRegularMoveUsed != nil
          @battle.pbCommonAnimation("GRUDGE",target)
          target.effects[PBEffects::Disable]     = 5
          target.effects[PBEffects::DisableMove] = target.lastRegularMoveUsed
          @battle.pbDisplay(_INTL("{1}'s {2} was disabled!",target.pbThis,
            GameData::Move.get(target.lastRegularMoveUsed).name))
        end
      end
    end,
   "turnEnd4"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        target = @battlers[0]
        if target.lastRegularMoveUsed != nil
          @battle.pbCommonAnimation("GRUDGE",target)
          target.effects[PBEffects::Disable]     = 5
          target.effects[PBEffects::DisableMove] = target.lastRegularMoveUsed
          @battle.pbDisplay(_INTL("{1}'s {2} was disabled!",target.pbThis,
            GameData::Move.get(target.lastRegularMoveUsed).name))
        end
      end
    end,
   "faintedOpp" => "I see... Is this how strong the outside is?",
   "afterLastOpp" => {
   	:bgm => "Battle Gym Leader FINAL",
   	:text => "I didn't see this one coming... But I'm not done yet!"
   	},
   "loss" => "Umm... is it over?",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Hah! I know a thing or two, you know! I'm not here for no reason!")
      end
   end
  }
  #-----------------------------------------------------------------------------
  ANGELINE = {
   "turnStart0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @weatherType = :Hail
      else
        @weatherType = :None
      end
    end,
   "faintedOpp" => "Oh...",
   "afterLastOpp" => {
   	:bgm => "Battle Gym Leader FINAL",
   	:text => "This is looking rather south for me..."
   	},
   "loss" => "It's over...",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Are you losing? To me? Oh my...")
      end
   end
  }
  #-----------------------------------------------------------------------------
  LEONARD = {
   "faintedOpp" => "Hmph! You've got some bite, huh...",
   "afterLastOpp" => {
   	:bgm => "Battle Gym Leader FINAL",
   	:text => "Hehe... HAH-HAH-HAH!! That's it!! Let's finish this fight!!"
   	},
   "loss" => "Hmph... I thought you would do better.",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Hehe... Don't tell me you're finished! Come on and fight!")
      end
   end
  }
  #-----------------------------------------------------------------------------
  YIRA = {
   "turnStart0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        target = @battlers[1]
        target.pbOwnSide.effects[PBEffects::Reflect] = 5
        target.pbOwnSide.effects[PBEffects::LightScreen] = 5
        @battle.pbDisplay(_INTL("A barrier raised the opponent's Defense and Special Defense!"))
      end
    end,
    "faintedOpp" => "Good work, but be warned: this is only the start.",
    "afterLastOpp" => {
	:bgm => "Battle Gym Leader FINAL",
	:text => "A valiant effort thus far, but I won't falter."
	},
    "loss" => "Is this your true strength?",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("What will you do? Persevere, or concede?")
      end
   end
  }
#===============================================================================
  # RIVALS
#===============================================================================
  #-----------------------------------------------------------------------------
  GILTBERT_1 = {
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].full_name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["You already share such a strong bond with your Pokémon!",
                            "But me and #{pname}, we trust each other fully!"
                          ])
      if $game_switches[Settings::HARDER_BOSSES]
        # play aura flare
        @scene.pbDisplay("#{tname} and #{pname} are breathing in perfect sync with one another!")
        EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
        @vector.reset # AURAFLARE doesn't reset the vector by default
        @scene.wait(16, true) # set true to anchor the sprites to vector
        # raise battler stats (doesn't display text)
        @battlers[1].effects[PBEffects::Substitute] = (@battlers[1].totalhp / 2).clamp(1,999)
        @battlers[1].effects[PBEffects::ImmuneStatus] = true
        @battlers[1].effects[PBEffects::FocusEnergy] = 2
        @scene.pbDisplay("#{pname} is getting pumped!")
        @scene.pbDisplay("#{pname}'s determination will shrug off status ailments!")
      end
    end,
    "loss" => "Whoo! We did it, yes!",
   "afterLast" => proc do
      pname = @battlers[1].name
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Yeah! Our emotions are in tune, #{pname}!")
      end
   end
    }
  #-----------------------------------------------------------------------------
  GILTBERT_3 = {
   "faintedOpp" => "Oof! You're good as always, ha ha!",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].full_name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["My last Pokémon... We'll keep fighting until the end!",
                            "We'll show you the combined might of me and #{pname}!"
                          ])
      if $game_switches[Settings::HARDER_BOSSES]
        # play aura flare
        @scene.pbDisplay("#{tname} and #{pname} are breathing in perfect sync with one another!")
        EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
        @vector.reset # AURAFLARE doesn't reset the vector by default
        @scene.wait(16, true) # set true to anchor the sprites to vector
        # raise battler stats (doesn't display text)
        @battlers[1].effects[PBEffects::Substitute] = (@battlers[1].totalhp / 2).clamp(1,999)
        @battlers[1].effects[PBEffects::ImmuneStatus] = true
        @battlers[1].effects[PBEffects::FocusEnergy] = 2
        @scene.pbDisplay("#{pname} is getting pumped!")
        @scene.pbDisplay("#{pname}'s determination will shrug off status ailments!")
      end
    end,
   "loss" => "My Pokémon are my strength, and I'm theirs! We'll always cover each other.",
   "afterLast" => proc do
      pname = @battlers[1].name
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Victory is nearly there, #{pname}! Lend me your power just a bit longer!")
      end
   end
    }
  #-----------------------------------------------------------------------------
  GILTBERT_4 = {
   "faintedOpp" => "You did your best, buddy. It's not over yet!",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].full_name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["My Pokémon and I will never yield, no matter what happens!",
                            "You'll see the power of the bond #{pname} and I share!"
                          ])
      if $game_switches[Settings::HARDER_BOSSES]
        # play aura flare
        @scene.pbDisplay("#{tname} and #{pname} are breathing in perfect sync with one another!")
        EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
        @vector.reset # AURAFLARE doesn't reset the vector by default
        @scene.wait(16, true) # set true to anchor the sprites to vector
        # raise battler stats (doesn't display text)
        @battlers[1].effects[PBEffects::Substitute] = (@battlers[1].totalhp / 2).clamp(1,999)
        @battlers[1].effects[PBEffects::ImmuneStatus] = true
        @battlers[1].effects[PBEffects::FocusEnergy] = 2
        @scene.pbDisplay("#{pname} is getting pumped!")
        @scene.pbDisplay("#{pname}'s determination will shrug off status ailments!")
      end
    end,
   "loss" => "Our power comes from our unbreakable bond. That's what makes us a good team!",
   "afterLast" => proc do
      pname = @battlers[1].name
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("This is it! #{pname}! Everyone! We can win this!")
      end
   end
    }
  #-----------------------------------------------------------------------------
  GILTBERT_6 = {
   "faintedOpp" => "Your Pokémon and you look like they're in complete sync!",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["It's just us now, #{pname}...",
                             "As long as we're in tune, we can win!",
                             "So... lend me your strength! And I'll give you mine!"
                           ])
      # play aura flare
      @scene.pbDisplay("#{tname} and #{pname} are breathing in perfect sync with one another!")
      EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
      @vector.reset # AURAFLARE doesn't reset the vector by default
      @scene.wait(16, true) # set true to anchor the sprites to vector
      # raise battler stats (doesn't display text)
      @battlers[1].pbRaiseStatStageBasic(:SPECIAL_ATTACK, 2)
      @battlers[1].pbRaiseStatStageBasic(:SPEED, 2) 
      if $game_switches[Settings::HARDER_BOSSES]
        @battlers[1].effects[PBEffects::Substitute] = (@battlers[1].totalhp / 2).clamp(1,999)
        @battlers[1].effects[PBEffects::ImmuneStatus] = true
        @battlers[1].effects[PBEffects::FocusEnergy] = 2
        @scene.pbDisplay("#{pname} is getting pumped!")
        @scene.pbDisplay("#{pname}'s determination will shrug off status ailments!")
      end
    end,
   "loss" => "I really thought you'd win... It's thanks to you that I even got this far.",
   "afterLast" => proc do
      pname = @battlers[1].name
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("#{pname}, we're almost there! Let's do this!")
      end
   end
    }
  #-----------------------------------------------------------------------------
  DIANA_1 = {
    "loss" => "Hmph! What a waste of time.",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Seriously? Tch...")
      end
   end
    }
  #-----------------------------------------------------------------------------
  DIANA_2 = {
   "faintedOpp" => "Tch... I'm not done yet!",
   "afterLastOpp" => {
   	:text => "So you know how to keep up..."
   	},
   "loss" => "Sorry, but that wasn't even close.",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("I'm almost ashamed over here... Come on, pick it up!")
      end
   end
    }
  #-----------------------------------------------------------------------------
  DIANA_3 = {
   "faintedOpp" => "Tch... So what?",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].full_name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["The stars did not foretell this... So you know how to keep up...",
                            "#{pname} and I are far from done. Our victory was written in the stars."
                          ])
      if $game_switches[Settings::HARDER_BOSSES]
        # play aura flare
        @scene.pbDisplay("#{tname} and #{pname} are breathing in perfect sync with one another!")
        EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
        @vector.reset # AURAFLARE doesn't reset the vector by default
        @scene.wait(16, true) # set true to anchor the sprites to vector
        # raise battler stats (doesn't display text)
        @battlers[1].effects[PBEffects::Substitute] = (@battlers[1].totalhp / 2).clamp(1,999)
        @battlers[1].effects[PBEffects::ImmuneStatus] = true
        @battlers[1].effects[PBEffects::FocusEnergy] = 2
        @scene.pbDisplay("#{pname} is getting pumped!")
        @scene.pbDisplay("#{pname}'s determination will shrug off status ailments!")
      end
    end,
   "loss" => "What's up with you? Stop wasting my time...",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("What happened? Didn't train hard enough?")
      end
   end
    }
  #-----------------------------------------------------------------------------
  DIANA_5 = {
   "faintedOpp" => "I'm sensing it... Your great power...",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].full_name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["When the stars foretold this battle, I truly don't believe they expected this.",
                            "But no matter. The stars do not reveal all. Me and #{pname} will not lose here!"
                          ])
      if $game_switches[Settings::HARDER_BOSSES]
        # play aura flare
        @scene.pbDisplay("#{tname} and #{pname} are breathing in perfect sync with one another!")
        EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
        @vector.reset # AURAFLARE doesn't reset the vector by default
        @scene.wait(16, true) # set true to anchor the sprites to vector
        # raise battler stats (doesn't display text)
        @battlers[1].effects[PBEffects::Substitute] = (@battlers[1].totalhp / 2).clamp(1,999)
        @battlers[1].effects[PBEffects::ImmuneStatus] = true
        @battlers[1].effects[PBEffects::FocusEnergy] = 2
        @scene.pbDisplay("#{pname} is getting pumped!")
        @scene.pbDisplay("#{pname}'s determination will shrug off status ailments!")
      end
    end,
   "loss" => "Sadly, I'm disappointed with what you have to offer.",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("What now? Are you going to lose, just like that?")
      end
   end
    }
  #-----------------------------------------------------------------------------
  DIANA_6 = {
   "faintedOpp" => "We're not done yet, I'm only getting started!",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["The stars are always on your side...",
                             "But I don't intend to give up so easily!",
                             "With my passion, I'll claim the title of Solar Monarch!"
                           ])
      # play aura flare
      @scene.pbDisplay("#{tname} and #{pname} are breathing in perfect sync with one another!")
      EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
      @vector.reset # AURAFLARE doesn't reset the vector by default
      @scene.wait(16, true) # set true to anchor the sprites to vector
      # raise battler stats (doesn't display text)
      @battlers[1].pbRaiseStatStageBasic(:SPECIAL_ATTACK, 2)
      if $game_switches[Settings::HARDER_BOSSES]
        @battlers[1].effects[PBEffects::Substitute] = (@battlers[1].totalhp / 2).clamp(1,999)
        @battlers[1].effects[PBEffects::ImmuneStatus] = true
        @battlers[1].effects[PBEffects::FocusEnergy] = 2
        @scene.pbDisplay("#{pname} is getting pumped!")
        @scene.pbDisplay("#{pname}'s determination will shrug off status ailments!")
      end
    end,
   "loss" => "I won... The stars were always on my side, after all.",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("You've always pushed me to my limits. Come on, push your own!")
      end
   end
    }
  #-----------------------------------------------------------------------------
  DIANA_8 = {
   "turnStart0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battle.pbStartTerrain(@battlers[1], :Misty)
      end
    end,
   "faintedOpp" => "That was just a warm-up. Let the real match commence.",
   "afterLastOpp" => {
   	:bgm => "Battle Gym Leader FINAL",
   	:text => "That's it, keep it up. However, I won't let you win so easily."
   	},
   "loss" => "To be victorious against the first Challenger to my Gym... You're playing with me.",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Now of all times? You're better than this.")
      end
   end
  }
  #-----------------------------------------------------------------------------
  HELIO = {
    "faintedOpp" => "You're good... What am I to do now...",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["Can I still make a comeback...? Oh, I can't give up now..."
                           ])
      if $game_switches[Settings::HARDER_BOSSES]
        # play aura flare
        EliteBattle.playCommonAnimation(:USEITEM, @scene, 1)
        spAtk = @battlers[1].spatk
        atk = @battlers[1].attack
        echoln("Atk: " + atk.to_s + " / Sp. Atk: " + spAtk.to_s)
        # Prefer Atk over Sp. Atk
        if spAtk > atk
          @scene.pbDisplay("#{tname} used the X Sp. Atk!")
          # raise battler stats
          @battlers[1].pbRaiseStatStage(:SPECIAL_ATTACK, 2, @battlers[1])
        else
          @scene.pbDisplay("#{tname} used the X Attack!")
          # raise battler stats
          @battlers[1].pbRaiseStatStage(:ATTACK, 2, @battlers[1])
        end
      end
    end,
    "loss" => "Ah, I won then... I guess I managed to catch up to you.",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Am I really giving you a run for your money...?")
      end
   end
  }
  #-----------------------------------------------------------------------------
  SHOKO = {
    "faintedOpp" => "...! I'll try to be more serious, then...!",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["You really got me cornered here... But I won't give up...!"
                           ])
      if $game_switches[Settings::HARDER_BOSSES]
        # play aura flare
        EliteBattle.playCommonAnimation(:USEITEM, @scene, 1)
        spAtk = @battlers[1].spatk
        atk = @battlers[1].attack
        echoln("Atk: " + atk.to_s + " / Sp. Atk: " + spAtk.to_s)
        # Prefer Sp. Atk over Atk
        if spAtk >= atk
          @scene.pbDisplay("#{tname} used the X Sp. Atk!")
          # raise battler stats
          @battlers[1].pbRaiseStatStage(:SPECIAL_ATTACK, 2, @battlers[1])
        else
          @scene.pbDisplay("#{tname} used the X Attack!")
          # raise battler stats
          @battlers[1].pbRaiseStatStage(:ATTACK, 2, @battlers[1])
        end
      end
    end,
    "loss" => "Yes...! I won!! I can't believe I did it!",
    "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Did you underestimate me? Then say good night...!")
      end
   end
  }
  #-----------------------------------------------------------------------------
#===============================================================================
  # TEAM SOL
#===============================================================================
  #-----------------------------------------------------------------------------
  AELIA_1 = {
   "turnEnd0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battle.sides[0].effects[PBEffects::Swamp] = 4
      end
    end,
    "faintedOpp" => "Aha... So this is a Gym Challenger's might.",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].name
      # begin code block for the first turn
      pbBGMPlay("Battle Team Sol Admin Salty")
      @scene.pbTrainerSpeak(["This is my last Pokémon... Won't you go a bit easier on me?"
                           ])
      if $game_switches[Settings::HARDER_BOSSES]
        # play aura flare
        EliteBattle.playCommonAnimation(:USEITEM, @scene, 1)
        @scene.pbDisplay("#{tname} used the X Attack!")
        # raise battler stats
        @battlers[1].pbRaiseStatStage(:ATTACK, 1, @battlers[1])
      end
    end,
    "loss" => "Honestly, it was... easier than I thought or hoped...",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("My, my, my... Is this going to be your trump card?")
      end
   end
    }
  #-----------------------------------------------------------------------------
  AELIA_2 = {
   "turnEnd0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battle.sides[0].effects[PBEffects::Swamp] = 4
      end
    end,
    "faintedOpp" => "Good, good! Your skill doesn't disappoint.",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["You've really backed me into a corner now...",
                             "It's time for me to show you what Team Sol can offer.",
                             "Allow me to unlock #{pname}'s true potential!"
                           ])
      # play aura flare
      EliteBattle.playCommonAnimation(:USEITEM, @scene, 1)
      @scene.pbDisplay("#{tname} used the Solarium!")
      @scene.pbDisplay("Immense energy is swelling up in #{pname}!")
      pbBGMPlay("Battle Team Sol Admin Salty")
      EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
      @vector.reset # AURAFLARE doesn't reset the vector by default
      @scene.wait(16, true) # set true to anchor the sprites to vector
      # raise battler stats (doesn't display text)
      @battlers[1].pbRaiseStatStageBasic(:DEFENSE, 1)
      @battlers[1].pbRaiseStatStageBasic(:SPECIAL_DEFENSE, 1)
    end,
    "loss" => "Do you see now? Team Sol is the future!",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Am I too much for you? It's fine to admit defeat.")
      end
   end
    }
  #-----------------------------------------------------------------------------
  AELIA_3 = {
   "turnEnd0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battle.sides[0].effects[PBEffects::Swamp] = 4
      end
    end,
    "faintedOpp" => "Here we go... You're as good as ever.",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["Everyone! Please bear witness...",
                             "Allow me to unlock #{pname}'s true potential!"
                           ])
      # play aura flare
      EliteBattle.playCommonAnimation(:USEITEM, @scene, 1)
      @scene.pbDisplay("#{tname} used the Solarium!")
      @scene.pbDisplay("Immense energy is swelling up in #{pname}!")
      pbBGMPlay("Battle Team Sol Admin Salty")
      EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
      @vector.reset # AURAFLARE doesn't reset the vector by default
      @scene.wait(16, true) # set true to anchor the sprites to vector
      # raise battler stats (doesn't display text)
      @battlers[1].pbRaiseStatStageBasic(:SPECIAL_ATTACK, 1)
      @battlers[1].pbRaiseStatStageBasic(:SPECIAL_DEFENSE, 1)
    end,
    "loss" => "This is the power that Team Sol can offer to everyone.",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("This is far from your best. Come on, let's give everyone a show.")
      end
   end
    }
  #-----------------------------------------------------------------------------
  LAIRUS_1 = {
   "turnEnd0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        b = @battlers[0]
        b.pbBurn if b.pbCanBurn?(b,false,self)
      end
    end,
   "turnEnd1"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        b = @battlers[0]
        b.pbBurn if b.pbCanBurn?(b,false,self)
      end
    end,
   "turnEnd2"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        b = @battlers[0]
        b.pbBurn if b.pbCanBurn?(b,false,self)
      end
    end,
   "turnEnd3"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        b = @battlers[0]
        b.pbBurn if b.pbCanBurn?(b,false,self)
      end
    end,
   "turnEnd4"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        b = @battlers[0]
        b.pbBurn if b.pbCanBurn?(b,false,self)
      end
    end,
    "faintedOpp" => "You've got some nerve!",
    "afterLastOpp" => "Not bad, punk! But I'm not backing down.",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].name
      # begin code block for the first turn
      if $game_switches[Settings::HARDER_BOSSES]
        @scene.pbTrainerSpeak(["Not bad, punk! But I'm not backing down.",
                                "Tsk... I'll just have to use THIS!",
                           ])
        # play aura flare
        EliteBattle.playCommonAnimation(:USEITEM, @scene, 1)
        @scene.pbDisplay("#{tname} used a strange item!")
        @scene.pbDisplay("Immense energy is swelling up in #{pname}!")
        pbBGMPlay("Battle Team Sol Admin Salty")
        EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
        @vector.reset # AURAFLARE doesn't reset the vector by default
        @scene.wait(16, true) # set true to anchor the sprites to vector
        # raise battler stats (doesn't display text)
        @battlers[1].pbRaiseStatStageBasic(:ATTACK, 1)
        @battlers[1].pbRaiseStatStageBasic(:SPEED, 1)
      else
        pbBGMPlay("Battle Team Sol Admin Salty")
        @scene.pbTrainerSpeak(["Not bad, punk. But I'm not backing down!"
                           ])
      end
    end,
    "loss" => "Hah! You lose. Now get lost!",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Starting to feel hopeless? You shouldn't have messed with me!")
      end
   end
    }
 #-----------------------------------------------------------------------------
  LAIRUS_2 = {
   "turnEnd0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        b = @battlers[0]
        b.pbFreeze if b.pbCanFreeze?(b,false,self)
      end
    end,
   "turnEnd1"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        b = @battlers[0]
        b.pbFreeze if b.pbCanFreeze?(b,false,self)
      end
    end,
   "turnEnd2"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        b = @battlers[0]
        b.pbFreeze if b.pbCanFreeze?(b,false,self)
      end
    end,
   "turnEnd3"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        b = @battlers[0]
        b.pbFreeze if b.pbCanFreeze?(b,false,self)
      end
    end,
   "turnEnd4"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        b = @battlers[0]
        b.pbFreeze if b.pbCanFreeze?(b,false,self)
      end
    end,
    "faintedOpp" => "Strong start, eh? Well, let's see you handle this!",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["I've had it with you, punk!",
                             "Let's see if you'll be able to handle my #{pname} after I use this!"
                           ])
      # play aura flare
      EliteBattle.playCommonAnimation(:USEITEM, @scene, 1)
      @scene.pbDisplay("#{tname} used the Solarium!")
      @scene.pbDisplay("Immense energy is swelling up in #{pname}!")
      pbBGMPlay("Battle Team Sol Admin Salty")
      EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
      @vector.reset # AURAFLARE doesn't reset the vector by default
      @scene.wait(16, true) # set true to anchor the sprites to vector
      # raise battler stats (doesn't display text)
      @battlers[1].pbRaiseStatStageBasic(:ATTACK, 1)
      @battlers[1].pbRaiseStatStageBasic(:SPEED, 1)
    end,
    "loss" => "Bwhahaha!! You only have yourself to blame.",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon      
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Where did that attitude from before go? Face it, it's game over!")
      end
   end
    }
 #-----------------------------------------------------------------------------
  LAIRUS_3 = {
   "turnEnd0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        b = @battlers[0]
        b.pbParalyze if b.pbCanParalyze?(b,false,self)
      end
    end,
   "turnEnd1"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        b = @battlers[0]
        b.pbParalyze if b.pbCanParalyze?(b,false,self)
      end
    end,
   "turnEnd2"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        b = @battlers[0]
        b.pbParalyze if b.pbCanParalyze?(b,false,self)
      end
    end,
   "turnEnd3"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        b = @battlers[0]
        b.pbParalyze if b.pbCanParalyze?(b,false,self)
      end
    end,
   "turnEnd4"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        b = @battlers[0]
        b.pbParalyze if b.pbCanParalyze?(b,false,self)
      end
    end,
    "faintedOpp" => "Not bad... But this is only the beginning.",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["You're really getting me fired up here, noobie!",
                             "#{pname}, you feel the same way right? Let's crush them!"
                           ])
      # play aura flare
      EliteBattle.playCommonAnimation(:USEITEM, @scene, 1)
      @scene.pbDisplay("#{tname} used the Solarium!")
      @scene.pbDisplay("Immense energy is swelling up in #{pname}!")
      pbBGMPlay("Battle Team Sol Admin Salty")
      EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
      @vector.reset # AURAFLARE doesn't reset the vector by default
      @scene.wait(16, true) # set true to anchor the sprites to vector
      # raise battler stats (doesn't display text)
      @battlers[1].pbRaiseStatStageBasic(:ATTACK, 1)
      @battlers[1].pbRaiseStatStageBasic(:SPEED, 1)
    end,
    "loss" => "Ha... Ha...",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon      
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("I can taste it...! Sweet victory, it's within my grasp!")
      end
   end
    }
  #-----------------------------------------------------------------------------
  SIENNA_1 = {
   "turnEnd0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battle.sides[1].effects[PBEffects::Tailwind] = 4
      end
    end,
    "faintedOpp" => "Unfortunate.",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].full_name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["Have I miscalculated?",
                             "It seems that I will be forced to use...this."
                           ])
      # play aura flare
      EliteBattle.playCommonAnimation(:USEITEM, @scene, 1)
      @scene.pbDisplay("#{tname} used the Solarium!")
      @scene.pbDisplay("Immense energy is swelling up in #{pname}!")
      pbBGMPlay("Battle Team Sol Admin Salty")
      EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
      @vector.reset # AURAFLARE doesn't reset the vector by default
      @scene.wait(16, true) # set true to anchor the sprites to vector
      # raise battler stats (doesn't display text)
      @battlers[1].pbRaiseStatStageBasic(:ATTACK, 1)
      @battlers[1].pbRaiseStatStageBasic(:DEFENSE, 1)
    end,
    "loss" => "You have been defeated.",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("It is useless to resist.")
      end
   end
    }
  #-----------------------------------------------------------------------------
  SIENNA_2 = {
   "turnEnd0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battle.sides[1].effects[PBEffects::Tailwind] = 4
      end
    end,
    "faintedOpp" => "Interesting.",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].full_name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["I have not tested this potent version of Solarium, yet.",
                             "But risks are a part of laboratory science."
                           ])
      # play aura flare
      EliteBattle.playCommonAnimation(:USEITEM, @scene, 1)
      @scene.pbDisplay("#{tname} used the Solarium!")
      @scene.pbDisplay("Immense energy is swelling up in #{pname}!")
      pbBGMPlay("Battle Team Sol Admin Salty")
      EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
      @vector.reset # AURAFLARE doesn't reset the vector by default
      @scene.wait(16, true) # set true to anchor the sprites to vector
      # raise battler stats (doesn't display text)
      @battlers[1].pbRaiseStatStageBasic(:ATTACK, 2)
      @battlers[1].pbRaiseStatStageBasic(:DEFENSE, 1)
    end,
    "loss" => "You have been bested. But the odds were never in your favour.",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Yes. These are very, very good results.")
      end
   end
    }
  #-----------------------------------------------------------------------------
  SIENNA_3 = {
   "turnEnd0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battle.sides[1].effects[PBEffects::Tailwind] = 4
      end
    end,
    "faintedOpp" => "...",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].full_name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["Day in and day out, I fine tune everything I can.",
                             "This is the best that I can offer you. We'll see if it's enough."
                           ])
      # play aura flare
      EliteBattle.playCommonAnimation(:USEITEM, @scene, 1)
      @scene.pbDisplay("#{tname} used the Solarium!")
      @scene.pbDisplay("Immense energy is swelling up in #{pname}!")
      pbBGMPlay("Battle Team Sol Admin Salty")
      EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
      @vector.reset # AURAFLARE doesn't reset the vector by default
      @scene.wait(16, true) # set true to anchor the sprites to vector
      # raise battler stats (doesn't display text)
      @battlers[1].pbRaiseStatStageBasic(:ATTACK, 1)
      @battlers[1].pbRaiseStatStageBasic(:DEFENSE, 1)
    end,
    "loss" => "I have bested you. Your services are no longer required.",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Interesting. Have I cracked the formula?")
      end
   end
    }
  #-----------------------------------------------------------------------------
  ZENITHIAN_1 = {
   "turnEnd0"   => proc do
      if $game_switches[Settings::HARDER_BOSSES]
        @battle.sides[0].effects[PBEffects::Spikes] = 2
      end
    end,
    "faintedOpp" => "Alright, alright... But how about this?",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].full_name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["You're really living up to your title, aren't you?",
                             "Let's make things interesting, shall we?"
                           ])
      # play aura flare
      EliteBattle.playCommonAnimation(:USEITEM, @scene, 1)
      @scene.pbDisplay("#{tname} used the Solarium!")
      @scene.pbDisplay("Immense energy is swelling up in #{pname}!")
      EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
      @vector.reset # AURAFLARE doesn't reset the vector by default
      @scene.wait(16, true) # set true to anchor the sprites to vector
      # raise battler stats (doesn't display text)
      @battlers[1].pbRaiseStatStageBasic(:ATTACK, 1)
      @battlers[1].pbRaiseStatStageBasic(:DEFENSE, 1)
      @battlers[1].pbRaiseStatStageBasic(:SPECIAL_ATTACK, 1)
      @battlers[1].pbRaiseStatStageBasic(:SPECIAL_DEFENSE, 1)
    end,
    "loss" => "And it seems that you've been defeated!! How sad...",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("You're in a tough spot, it seems! Can you make a comeback?")
      end
   end
    }
  #-----------------------------------------------------------------------------
  ZENITHIAN_2 = {
   "turnStart0"   => proc do
      @battlers[1].effects[PBEffects::INVINCIBLE] = true
      @battlers[1].effects[PBEffects::ImmuneStatus] = true
      @battlers[1].effects[PBEffects::ImmuneDebuffs] = true
      pname = @battlers[1].name
      @scene.pbDisplay("#{pname} is radiating sheer might!!!")
    end,
   "turnStart4"   => proc do
      # Just get it over with
      @battlers[1].effects[PBEffects::UNSTOPPABLE] = true
    end,
   "fainted"   => proc do
      @scene.pbTrainerSpeak(["I assume you're too stupid to realise, so spoiler alert...",
                             "But there is absolutely NOTHING you can do! So just admit defeat!"
                           ])
   end
  }
  #-----------------------------------------------------------------------------
  ZENITHIAN_3 = {
   "turnStart0"   => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].full_name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["I am the new Solar King of Solaria! Not you!",
                             "Witness the power of my improved #{pname}! Of the future!!"
                           ])
      # Solgaleo cannot be force switched, due to dialogue later on.
      pkmn = @battlers[1]
      pkmn.effects[PBEffects::ImmuneSwitching] = true
      pkmn.effects[PBEffects::ImmuneStatus] = true
      pkmn.effects[PBEffects::ImmuneDebuffs] = true
      pkmn.effects[PBEffects::SuperSturdy] = true
      @scene.pbDisplay("#{pname} is radiating sheer might!")

      if $game_switches[Settings::HARDER_BOSSES]
        @battle.sides[0].effects[PBEffects::Spikes] = 2
      end
    end,
   "halfHPOpp"   => proc do
      pname = @battlers[1].name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["How are you hurting my #{pname}?!",
                             "Don't tell me that #{pname} actually wants you to defeat it...",
                             "You're supposed to obey MY commands, #{pname}!!"
                           ])
      # play aura flare
      EliteBattle.playCommonAnimation(:USEITEM, @scene, 1)
      @scene.pbDisplay("Solarium is being pumped into #{pname} due to the armour!")
      @scene.pbDisplay("Immense energy is swelling up in #{pname}!")
      EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
      @vector.reset # AURAFLARE doesn't reset the vector by default
      @scene.wait(16, true) # set true to anchor the sprites to vector
      # raise battler stats (doesn't display text)
      @battlers[1].pbRaiseStatStageBasic(:ATTACK, 1)
      @battlers[1].pbRaiseStatStageBasic(:DEFENSE, 1)
      @battlers[1].pbRaiseStatStageBasic(:SPECIAL_ATTACK, 1)
      @battlers[1].pbRaiseStatStageBasic(:SPECIAL_DEFENSE, 1)
    end,
   "faintedOpp" => "No!! It's not supposed to go like this!!",
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].full_name
      # begin code block for the first turn
      @scene.pbTrainerSpeak(["How are you beating me? How are you beating me?!",
                             "No no no!! This isn't how it's supposed to go!!",
                             "I'm going to ruin you!!!"
                           ])
      # play aura flare
      EliteBattle.playCommonAnimation(:USEITEM, @scene, 1)
      @scene.pbDisplay("#{tname} used the Solarium!")
      @scene.pbDisplay("Immense energy is swelling up in #{pname}!")
      EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
      @vector.reset # AURAFLARE doesn't reset the vector by default
      @scene.wait(16, true) # set true to anchor the sprites to vector
      # raise battler stats (doesn't display text)
      @battlers[1].pbRaiseStatStageBasic(:ATTACK, 1)
      @battlers[1].pbRaiseStatStageBasic(:DEFENSE, 1)
      @battlers[1].pbRaiseStatStageBasic(:SPECIAL_ATTACK, 1)
      @battlers[1].pbRaiseStatStageBasic(:SPECIAL_DEFENSE, 1)
    end,
    "loss" => {
      :text => [
        "This was an important victory for our region...",
        "Thanks to you, a new dawn for Solaria has broken!"
      ]
   },
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Can you envision it? The better Solaria that I will create in just a few moments?")
      end
   end
  }
  #-----------------------------------------------------------------------------
#===============================================================================
  # OTHERS
#===============================================================================
  #-----------------------------------------------------------------------------
  HIRO_1 = {
   "faintedOpp" => "So that's how it's gonna be, huh?",
   "afterLastOpp" => {
   	:bgm => "Battle Gym Leader FINAL",
   	:text => "Here we go! Don't cave in now!"
   	},
   "loss" => "Good fight!",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Is this it? You've got more left, right?")
      end
   end
  }
  #-----------------------------------------------------------------------------
  XENIA_1 = {
   "faintedOpp" => "Your strength thus far has piqued my interest!",
   "afterLastOpp" => {
   	:text => "I can see you raised your Pokémon well!"
   	},
   "loss" => "Oh dear... You've still got much to learn, it seems.",
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
        @scene.pbTrainerSpeak("Did you lose your composure?")
      end
   end
  }
  #-----------------------------------------------------------------------------
  DEVS = {
   "turnStart0"   => proc do
      @battlers[1].effects[PBEffects::INVINCIBLE] = true
      @battlers[1].effects[PBEffects::ImmuneStatus] = true
      @battlers[1].effects[PBEffects::ImmuneDebuffs] = true
      @battlers[1].effects[PBEffects::UNSTOPPABLE] = true
    end
  }
  #-----------------------------------------------------------------------------
  BATTLEARCADE = {
   "turnEnd0"   => proc do
      case pbGet(162)
      when 24
        @battle.field.effects[PBEffects::TrickRoom] = 5
      end
    end
  }
  #-------------------------------------s----------------------------------------
  FRIEND = {
   "loss" => proc do
      speach = pbGet(166)[:win] || "I've won!"
      @scene.pbTrainerSpeak("#{speach}")
   end
  }
  #-------------------------------------s----------------------------------------
  NEED2LOSE = {
   "fainted0" => proc do
      v = pbGet(219) + 1
      pbSet(219,v)
      echoln(pbGet(219))
   end,
   "fainted1" => proc do
      v = pbGet(219) + 1
      pbSet(219,v)
      echoln(pbGet(219))
   end,
   "fainted2" => proc do
      v = pbGet(219) + 1
      pbSet(219,v)
      echoln(pbGet(219))
   end,
   "fainted3" => proc do
      v = pbGet(219) + 1
      pbSet(219,v)
      echoln(pbGet(219))
   end,
   "fainted4" => proc do
      v = pbGet(219) + 1
      pbSet(219,v)
      echoln(pbGet(219))
   end,
   "fainted5" => proc do
      v = pbGet(219) + 1
      pbSet(219,v)
      echoln(pbGet(219))
   end
  }
  #-----------------------------------------------------------------------------
  BOSS = {
    "afterLastOpp" => proc do
      pname = @battlers[1].name
      tname = @battle.opponent[0].full_name
      # begin code block for the first turn
      @scene.pbDisplay("Immense energy is swelling up in #{pname}!")
      EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
      @vector.reset # AURAFLARE doesn't reset the vector by default
      @scene.wait(16, true) # set true to anchor the sprites to vector
      # raise battler stats (doesn't display text)
      @battlers[1].pbRaiseStatStageBasic(:ATTACK, 1)
      @battlers[1].pbRaiseStatStageBasic(:DEFENSE, 1)
      @battlers[1].pbRaiseStatStageBasic(:SPECIAL_ATTACK, 1)
      @battlers[1].pbRaiseStatStageBasic(:SPECIAL_DEFENSE, 1)
      @battlers[1].effects[PBEffects::Substitute] = (@battlers[1].totalhp / 2).clamp(1,999)
      @battlers[1].effects[PBEffects::SuperSturdy] = true
    end,
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
      end
   end
  }
  #-----------------------------------------------------------------------------
  JIRACHI = {
   "turnStart0"   => proc do
      EliteBattle.playCommonAnimation(:AURAFLARE, @scene, 1)
      @vector.reset # AURAFLARE doesn't reset the vector by default
      @scene.wait(16, true) # set true to anchor the sprites to vector
      @battlers[1].effects[PBEffects::SemiInvincible] = true
      pname = @battlers[1].name
      @scene.pbDisplay("#{pname} is radiating sheer might!")
    end,
   "afterLast" => proc do
      hasRevive = $PokemonBag.pbHasItem?(:REVIVE) || $PokemonBag.pbHasItem?(:MAXREVIVE) || $PokemonBag.pbHasItem?(:REVIVALHERB)
      lastMon = (!hasRevive || $game_switches[Settings::BAN_REVIVAL])
      if lastMon
        pbBGMPlay("EBDX/Low HP Battle") if $PokemonSystem.lowhp<2
      end
   end
    }
  #-----------------------------------------------------------------------------
  # example Dialga fight
  DIALGA = {
    "turnStart0" => proc do
      # hide databoxes
      @scene.pbHideAllDataboxes
      # show flavor text
      @scene.pbDisplay("The ruler of time itself; Dialga starts to radiate tremendous amounts of energy!")
      @scene.pbDisplay("Something is about to happen ...")
      # play common animation
      EliteBattle.playCommonAnimation(:ROAR, @scene, 1)
      @scene.pbDisplay("Dialga's roar is pressurizing the air around you! You feel its intensity!")
      # change the battle environment (use animation to transition)
      @sprites["battlebg"].reconfigure(:DIMENSION, :DISTORTION)
      @scene.pbDisplay("Its roar distorted the dimensions!")
      @scene.pbDisplay("Dialga is controlling the domain.")
      # show databoxes
      @scene.pbShowAllDataboxes
    end
  }
  #-----------------------------------------------------------------------------
end
