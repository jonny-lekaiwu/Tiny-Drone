set(WIFI_REQUIRES crazyflie platform config esp_wifi web_ota)

if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND WIFI_REQUIRES espressif__esp32-camera espressif__esp_jpeg)
endif()

idf_component_register(SRCS "wifi_esp32.c"
                      INCLUDE_DIRS "." "include"
                      REQUIRES ${WIFI_REQUIRES}
                      EMBED_TXTFILES "web_remote.html")
