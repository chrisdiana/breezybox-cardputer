local path = "/root/lua_output.txt"
local text = "written from Lua at cwd=" .. breezy.cwd() .. "\n"

assert(breezy.write_file(path, text, true))
print("Appended to " .. path)
print("Current contents:")
print(breezy.read_file(path))
