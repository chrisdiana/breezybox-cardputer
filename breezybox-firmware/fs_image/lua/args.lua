print("Lua script arguments:")

for i, value in ipairs(arg) do
  print(string.format("arg[%d] = %s", i - 1, value))
end

if #arg == 0 then
  print("Try: lua /root/lua/args.lua one two three")
end
