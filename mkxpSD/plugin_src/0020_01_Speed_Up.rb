module Input

  def self.update
    update_KGC_ScreenCapture
    if trigger?(Input::F8)
      pbScreenCapture
    end
    if $CanToggle
      if trigger?(Input::AUX2)
        pbSpeedUp($GameSpeed += 1)
      end
    end
  end
end

def pbAllowSpeedup
  $CanToggle = true
end

def pbDisallowSpeedup
  $CanToggle = false
  $GameSpeed = 0
end

def pbSpeedUp(speed)
  $GameSpeed = speed
  $GameSpeed = 0 if $GameSpeed >= SPEEDUP_STAGES.size || $GameSpeed < 0
  pbSEPlay("Voltorb Flip tile",100,100+25*$GameSpeed)
end

SPEEDUP_STAGES = [1,2,4]
$GameSpeed = 0
$frame = 0
$CanToggle = true

module Graphics
  class << Graphics
    alias fast_forward_update update
  end

  def self.update
    $frame += 1
    return unless $frame % SPEEDUP_STAGES[$GameSpeed] == 0
    fast_forward_update
    $frame = 0
  end
end
