local breezy = require("breezy")

print("template app")
print("cwd:", breezy.cwd())
print("started at ms:", breezy.now_ms())

local args = {}
for i = 0, #arg do
  args[#args + 1] = arg[i]
end

if #args > 0 then
  print("args:")
  for i, v in ipairs(args) do
    print(i, v)
  end
end

-- Example filesystem use:
-- local text = breezy.read_file("/root/init.sh")
-- print(text)

-- Example storage/network use:
-- print(breezy.storage.sd_mounted())
-- print(breezy.network.is_connected())
-- local info = breezy.storage.info("/root")
-- print(info.total, info.used, info.free)

-- Example shell use:
-- breezy.exec("ls /root")

-- Example pin/ADC use:
-- breezy.pin.mode(13, "out")
-- breezy.pin.write(13, 1)
-- print(breezy.adc.read(1))

-- Example I2C use:
-- local i2c = breezy.i2c
-- i2c.open(1, 2, { freq = 400000 })
-- for _, addr in ipairs(i2c.scan()) do
--   print(string.format("0x%02X", addr))
-- end
-- i2c.close()

-- Example SPI use:
-- local spi = breezy.spi
-- spi.open(3, 4, 5, 6, { mode = 0, delay_us = 1 })
-- local rx = spi.transfer(string.char(0x9F))
-- print(rx and #rx or 0)
-- spi.close()

-- Example input/timing loop:
-- local keyboard = breezy.keyboard
-- while true do
--   local key = breezy.readkey(0)
--   if key == "q" then
--     break
--   end
--   breezy.sleep_ms(16)
-- end
-- local ev = keyboard.read_event(0)
-- if ev and ev.pressed then
--   print(ev.base, ev.shift, ev.ctrl, ev.alt, ev.fn)
-- end

-- Example battery/config/network helpers:
-- print("battery:", breezy.battery.read_uv(), breezy.battery.read_pct())
-- local cfg = breezy.config.load("/root/myapp.json")
-- cfg.runs = (cfg.runs or 0) + 1
-- breezy.config.save(cfg, "/root/myapp.json")
-- local res = breezy.network.http_get("http://example.com/")
-- print(res.status, #res.body)

-- Example text UI use:
-- local tui = breezy.tui
-- breezy.clear()
-- tui.cursor(false)
-- tui.box(1, 1, 40, 16, " my app ")
-- tui.center("press q to quit", 3)
-- tui.status("ready")
-- while true do
--   local key = breezy.readkey()
--   if key == "q" then
--     break
--   end
--   tui.write_at(3, 6, "key: " .. tostring(key), true)
-- end
-- tui.cursor(true)

-- Example graphics use:
-- local gfx = breezy.gfx
-- gfx.mode("150p")
-- gfx.font("term16")
-- gfx.clear(0)
-- gfx.text(8, 8, "hello", 15)
-- gfx.rectfill(20, 20, 40, 20, 12)
-- local img = gfx.new_image(8, 8, 0)
-- img:rectfill(1, 1, 6, 6, 14)
-- gfx.blit(img, 60, 40, 0)
-- gfx.backlight(220)
-- breezy.sleep(1)
-- gfx.mode("text")

-- Example UART use:
-- local uart = breezy.uart
-- local h = uart.open(1, 115200, 1, 2)
-- uart.write(h, "hello\r\n")
-- print(uart.read(h, 64, 100))
-- uart.close(h)

-- Example built-in speaker use:
-- breezy.sound.tone(880, 120)
-- breezy.sound.play_notes("C4:4 E4:4 G4:4 C5:8")
