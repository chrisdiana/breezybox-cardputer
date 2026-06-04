set(BOARD_SRCS
    "main.c"
    "my_console_io.c"
    "cmd_app_compat.c"
    "cmd_testgfx.c"
    "cmd_plasma_builtin.c"
    "cmd_termbench_builtin.c"
    "cmd_vi_builtin.c"
    "cmd_wget_builtin.c"
    "cmd_gzip_builtin.c"
    "cmd_gunzip_builtin.c"
)

set(BOARD_INCLUDE_DIRS ".")

if(BREEZY_BOARD STREQUAL "sticks3")
    list(APPEND BOARD_SRCS "board_m5stickc.c")
else()
    list(APPEND BOARD_SRCS
        "board_cardputer.c"
        "cardputer_keyboard.c"
        "ssh_app_mode.c"
        "cmd_ccleste_builtin.c"
        "../../breezybox-cardputer/apps/ccleste/player.c"
        "../../breezybox-cardputer/apps/ccleste/level.c"
        "../../breezybox-cardputer/apps/ccleste/vfx.c"
        "../../breezybox-cardputer/apps/ccleste/gfx_esp32.c"
        "../../breezybox-cardputer/apps/ccleste/sprites.c"
    )
    list(APPEND BOARD_INCLUDE_DIRS "../../breezybox-cardputer/apps/ccleste")
endif()

# Register the 'main' component with the build system.
idf_component_register(
    # --- Source Files ---
    SRCS ${BOARD_SRCS}

    # --- Dependencies ---
    PRIV_REQUIRES
        spi_flash
        driver
        fatfs
        esp_lcd
        breezybox-cardputer
        breezy_bt
        breezy_rgb_lcd
        heap
        esp_hid
        bt
        nvs_flash
        hal
        libssh_esp32

    # --- Include Directories ---
    INCLUDE_DIRS
        ${BOARD_INCLUDE_DIRS}
)
