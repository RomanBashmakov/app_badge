/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * GATT-сервисы данных жетона: телеметрия и конфигурация.
 *
 * Контракт с Android-приложением: android_badge/PROTOCOL.md.
 *
 * Состав (стандартные BAS/DIS/GAP включаются Kconfig в prj.conf):
 *   - ESS 0x181A / Temperature 0x2A6E, notify — в Zephyr нет готового
 *     ESS-сервиса (CONFIG_BT_ESS не существует), SIG-UUID описаны здесь;
 *     источник — термодатчик кристалла LIS2DH12 (грубая точность);
 *   - кастомный BAD6E001-1234-5678-ABCD-EF0123456789:
 *       BAD6E002 — акселерометр, notify, 6 байт int16 X/Y/Z mg, LE;
 *       BAD6E003 — конфигурация, read/write, 12 байт LE,
 *                  хранение в settings/NVS (ключ badge/cfg);
 *   - BAS 0x180F — заряд: пока заглушка 100 % (мониторинга АКБ нет).
 *
 * Порядок инициализации: gatt_badge_init() вызывается в main() ДО
 * ble_init() — обработчик settings должен быть зарегистрирован до
 * settings_load() в bt_ready() (см. ble.c).
 */

#ifndef GATT_BADGE_H
#define GATT_BADGE_H

/*
 * Регистрация callback'а телеметрии акселерометра (в gatt_badge
 * данные публикуются в BAD6E002/ESS) и settings-обработчика badge/cfg.
 *
 * Возвращает 0 при успехе.
 */
int gatt_badge_init(void);

/*
 * Вызывается из ble.c в bt_ready() ПОСЛЕ settings_load():
 * конфигурация из NVS применена, можно задавать стартовые значения
 * стандартных сервисов (BAS = 100 % заглушка).
 */
void gatt_badge_bt_ready(void);

/*
 * Публикация замера акселерометра: кэшируется (read BAD6E002) и
 * рассылается notify всем подписанным. Вызывается из потока опроса
 * акселерометра (accel.c); без подписчиков — только кэш.
 */
void gatt_badge_publish_accel(int16_t x_mg, int16_t y_mg, int16_t z_mg);

/*
 * Публикация температуры (ESS 0x2A6E, сотые доли °C, sint16 LE):
 * кэш + notify подписанным.
 */
void gatt_badge_publish_temp(int16_t temp_c01);

#endif /* GATT_BADGE_H */
