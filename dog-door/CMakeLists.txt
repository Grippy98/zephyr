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
  src/web_server.c
)

target_include_directories(app PRIVATE src)

set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated)

foreach(web_resource
  index.html
  app.css
  app.js
)
  generate_inc_file_for_target(app
    web/${web_resource}
    ${gen_dir}/${web_resource}.gz.inc
    --gzip
  )
endforeach()

zephyr_linker_sources(SECTIONS sections-rom.ld)
zephyr_linker_section(
  NAME http_resource_desc_dog_door_service
  KVMA RAM_REGION
  GROUP RODATA_REGION
)
