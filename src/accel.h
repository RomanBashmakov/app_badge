/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Акселерометр LIS2DH12 (модуль RobotClass «LIS2DH12 с QIIC»,
 * I2C-адрес 0x19; узел &lis2dh12 на шине &i2c1 — app.overlay,
 * alias accel0). Встроенный датчик температуры кристалла —
 * CONFIG_LIS2DH_MEASURE_TEMPERATURE, канал SENSOR_CHAN_DIE_TEMP.
 *
 * Режим (этап 1+): поток опроса читает X/Y/Z (+ температуру кристалла)
 * с периодом telemetry_period_s из конфигурации BAD6E003 и отдаёт замер
 * наружу через callback (получатель — GATT-сервисы, gatt_badge.c).
 * ODR чипа и период опроса меняются на лету (без перезагрузки) —
 * accel_set_odr()/accel_set_telemetry_period(): вызываются из
 * system workqueue (gatt_badge.c), применения — в потоке опроса.
 *
 * Этап 2 (план): триггеры по INT1 чипа (пин PB0) — data-ready
 * («данные готовы», чип сам зовёт CPU) и детектор движения
 * (SENSOR_TRIG_DELTA) — задел под пробуждение бейджа от тряски.
 */

#ifndef ACCEL_H
#define ACCEL_H

/*
 * Один замер телеметрии.
 *   mg[3]      — ускорение X/Y/Z, милли-g (int16; ±2g → ±2000);
 *   temp_c01   — температура кристалла, сотые доли °C (int16);
 *                0, если CONFIG_LIS2DH_MEASURE_TEMPERATURE выключен.
 */
struct accel_telemetry {
	int16_t mg[3];
	int16_t temp_c01;
};

/* Тип callback'а телеметрии: вызывается из потока опроса акселерометра. */
typedef void (*accel_telemetry_cb_t)(const struct accel_telemetry *t);

/*
 * Инициализация акселерометра:
 *   - проверка готовности устройства (nodelabel lis2dh12 → &i2c1;
 *     если чип не ответил на WHO_AM_I, драйвер не создаст device);
 *   - установка диапазона ±2g и ODR по умолчанию 10 Гц через
 *     sensor_attr_set() (runtime-атрибуты подсистемы SENSOR);
 *   - запуск потока опроса (период по умолчанию 1 с).
 *
 * Возвращает 0 при успехе, отрицательный код ошибки при неудаче.
 */
int accel_init(void);

/*
 * Регистрация callback'а телеметрии. Вызывать ОДИН раз и ДО
 * accel_init(): callback начинает приходить с первым замером.
 */
int accel_set_telemetry_cb(accel_telemetry_cb_t cb);

/*
 * Смена ODR чипа (Гц). Значение — только из поддерживаемых LIS2DH12
 * (1/10/25/50/100/200; см. snap в gatt_badge.c). Применяется в потоке
 * опроса перед следующим замером — весь I2C только из потока опроса
 * (Zephyr I2C API не гарантирует потокобезопасность на устройство).
 * Возвращает 0 — значение принято; ошибки применения — в RTT-лог.
 */
int accel_set_odr(uint16_t odr_hz);

/*
 * Смена периода опроса (секунды, 1..3600). Вступает в силу со
 * следующего цикла потока опроса.
 */
void accel_set_telemetry_period(uint16_t period_s);

#endif /* ACCEL_H */

