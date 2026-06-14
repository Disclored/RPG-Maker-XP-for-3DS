#-------------------------------------------------------------------------------
# Voltseon's Pause Menu
# Pause with style 😎
#-------------------------------------------------------------------------------
#
# Original Script by Yankas
# Updated compatablilty by Cony
# Edited and modified by Voltseon, Golisopod User and ENLS
#
# Made for people who dont want
# to have ugly pause menus
# so here's a really cool one!
# Version: 1.7
#
#
#-------------------------------------------------------------------------------
# Menu Options
#-------------------------------------------------------------------------------
# Main file path for the menu
MENU_FILE_PATH = "Graphics/Pictures/Voltseon's Pause Menu/"

# An array of aLL the Menu Entry Classes from 005_VoltseonMenu_Entries that
# need to be loaded
MENU_ENTRIES = [
  "MenuEntryPokemon", "MenuEntryPokedex", "MenuEntryBag", "MenuEntryPokegear",
  "MenuEntryTrainer", "MenuEntryMap", "MenuEntryExitBugContest",
  "MenuEntryExitSafari", "MenuEntrySave", "MenuEntryDebug", "MenuEntryOptions"
]

# An array of aLL the Menu Component Classes from 004_VoltseonMenu_Components
# that need to be loaded
MENU_COMPONENTS = [
  "SafariHud", "BugContestHud", "PokemonPartyHud", "DateAndTimeHud", "NewQuestHud"
]

# The default theme for the menu screen
DEFAULT_MENU_THEME = 2

# Change Theme in the Options Menu
CHANGE_THEME_IN_OPTIONS = false

#-------------------------------------------------------------------------------
# Look and Feel
#-------------------------------------------------------------------------------
# Background options Tone (Red, Green, Blue, Grey) applied to the background/map.
BACKGROUND_TINT = [
            Color.new(255,-255,-255,128),
            Color.new(255,64,-255,128),
            Color.new(255,180,-255,128),
            Color.new(-255,255,-255,128),
            Color.new(-255,-255,255,128),
            Color.new(128,-255,200,128),
            Color.new(255,-255,64,128),
            Color.new(-30,-30,-30,128),
            Color.new(255,255,255,128)
          ]

MENU_THEMES = [
            ["arrow_left_0","arrow_right_0","bg_back_0","bg_hud_0","bg_location_0","player0"],
            ["arrow_left_1","arrow_right_1","bg_back_1","bg_hud_1","bg_location_1","player1"],
            ["arrow_left_2","arrow_right_2","bg_back_2","bg_hud_2","bg_location_2","player2"],
            ["arrow_left_3","arrow_right_3","bg_back_3","bg_hud_3","bg_location_3","player3"],
            ["arrow_left_4","arrow_right_4","bg_back_4","bg_hud_4","bg_location_4","player4"],
            ["arrow_left_5","arrow_right_5","bg_back_5","bg_hud_5","bg_location_5","player5"],
            ["arrow_left_6","arrow_right_6","bg_back_6","bg_hud_6","bg_location_6","player6"],
            ["arrow_left_7","arrow_right_7","bg_back_7","bg_hud_7","bg_location_7","player7"],
            ["arrow_left_8","arrow_right_8","bg_back_8","bg_hud_8","bg_location_8","player8"],
          ]

SHOW_MENU_NAMES = true # Whether or not the Menu option Names show on screen (true = show names)

# Icon options
ACTIVE_SCALE = 1.5

MENU_TEXTCOLOR = [
            Color.new(248,248,248)
          ]
MENU_TEXTOUTLINE = [
            Color.new(64,64,64)
          ]
LOCATION_TEXTCOLOR = [
            Color.new(248,248,248)
          ]
LOCATION_TEXTOUTLINE = [
            Color.new(64,64,64)
          ]

# Sound Options
MENU_OPEN_SOUND   = "GUI menu open"
MENU_CLOSE_SOUND  = "GUI menu close"
MENU_CURSOR_SOUND = "GUI sel cursor"
