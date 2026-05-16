// Build the BreezyApps vi implementation into the firmware as a built-in
// command so Cardputer does not depend on ELF relocation for the editor.
#define main cmd_vi_builtin_main
#include "../../breezybox-cardputer/apps/vi/vi.c"
