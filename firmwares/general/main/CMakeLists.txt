idf_component_register(
    SRCS "main.cpp" "link.cpp" "notify.cpp" "rpc.cpp" "events.cpp" "streams.cpp" "poller.cpp" "i2c_bus.cpp" "power.cpp" "methods_sys.cpp" "methods_debug.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_usb_serial_jtag esp_driver_i2c esp_driver_tsens esp_app_format esp_hw_support esp_system esp_timer json log
)
