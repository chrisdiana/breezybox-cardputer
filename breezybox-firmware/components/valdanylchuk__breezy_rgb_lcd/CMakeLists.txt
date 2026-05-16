idf_component_register(
    SRCS "rgb_display.c" "rgb_gfx.c" "terminus16.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../../main"
    REQUIRES esp_lcd heap esp_driver_ledc
)
