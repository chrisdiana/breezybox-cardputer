cmake_minimum_required(VERSION 3.16)

set(BREEZY_BOARD "cardputer" CACHE STRING "Target BreezyBox board")
set_property(CACHE BREEZY_BOARD PROPERTY STRINGS cardputer sticks3)

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
else()
    add_compile_definitions(
        BREEZY_BOARD_CARDPUTER=1
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
set(BREEZY_FS_APPS_DIR ${BREEZY_FS_IMAGE_DIR}/apps)
set(BREEZY_FS_BREEZYAPPS_DIR ${BREEZY_FS_IMAGE_DIR}/breezyapps)

file(MAKE_DIRECTORY ${BREEZY_FS_IMAGE_DIR})

add_custom_target(stage_fs_image ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${BREEZY_FS_APPS_DIR}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${BREEZY_FS_BREEZYAPPS_DIR}
    COMMENT "Cleaning fs_image app bundles"
)

littlefs_create_partition_image(storage ${BREEZY_FS_IMAGE_DIR} FLASH_IN_PROJECT
    DEPENDS stage_fs_image
)

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
