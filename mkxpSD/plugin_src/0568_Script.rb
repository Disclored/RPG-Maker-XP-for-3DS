 def pbZoomIn
  $zoom.dispose if $zoom
  vp = Viewport.new(0,0,Graphics.width,Graphics.height)
  vp.z = 1000000
  $zoom = Sprite.new(vp)
  $zoom.bitmap = Graphics.snap_to_bitmap
  $zoom.x = $zoom.bitmap.width / 2
  $zoom.y = $zoom.bitmap.height / 2
  $zoom.ox = $zoom.bitmap.width / 2
  $zoom.oy = $zoom.bitmap.height / 2
end
 
def pbUpdateZoom(time)
  time.times do
	Graphics.update
	Input.update
	$zoom.zoom_x += 0.01
	$zoom.zoom_y += 0.01
  end
end
 
def pbFadeOutZoom
  32.times do
	Graphics.update
	Input.update
	$zoom.zoom_x += 0.01
	$zoom.zoom_y += 0.01
	$zoom.opacity -= 255 / 32.0
  end
  $zoom.dispose
end
