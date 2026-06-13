#==============================================================================#
# Tileset Rearranger                                                           #
# Version 1.0                                                                  #
# By Maruno                                                                    #
#==============================================================================#
#                                                                              #
# This plugin allows you to rearrange tiles in a tileset. You can swap tiles   #
# (individual and sets thereof), clear tiles (individual and sets thereof),    #
# insert whole rows and delete whole rows.                                     #
#                                                                              #
# All instructions are shown on-screen, and are dynamic (they change depending #
# on what is possible at any given moment).                                    #
#                                                                              #
# Tiles that are in use by a map are indicated with a star. This takes into    #
# account all tiles covered by an event with a name of "size(x,y)" - all       #
# covered tiles will be marked as used. You cannot by any means delete a tile  #
# which is in use by a map.                                                    #
#                                                                              #
# When saving, the following are affected:                                     #
#                                                                              #
#   * Tileset data (Tilesets.rxdata).                                          #
#   * Map files (Map###.rxdata) that use the amended tileset - alters tiles    #
#     used and event graphics if they use a tile from the tileset.             #
#   * Tileset graphic in Graphics/Tilesets/.                                   #
#                                                                              #
# For each of these, a backup is first created (with "_backup" appended to the #
# filename). A new backup will overwrite an existing backup file. There is no  #
# built-in way to restore from a backup, so you will have to rename the files  #
# manually.                                                                    #
#                                                                              #
# A tileset may be loaded and rearranged if its graphic is taller than the GPU #
# cache size limit (Bitmap.max_size), but it cannot be saved until it is       #
# shrunk down to this limit or smaller. The current height and maximum allowed #
# height are shown in the editor.                                              #
#                                                                              #
#==============================================================================#

