cmake_minimum_required(VERSION 3.20.0)
set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/chest_node.overlay")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(calibrate_ad8232)

target_sources(app PRIVATE
    core/main.c
    drivers/ad8232/ad8232.c
    services/signal_mux/signal_mux.c
    services/calibration/calibration.c
)

target_include_directories(app PRIVATE
    drivers/ad8232
    services/signal_mux
    services/calibration
)
