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
# Main class for handling the Trade Expert
#===============================================================================
module TradeExpert
  #-----------------------------------------------------------------------------
  # starts up the Trade Expert
  #-----------------------------------------------------------------------------
  def self.start(margin = 0.05)
    # flavour text displaying a little intro for the Trade Expert
#    self.displayMsg("intro")
    # confirmation for the trade
    if self.confirmMsg("asktrade")
      giveID = self.givePokemon
      return self.displayMsg("invalid") if giveID.nil?
      give = $Trainer.party[giveID]
      giveBST = self.calcBST(give.species)
      # protects trading from abuse
      if !$cachedTrade["#{giveBST}"].nil?
        recv = $cachedTrade["#{giveBST}"]
      else
        recv = self.fetchEqualSpecies(give.species, margin)
        # cancels the trade if the Trade Expert cannot offer you anything in return
        if recv.length < 1
          self.displayMsg("notrade", GameData::Species.get(give.species).real_name)
          return false
        end
        i = ($Trainer.id).to_i % recv.length
        recv = recv[i]
      end
      self.displayMsg("thinking")
      bst = self.calcBST(recv)
      # flavour text displayed when evaluating the offered Pokemon species
      if bst <= 200
        self.displayMsg("trade0", GameData::Species.get(give.species).real_name)
      elsif bst <= 300
        self.displayMsg("trade1", GameData::Species.get(give.species).real_name)
      elsif bst <= 400
        self.displayMsg("trade2", GameData::Species.get(give.species).real_name)
      elsif bst <= 500
        self.displayMsg("trade3", GameData::Species.get(give.species).real_name)
      elsif bst <= 600
        self.displayMsg("trade4", GameData::Species.get(give.species).real_name)
      else
        self.displayMsg("trade5", GameData::Species.get(give.species).real_name)
      end
      # protects trading from abuse
      $cachedTrade["#{giveBST}"] = recv
      # final confirmation for the trade
      if self.confirmMsg("propose", GameData::Species.get(recv).real_name, GameData::Species.get(give.species).real_name)
        $cachedTrade.delete("#{giveBST}")
        self.tradePoke(giveID, recv)
        self.displayMsg("accepttrade")
        self.saveCache
        return true
      else
        self.displayMsg("rejecttrade")
        self.saveCache
        return false
      end
    else
      # the trade was cancelled at the start
      return self.displayMsg("canceltrade")
    end
  end
  #-----------------------------------------------------------------------------
  # method used to calculate the base stat total of a Pokemon
  #-----------------------------------------------------------------------------
  def self.calcBST(species)
    pkmn = GameData::Species.get(species)
    stats = pkmn.base_stats
    bst = 0
    for stat in stats.keys
      bst += stats[stat]
    end
    echoln(pkmn.name + ": " + bst.to_s)
    return bst
  end
  #-----------------------------------------------------------------------------
  # get all species keys
  #-----------------------------------------------------------------------------
  def self.all_species
    keys = []
    GameData::Species.each do |species|
      next if !pbAllRegionalSpecies(0).include?(species.id) && !$game_switches[Settings::NEW_GAME_PLUS] # NG+
      next if [:SOLGALEO,:LUNALA].include?(species.id) && !$game_switches[12] # Post-game
      next if species.form != 0
      keys.push(species.id)
    end
    return keys
  end
  #-----------------------------------------------------------------------------
  # method used to return a list of Pokemon that would be of similar value to your Pokemon
  #-----------------------------------------------------------------------------
  def self.fetchEqualSpecies(poke, margin = 0.1)
    bst = self.calcBST(poke)
