-- hello.lua — minimal Halo application.
-- Install with:  python3 tools/run_app.py examples/hello.lua
--
-- The display parks in power-save after the boot logo, so every app
-- must wake it before drawing.  show() is a no-op on Halo: text/clear
-- draw straight into the framebuffer.

frame.display.power_save(false)

local presses = 0
frame.button.single(function()
    presses = presses + 1
    print('button pressed! total: ' .. presses)
end)

local n = 0
while true do
    frame.display.clear(0x000000)
    frame.display.text('MY FIRST HALO APP', 24, 60)
    frame.display.text('uptime  ' .. n .. ' s', 24, 110, 0x00FF88)
    frame.display.text('presses ' .. presses, 24, 140, 0xFFAA00)
    frame.sleep(1)
    n = n + 1
end
