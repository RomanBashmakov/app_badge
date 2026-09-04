/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bluetooth LE бейджа: реклама + подключение + сон/пробуждение.
 *
 * GATT-профиль данных бейджа (передача пакетов по BLE) — следующий
 * этап разработки; сейчас приложение только рекламируется и держит
 * подключение (задел: SMP + bonding, см. prj.conf).
 *
 * Имя устройства задаётся в prj.conf: CONFIG_BT_DEVICE_NAME="Badge".
 */

#ifndef BLE_H
#define BLE_H

/*
 * Инициализация Bluetooth LE:
 *   - bt_enable() с callback bt_ready;
 *   - settings_load() и запуск рекламы в bt_ready().
 *
 * Возвращает 0 при успехе, отрицательный код ошибки при неудаче
 * (аналогично bt_enable()).
 */
int ble_init(void);

/*
 * Останавливает BLE-рекламу для снижения энергопотребления.
 * Если есть активное подключение — отключает его.
 * Радио CPU2 прекращает передачу advertising-пакетов.
 */
void ble_sleep(void);

/*
 * Возобновляет BLE-рекламу после сна.
 */
void ble_wake(void);

#endif /* BLE_H */
