cmake_minimum_required(VERSION 3.20.0)
set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/chest_node.overlay")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(chest_node)

target_sources(app PRIVATE
    #core/main.c
    test/ad8232_test.c
    drivers/ad8232/ad8232.c
)
target_include_directories(app PRIVATE
    core
    drivers/ad8232
)