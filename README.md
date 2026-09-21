# app_badge — ВПО бейджа на Zephyr RTOS

Бейдж: **BLE + LoRa SX1272 + акселерометр LIS2DH12 + кнопка sw0 +
светодиод led0**, с фокусом на энергопотребление. Отладочная плата —
**WeAct Studio STM32WB55 Core Board**.

Проект — слияние двух проверенных на этом железе проектов:

| Источник | Что взято |
|---|---|
| `coconut_vibe` | BLE (on-chip радио CPU2), PM/STOP2, RTT-лог, кнопка+LED, таймер сна |
| `zephyr_tutorial` | LoRa SX1272 через подсистему Zephyr `LORA` (backend loramac-node), распиновка SPI1 |

---

## Возможности

- **Bluetooth LE** (радио CPU2, драйвер `CONFIG_BT_STM32_IPM`): реклама
  (имя «Badge» + UUID сервиса BAD6E001), подключение, шифрование (SMP),
  bonding в NVS между перезагрузками.
  GATT-сервисы (контракт `android_badge/PROTOCOL.md`):

  | Сервис | UUID | Назначение |
  |---|---|---|
  | BAS | 0x180F | заряд (заглушка 100 % — мониторинга АКБ на плате нет) |
  | ESS | 0x181A/0x2A6E | температура (датчик кристалла LIS2DH12), notify |
  | DIS | 0x180A/0x2A26 | версия ПО («1.0.0») |
  | BAD6E001 | 128-бит, кастом | BAD6E002 аксель notify (6 байт mg); BAD6E003 конфиг read/write (12 байт, NVS `badge/cfg`) |
  | MCUmgr SMP | 8D53DC1D-… | OTA (upload/test/reset/confirm), info, reset |
- **LoRa SX1272**: приёмник 868 МГц (BW 125 кГц, SF 7, CR 4/5, CRC),
  непрерывный асинхронный приём (RXCONTINUOUS) по прерыванию DIO0.
- **Акселерометр LIS2DH12** (модуль RobotClass, I2C1): поллинг X/Y/Z
  раз в секунду в RTT-лог (этап 1); задел — триггеры по INT1:
  data-ready и детектор движения («разбудить бейдж от тряски»).
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
| LIS2DH12 `SCL` | PB8 | `i2c1_scl_pb8` (базовый DTS платы) |
| LIS2DH12 `SDA` | PB9 | `i2c1_sda_pb9` (базовый DTS платы) |
| LIS2DH12 `AD` (SA0) | → 3V3 | I2C-адрес **0x19** (на GND → 0x18) |
| LIS2DH12 `CS` | → 3V3 | CS=1: выбран I2C-режим чипа |
| LIS2DH12 `IT1` (INT1) | PB0 | задел: триггеры (этап 2) |

Узлы в `app.overlay`: `&spi1 → sx1272` (`compatible = "semtech,sx1272"`,
alias `lora0`) и `&i2c1 → lis2dh12` (`compatible = "st,lis2dh12",
"st,lis2dh"`, alias `accel0`). Неиспользуемая периферия (`usart1`,
`spi2`) отключена для снижения тока.

## Энергосбережение

> **Временно частично отключено**: `CONFIG_PM=n` — с включённым PM
> I2C-обмен с LIS2DH12 сыпется ошибками после загрузки (конфликт
> suspend/resume I2C1 при STOP2, нет sleep-pinctrl у i2c1 в DTS
> платы). Починить и вернуть — следующая задача. Подробности в
> `prj.conf`.

1. **CPU1 (M4) → STOP2** при idle: `CONFIG_PM` + `CONFIG_PM_DEVICE` +
   `CONFIG_TICKLESS_KERNEL`, `k_sleep(K_FOREVER)` в `main()`.
   Пробуждение: IPCC (BLE от CPU2), EXTI кнопки (PH3), EXTI DIO0
   (PC13, входящий LoRa-пакет) — LoRa-приём в сне продолжается.
2. **BLE connection parameters**: после подключения радио CPU2
   просыпается раз в ~4 секунды (interval 400 мс, latency 9).
3. **Программный сон**: через 10 секунд (`SLEEP_TIMEOUT_SEC`) без
   нажатий останавливается BLE-реклама; LoRa-приём не останавливается.
   Нажатие кнопки будит BLE.

## Акселерометр LIS2DH12

Модуль RobotClass «LIS2DH12 с QIIC» (на плате подпись `i2c:0x19`).
Чип — 3-осевой MEMS-акселерометр ST: измеряет ускорение по X/Y/Z,
в покое «видит» гравитацию (≈1g ≈ 9.81 м/с² по одной из осей), поэтому
пригоден для наклона/тряски/свободного падения. Диапазоны ±2/4/8/16g,
частота измерений (ODR) 1 Гц…5.4 кГц, встроенные детекторы событий
(движение, тап) с выходами INT1/INT2.

