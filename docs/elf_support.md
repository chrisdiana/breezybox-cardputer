
### Why External ELF Apps Worked On The Original Demo But Not Here

The original Waveshare `breezydemo` target could run external ELF apps because its memory layout was more favorable to the ELF loader, and it was designed with PSRAM-heavy assumptions. This Cardputer ADV port is different even though it is still an ESP32-S3-family board.

Important differences:

- the original design assumed PSRAM for fuller functionality
- this Cardputer build does not have `CONFIG_SPIRAM` enabled
- runtime testing on Cardputer showed `EXEC free=0 largest=0` before ELF launch
- without executable heap, relocated ELF code falls back into normal SRAM/DRAM
- that memory is not executable for this loader path, so jumping into the ELF causes `InstructionFetchError`

So the blocker is not just command lookup or one small loader bug. It is mainly the runtime memory model on this board and firmware configuration. Built-in apps work because they are part of the firmware image and run from the normal flash/IRAM mapping, while external ELF apps require executable relocation space that this build does not currently have.
