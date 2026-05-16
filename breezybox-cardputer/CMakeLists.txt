set(BREEZYBOX_CARDPUTER_REQUIRES
    console littlefs fatfs nvs_flash esp_wifi esp_netif esp_http_server
    esp_http_client json vfs mbedtls zlib breezy_term
    valdanylchuk__breezy_rgb_lcd driver esp_driver_sdspi esp_driver_spi
    esp_driver_uart esp_driver_i2s esp_driver_ledc esp_adc sdmmc lwip
    libssh_esp32 app_update esp_timer
)

if(NOT BREEZY_BOARD STREQUAL "sticks3")
    list(APPEND BREEZYBOX_CARDPUTER_REQUIRES elf_loader)
endif()

idf_component_register(
    SRCS
        "breezybox.c"
        "breezy_vfs.c"
        "breezy_wrap.c"
        "breezy_exec.c"
        "breezy_exports.c"
        "breezy_http.c"
        "breezy_pager.c"
        "cmd/ls.c"
        "cmd/cat.c"
        "cmd/shell_extra.c"
        "cmd/mkdir.c"
        "cmd/wifi.c"
        "cmd/httpd.c"
        "cmd/head.c"
        "cmd/tail.c"
        "cmd/more.c"
        "cmd/wc.c"
        "cmd/cp.c"
        "cmd/mv.c"
        "cmd/rm.c"
        "cmd/df.c"
        "cmd/du.c"
        "cmd/date.c"
        "cmd/sleep.c"
        "cmd/eget.c"
        "cmd/ping.c"
        "cmd/lua.c"
        "cmd/ssh.c"
        "third_party/lua/onelua.c"
    INCLUDE_DIRS "include" "third_party/lua"
    PRIV_INCLUDE_DIRS "../breezybox-firmware/main"
    REQUIRES ${BREEZYBOX_CARDPUTER_REQUIRES}
)

set_source_files_properties(
    "third_party/lua/onelua.c"
    PROPERTIES COMPILE_DEFINITIONS "MAKE_LIB"
)

# Propagate linker wrap options to any project using this component
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,-wrap,fopen"
    "-Wl,-wrap,open"
    "-Wl,-wrap,mkdir"
    "-Wl,-wrap,rename"
    "-Wl,-wrap,remove"
    "-Wl,-wrap,rmdir"
    "-Wl,-wrap,chdir"
    "-Wl,-wrap,getcwd"
    "-Wl,-wrap,opendir"
    "-Wl,-wrap,readdir"
    "-Wl,-wrap,closedir"
    "-Wl,-wrap,rewinddir"
    "-Wl,-wrap,stat"
    "-Wl,-wrap,realpath"
)
