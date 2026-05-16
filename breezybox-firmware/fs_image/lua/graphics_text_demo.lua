local breezy = require("breezy")
local gfx = breezy.gfx

gfx.mode("150p")
gfx.clear(0)

gfx.font("small")
gfx.text(8, 8, "small font 5x7", 14)
gfx.text(8, 20, "0123456789", 11)

gfx.font("term16")
gfx.text(8, 40, "term16 font", 12)
gfx.text(8, 60, "Lua on Cardputer", 15)

gfx.rect(4, 4, 120, 28, 10)
gfx.rect(4, 34, 160, 48, 9)

breezy.sleep(2)
gfx.mode("text")
print("graphics text demo done")
