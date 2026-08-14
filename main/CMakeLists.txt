idf_component_register(
    SRCS
        "main.c"
        "display.c"
        "button.c"
        "touch.c"
        "audio.c"
        "imu.c"
        "cat.c"
        "cat_sprite.c"
        "cat_bg.c"
        "battery.c"
        "rtc.c"
        "wifi_time.c"
    INCLUDE_DIRS "."
)

target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

# rtc.c bakes __DATE__/__TIME__ as the clock-set reference; touch it every
# build so the timestamps are always this build's.
add_custom_target(refresh_build_time
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_SOURCE_DIR}/rtc.c)
add_dependencies(${COMPONENT_LIB} refresh_build_time)
