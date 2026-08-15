set(srcs
    "main.c"
    "display.c"
    "button.c"
    "touch.c"
    "audio.c"
    "imu.c"
    "cat.c"
    "cat_sprite.c"
    "battery.c"
    "rtc.c"
    "wifi_time.c"
    "power.c"
    "stats.c"
    "model.c"
    "sdcard.c"
    "world.c"
    "logbook.c"
    "provision.c")

# config.h decides where the park comes from; this reads the same line so
# the two can never disagree. Streaming it from the SD card keeps ~8.7 MB of
# baked strips out of the firmware image — nine tenths of it.
# file(STRINGS ... REGEX) returns only the matching line: reading the whole
# header into a variable breaks IDF's requirement serialisation.
# CMAKE_CURRENT_LIST_DIR, not SOURCE_DIR: IDF's early-expansion pass runs
# this file with the source dir pointed at build/.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/config.h" _world_from_sd
     REGEX "^#define WORLD_FROM_SD 1$")
if(_world_from_sd)
    message(STATUS "pixelcat: park streams from SD (cat_bg.c not compiled)")
else()
    message(STATUS "pixelcat: park baked into flash")
    list(APPEND srcs "cat_bg.c")
endif()
unset(_world_from_sd)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ".")

target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

# rtc.c bakes __DATE__/__TIME__ as the clock-set reference; touch it every
# build so the timestamps are always this build's.
add_custom_target(refresh_build_time
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_SOURCE_DIR}/rtc.c)
add_dependencies(${COMPONENT_LIB} refresh_build_time)
