local breezy = require("breezy")
local gfx = breezy.gfx
local keyboard = breezy.keyboard

local TILE = 8
local SCREEN_W = 240
local SCREEN_H = 150
local WORLD_W = 30
local WORLD_H = 38
local GRAVITY = 0.18
local MAX_FALL = 2.4
local RUN_ACCEL = 0.26
local RUN_DECEL = 0.32
local MAX_RUN = 1.35
local JUMP_SPEED = -3.15
local DASH_SPEED = 4.1
local DASH_FRAMES = 8

local palette = {
  {0, 0, 0},
  {29, 43, 83},
  {126, 37, 83},
  {0, 135, 81},
  {171, 82, 54},
  {95, 87, 79},
  {194, 195, 199},
  {255, 241, 232},
  {255, 0, 77},
  {255, 163, 0},
  {255, 236, 39},
  {0, 228, 54},
  {41, 173, 255},
  {131, 118, 156},
  {255, 119, 168},
  {255, 204, 170},
}

local raw_map = {
  "##############################",
  "#............................#",
  "#............................#",
  "#......................F.....#",
  "#....................####....#",
  "#............................#",
  "#...............####.........#",
  "#............................#",
  "#.........####...............#",
  "#............................#",
  "#..................^^^^......#",
  "#......####..................#",
  "#............................#",
  "#............^^^^............#",
  "#..####......................#",
  "#............................#",
  "#....................####....#",
  "#.........^^^^...............#",
  "#............................#",
  "#....####....................#",
  "#............................#",
  "#..............####..........#",
  "#.........................B..#",
  "#.......^^^^.................#",
  "#............................#",
  "#................####........#",
  "#............................#",
  "#..........B.................#",
  "#......####..................#",
  "#............................#",
  "#.....................^^^^...#",
  "#..............####..........#",
  "#............................#",
  "#..P.........................#",
  "#........######..............#",
  "#............................#",
  "#.................^^^^.......#",
  "##############################",
}

local player = {}
local camera_y = 0
local deaths = 0
local start_ms = 0
local won = false
local quit = false
local prev_jump = false
local prev_dash = false
local prev_restart = false
local prev_quit = false
local balloons = {}
local particles = {}

local function set_palette()
  for i, rgb in ipairs(palette) do
    gfx.palette(i - 1, rgb[1], rgb[2], rgb[3])
  end
end

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

local function approach(v, target, step)
  if v < target then
    return math.min(v + step, target)
  end
  if v > target then
    return math.max(v - step, target)
  end
  return target
end

local function tile_at(tx, ty)
  if tx < 1 or tx > WORLD_W or ty < 1 or ty > WORLD_H then
    return "#"
  end
  return raw_map[ty]:sub(tx, tx)
end

local function solid_tile(ch)
  return ch == "#"
end

local function spike_tile(ch)
  return ch == "^"
end

local function rect_hits_tile(x, y, w, h, pred)
  local x0 = math.floor(x / TILE) + 1
  local y0 = math.floor(y / TILE) + 1
  local x1 = math.floor((x + w - 1) / TILE) + 1
  local y1 = math.floor((y + h - 1) / TILE) + 1
  for ty = y0, y1 do
    for tx = x0, x1 do
      if pred(tile_at(tx, ty)) then
        return true
      end
    end
  end
  return false
end

local function solid_at(x, y, w, h)
  return rect_hits_tile(x, y, w, h, solid_tile)
end

local function spike_at(x, y, w, h)
  return rect_hits_tile(x, y, w, h, spike_tile)
end

local function spawn_particles(x, y, color, count)
  for _ = 1, count do
    particles[#particles + 1] = {
      x = x,
      y = y,
      vx = (math.random() * 2 - 1) * 1.3,
      vy = (math.random() * 2 - 1) * 1.3,
      t = 18 + math.floor(math.random() * 10),
      c = color,
    }
  end
end

local function find_spawn()
  balloons = {}
  for y, row in ipairs(raw_map) do
    for x = 1, #row do
      local ch = row:sub(x, x)
      if ch == "P" then
        return (x - 1) * TILE, (y - 1) * TILE
      elseif ch == "B" then
        balloons[#balloons + 1] = {
          x = (x - 1) * TILE,
          y = (y - 1) * TILE,
          respawn = 0,
        }
      end
    end
  end
  return 16, (WORLD_H - 3) * TILE
end

