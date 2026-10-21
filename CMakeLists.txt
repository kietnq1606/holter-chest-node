cmake_minimum_required(VERSION 3.20.0)
set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/chest_node.overlay")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(chest_node)

target_sources(app PRIVATE
    drivers/signal_mux/signal_mux.c
    drivers/debug_io/debug_io.c
    #core/main.c
    #test/w25n01gv_test.c
    test/ad8232_test.c
    services/storage_service/storage.c
    service/ble_service/nus.c

)
target_include_directories(app PRIVATE
    drivers/signal_mux
    drivers/debug_io
    core
    drivers/ad8232
    drivers/bmi323
    drivers/w25n
    services/ble_service
    services/storage_service
)
