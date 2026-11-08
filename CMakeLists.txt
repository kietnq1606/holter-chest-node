cmake_minimum_required(VERSION 3.20.0)
set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/chest_node.overlay")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(chest_node)

target_sources(app PRIVATE
    test/sw_led_test.c
    services/debug_io/debug_io.c
)

target_include_directories(app PRIVATE
    services/debug_io
) )