local function reset_player()
  local sx, sy = find_spawn()
  player = {
    x = sx,
    y = sy - 2,
    w = 6,
    h = 8,
    vx = 0,
    vy = 0,
    facing = 1,
    grounded = false,
    grace = 0,
    jump_buf = 0,
    dashes = 1,
    dash_timer = 0,
    dash_vx = 0,
    dash_vy = 0,
  }
  camera_y = clamp(player.y - 96, 0, WORLD_H * TILE - SCREEN_H)
end

local function restart()
  deaths = 0
  won = false
  start_ms = breezy.now_ms()
  reset_player()
  particles = {}
end

local function die()
  deaths = deaths + 1
  spawn_particles(player.x + 3, player.y + 4, 8, 18)
  reset_player()
end

local function move_axis(axis, amount)
  local step = amount > 0 and 1 or -1
  local remaining = math.abs(amount)
  while remaining > 0 do
    local delta = math.min(1, remaining) * step
    if axis == "x" then
      if not solid_at(player.x + delta, player.y, player.w, player.h) then
        player.x = player.x + delta
      else
        player.vx = 0
        return
      end
    else
      if not solid_at(player.x, player.y + delta, player.w, player.h) then
        player.y = player.y + delta
      else
        player.vy = 0
        return
      end
    end
    remaining = remaining - 1
  end
end

local function key_down(name)
  return keyboard.is_down(name)
end

local function read_input()
  local left = key_down("a")
  local right = key_down("d")
  local up = key_down("w")
  local down = key_down("s")
  local jump = key_down("z")
  local dash = key_down("x")
  local restart_key = key_down("r")
  local quit_key = key_down("q")

  local input = {
    left = left,
    right = right,
    up = up,
    down = down,
    jump = jump,
    dash = dash,
    jump_pressed = jump and not prev_jump,
    dash_pressed = dash and not prev_dash,
    restart_pressed = restart_key and not prev_restart,
    quit_pressed = quit_key and not prev_quit,
  }

  prev_jump = jump
  prev_dash = dash
  prev_restart = restart_key
  prev_quit = quit_key
  return input
end

local function update_balloons()
  for _, b in ipairs(balloons) do
    if b.respawn > 0 then
      b.respawn = b.respawn - 1
    elseif player.x < b.x + 8 and player.x + player.w > b.x and
        player.y < b.y + 8 and player.y + player.h > b.y then
      player.dashes = 1
      b.respawn = 90
      spawn_particles(b.x + 4, b.y + 4, 12, 8)
    end
  end
end

local function update_player(input)
  if input.restart_pressed then
    restart()
    return
  end
  if input.quit_pressed then
    quit = true
    return
  end
  if won then
    return
  end

  local dir = (input.right and 1 or 0) - (input.left and 1 or 0)
  if dir ~= 0 then
    player.vx = approach(player.vx, dir * MAX_RUN, RUN_ACCEL)
    player.facing = dir
  else
    player.vx = approach(player.vx, 0, RUN_DECEL)
  end

  if input.jump_pressed then
    player.jump_buf = 5
  elseif player.jump_buf > 0 then
    player.jump_buf = player.jump_buf - 1
  end

  if input.dash_pressed and player.dashes > 0 then
    local dx = dir
    local dy = (input.down and 1 or 0) - (input.up and 1 or 0)
    if dx == 0 and dy == 0 then
      dx = player.facing
    end
    if dx ~= 0 and dy ~= 0 then
      dx = dx * 0.707
      dy = dy * 0.707
    end
    player.dashes = player.dashes - 1
    player.dash_timer = DASH_FRAMES
    player.dash_vx = dx * DASH_SPEED
    player.dash_vy = dy * DASH_SPEED
    player.vx = player.dash_vx
    player.vy = player.dash_vy
    spawn_particles(player.x + 3, player.y + 4, 12, 7)
  end

  if player.dash_timer > 0 then
    player.dash_timer = player.dash_timer - 1
    player.vx = player.dash_vx
    player.vy = player.dash_vy
  else
    if player.jump_buf > 0 and player.grace > 0 then
      player.vy = JUMP_SPEED
      player.jump_buf = 0
      player.grace = 0
      spawn_particles(player.x + 3, player.y + 8, 7, 5)
    end
    player.vy = math.min(player.vy + GRAVITY, MAX_FALL)
  end

  move_axis("x", player.vx)
  move_axis("y", player.vy)

  player.grounded = solid_at(player.x, player.y + 1, player.w, player.h)
  if player.grounded then
    player.grace = 6
    player.dashes = 1
  elseif player.grace > 0 then
    player.grace = player.grace - 1
  end

  if spike_at(player.x, player.y, player.w, player.h) or player.y > WORLD_H * TILE + 16 then
    die()
    return
  end

  update_balloons()

  local tx = math.floor((player.x + 3) / TILE) + 1
  local ty = math.floor((player.y + 4) / TILE) + 1
  if tile_at(tx, ty) == "F" then
    won = true
    spawn_particles(player.x + 3, player.y + 4, 10, 24)
  end

  local target = clamp(player.y - 92, 0, WORLD_H * TILE - SCREEN_H)
  camera_y = camera_y + (target - camera_y) * 0.12
