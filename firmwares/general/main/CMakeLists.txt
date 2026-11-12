idf_component_register(
    SRCS
        "main.cpp" "link.cpp" "notify.cpp" "rpc.cpp" "events.cpp" "streams.cpp" "poller.cpp"
        "i2c_bus.cpp" "board.cpp" "power.cpp"
        "lcd.cpp" "gfx.cpp" "face.cpp" "modal.cpp" "display.cpp" "touch.cpp"
        "imu.cpp" "light.cpp" "rtc.cpp" "drivers/bmi270_config.c"
        "methods_sys.cpp" "methods_debug.cpp"
    INCLUDE_DIRS "."
    EMBED_FILES "assets/font16.fnt"
    REQUIRES
        esp_driver_usb_serial_jtag esp_driver_i2c esp_driver_spi esp_driver_tsens esp_lcd
        esp_app_format esp_hw_support esp_system esp_timer json log mbedtls
)
