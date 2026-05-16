local breezy = require("breezy")
local sound = breezy.sound

print("speaker demo")
sound.speaker_open(16000, 180)
sound.play_notes("C4:4 E4:4 G4:4 C5:8 R:4 C5:8")
sound.tone(660, 120, 200)
sound.tone(880, 120, 200)
sound.speaker_close()
print("done")