#    upper = bst*(1 + margin)
    upper = bst*(1 + 0)
    lower = bst*(1 - margin)
    potential = []
    for spec in self.all_species
      stats = self.calcBST(spec)
      potential.push(spec) if stats >= lower && stats <= upper && !TradeExpert::TRADING_BLACKLIST.include?(spec) && spec != poke && pbAllRegionalSpecies(0).include?(spec)
    end
    return potential
  end
  #-----------------------------------------------------------------------------
  # brings up the UI to select a Pokemon to give to the Trade Expert
  #-----------------------------------------------------------------------------
  def self.givePokemon
    # decides the parameters that determine Pokemon trade elegibility
    ableProc = proc{|poke| !poke.egg? && !poke.isShadow? && !TradeExpert::GIVING_BLACKLIST.include?(poke.species) && poke.owner.id != TradeExpert::TRAINER_ID}
    chosen = 0
    pbFadeOutIn(99999){
    scene = PokemonParty_Scene.new
    screen = PokemonPartyScreen.new(scene, $Trainer.party)
    if ableProc
      chosen = screen.pbChooseAblePokemon(ableProc, false)
    else
      screen.pbStartScene(_INTL("Choose a Pokémon."), false)
      chosen = screen.pbChoosePokemon
      screen.pbEndScene
    end
    }
    return (chosen >= 0 ? chosen : nil)
  end
  #-----------------------------------------------------------------------------
  # brings up the trading UI (you can further customize the traded Pokemon here)
  #-----------------------------------------------------------------------------
  def self.tradePoke(give, recv)
    myPokemon = $Trainer.party[give]
    # name of the Trade Expert is decided here
    opponent = NPCTrainer.new(TradeExpert::TRAINER_NAME, 0)
    # custom trainer ID of the Trade Expert
    opponent.id = TradeExpert::TRAINER_ID
    # generates the Pokemon
    yourPokemon = Pokemon.new(recv, myPokemon.level, opponent)
    # sets the Pokemon's mode to be traded
    yourPokemon.obtain_method = 2
    # handles moves
    yourPokemon.reset_moves
    yourPokemon.record_first_moves
    # registers Pokemon in the Pokedex
    $Trainer.pokedex.register(yourPokemon)
    $Trainer.pokedex.set_owned(recv)
#    pbSeenForm(yourPokemon)
    # starts trading sequence
    pbFadeOutInWithMusic(99999){
      evo = PokemonTrade_Scene.new
      evo.pbStartScreen(myPokemon, yourPokemon, $Trainer.name, opponent.name)
      evo.pbTrade
      evo.pbEndScreen
    }
    # sets traded Pokemon
    $Trainer.party[give] = yourPokemon
  end
  #-----------------------------------------------------------------------------
  # saves cached trades
  #-----------------------------------------------------------------------------
  def self.saveCache
    File.open(RTP.getSaveFileName("trexpert_reg"),"wb"){|f|
      Marshal.dump($cachedTrade,f)
    }
  end
  #-----------------------------------------------------------------------------
  # display message
  #-----------------------------------------------------------------------------
  def self.displayMsg(msg, *args)
    msg = TradeExpert::TEXT[msg]
    msg = [msg] if !msg.is_a?(Array)
    for m in msg
      pbMessage(_INTL(m, *args)) if m.is_a?(String)
    end
  end
  def self.confirmMsg(msg, *args)
    msg = TradeExpert::TEXT[msg]
    msg = [msg] if !msg.is_a?(Array)
    if msg.length > 1
      for m in msg[0...-1]
        pbMessage(_INTL(m, *args)) if m.is_a?(String)
      end
    end
    return pbConfirmMessage(_INTL(msg[-1], *args))
  end
  #-----------------------------------------------------------------------------
end
#-------------------------------------------------------------------------------
# Method used to call the Trade Expert in event
#-------------------------------------------------------------------------------
def tradeExpert(margin = 0.1)
  return TradeExpert.start(margin)
end
#-------------------------------------------------------------------------------
# Used so that the Player can't infinitely reject and get whatever new Pokemon
# before completing an old trade first
#-------------------------------------------------------------------------------
if safeExists?(RTP.getSaveFileName("trexpert"))
  File.delete(RTP.getSaveFileName("trexpert"))
end

if safeExists?(RTP.getSaveFileName("trexpert_reg"))
  File.open(RTP.getSaveFileName("trexpert_reg")){|f|
    $cachedTrade = Marshal.load(f)
  }
else
  $cachedTrade = {}
end