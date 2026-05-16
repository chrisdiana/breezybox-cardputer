
## Lua Scripting

The active scripting path is now an embedded Lua runtime inside the main
firmware image.

Current command forms:

```sh
lua guide
lua
lua shell
lua -e 'print(2+2)'
lua /root/lua/hello.lua
lua /root/lua/args.lua arg1 arg2
```

Bundled demo scripts are staged into `/root/lua/`:

```sh
more /root/lua/GUIDE.txt
lua /root/lua/hello.lua
lua /root/lua/list_root.lua
lua /root/lua/sysinfo.lua
lua /root/lua/args.lua one two three
lua /root/lua/write_demo.lua
lua /root/lua/graphics_demo.lua
lua /root/lua/graphics_text_demo.lua
lua /root/lua/tui_demo.lua
lua /root/lua/keyboard_demo.lua
lua /root/lua/battery_demo.lua
lua /root/lua/config_demo.lua
lua /root/lua/http_demo.lua
lua /root/lua/web_get_example.lua
lua /root/lua/web_post_example.lua
lua /root/lua/sound_demo.lua
lua /root/lua/sprite_demo.lua
lua /root/lua/i2c_scan.lua
lua /root/lua/i2s_demo.lua
lua /root/lua/spi_demo.lua
lua /root/lua/uart_demo.lua
lua /root/lua/hardware_info.lua
```

Starter files:

- `/root/lua/GUIDE.txt` compact on-device Lua quick guide
- `/root/lua/template.lua` starter script template
- `/root/lua/web_get_example.lua` raw-TCP example that fetches page contents from `example.com`
- `/root/lua/web_post_example.lua` raw-TCP example that sends a small HTTP POST request

The current embedded module is `breezy`:

```lua
print(breezy.cwd())
breezy.cd("/root")
for i, name in ipairs(breezy.listdir("/root")) do print(name) end
print(breezy.read_file("/root/init.sh"))
breezy.write_file("/root/test.lua", "print('hi')\n")
print(breezy.exists("/root/test.lua"))
breezy.sleep(0.5)
breezy.sleep_ms(16)
breezy.now_ms()
breezy.clear()
print(breezy.term_size())
print(breezy.readkey(0))
print(breezy.keyboard.mods().shift)
print(breezy.exec("ls /root"))
print(breezy.battery.read_pct())
breezy.config.set("theme", "amber")
print(breezy.config.get("theme"))
breezy.pin.mode(13, "out")
breezy.pin.write(13, 1)
print(breezy.adc.read(1))
breezy.i2c.open(1, 2, { freq = 400000 })
print(#breezy.i2c.scan())
breezy.i2c.close()
breezy.i2s.open_tx(10, 11, 12, { sample_rate = 16000, bits = 16, channels = 1 })
breezy.i2s.close("tx")
breezy.spi.open(3, 4, 5, 6, { mode = 0, delay_us = 1 })
print(#breezy.spi.transfer(string.char(0x9F)))
breezy.spi.close()
print(breezy.storage.sd_mounted())
print(breezy.network.is_connected())
local res = breezy.network.http_get("http://example.com/")
print(res.status, #res.body)
breezy.tui.box(1, 1, 40, 16, "demo")
breezy.tui.status("ready")
breezy.gfx.mode("150p")
breezy.gfx.font("term16")
local w, h = breezy.gfx.size()
breezy.gfx.clear(0)
breezy.gfx.text(8, 8, "hello", 15)
breezy.gfx.rect(10, 10, 40, 30, 12)
breezy.gfx.rectfill(20, 20, 20, 12, 4)
local img = breezy.gfx.new_image(8, 8, 0)
img:rectfill(1, 1, 6, 6, 14)
breezy.gfx.blit(img, 60, 30, 0)
breezy.gfx.backlight(220)
breezy.gfx.mode("text")
breezy.sound.tone(880, 120)
breezy.sound.play_notes("C4:4 E4:4 G4:4 C5:8")
local h1 = breezy.uart.open(1, 115200, 1, 2)
breezy.uart.write(h1, "hello\\r\\n")
print(breezy.uart.read(h1, 64, 100))
breezy.uart.close(h1)
```





The current Lua surface is still intentionally small, but it now includes:

- basic shell/filesystem helpers
- `breezy.keyboard` for raw key events and modifier state
- `breezy.battery` for battery voltage and rough level helpers
- `breezy.config` for lightweight JSON-backed app settings
- `breezy.pin`, `breezy.adc`, `breezy.storage`, and `breezy.network`
- `breezy.network.tcp_*` and `breezy.network.http_*` for simple client apps
- `breezy.i2c` for external sensors on an explicit I2C bus
- `breezy.i2s` for external standard-I2S TX/RX on explicit pins
- `breezy.spi` for external sensors and devices over software SPI
- `breezy.readkey()` and timing helpers for small CLI/TUI apps
- `breezy.tui` for lightweight text UIs
- `breezy.gfx` for graphics mode, images/sprites, blits, and backlight control
- `breezy.sound` for built-in speaker playback, tones, note helpers, and mic capture
- `breezy.uart` for external UART/serial devices on ports `1` and `2`

Notes:

- `breezy.i2s` is still for explicit external pins and raw sample bytes.
- `breezy.sound.mic_open()` is also explicit-pin I2S input for now.
- `breezy.sound` targets the Cardputer built-in speaker path.
