idf_component_register(
    SRCS "main.cpp" "link.cpp" "notify.cpp" "rpc.cpp" "methods_sys.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_usb_serial_jtag esp_app_format esp_hw_support esp_system esp_timer json log
)
