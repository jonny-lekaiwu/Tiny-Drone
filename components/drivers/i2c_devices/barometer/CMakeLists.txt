idf_component_register(SRCS "barometer.c"
                       INCLUDE_DIRS "include"
                       REQUIRES i2c_bus log)