end

local function update_particles()
  for i = #particles, 1, -1 do
    local p = particles[i]
    p.x = p.x + p.vx
    p.y = p.y + p.vy
    p.vy = p.vy + 0.03
    p.t = p.t - 1
    if p.t <= 0 then
      table.remove(particles, i)
    end
  end
end

local function draw_tile(ch, x, y)
  if ch == "#" then
    gfx.rectfill(x, y, TILE, TILE, 5)
    gfx.rectfill(x + 1, y + 1, TILE - 2, TILE - 2, 13)
  elseif ch == "^" then
    gfx.rectfill(x, y + 6, TILE, 2, 5)
    gfx.rectfill(x + 3, y + 1, 2, 6, 8)
    gfx.pixel(x + 2, y + 3, 8)
    gfx.pixel(x + 5, y + 3, 8)
  elseif ch == "F" then
    gfx.rectfill(x + 1, y, 1, TILE, 7)
    gfx.rectfill(x + 2, y, 5, 4, 10)
  end
end

local function draw_balloons()
  for _, b in ipairs(balloons) do
    if b.respawn == 0 then
      local x = b.x
      local y = b.y - camera_y
      gfx.rectfill(x + 2, y + 1, 4, 5, 14)
      gfx.pixel(x + 3, y + 6, 7)
      gfx.pixel(x + 4, y + 6, 7)
    end
  end
end

local function draw_player()
  local x = math.floor(player.x)
  local y = math.floor(player.y - camera_y)
  local body = player.dashes > 0 and 8 or 12
  if player.dash_timer > 0 then
    body = 10
  end

  gfx.rectfill(x + 1, y + 2, 4, 6, body)
  gfx.rectfill(x + 2, y, 3, 3, 15)
  gfx.pixel(x + (player.facing > 0 and 4 or 2), y + 1, 0)
  gfx.pixel(x, y + 3, 8)
end

local function draw_particles()
  for _, p in ipairs(particles) do
    local x = math.floor(p.x)
    local y = math.floor(p.y - camera_y)
    if x >= 0 and x < SCREEN_W and y >= 0 and y < SCREEN_H then
      gfx.pixel(x, y, p.c)
    end
  end
end

local function draw_hud()
  gfx.rectfill(0, 0, SCREEN_W, 9, 1)
  local elapsed = math.floor((breezy.now_ms() - start_ms) / 1000)
  local text = string.format("ccleste  deaths:%d  %02d:%02d", deaths, math.floor(elapsed / 60), elapsed % 60)
  gfx.text(2, 1, text, 7)
  if won then
    gfx.rectfill(58, 54, 124, 34, 1)
    gfx.rect(58, 54, 124, 34, 7)
    gfx.text(75, 62, "summit reached", 10)
    gfx.text(73, 74, "r restart q quit", 7)
  end
end

local function draw()
  gfx.clear(1)
  local first_ty = math.floor(camera_y / TILE) + 1
  local last_ty = math.min(WORLD_H, first_ty + math.ceil(SCREEN_H / TILE) + 1)
  for ty = first_ty, last_ty do
    local row = raw_map[ty]
    local y = (ty - 1) * TILE - math.floor(camera_y)
    for tx = 1, WORLD_W do
      local ch = row:sub(tx, tx)
      if ch ~= "." and ch ~= "P" and ch ~= "B" then
        draw_tile(ch, (tx - 1) * TILE, y)
      end
    end
  end

  draw_balloons()
  draw_player()
  draw_particles()
  draw_hud()
end

math.randomseed(breezy.now_ms())
gfx.mode("150p")
set_palette()
gfx.font("small")
gfx.backlight(220)
restart()

while not quit do
  local input = read_input()
  update_player(input)
  update_particles()
  draw()
  gfx.wait_vsync()
  breezy.sleep_ms(16)
end

gfx.mode("text")
print("ccleste closed")
