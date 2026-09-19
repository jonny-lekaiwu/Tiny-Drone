idf_component_register(SRCS "device_activation.c"
                       INCLUDE_DIRS "include"
                       REQUIRES nvs_flash esp_driver_usb_serial_jtag esp_hw_support vfs)