Подключение: `SCL→PB8`, `SDA→PB9`, `AD→3V3` (адрес 0x19),
`CS→3V3` (I2C-режим), `IT1→PB0` (этап 2), `IT2` не подключён.

«По-zephyr'ому»: чип описан в devicetree, штатный драйвер
`drivers/sensor/st/lis2dh` включается по факту узла в DT, приложение
работает через API подсистемы SENSOR — без обращения к регистрам:

- **Узел**: `&i2c1 → lis2dh12@19` (`compatible = "st,lis2dh12",
  "st,lis2dh"` — второй, базовый compatible нужен драйверу для поиска
  инстансов; alias `accel0`).
- **prj.conf**: `CONFIG_SENSOR`, `CONFIG_I2C`, `CONFIG_LIS2DH`
  (normal-режим 10 бит; диапазон и ODR — runtime, задаём из кода),
  `CONFIG_CBPRINTF_FP_SUPPORT` для печати `%f`.
- **src/accel.c**: `accel_init()` проверяет готовность устройства
  (драйвер на старте сам читает WHO_AM_I чипа), задаёт ±2g и 10 Гц
  через `sensor_attr_set()`; затем поток (период из конфига BAD6E003,
  по умолчанию 1 с) читает X/Y/Z и температуру кристалла
  (`sensor_sample_fetch` + `sensor_channel_get`, `SENSOR_CHAN_ALL`
  + `SENSOR_CHAN_DIE_TEMP`), пишет в RTT-лог и отдаёт замер через
  callback в `gatt_badge.c` (notify BAD6E002 + ESS):

        [accel] x=  156 y=-1250 z=  973 мг | T=25.50 °C

**Этап 2 (план)**: включить `CONFIG_LIS2DH_TRIGGER_GLOBAL_THREAD` и
перейти с поллинга на прерывания чипа по INT1 (PB0): data-ready
(`SENSOR_TRIG_DATA_READY`) и детектор движения (`SENSOR_TRIG_DELTA`
с `anym-on-int1`) — задел под пробуждение бейджа от тряски. Поллинг
1 Гц будит CPU1 каждую секунду; на триггерах CPU будет спать до события.

## Структура проекта

```
.
├── prj.conf           — конфигурация Zephyr (LoRa, PM, RTT, BLE, MCUmgr)
├── app.overlay        — devicetree: SX1272 на SPI1, отключение периферии
├── CMakeLists.txt     — сборка; include radio.h из loramac-node
├── scripts/zw         — обёртка west в docker zephyr-build
├── .vscode/           — задачи сборки, IntelliSense, отладка (OpenOCD)
└── src/
    ├── main.c         — точка входа: init + таймер сна
    ├── accel.c / accel.h — LIS2DH12: ±2g, поток телеметрии X/Y/Z + T кристалла
    ├── ble.c / ble.h  — BLE: реклама, подключение, сон/пробуждение
    ├── gatt_badge.c / gatt_badge.h — GATT-сервисы: ESS, BAD6E001, конфиг в NVS
    ├── button.c/.h    — кнопка sw0 + led0 (EXTI + антидребезг)
    └── lora.c / lora.h — SX1272: конфиг модема, диагностика, приём
```

## Сборка и прошивка (MCUboot + приложение)

Приложение собирается под MCUboot (`CONFIG_BOOTLOADER_MCUBOOT=y`):
приложение живёт в **slot0 0x0800C000**, бутлоадер — **0x08000000**
(партии уже описаны в DTS платы). OTA-образ — подписанный
`build/zephyr/app_update.bin`.

Требования: docker-образ `zephyrprojectrtos/zephyr-build` (уже скачан).

```bash
# === 1. MCUboot (один раз) ===
./scripts/zw west build -p always -b weact_stm32wb55_core \
    -d /workdir/app_badge/build_mcuboot /workdir/bootloader/mcuboot/boot/zephyr
./scripts/jlink.sh flashboot     # → 0x08000000

# === 2. Приложение ===
./scripts/zw west build -p always -b weact_stm32wb55_core .   # с нуля
./scripts/zw west build                                       # пересборка
./scripts/jlink.sh flash       # подписанный app_update.bin → slot0
./scripts/jlink.sh flashboot   # MCUboot → 0x08000000 (после сборки п.1)
./scripts/zw bash              # оболочка в контейнере
```

Подпись: ключ `bootloader/mcuboot/root-rsa-2048.pem` (RSA-2048,
девелоперский ключ модуля MCUboot — одинаковый у бутлоадера и
приложения; задан `CONFIG_MCUBOOT_SIGNATURE_KEY_FILE` в prj.conf,
путь относительно west topdir). `app_update.bin` подписывается
автоматически при сборке.

OTA с телефона: приложение `android_badge` (вкладка OTA) шлёт
`build/zephyr/app_update.bin` через SMP: upload → test → reset →
confirm. Логи MCUboot — на RTT (видны перед стартом приложения).
Прошивка — только через `jlink.sh` (`west flash` не учитывает
раскладку со слотами).

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
