local path = arg[0] or "/root"

print("Listing " .. path)
for i, name in ipairs(breezy.listdir(path)) do
  print(string.format("%2d  %s", i, name))
end
