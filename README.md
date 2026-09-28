# app_badge

Прошивка жетона ЭМИ-01 на **Zephyr RTOS v4.4** для STM32WB55: BLE, LoRa SX1272, акселерометр LIS2DH12, внешняя FLASH W25Q32, кнопка и LED. Плата — WeAct Studio STM32WB55 Core Board, бутлоадер MCUboot.

- **Сборка:** docker-образ `zephyrprojectrtos/zephyr-build`, west-workspace в родительской папке, плата `weact_stm32wb55_core`.
- **Прошивка/отладка:** J-Link (`scripts/jlink.sh flash|rtt|reset`), логи — RTT (SEGGER).

Полная документация (архитектура, протоколы, этапы работ, история) — во внутренней базе знаний проекта.
