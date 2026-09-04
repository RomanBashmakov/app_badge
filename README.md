# app_badge — ВПО бейджа на Zephyr RTOS

Бейдж: **BLE + LoRa SX1272 + кнопка sw0 + светодиод led0**, с фокусом на
энергопотребление. Отладочная плата — **WeAct Studio STM32WB55 Core Board**.

Проект — слияние двух проверенных на этом железе проектов:

| Источник | Что взято |
|---|---|
| `coconut_vibe` | BLE (on-chip радио CPU2), PM/STOP2, RTT-лог, кнопка+LED, таймер сна |
| `zephyr_tutorial` | LoRa SX1272 через подсистему Zephyr `LORA` (backend loramac-node), распиновка SPI1 |

---

## Возможности

- **Bluetooth LE** (радио CPU2, драйвер `CONFIG_BT_STM32_IPM`): реклама,
  подключение, шифрование (SMP), bonding в NVS между перезагрузками.
  GATT-профиль данных бейджа — следующий этап.
- **LoRa SX1272**: приёмник 868 МГц (BW 125 кГц, SF 7, CR 4/5, CRC),
  непрерывный асинхронный приём (RXCONTINUOUS) по прерыванию DIO0.
- **Энергосбережение** в трёх уровнях (см. ниже).
- **RTT-лог**: консоль на Channel 0, логи на Channel 1.
- **Кнопка sw0** (PH3) и **светодиод led0** (PE4).

## Железо

- **Плата**: WeAct Studio STM32WB55 Core Board
- **Чип**: STM32WB55CGU6 (Cortex-M4 + Cortex-M0+, 1 MB Flash, 256 KB RAM)
- **Target Zephyr**: `weact_stm32wb55_core`

### Распиновка

| Компонент | Порт | Примечание |
|-----------|------|-----------------------------------------|
| Кнопка `sw0` (BOOT) | PH3 | базовый DTS платы (`boot_button`) |
| Светодиод `led0` | PE4 | базовый DTS платы |
| SX1272 `SCK` | PA5 | `spi1_sck_pa5` |
| SX1272 `MISO` | PB4 | `spi1_miso_pb4` |
| SX1272 `MOSI` | PB5 | `spi1_mosi_pb5` |
| SX1272 `CS` | PA4 | программный CS (`cs-gpios`) |
| SX1272 `RESET` | PB3 | активный высокий (open-drain) |
| SX1272 `DIO0` | PC13 | RxDone — будит CPU1 из STOP2 |
| SX1272 `DIO1..DIO3` | PC14, PC15, PC0 | задел: CAD/Timeout |

Узел в `app.overlay`: `&spi1 → sx1272` (`compatible = "semtech,sx1272"`,
alias `lora0`). Неиспользуемая периферия (`i2c1`, `usart1`, `spi2`)
отключена для снижения тока.

## Энергосбережение

1. **CPU1 (M4) → STOP2** при idle: `CONFIG_PM` + `CONFIG_PM_DEVICE` +
   `CONFIG_TICKLESS_KERNEL`, `k_sleep(K_FOREVER)` в `main()`.
   Пробуждение: IPCC (BLE от CPU2), EXTI кнопки (PH3), EXTI DIO0 (PC13,
   входящий LoRa-пакет) — LoRa-приём в сне продолжается.
2. **BLE connection parameters**: после подключения радио CPU2
   просыпается раз в ~4 секунды (interval 400 мс, latency 9).
3. **Программный сон**: через 10 секунд (`SLEEP_TIMEOUT_SEC`) без
   нажатий останавливается BLE-реклама; LoRa-приём не останавливается.
   Нажатие кнопки будит BLE.

## Структура проекта

```
.
├── prj.conf           — конфигурация Zephyr (LoRa, PM, RTT, BLE)
├── app.overlay        — devicetree: SX1272 на SPI1, отключение периферии
├── CMakeLists.txt     — сборка; include radio.h из loramac-node
├── scripts/zw         — обёртка west в docker zephyr-build
├── .vscode/           — задачи сборки, IntelliSense, отладка (OpenOCD)
└── src/
    ├── main.c         — точка входа: init + таймер сна
    ├── ble.c / ble.h  — BLE: реклама, подключение, сон/пробуждение
    ├── button.c/.h    — кнопка sw0 + led0 (EXTI + антидребезг)
    └── lora.c / lora.h — SX1272: конфиг модема, диагностика, приём
```

## Сборка и прошивка

Требования: docker-образ `zephyrprojectrtos/zephyr-build` (уже скачан).

```bash
# из каталога app_badge (или задачами VS Code: Terminal → Run Task)
./scripts/zw west build -b weact_stm32wb55_core .   # первичная сборка
./scripts/zw west build                              # пересборка
./scripts/zw west build -p always .                  # с нуля
./scripts/zw --usb west flash                        # прошивка (ST-Link)
./scripts/zw bash                                    # оболочка в контейнере
```

После пересборки — задача `Remap compile_commands paths (Docker → Host)`
(пути `/workdir` → хост для IntelliSense).

## Отладка (VS Code)

- Расширение **cortex-debug**; конфигурация `Badge Debug (OpenOCD + ST-Link)`
  в `.vscode/launch.json`.
- На хосте должны быть: `openocd` (есть, `/usr/bin/openocd`) и
  **`gdb-multiarch`** (нет — установить: `sudo apt install gdb-multiarch`).
- `substitutePath: /workdir → /home/roman/Projects/Badge/Zephyr_Badge`
  переводит пути отладочной информации (собрано в docker) в пути хоста.
- SVD-файл периферии: `.vscode/STM32WB55_CM4.svd`.
- Логи: RTT Channel 0 (консоль) / Channel 1 (LOG_*), например через
  J-Link RTT Viewer или RTT-сервер openocd.

## Workspace и git

`app_badge` — самостоятельный git-репозиторий внутри west-workspace
(`~/Projects/Badge/Zephyr_Badge`). Соседние каталоги `zephyr/`,
`modules/`, `tools/`, `bootloader/` — git-клоны, управляемые west
(руками не менять). Версия Zephyr на момент создания каркаса:
`main`, commit `8a4e180b93e` (`v4.4.0-14148-g8a4e180b93e`).
