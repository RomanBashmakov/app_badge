# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(badge)

target_sources(app PRIVATE
    src/main.c
    src/ble.c
    src/button.c
    src/lora.c)

# Доступ к низкоуровневому API модуля loramac-node (radio.h: struct Radio_s)
# для прямого чтения/записи регистров SX1272 по SPI (диагностика в lora.c).
#
# ZEPHYR_LORAMAC_NODE_MODULE_DIR недоступна на этом этапе, поэтому вычисляем
# путь к модулю относительно ZEPHYR_BASE (ZEPHYR_BASE/../modules/...).
get_filename_component(WEST_TOPDIR "${ZEPHYR_BASE}/.." ABSOLUTE)
target_include_directories(app PRIVATE
    ${WEST_TOPDIR}/modules/lib/loramac-node/src/radio)
