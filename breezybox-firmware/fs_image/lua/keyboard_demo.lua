local breezy = require("breezy")
local keyboard = breezy.keyboard
local tui = breezy.tui

breezy.clear()
tui.cursor(false)
tui.box(1, 1, 40, 16, " raw keyboard ")
tui.center("press q to quit", 3)
tui.status("showing key press/release events")

while true do
  local ev = keyboard.read_event()
  if ev then
    tui.write_at(2, 6, string.format(
      "%s base=%q shifted=%q",
      ev.pressed and "down" or "up",
      ev.base,
      ev.shifted
    ), true)
    tui.write_at(2, 8, string.format(
      "mods shift=%s ctrl=%s alt=%s fn=%s opt=%s",
      tostring(ev.shift),
      tostring(ev.ctrl),
      tostring(ev.alt),
      tostring(ev.fn),
      tostring(ev.opt)
    ), true)
    tui.write_at(2, 10, string.format("row=%d col=%d", ev.row, ev.col), true)
    if ev.pressed and ev.base == "q" and not ev.ctrl and not ev.alt and not ev.fn then
      break
    end
  end
end

tui.cursor(true)
print("done")