class TilesetRearranger
  # Fundamental values, don't change these
  TILE_SIZE            = Game_Map::TILE_WIDTH
  TILES_PER_ROW        = 8
  TILESET_WIDTH        = TILES_PER_ROW * TILE_SIZE
  TILES_PER_AUTOTILE   = 48
  TILESET_START_ID     = TILES_PER_ROW * TILES_PER_AUTOTILE
  MAX_TILESET_ROWS     = Bitmap.max_size / TILE_SIZE   # Number of tiles vertically
  # Size and positioning of screen/elements
  SCREEN_WIDTH         = TILESET_WIDTH * 2 + 128
  SCREEN_HEIGHT        = Settings::SCREEN_HEIGHT * 2
  TILESET_OFFSET_X     = 32    # To allow cursor to fit on the left for row insertion
  TILESET_OFFSET_Y     = 0     # For the sake of it
  NUM_ROWS_VISIBLE     = (SCREEN_HEIGHT - TILESET_OFFSET_Y) / TILE_SIZE
  # Other numbers
  MAX_SELECTION_HEIGHT = 14    # For swapping tiles only
  HISTORY_LENGTH       = 100   # Arbitrary value, probably big enough
  # Colors
  CURSOR_COLOR         = Color.new(224, 0, 0)       # Red (current cursor)
  SELECTION_COLOR      = Color.new(0, 104, 224)     # Blue (previously selected tiles)
  BLANK_TILE_BG_COLOR  = Color.new(255, 255, 255)   # White background
  BLANK_TILE_X_COLOR   = Color.new(255, 0, 0)       # Red [X] on top

  def initialize
    @tilesets_data = load_data("Data/Tilesets.rxdata")
    @mode = 0   # 0 = swap tiles, 1 = clear tiles, 2 = insert row, 3 = delete row
    @height = 0
    reset_positionings
    clear_history
    initialize_tileset_allocation_info
    @viewport = Viewport.new(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT)
    @viewport.z = 99999
    @sprites = {}
    @sprites["title"] = Window_UnformattedTextPokemon.newWithSize("",
       TILESET_WIDTH + TILESET_OFFSET_X, 0, SCREEN_WIDTH - TILESET_WIDTH - TILESET_OFFSET_X, 128, @viewport)
    @sprites["help_text"] = Window_UnformattedTextPokemon.newWithSize(_INTL("Choose tileset to load"),
       TILESET_WIDTH + TILESET_OFFSET_X, SCREEN_HEIGHT - 64, SCREEN_WIDTH - TILESET_WIDTH - TILESET_OFFSET_X, 64, @viewport)
    @sprites["tileset"] = BitmapSprite.new(TILESET_WIDTH, NUM_ROWS_VISIBLE * TILE_SIZE, @viewport)
    @sprites["tileset"].x = TILESET_OFFSET_X
    @sprites["tileset"].y = TILESET_OFFSET_Y
    @sprites["selection"] = BitmapSprite.new(TILESET_WIDTH + 2, MAX_SELECTION_HEIGHT * TILE_SIZE + 2, @viewport)
    @sprites["selection"].x = SCREEN_WIDTH - TILESET_OFFSET_X - @sprites["selection"].bitmap.width + 1
    @sprites["selection"].y = (SCREEN_HEIGHT - @sprites["selection"].bitmap.height) / 2
    @sprites["cursor"] = BitmapSprite.new(SCREEN_WIDTH, SCREEN_HEIGHT, @viewport)
    pbSetSystemFont(@sprites["cursor"].bitmap)
    @sprites["overlay"] = BitmapSprite.new(SCREEN_WIDTH, SCREEN_HEIGHT, @viewport)
    pbSetSystemFont(@sprites["overlay"].bitmap)
    initialize_bitmaps
    draw_title_text
    clear_selection
  end

  # Goes through all maps to find which tileset each uses. Also gets map names.
  def initialize_tileset_allocation_info
    @tilesets_usage = []
    @map_names = []
    map_infos = pbLoadMapInfos
    for id in map_infos.keys
      next if !map_infos[id]
      map = load_data(sprintf("Data/Map%03d.rxdata", id))
      @tilesets_usage[map.tileset_id] = [] if !@tilesets_usage[map.tileset_id]
      @tilesets_usage[map.tileset_id].push(id)
      @map_names[id] = map_infos[id].name
    end
  end

  # Generates graphics used only in this editor.
  def initialize_bitmaps
    # Star bitmap (indicates tiles used by a map)
    @star_bitmap = BitmapWrapper.new(TILE_SIZE, TILE_SIZE)
    star_width = 13
    star = [
      0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 1, 2, 2, 2, 1, 0, 0, 0, 0,
      1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1,
      1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1,
      0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0,
      0, 0, 1, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0,
      0, 0, 0, 1, 2, 2, 2, 2, 2, 1, 0, 0, 0,
      0, 0, 1, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0,
      0, 0, 1, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0,
      0, 1, 2, 2, 2, 1, 1, 1, 2, 2, 2, 1, 0,
      0, 1, 2, 1, 1, 0, 0, 0, 1, 1, 2, 1, 0,
      0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0
    ]
    x_offset = (TILE_SIZE - star_width) / 2
    y_offset = (TILE_SIZE - (star.length / star_width)) / 2
    star.each_with_index do |val, i|
      next if val == 0
      color = Color.new(255 * (val - 1), 255 * (val - 1), 255 * (val - 1))
      @star_bitmap.fill_rect(x_offset + i % star_width, y_offset + i / star_width, 1, 1, color)
    end
    # Blank tile bitmap (used for cleared tiles: white with red [X])
    @blank_tile_bitmap = BitmapWrapper.new(TILE_SIZE, TILE_SIZE)
    @blank_tile_bitmap.fill_rect(0, 0, TILE_SIZE, TILE_SIZE, BLANK_TILE_BG_COLOR)
    @blank_tile_bitmap.fill_rect(0,             0,             TILE_SIZE, 1, BLANK_TILE_X_COLOR)
    @blank_tile_bitmap.fill_rect(0,             0,             1, TILE_SIZE, BLANK_TILE_X_COLOR)
    @blank_tile_bitmap.fill_rect(0,             TILE_SIZE - 1, TILE_SIZE, 1, BLANK_TILE_X_COLOR)
    @blank_tile_bitmap.fill_rect(TILE_SIZE - 1, 0,             1, TILE_SIZE, BLANK_TILE_X_COLOR)
    for i in 0...TILE_SIZE
      @blank_tile_bitmap.fill_rect(i, i,                 1, 1, BLANK_TILE_X_COLOR)
      @blank_tile_bitmap.fill_rect(i, TILE_SIZE - i - 1, 1, 1, BLANK_TILE_X_COLOR)
    end
    # Arrow cursor (for mode 2)
    @arrow_bitmap = BitmapWrapper.new(26, 26)
    arrow = [
      0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 1, 2, 2, 1, 1, 0, 0, 0,
      0, 0, 0, 0, 0, 1, 2, 2, 2, 1, 1, 0, 0,
      0, 0, 0, 0, 0, 1, 2, 2, 2, 2, 1, 1, 0,
      1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 1, 1,
      1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1,
      1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1,
      1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 1, 1,
      0, 0, 0, 0, 0, 1, 2, 2, 2, 2, 1, 1, 0,
      0, 0, 0, 0, 0, 1, 2, 2, 2, 1, 1, 0, 0,
      0, 0, 0, 0, 0, 1, 2, 2, 1, 1, 0, 0, 0,
      0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0
    ]
    white_color = Color.new(255, 255, 255)
    arrow.each_with_index do |val, i|
      next if val == 0
      color = (val == 2) ? white_color : CURSOR_COLOR
      @arrow_bitmap.fill_rect(
         (i % (@arrow_bitmap.width / 2)) * 2, (i / (@arrow_bitmap.width / 2)) * 2, 2, 2, color)
    end
  end

  #-----------------------------------------------------------------------------

  def draw_tile_onto_bitmap(bitmap, tile_x, tile_y, tile_id, background = false)
    if tile_id < 0
      bitmap.blt(tile_x, tile_y, @blank_tile_bitmap, Rect.new(0, 0, TILE_SIZE, TILE_SIZE))
    else
      bitmap.fill_rect(tile_x, tile_y, TILE_SIZE, TILE_SIZE, Color.new(255, 0, 255)) if background
      @tilehelper.bltTile(bitmap, tile_x, tile_y, tile_id)
    end
  end

  # Draws the tileset on the left.
  def draw_tileset
    @sprites["tileset"].bitmap.clear
    for yy in 0...NUM_ROWS_VISIBLE
      id_y_offset = (@top_y + yy) * TILES_PER_ROW
      for xx in 0...TILES_PER_ROW
        id = @tile_ID_map[id_y_offset + xx]
        break if !id
        tile_id = (id < 0) ? id : TILESET_START_ID + id
        draw_tile_onto_bitmap(@sprites["tileset"].bitmap, xx * TILE_SIZE, yy * TILE_SIZE, tile_id, true)
      end
    end
    draw_tileset_overlay
  end

  # Draws "used tile" icon over tiles in use by a map.
  def draw_tileset_overlay
    star_rect = Rect.new(0, 0, TILE_SIZE, TILE_SIZE)
    for yy in 0...NUM_ROWS_VISIBLE
      id_y_offset = (@top_y + yy) * TILES_PER_ROW
      for xx in 0...TILES_PER_ROW
        id = @tile_ID_map[id_y_offset + xx]
        next if !id || id < 0
        next if !@used_ids[TILESET_START_ID + id]
        @sprites["tileset"].bitmap.blt(xx * TILE_SIZE, yy * TILE_SIZE, @star_bitmap, star_rect)
      end
    end
  end

  # Draws the cursor over the tileset. Also draws the selection area if there is one.
  def draw_cursor
    bitmap = @sprites["cursor"].bitmap
    bitmap.clear
    case @mode
    when 0, 1   # Swap tiles, clear tiles
      # Draw pre-selected area
      if @selected_width > 0
        pre_x = @selected_x * TILE_SIZE + TILESET_OFFSET_X
        pre_y = (@selected_y - @top_y) * TILE_SIZE + TILESET_OFFSET_Y
        pre_width = @selected_width * TILE_SIZE
        pre_height = @selected_height * TILE_SIZE
        bitmap.fill_rect(pre_x,                 pre_y,                  pre_width, 4,  SELECTION_COLOR)
        bitmap.fill_rect(pre_x,                 pre_y,                  4, pre_height, SELECTION_COLOR)
        bitmap.fill_rect(pre_x,                 pre_y + pre_height - 4, pre_width, 4,  SELECTION_COLOR)
        bitmap.fill_rect(pre_x + pre_width - 4, pre_y,                  4, pre_height, SELECTION_COLOR)
      end
      # Draw cursor
      cursor_top_left_x = @x
      cursor_top_left_y = @y
      cursor_width = cursor_height = 1
      if @selected_x >= 0
        if @selected_width > 0
          cursor_width = @selected_width
          cursor_height = @selected_height
        else
          cursor_top_left_x = [@x, @selected_x].min
          cursor_top_left_y = [@y, @selected_y].min
          cursor_width = [@x, @selected_x].max - [@x, @selected_x].min + 1
          cursor_height = [@y, @selected_y].max - [@y, @selected_y].min + 1
        end
      end
      cursor_x = cursor_top_left_x * TILE_SIZE + TILESET_OFFSET_X
      cursor_y = (cursor_top_left_y - @top_y) * TILE_SIZE + TILESET_OFFSET_Y
      cursor_width = cursor_width * TILE_SIZE
      cursor_height = cursor_height * TILE_SIZE
      bitmap.fill_rect(cursor_x,                    cursor_y,                     cursor_width, 4,  CURSOR_COLOR)
      bitmap.fill_rect(cursor_x,                    cursor_y,                     4, cursor_height, CURSOR_COLOR)
      bitmap.fill_rect(cursor_x,                    cursor_y + cursor_height - 4, cursor_width, 4,  CURSOR_COLOR)
      bitmap.fill_rect(cursor_x + cursor_width - 4, cursor_y,                     4, cursor_height, CURSOR_COLOR)
    when 2   # Insert row
      cursor_y = TILESET_OFFSET_Y + (@y - @top_y) * TILE_SIZE
      bitmap.blt(4, cursor_y - 12, @arrow_bitmap, Rect.new(0, 0, @arrow_bitmap.width, @arrow_bitmap.height))
      bitmap.fill_rect(TILESET_OFFSET_X, cursor_y - 2, TILES_PER_ROW * TILE_SIZE, 4,  CURSOR_COLOR)
    when 3   # Delete row
      cursor_x = TILESET_OFFSET_X
      cursor_y = (@y - @top_y) * TILE_SIZE + TILESET_OFFSET_Y
      cursor_width = TILES_PER_ROW * TILE_SIZE
      cursor_height = TILE_SIZE
      bitmap.fill_rect(cursor_x,                    cursor_y,                     cursor_width, 4,  CURSOR_COLOR)
      bitmap.fill_rect(cursor_x,                    cursor_y,                     4, cursor_height, CURSOR_COLOR)
      bitmap.fill_rect(cursor_x,                    cursor_y + cursor_height - 4, cursor_width, 4,  CURSOR_COLOR)
      bitmap.fill_rect(cursor_x + cursor_width - 4, cursor_y,                     4, cursor_height, CURSOR_COLOR)
    end
  end

  # Draws copy of selected tiles on the right.
  def draw_tile_selection
    bitmap = @sprites["selection"].bitmap
    bitmap.clear
    return if @mode != 0 || @selected_width == 0
    # Draw selected tiles
    tile_rect = Rect.new(0, 0, TILE_SIZE, TILE_SIZE)
    start_x = (bitmap.width - @selected_width * TILE_SIZE) / 2
    start_y = (bitmap.height - @selected_height * TILE_SIZE) / 2
    for yy in 0...@selected_height
      id_y_offset = (@selected_y + yy) * TILES_PER_ROW
      for xx in 0...@selected_width
        id = @tile_ID_map[id_y_offset + @selected_x + xx]
        break if !id
        tile_id = (id < 0) ? id : TILESET_START_ID + id
        draw_tile_onto_bitmap(bitmap, xx * TILE_SIZE + start_x, yy * TILE_SIZE + start_y, tile_id, true)
      end
    end
    # Draw white box around selected tiles
    outline_width = @selected_width * TILE_SIZE
    outline_height = @selected_height * TILE_SIZE
    bitmap.fill_rect(start_x - 1,             start_y - 1,              outline_width + 2, 1,  Color.new(255, 255, 255))
    bitmap.fill_rect(start_x - 1,             start_y - 1,              1, outline_height + 2, Color.new(255, 255, 255))
    bitmap.fill_rect(start_x - 1,             start_y + outline_height, outline_width + 2, 1,  Color.new(255, 255, 255))
    bitmap.fill_rect(start_x + outline_width, start_y - 1,              1, outline_height + 2, Color.new(255, 255, 255))
  end

  # Sets the text in the top window (informational)
  def draw_title_text
    text = _INTL("Tileset Rearranger")
    text += "\r\n"
    case @mode
    when 0
      text += _INTL("Mode: Swap tiles")
    when 1
      text += _INTL("Mode: Clear unused tiles")
    when 2
      text += _INTL("Mode: Insert row")
    when 3
      text += _INTL("Mode: Delete row")
    end
    if @height > 0
      text += "\r\n"
      if @height > MAX_TILESET_ROWS
        text += _INTL("Height: {1}/{2} rows [!]", @height, MAX_TILESET_ROWS)
      else
        text += _INTL("Height: {1}/{2} rows", @height, MAX_TILESET_ROWS)
      end
    end
    @sprites["title"].text = text
  end

  # Sets the text in the bottom window (controls)
  def draw_help_text
    text = []
    case @mode
    when 0   # Swap tiles
      if @selected_x >= 0
        if @selected_width > 0
          text.push(_INTL("C: Swap tiles")) if !areas_overlap?
          text.push(_INTL("X: Cancel tile swap"))
        else
          text.push(_INTL("ARROWS: Select multiple tiles"))
          text.push(_INTL("RELEASE C: Finish tile selection"))
        end
      else
        text.push(_INTL("C: Select tile"))
        text.push(_INTL("HOLD C: Select multiple tiles"))
      end
    when 1   # Clear tiles
      if @selected_x >= 0
        text.push(_INTL("ARROWS: Select multiple tiles"))
        text.push(_INTL("RELEASE C: Clear tiles"))
      else
        text.push(_INTL("C: Clear tile"))
        text.push(_INTL("HOLD C: Clear multiple tiles"))
      end
    when 2   # Insert row
      text.push(_INTL("C: Insert row of tiles"))
    when 3   # Delete row
      text.push(_INTL("C: Delete row of tiles")) if @height > 1
    end
    text.push(_INTL("A/S: Jump up/down tileset"))
    if @mode >= 2 || @selected_x < 0
      text.push(_INTL("Z: Change mode"))
      text.push(_INTL("D: Open menu"))
    end
    if @history.length > 0
      if @future_history.length > 0
        text.push(_INTL("Q: Undo ({1}) - W: Redo ({2})", @history.length, @future_history.length))
      else
        text.push(_INTL("Q: Undo ({1})", @history.length))
      end
    elsif @future_history.length > 0
      text.push(_INTL("W: Redo ({1})", @future_history.length))
    end
    text_string = (text.length == 0) ? "" : text.join("\r\n")
    @sprites["help_text"].height = (text.length + 1) * 32
    @sprites["help_text"].text = text_string
    pbBottomRight(@sprites["help_text"])
  end

  #-----------------------------------------------------------------------------

  def choose_tileset
    commands = []
    for i in 1...@tilesets_data.length
      commands.push(sprintf("%03d %s", i, @tilesets_data[i].name))
    end
    ret = pbShowCommands(nil, commands, -1)
    return false if ret < 0
    load_tileset(ret + 1)
    return true
  end

  def open_menu
    commands = [
       _INTL("Go to bottom"),
       _INTL("Go to top"),
       _INTL("Clear all unused tiles"),
       _INTL("Delete all unused rows"),
       _INTL("List maps using this tileset"),
       _INTL("Change tileset"),
       _INTL("Cancel")
    ]
    case pbShowCommands(nil, commands, -1)
    when 0   # Go to bottom
      update_cursor_position(0, @height)
    when 1   # Go to top
      update_cursor_position(0, -@height)
    when 2   # Clear all unused tiles
      add_to_history
      clear_unused_tiles_in_area(0, 0, TILES_PER_ROW, @height)
      draw_help_text
    when 3   # Delete all unused rows
      add_to_history
      row = 0
      did_something = false
      loop do
        break if row >= @height
        used = false
        for i in 0...TILES_PER_ROW
          tile_id = @tile_ID_map[row * TILES_PER_ROW + i]
          next if !tile_id || tile_id < 0 || !@used_ids[TILESET_START_ID + tile_id]
          used = true
          break
        end
        if used
          row += 1
          next
        end
        did_something = true
        # Row is empty, delete it
		if @height > 1
          TILES_PER_ROW.times { @tile_ID_map.delete_at(row * TILES_PER_ROW) }
          @height -= 1
		else
		  clear_unused_tiles_in_area(0, 0, TILES_PER_ROW, @height)
		  break
		end
      end
      if did_something
        ensure_cursor_and_tileset_on_screen
      else
        @history.pop
      end
      draw_tileset
      draw_cursor
      draw_title_text
      draw_help_text
    when 4   # List maps using this tileset
	  if @tilesets_usage[@tileset_id] && @tilesets_usage[@tileset_id].length > 0
	    map_names = []
        @tilesets_usage[@tileset_id].each do |map_id|
          map_names.push(sprintf("%03d: %s", map_id, @map_names[map_id]))
        end
        map_names.sort!
        map_names.insert(0, _ISPRINTF("Maps using tileset {1:03d}: {2:s}:", @tileset_id, @tilesets_data[@tileset_id].name))
        pbShowCommands(nil, map_names, -1)
	  else
        pbMessage(_ISPRINTF("No maps use tileset {1:03d}: {2:s}.", @tileset_id, @tilesets_data[@tileset_id].name))
	  end
    when 5   # Change tileset
      choose_tileset
    end
  end

  #-----------------------------------------------------------------------------

  def load_tileset(id)
    new_tileset_data = @tilesets_data[id]
    new_tileset_rows = (new_tileset_data.terrain_tags.xsize - TILESET_START_ID) / TILES_PER_ROW
    @tileset_id = id
    @tileset_data = new_tileset_data
    @tilehelper.dispose if @tilehelper
    @tilehelper = TileDrawingHelper.fromTileset(@tileset_data)
    reset_positionings
    @height = new_tileset_rows
    @tile_ID_map = Array.new(new_tileset_rows * TILES_PER_ROW) { |i| i }
    find_used_tiles_in_tileset
    clear_history
    clear_selection
    draw_tileset
    draw_title_text
    draw_help_text
  end

  def find_used_tiles_in_tileset
    @used_ids = []
    return if !@tilesets_usage[@tileset_id]
    @tilesets_usage[@tileset_id].each do |id|
      map = load_data(sprintf("Data/Map%03d.rxdata", id))
      # Go through all map tiles
      for y in 0...map.height
        for x in 0...map.width
          for z in 0...3
            tile_id = map.data[x, y, z]
            tile_id = 0 if !tile_id
            @used_ids[tile_id] = true
          end
        end
      end
      # Look through events for tile usage (takes into account events with size)
      map.events.each_value do |event|
        next if !event
        # Get the event's size
        event_width = event_height = 1
        if event.name[/size\((\d+),(\d+)\)/i]
          event_width = $~[1].to_i
          event_height = $~[2].to_i
        end
        # Go through each page of the event in turn to check their graphics
        event.pages.each do |page|
          next if page.graphic.tile_id <= 0
          start_x = (page.graphic.tile_id - TILESET_START_ID) % TILES_PER_ROW
          start_y = (page.graphic.tile_id - TILESET_START_ID) / TILES_PER_ROW
          for yy in 0...event_height
            break if start_y - yy < 0
            for xx in 0...event_width
              next if start_x + xx >= TILES_PER_ROW
              @used_ids[page.graphic.tile_id - yy * TILES_PER_ROW + xx] = true
            end
          end
        end
      end
    end
  end

  #-----------------------------------------------------------------------------

  def save_tileset
    if @height > MAX_TILESET_ROWS
      pbMessage(_INTL("This tileset is too tall ({1} rows) and cannot be saved. Please shrink it to at most {2} rows tall.",
         @height, MAX_TILESET_ROWS))
      return
    end
    # Determine height of tileset (trim off blank rows at bottom)
    tileset_height = @height
    loop do
      used = false
      for i in 0...TILES_PER_ROW
        next if @tile_ID_map[tileset_height * TILES_PER_ROW - 1 - i] == -1
        used = true
        break
      end
      break if used
      tileset_height -= 1
    end
	tileset_height = 1 if tileset_height < 1
    # Generate and save new tileset graphic
    save_tileset_graphic(tileset_height, @tileset_data.tileset_name)
    # Modify data of all maps using this tileset to reflect changes to tile IDs
    save_map_tile_data
    # Modify tileset data to reflect changes to tile IDs
    save_tileset_data(tileset_height)
    # Reload tilesets data, refresh map to account for changes
    $data_tilesets = @tilesets_data
    if $game_map && $MapFactory
      $MapFactory.setup($game_map.map_id)
      $game_player.center($game_player.x, $game_player.y)
      if $scene.is_a?(Scene_Map)
        $scene.disposeSpritesets
        $scene.createSpritesets
      end
    end
    load_tileset(@tileset_id)
    pbMessage(_INTL("Changes saved. To ensure that they are applied properly, close and reopen RPG Maker XP."))
  end

  # Generate and save new tileset graphic.
  def save_tileset_graphic(height, filename)
    bitmap = Bitmap.new(TILESET_WIDTH, height * TILE_SIZE)
    tile_rect = Rect.new(0, 0, TILE_SIZE, TILE_SIZE)
    for yy in 0...height
      for xx in 0...TILES_PER_ROW
        tile_id = @tile_ID_map[yy * TILES_PER_ROW + xx] || -1
        tile_id += TILESET_START_ID if tile_id >= 0
        draw_tile_onto_bitmap(bitmap, xx * TILE_SIZE, yy * TILE_SIZE, tile_id)
      end
    end
    @tilehelper.tileset.to_file("Graphics/Tilesets/" + filename + "_backup.png")
    bitmap.to_file("Graphics/Tilesets/" + filename + ".png")
  end

  # Modify data of all maps using this tileset to reflect changes to tile IDs.
  def save_map_tile_data
    return if !@tilesets_usage[@tileset_id]
    @tilesets_usage[@tileset_id].each do |id|
      filename = sprintf("Data/Map%03d.rxdata", id)
      map = load_data(filename)
      save_data(map, sprintf("Data/Map%03d_backup.rxdata", id))
      # Go through all tiles
      for y in 0...map.height
        for x in 0...map.width
          for z in 0...3
            old_tile_id = map.data[x, y, z] || 0
            if old_tile_id < TILESET_START_ID
              map.data[x, y, z] = old_tile_id   # Just in case it's nil somehow
            else
              map.data[x, y, z] = @tile_ID_map.index(old_tile_id - TILESET_START_ID) + TILESET_START_ID
            end
          end
        end
      end
      # Change tile usage in events
      map.events.each_value do |event|
        next if !event
        event.pages.each do |page|
          next if page.graphic.tile_id <= 0
          page.graphic.tile_id = @tile_ID_map.index(page.graphic.tile_id - TILESET_START_ID) + TILESET_START_ID
        end
      end
      save_data(map, filename)
    end
  end

  # Modify tileset data to reflect changes to tile IDs.
  def save_tileset_data(height)
    save_data(@tilesets_data, "Data/Tilesets_backup.rxdata")
    new_passages = Table.new(TILESET_START_ID + height * TILES_PER_ROW)
    new_priorities = Table.new(TILESET_START_ID + height * TILES_PER_ROW)
    new_terrain_tags = Table.new(TILESET_START_ID + height * TILES_PER_ROW)
    # Fill in table data
    for i in 0...TILESET_START_ID + height * TILES_PER_ROW
      old_id = i
      if i >= TILESET_START_ID
        old_id = @tile_ID_map[i - TILESET_START_ID] || -1
        old_id += TILESET_START_ID if old_id >= 0
      end
      if old_id == -1
        new_passages[i] = 0
        new_priorities[i] = 0
        new_terrain_tags[i] = 0
      else
        new_passages[i] = @tileset_data.passages[old_id] || 0
        new_priorities[i] = @tileset_data.priorities[old_id] || 0
        new_terrain_tags[i] = @tileset_data.terrain_tags[old_id] || 0
      end
    end
    # Apply new tileset data
    @tileset_data.passages = new_passages
    @tileset_data.priorities = new_priorities
    @tileset_data.terrain_tags = new_terrain_tags
    # Save tileset data
    save_data(@tilesets_data, "Data/Tilesets.rxdata")
  end

  #-----------------------------------------------------------------------------

  def areas_overlap?
    return false if @mode != 0 || @selected_x < 0 || @selected_width == 0
    return false if @x + @selected_width <= @selected_x
    return false if @selected_x + @selected_width <= @x
    return false if @y + @selected_height <= @selected_y
    return false if @selected_y + @selected_height <= @y
    return true
  end

  def reset_positionings
    @x = 0
    @y = 0
    @top_y = 0
  end

  def clear_selection
    @selected_x = -1
    @selected_y = -1
    @selected_width = 0
    @selected_height = 0
    draw_cursor
    draw_tile_selection
  end

  def clear_history
    @history = []
    @future_history = []
  end

  def add_to_history(by_redo = false)
    @history.push([@tile_ID_map.clone, @height])
    @history.shift if @history.length > HISTORY_LENGTH
    @future_history.clear if !by_redo
  end

  def pop_from_history
    return if @history.length == 0
    add_to_future_history
    last_state = @history.pop
    @tile_ID_map = last_state[0]
    @height = last_state[1]
    clear_selection
    draw_tileset
    draw_title_text
    draw_help_text
  end

  def add_to_future_history
    @future_history.push([@tile_ID_map.clone, @height])
  end

  def pop_from_future_history
    return if @future_history.length == 0
    add_to_history(true)
    last_state = @future_history.pop
    @tile_ID_map = last_state[0]
    @height = last_state[1]
    clear_selection
    draw_tileset
    draw_title_text
    draw_help_text
  end

  def clear_unused_tiles_in_area(start_x, start_y, width, height)
    ret = true
    for yy in 0...height
      y_offset = (start_y + yy) * TILES_PER_ROW
      for xx in 0...width
        position_id = y_offset + start_x + xx
        tile_id = @tile_ID_map[position_id]
        if tile_id && tile_id > 0 && @used_ids[TILESET_START_ID + tile_id]
          ret = false
          next
        end
        @tile_ID_map[position_id] = -1
      end
    end
    draw_tileset
    return ret
  end

  #-----------------------------------------------------------------------------

  def ensure_cursor_and_tileset_on_screen
    # Ensure cursor position is within tileset
    @y = @y.clamp(0, @height - 1)
    # Ensure cursor is on-screen
    @top_y = @y if @top_y > @y
    @top_y = @y - NUM_ROWS_VISIBLE + 1 if @y >= @top_y + NUM_ROWS_VISIBLE
    # Ensure tileset touches the bottom of the screen
    @top_y = @height - NUM_ROWS_VISIBLE if @height < @top_y + NUM_ROWS_VISIBLE
    # Ensure tileset touches the top of the screen
    @top_y = 0 if @top_y < 0
  end

  def update_cursor_position(x_offset, y_offset)
    old_x = @x
    old_y = @y
    old_top_y = @top_y
    if x_offset != 0
      @x += x_offset
      @x = @x.clamp(0, TILES_PER_ROW - 1)
      if @mode == 0 && @selected_width > 0
        @x = @x.clamp(0, TILES_PER_ROW - @selected_width)
      end
    end
    if y_offset != 0
      @y += y_offset
      if @mode == 0
        if @selected_height > 0
          @y = @y.clamp(0, @height - @selected_height)
        elsif @selected_x >= 0
          @y = @y.clamp(@selected_y - MAX_SELECTION_HEIGHT + 1, @selected_y + MAX_SELECTION_HEIGHT - 1)
        end
      end
      ensure_cursor_and_tileset_on_screen
    end
    draw_tileset if @top_y != old_top_y
    if @x != old_x || @y != old_y
      draw_cursor
      draw_help_text
    end
  end

  def update
    if Input.repeat?(Input::UP)
      update_cursor_position(0, -1)
    elsif Input.repeat?(Input::DOWN)
      update_cursor_position(0, 1)
    elsif Input.repeat?(Input::LEFT) && @mode < 2
      update_cursor_position(-1, 0)
    elsif Input.repeat?(Input::RIGHT) && @mode < 2
      update_cursor_position(1, 0)
    elsif Input.repeat?(Input::JUMPUP)
      update_cursor_position(0, -NUM_ROWS_VISIBLE)
    elsif Input.repeat?(Input::JUMPDOWN)
      update_cursor_position(0, NUM_ROWS_VISIBLE)
    elsif Input.trigger?(Input::AUX1)   # Undo
      pop_from_history
    elsif Input.trigger?(Input::AUX2)   # Redo
      pop_from_future_history
    elsif Input.trigger?(Input::BACK)   # X: Cancel
      if @mode == 0 && @selected_width > 0
        clear_selection
        draw_help_text
      elsif @mode >= 2 || @selected_x < 0
        save_tileset if pbConfirmMessageSerious(_INTL("Save changes?"))
        return false if pbConfirmMessage(_INTL("Exit from the editor?"))
      end
    elsif Input.trigger?(Input::ACTION)   # Z: Toggle mode
      if @mode >= 2 || @selected_x < 0   # Can't toggle mode while selecting tiles in modes 0/1
        @mode += 1
        @mode = @mode % 4
        draw_title_text
        draw_cursor
        draw_help_text
      end
    elsif Input.trigger?(Input::SPECIAL)   # D: Open menu
      open_menu if @mode >= 2 || @selected_x < 0
    else
      case @mode
      when 0, 1   # Swap tiles, clear tiles
        if @selected_x < 0   # Start selecting tiles
          if Input.trigger?(Input::USE)
            @selected_x = @x
            @selected_y = @y
            draw_help_text
          end
        elsif @selected_width == 0   # Finish selecting tiles
          if !Input.press?(Input::USE)
            sel_x = [@x, @selected_x].min
            @selected_width = [@x, @selected_x].max - sel_x + 1
            @selected_x = sel_x
            sel_y = [@y, @selected_y].min
            @selected_height = [@y, @selected_y].max - sel_y + 1
            @selected_y = sel_y
            if @mode == 0   # Swap tiles (finish selection)
              # Reposition cursor
              @x = @selected_x
              @y = @selected_y
              draw_cursor
              draw_tile_selection
            elsif @mode == 1   # Clear tiles
              add_to_history
              clear_unused_tiles_in_area(@selected_x, @selected_y, @selected_width, @selected_height)
              clear_selection
              draw_tileset
            end
            draw_help_text
          end
        else   # Swap tiles (mode 0 only)
          if Input.trigger?(Input::USE) && !areas_overlap?
            add_to_history
            for j in 0...@selected_height
              for i in 0...@selected_width
                offset = j * TILES_PER_ROW + i
                first_idx = @y * TILES_PER_ROW + @x + offset
                second_idx = @selected_y * TILES_PER_ROW + @selected_x + offset
                @tile_ID_map[first_idx], @tile_ID_map[second_idx] = @tile_ID_map[second_idx], @tile_ID_map[first_idx]
              end
            end
            clear_selection
            draw_tileset
            draw_help_text
          end
        end
      when 2   # Insert row
        if Input.trigger?(Input::USE)
          add_to_history
          TILES_PER_ROW.times { @tile_ID_map.insert(@y * TILES_PER_ROW, -1) }
          @height += 1
          draw_tileset
          draw_cursor
          draw_title_text
          draw_help_text
        end
      when 3   # Delete row
        if Input.trigger?(Input::USE) && @height > 1
          add_to_history
          if clear_unused_tiles_in_area(0, @y, TILES_PER_ROW, 1)
            TILES_PER_ROW.times { @tile_ID_map.delete_at(@y * TILES_PER_ROW) }
            @height -= 1
            ensure_cursor_and_tileset_on_screen
          end
          draw_tileset
          draw_cursor
          draw_title_text
          draw_help_text
        end
      end
    end
    return true
  end

  def open_screen
    Graphics.resize_screen(SCREEN_WIDTH, SCREEN_HEIGHT)
    pbSetResizeFactor(1)
    return choose_tileset
  end

  def close_screen
    pbDisposeSpriteHash(@sprites)
    @star_bitmap.dispose
    @blank_tile_bitmap.dispose
    @arrow_bitmap.dispose
    @viewport.dispose
    @tilehelper.dispose if @tilehelper
    Graphics.resize_screen(Settings::SCREEN_WIDTH, Settings::SCREEN_HEIGHT)
    pbSetResizeFactor($PokemonSystem.screensize)
  end

  def main
    if open_screen
      loop do
        Graphics.update
        Input.update
        break if !update
      end
    end
    close_screen
  end
end

#===============================================================================
#
#===============================================================================
DebugMenuCommands.register("tileset_rearranger", {
  "parent"      => "editorsmenu",
  "name"        => _INTL("Tileset Rearranger"),
  "description" => _INTL("Rearrange tiles in tilesets."),
  "always_show" => true,
  "effect"      => proc { |sprites, viewport|
    pbFadeOutIn { TilesetRearranger.new.main }
  }
})
