#===============================================================================
#  The Trade Expert
#    by Luka S.J.
#
#  Provides an emulated experience of Wonder Trade, but uses the base stat total
#  of a Pokemon species to determine which Pokemon you'll obtain from the trade.
#  Includes dialogues to make the event more interesting when trading your Pokemon.
#  To use, simply call it in an event via the script command:
#      tradeExpert(margin)
#  where margin is a percentage value (from 0.0 to 1.0) that determines the
#  increase in upper and lower base stat total values for the traded Pokemon.
#  By default, it is set to 0.1; meaning that the Pokemon you recieve will be
#  in the base stat total range of within 90% - 110% of the base stat total of
#  the Pokemon you're giving up for trade. The 'margin' parameter can be
#  omitted.
#  Look at the 'def tradePokemon' if you want to customize the Pokemon you're
#  getting from the trade even further.
#
#  Enjoy the script, and make sure to give credit!
#===============================================================================
#  Trade Expert (settings)
#===============================================================================
module TradeExpert

  # A list of Pokemon you cannot obtain from the Trade Expert
  TRADING_BLACKLIST = [
    :F00,:MT,:MT2,:HUMANOID
  ]

  # A list of Pokemon you cannot give to the Trade Expert
  GIVING_BLACKLIST = [
    :F00,:MT,:MT2,:HUMANOID,:MONSTER
  ]

  TRAINER_NAME = "Mystery Man"
  TRAINER_ID = 1204

  # hash containing the spoken text
  # add multiple entries as array to speak in sequence, or assign single message
  # to each defined key
  TEXT = {
    # text when starting the event
    "intro" => [
      "I specialize in finding rare Pokémon and trading them to trainers for Pokémon of equal worth."
    ],
    # confirmation message from the expert
    "asktrade" => "Is there any Pokémon you'd like me to take a look at? I might have a Pokémon of equal worth.",
    # rejection message if no mon for trade
    "invalid" => "Looks like you don't have a Pokémon to give me.",
    # rejection if no offer found
    "notrade" => "Wow, that {1} is an overwhelming Pokémon! I don't really have anything I could possibly offer. Sorry!",
    # thinking intermission
    "thinking" => "Hmm ... hmm ...",
    # trade options
    "trade0" => "That {1} is a very weak Pokémon.",
    "trade1" => "That {1} isn't a very intimidating Pokémon.",
    "trade2" => "That {1} of yours isn't a bad Pokémon.",
    "trade3" => "That {1} of yours is certainly an interesting Pokémon.",
    "trade4" => "That {1} of yours is what I'd call a fierce Pokémon.",
    "trade5" => "That {1}! Now that ... is a great Pokémon!",
    # trade proposal
    "propose" => "How about my {1} for your {2}?",
    # trade acceptance (player action)
    "accepttrade" => "It's been nice doing business with you! Let me know if you want to do any more trades.",
    # trade rejection (player action)
    "rejecttrade" => [
      "I think it would have been a fair trade.",
      "Well ... if you ever change your mind, let me know."
    ],
    # trade cancelled (player action)
    "canceltrade" => "Well ... if you ever change your mind, let me know."
  }

end
