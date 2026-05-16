print("cwd: " .. breezy.cwd())
print("root exists: " .. tostring(breezy.exists("/root")))
print("sd exists:   " .. tostring(breezy.exists("/sd")))

print("\nRunning shell commands through breezy.exec:")
breezy.exec("pwd")
breezy.exec("df")
