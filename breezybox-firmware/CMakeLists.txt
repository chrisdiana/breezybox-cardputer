cmake_minimum_required(VERSION 3.16)

set(BREEZY_BOARD "cardputer" CACHE STRING "Target BreezyBox board")
set_property(CACHE BREEZY_BOARD PROPERTY STRINGS cardputer cardputer-adv sticks3)

if(BREEZY_BOARD STREQUAL "sticks3")
    add_compile_definitions(
        BREEZY_BOARD_STICKS3=1
        BREEZY_DISPLAY_COLS=40
        BREEZY_DISPLAY_ROWS=16
        BREEZY_VTERM_COLS=40
        BREEZY_VTERM_ROWS=16
        BREEZY_VTERM_COUNT=1
        BREEZY_VTERM_SCROLLBACK_LINES=64
    )
elseif(BREEZY_BOARD STREQUAL "cardputer-adv")
    add_compile_definitions(
        BREEZY_BOARD_CARDPUTER=1
        BREEZY_BOARD_CARDPUTER_ADV=1
        BREEZY_DISPLAY_COLS=40
        BREEZY_DISPLAY_ROWS=16
        BREEZY_VTERM_COLS=40
        BREEZY_VTERM_ROWS=16
        BREEZY_VTERM_COUNT=2
        BREEZY_VTERM_SCROLLBACK_LINES=96
    )
elseif(BREEZY_BOARD STREQUAL "cardputer")
    add_compile_definitions(
        BREEZY_BOARD_CARDPUTER=1
        BREEZY_BOARD_CARDPUTER_UNIVERSAL=1
        BREEZY_DISPLAY_COLS=40
        BREEZY_DISPLAY_ROWS=16
        BREEZY_VTERM_COLS=40
        BREEZY_VTERM_ROWS=16
        BREEZY_VTERM_COUNT=2
        BREEZY_VTERM_SCROLLBACK_LINES=96
    )
endif()

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/../breezybox-cardputer
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(breezybox_cardputer)

set(BREEZY_FS_IMAGE_DIR ${CMAKE_CURRENT_LIST_DIR}/fs_image)

file(MAKE_DIRECTORY ${BREEZY_FS_IMAGE_DIR})

littlefs_create_partition_image(spiffs ${BREEZY_FS_IMAGE_DIR} FLASH_IN_PROJECT)

# Copy our custom symbol table into the elf_loader managed component.
# Runs every build; copy_if_different avoids needless recompilation.
add_custom_target(copy_all_symbols ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${PROJECT_DIR}/main/all_my_symbols.c
        ${PROJECT_DIR}/components/espressif__elf_loader/src/esp_all_symbol.c
    COMMENT "Copying all_my_symbols.c -> esp_all_symbol.c"
)
# Ensure the copy finishes before the elf_loader component compiles.
if(TARGET __idf_espressif__elf_loader)
    add_dependencies(__idf_espressif__elf_loader copy_all_symbols)
endif()
