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
    INCLUDE_DIRS "."
)

target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
