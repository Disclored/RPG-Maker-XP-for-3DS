#===============================================================================
# Wild Drop Items - By Vendily [v21]
#===============================================================================
# This script adds in Wild Drop Items, allowing for Wild Pokémon to drop
#  specially defined items in the PBS upon fainting.
# They do not have to be the same as the items Wild Pokémon might generate with.
#===============================================================================
# The script is plug and play, you just need to add in the PBS information
#  `WildDropCommon`, `WildDropUncommon`, and `WildDropRare`, used in the same
#  way as the `WildItem` version. (You don't have to define all three.)
# The base rate is `[50,5,1]`, but if all three properties are the same then it
#  is a 100% rate.
# Check the `def pbFaint` here if you wish to modify the mechanics further
#  or change the windowskin colour to dark mode.
#===============================================================================
class PokeBattle_Battler
  alias wild_drop_pbFaint pbFaint
  def pbFaint(showMessage = true)
    old_fainted = @fainted
    wild_drop_pbFaint(showMessage)
    # we don't drop items if no message will show
    return unless showMessage
    # must be a wild battle, and this is a wild mon
    return unless @battle.wildBattle? && opposes?
    # must be fainted, and it can't already have been fainted
    return unless @fainted && old_fainted != @fainted
    # we need a pokemon, and this can't be a non-standard battle
    return unless @pokemon && @battle.internalBattle
    item = nil
    item = @pokemon.item if @pokemon.hasItem?
    return unless item
    # if we have an item and sucessfully added it
    if item != nil && !$game_switches[Settings::NO_WILD_ITEM_DROPS]
      $PokemonBag.pbStoreItem(@pokemon.item,1)
      item_data = GameData::Item.get(item)
      itemname = item_data.real_name
      article = "a"
      article = "an" if itemname.starts_with_vowel?
      # change the false to true if your battle window skin is dark
      colour_tag = getSkinColor(nil, 1, true)
      pbMEPlay("Battle Item Get")
      pic = IconSprite.new(0, 0, @battle.scene.viewport)
      pic.setBitmap("Graphics/Items/#{item_data.id}")
      pic.x = (pic.bitmap.width) - 8
      pic.y = (Graphics.height - pic.bitmap.height) - 96
      pic.z = 99999
      @battle.pbDisplayPaused(_INTL("{1} dropped\n{4} {2}{3}</c3>!", pbThis, colour_tag, itemname, article))
      pic.dispose
    end
  end
end