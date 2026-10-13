idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "."
  REQUIRES driver nvs_flash esp_driver_spi esp_driver_uart esp_driver_gpio esp_driver_i2c esp_timer
           esp_wifi esp_netif esp_event esp_eth mqtt lwip esp_http_server mdns)

# The web page (web/arad_monitor.html) is gzipped at build time and embedded in the firmware.
idf_build_get_property(python PYTHON)
set(page_src ${CMAKE_CURRENT_SOURCE_DIR}/../web/arad_monitor.html)
set(page_gz ${CMAKE_CURRENT_BINARY_DIR}/arad_page.html.gz)
add_custom_command(OUTPUT ${page_gz}
  COMMAND ${python} -c "import gzip,sys; open(sys.argv[2],'wb').write(gzip.compress(open(sys.argv[1],'rb').read(), 9, mtime=0))" ${page_src} ${page_gz}
  DEPENDS ${page_src} VERBATIM)
add_custom_target(arad_page DEPENDS ${page_gz})
add_dependencies(${COMPONENT_LIB} arad_page)
target_add_binary_data(${COMPONENT_LIB} ${page_gz} BINARY)
