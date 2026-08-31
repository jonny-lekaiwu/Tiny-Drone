set(COMPONENT_SRCDIRS ".")
set(COMPONENT_ADD_INCLUDEDIRS ".")    
set(COMPONENT_EMBED_TXTFILES "index.html")
set(COMPONENT_REQUIRES "spi_flash nvs_flash esp_event esp_netif esp_wifi esp_http_client esp_http_server esp_https_ota app_update esp_timer") 
#set(COMPONENT_REQUIRES "driver") 
register_component()
 
component_compile_options(
    -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
    -DLODEPNG_NO_COMPILE_ERROR_TEXT
    -Os
    -Wno-unused-function
)
  
