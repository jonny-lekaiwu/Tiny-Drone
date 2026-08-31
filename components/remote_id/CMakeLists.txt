# GB 46750-2025 mandatory-field broadcast encoder.
idf_component_register(SRCS "remote_id.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi crazyflie motors)
