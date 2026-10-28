cmake_minimum_required(VERSION 3.20.0)
set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/chest_node.overlay")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(chest_node)

target_sources(app PRIVATE
    core/main.c
    applications/app_api.c
    applications/acq_thread.c
    applications/storage_thread.c
    applications/ble_thread.c
    drivers/ad8232/ad8232.c
    drivers/bmi323/bmi323_i2c.c
    services/signal_mux/signal_mux.c
    services/debug_io/debug_io.c
    services/storage_service/storage.c
    services/ble_service/nus.c
)
target_include_directories(app PRIVATE
    core
    applications
    drivers/ad8232
    drivers/bmi323
    services/signal_mux
    services/debug_io
    services/ble_service
    services/storage_service
)
