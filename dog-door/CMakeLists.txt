# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
find_package(Python REQUIRED COMPONENTS Interpreter)

project(dog_door)

target_sources(app PRIVATE
  src/app_config.c
  src/door_controller.c
  src/led_controller.c
  src/main.c
  src/mqtt_bridge.c
  src/network_manager.c
  src/ota_manager.c
  src/web_server.c
)

target_include_directories(app PRIVATE src)

set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated)

# Serve one self-contained page from the device. Some mobile browsers cache a
# failed stylesheet or script request very aggressively, which can leave the
# dashboard unstyled across firmware updates. Inlining the assets also removes
# two extra HTTP transactions from the ESP32's small network stack.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html
  ${CMAKE_CURRENT_SOURCE_DIR}/web/app.css
  ${CMAKE_CURRENT_SOURCE_DIR}/web/app.js
)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html embedded_index_html)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/web/app.css embedded_app_css)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/web/app.js embedded_app_js)
string(REPLACE
  "<link rel=\"stylesheet\" href=\"/app.css\">"
  "<style>${embedded_app_css}</style>"
  embedded_index_html
  "${embedded_index_html}"
)
string(REPLACE
  "<script src=\"/app.js\"></script>"
  "<script>${embedded_app_js}</script>"
  embedded_index_html
  "${embedded_index_html}"
)
file(WRITE ${gen_dir}/index.embedded.html "${embedded_index_html}")

generate_inc_file_for_target(app
  ${gen_dir}/index.embedded.html
  ${gen_dir}/index.html.gz.inc
  --gzip
)

zephyr_linker_sources(SECTIONS sections-rom.ld)
zephyr_linker_section(
  NAME http_resource_desc_dog_door_service
  KVMA RAM_REGION
  GROUP RODATA_REGION
)
