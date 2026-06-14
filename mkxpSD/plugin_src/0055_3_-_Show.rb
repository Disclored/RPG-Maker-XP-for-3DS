module GenderPickSelection
	class Show

		def show
			create_scene
			loop do
				break if @exit
				# Update
				update_ingame
				update_scene
				set_input
				update_player
			end
		end

		def create_scene
			# Background
			@sprites = {}
			@viewport = Viewport.new(0,0,Graphics.width,Graphics.height)
			@viewport.z = 99999
			#create_sprite("bg", "BackgroundSelect", @viewport)
			addBackgroundPlane(@sprites,"bg", "GenderSelection/Background",@viewport)
			@sprites["elements"] = IconSprite.new(0,0,@viewport)
			@sprites["elements"].setBitmap(sprintf("Graphics/Pictures/GenderSelection/bgOverlay"))
			@sprites["platform"] = IconSprite.new(182,230,@viewport)
			@sprites["platform"].setBitmap(sprintf("Graphics/Pictures/GenderSelection/bgPlatform"))
			@sprites["preview"] = IconSprite.new(0,0,@viewport)
			update_preview
			# Bitmap
			@realquant.times { |i|
				create_sprite("player #{i}", @player[i], @viewport)
				@sprites["player #{i}"].z = 1
			}
			@tabAmount.times { |j|
				create_sprite("tab #{j}", "Tab_" + j.to_s, @viewport)
				@sprites["tab #{j}"].z = 1
			}
		end

		#-------#
		# Input #
		#-------#
		def set_input
			if @bg == 1
				if pbConfirmMessage("Are you sure you want to use this photo?")
					set_player(@position)
					$Trainer.outfit = @currentTab
					@exit = true
				else
					@bg = 0
					@refresh = true
					update_player
				end
				return
			end
			@bg = 1 if checkInput(Input::USE)
			if checkInput(Input::LEFT)
				@position -= 1
				if @position < @startnum
					if (@startnum - 1) >= 0
						@startnum -= 1
						@position  = @startnum
					elsif (@startnum - 1) < 0
						@startnum = @player.size - @realquant
						@position = @startnum + @realquant - 1
					end
				end
				@refresh = true
				update_player
			elsif checkInput(Input::RIGHT)
				@position += 1
				limit = @startnum + @realquant - 1
				if @position > limit
					if (limit + 1 + 1) <= @player.size
						@startnum += 1
						@position  = @startnum + @realquant - 1
					elsif (limit + 1 + 1) > @player.size
						@startnum = 0
						@position = 0
					end
				end
				@refresh = true
				update_player
			elsif checkInput(Input::UP)
				@position -= 2
				if @position < @startnum
					if (@startnum - 2) >= -1
						@startnum -= 2
						if @startnum < 0
							@startnum = 0 
							@position = @startnum
						end
					elsif (@startnum - 2) < -1
						@startnum = @player.size - @realquant
						@position = @startnum + @realquant - 1
					end
				end
				@refresh = true
				update_player
			elsif checkInput(Input::DOWN)
				@position += 2
				limit = @startnum + @realquant - 1
				if @position > limit
					if (limit + 1 + 2) <= @player.size + 1
						@startnum += 2
						@startnum  = @player.size - @realquant if @startnum + @realquant > @player.size
						@position  = @startnum + @realquant - 1 if @position >= @player.size
					elsif (limit + 1 + 2) > @player.size + 1
						@startnum = 0
						@position = 0
					end
				end
				@refresh = true
				update_player
			elsif checkInput(Input::JUMPDOWN) || checkInput(Input::ACTION) || checkInput(Input::SPECIAL)
				@currentTab += 1
				limit = 3
				if @currentTab > limit
					@currentTab = 0
				end
				@refresh = true
				update_player
			elsif checkInput(Input::JUMPUP)
				@currentTab -= 1
				if @currentTab < 0
					@currentTab = 3
				end
				@refresh = true
				update_player
			end
		end

		#--------#
		# Update #
		#--------#
		def update_scene
			if @sprites["bg"]
			  @sprites["bg"].ox+=1
			  @sprites["bg"].oy-=0
			end
		end

		def update_player
			case @bg
			when 0
				@realquant.times { |i|
					set_visible_sprite("player #{i}", true)
					if @refresh
						player = update_bitmap
						p = player[i].to_s
						@currentTab>0 ? c = "_" + @currentTab.to_s : c = ""
						filename = (p+c).to_sym
						set_sprite("player #{i}", filename)
					end
					x = 16 + (i.even? ? 0 : 358)
					y = 52 + 64 * (i / 2)
					set_xy_sprite("player #{i}", x, y)
					@sprites["player #{i}"].tone = (@startnum + i) == @position ? Tone.new(48,48,48) : Tone.new(0,0,0)
					update_preview
				}
				@tabAmount.times { |j|
					set_visible_sprite("tab #{j}", true)
					if @refresh
						tab = update_bitmap
						set_sprite("tab #{j}", "Tab_" + j.to_s)
					end
					x = 214 + 22 * (j)
					y = 312
					set_xy_sprite("tab #{j}", x, y)
					@sprites["tab #{j}"].tone = (@startnum + j) == @currentTab ? Tone.new(48,48,48) : Tone.new(0,0,0)
				}
				set_visible_sprite("elements", true)
				@refresh = false if @refresh
			when 1
				@realquant.times { |i|
					set_visible_sprite("player #{i}")
				}
				@tabAmount.times { |j|
					set_visible_sprite("tab #{j}")
				}
				set_visible_sprite("elements")
			end
		end

		def update_preview
			player = update_bitmap
			p = player[@position].to_s
			@currentTab>0 ? c = "_" + @currentTab.to_s : c = ""
			filename = (p+c).to_s
			filenamePreview = ("Graphics/Pictures/PlayerSprites/"+filename).to_s
			@sprites["preview"].setBitmap(sprintf(filenamePreview))
			@sprites["preview"].x = 160 - (@sprites["preview"].bitmap.width-128)/(2*1)
			@sprites["preview"].y = 68 - (@sprites["preview"].bitmap.height-128)/(1*1)
			@sprites["preview"].z = 2
			@sprites["preview"].zoom_x = 2
			@sprites["preview"].zoom_y = 2
		end

		def update_bitmap
			player = []
			n = @startnum + @realquant
			(@startnum...n).each { |i| player << @player[i] }
			return player
		end

	end
end