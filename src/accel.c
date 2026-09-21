/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Реализация работы с акселерометром LIS2DH12. Подробнее см. accel.h.
 *
 * «По-zephyr'ому» работа с сенсором выглядит так:
 *
 *   1) чип описан в devicetree (app.overlay: &lis2dh12 на &i2c1,
 *      compatible = "st,lis2dh12", "st,lis2dh" — второй, «базовый»
 *      compatible нужен драйверу drivers/sensor/st/lis2dh, который
 *      ищет инстансы по DT_DRV_COMPAT st_lis2dh);
 *   2) драйвер включается по факту узла в DT (CONFIG_LIS2DH),
 *      сам конфигурирует чип и проверяет WHO_AM_I (ожидает 0x33);
 *   3) приложение работает только через стандартный API подсистемы
 *      SENSOR (include/zephyr/drivers/sensor.h):
 *        sensor_attr_set()     — параметры (диапазон, ODR, пороги);
 *        sensor_sample_fetch() — «забрать свежий замер у чипа»;
 *        sensor_channel_get()  — «дай последний замер» (в м/с²).
 *      Регистры чипа из приложения не трогаем вовсе.
 *
 * Поток опроса читает X/Y/Z и температуру кристалла (DIE_TEMP),
 * конвертирует в формат контракта PROTOCOL.md (милли-g, 0.01 °C) и
 * отдаёт через callback телеметрии (получатель — gatt_badge.c:
 * notify BAD6E002 + ESS 0x2A6E). Весь I2C — только из этого потока.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

#include "accel.h"

LOG_MODULE_REGISTER(accel, CONFIG_LOG_DEFAULT_LEVEL);

/* ------------------------------------------------------------------ *
 *  Devicetree-узел и константы                                        *
 * ------------------------------------------------------------------ */

/* Узел &lis2dh12 в app.overlay (модуль RobotClass на i2c1, 0x19). */
#define ACCEL_NODE DT_NODELABEL(lis2dh12)
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(ACCEL_NODE),
	     "Не найден узел акселерометра lis2dh12 в devicetree. "
	     "Проверьте app.overlay: узел lis2dh12@19 на &i2c1.");

/* Ссылка на device, который создаёт драйвер для этого узла.
 * DEVICE_DT_GET() — константа времени компиляции; device_is_ready()
 * в accel_init() всё равно нужен: драйвер мог отказаться подниматься
 * (чип не ответил на I2C-адресе или WHO_AM_I != 0x33).
 */
static const struct device *const accel = DEVICE_DT_GET(ACCEL_NODE);

/* Callback телеметрии: регистрируется один раз ДО accel_init()
 * (gatt_badge_init()), поэтому обычного указателя достаточно. */
static accel_telemetry_cb_t telemetry_cb;

/* Период опроса (с) — меняется на лету из system workqueue
 * (конфигурация BAD6E003), читается в начале каждого цикла. */
static atomic_t telemetry_period_s = ATOMIC_INIT(1);

/* Отложенная смена ODR: 0 = изменений нет, иначе значение применяется
 * в потоке опроса перед следующим замером (I2C — только из потока). */
static atomic_t odr_hz_pending = ATOMIC_INIT(0);

/* Поток опроса: создаётся «спящим» (SYS_FOREVER_MS — не попадает
 * в планировщик до k_thread_start()), запускается из accel_init()
 * только после успешной настройки чипа.
 *   стек 2048 — с запасом под sensor API + логирование;
 *   приоритет 10 — низкий: телеметрия не критична ко времени.
 */
static void accel_poll_thread(void *arg1, void *arg2, void *arg3);
K_THREAD_DEFINE(accel_poll_tid, 2048,
		accel_poll_thread, NULL, NULL, NULL,
		10, 0, SYS_FOREVER_MS);

/* ------------------------------------------------------------------ *
 *  Замер телеметрии                                                    *
 * ------------------------------------------------------------------ */

/* Один замер: X/Y/Z + температура кристалла → формат контракта
 * (милли-g, сотые °C). Вызывается ТОЛЬКО из потока опроса. */
static int accel_fetch_telemetry(struct accel_telemetry *t)
{
	struct sensor_value val[3];	/* X, Y, Z [м/с²] */
	struct sensor_value tval;	/* температура [°C] */
	int ret;

	/* sensor_sample_fetch() без канала = SENSOR_CHAN_ALL: X/Y/Z и,
	 * при CONFIG_LIS2DH_MEASURE_TEMPERATURE, температура кристалла. */
	ret = sensor_sample_fetch(accel);
	if (ret == -EBADMSG) {
		/* Замер «просрочен»: новое измерение пришло раньше, чем
		 * мы прочитали предыдущее. Для опроса не страшно —
		 * читаем то, что лежит сейчас. */
		ret = 0;
	}
	if (ret < 0) {
		LOG_ERR("sensor_sample_fetch() не удался: %d", ret);
		return ret;
	}

	ret = sensor_channel_get(accel, SENSOR_CHAN_ACCEL_XYZ, val);
	if (ret < 0) {
		LOG_ERR("sensor_channel_get(XYZ) не удался: %d", ret);
		return ret;
	}

	/* sensor_value — это val1 + val2/1000000 (fixed point), [м/с²].
	 * Пересчёт в милли-g делаем ЦЕЛОЧИСЛЕННО (cbprintf в логах
	 * надёжно пакует лишь первые несколько double-аргументов):
	 *   mg = (val1*1e6 + val2) / 9807  (9807 ≈ 9.80665 * 1000).
	 * int16 хватает с запасом: ±2g → ±2000 мг. */
	for (int i = 0; i < 3; i++) {
		int64_t um = (int64_t)val[i].val1 * 1000000 + val[i].val2;

		t->mg[i] = (int16_t)(um / 9807);
	}

	/* Температура кристалла (для ESS 0x2A6E): сотые доли °C.
	 * Точность грубая (датчик кристалла) — до появления внешнего
	 * термометра по ТЗ (ревизия платы). */
	t->temp_c01 = 0;
	ret = sensor_channel_get(accel, SENSOR_CHAN_DIE_TEMP, &tval);
	if (ret < 0) {
		LOG_WRN("sensor_channel_get(DIE_TEMP): %d", ret);
	} else {
		int32_t c01 = tval.val1 * 100 + tval.val2 / 10000;

		t->temp_c01 = (int16_t)CLAMP(c01, INT16_MIN, INT16_MAX);
	}

	return 0;
}

/* Применение отложенной смены ODR (вызывается только из потока опроса). */
static void apply_pending_odr(void)
{
	struct sensor_value attr;
	uint16_t odr;
	int ret;

	odr = (uint16_t)atomic_set(&odr_hz_pending, 0);
	if (odr == 0) {
		return;
	}

	attr.val1 = odr;
	attr.val2 = 0;
	ret = sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ,
			      SENSOR_ATTR_SAMPLING_FREQUENCY, &attr);
	if (ret < 0) {
		LOG_ERR("Не удалось задать ODR %u Гц: %d", odr, ret);
	} else {
		LOG_INF("[accel] ODR = %u Гц", odr);
	}
}

static void accel_poll_thread(void *arg1, void *arg2, void *arg3)
{
	struct accel_telemetry t;
	uint16_t period;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	LOG_INF("Поток телеметрии запущен (период по умолчанию %d с)",
		(int)atomic_get(&telemetry_period_s));

	while (true) {
		period = (uint16_t)atomic_get(&telemetry_period_s);
		if (period == 0) {
			period = 1;
		}

		k_sleep(K_SECONDS(period));

		apply_pending_odr();

		if (accel_fetch_telemetry(&t) == 0) {
			int16_t tc = t.temp_c01;
			int t_int = tc / 100;
			int t_frac = tc % 100;

			if (t_frac < 0) {
				t_frac = -t_frac;
			}

			LOG_INF("[accel] x=%5d y=%5d z=%5d мг | "
				"T=%d.%02d °C",
				t.mg[0], t.mg[1], t.mg[2], t_int, t_frac);

			if (telemetry_cb != NULL) {
				telemetry_cb(&t);
			}
		}
	}
}

/* ------------------------------------------------------------------ *
 *  Публичные функции                                                  *
 * ------------------------------------------------------------------ */

int accel_set_telemetry_cb(accel_telemetry_cb_t cb)
{
	/* Регистрируется один раз до старта потока опроса. */
	telemetry_cb = cb;
	return 0;
}

int accel_set_odr(uint16_t odr_hz)
{
	if (odr_hz == 0) {
		return -EINVAL;
	}

	/* Значение подхватывает поток опроса перед следующим замером. */
	atomic_set(&odr_hz_pending, odr_hz);
	return 0;
}

void accel_set_telemetry_period(uint16_t period_s)
{
	if (period_s == 0) {
		period_s = 1;
	}

	atomic_set(&telemetry_period_s, period_s);
}

int accel_init(void)
{
	struct sensor_value attr;
	int ret;

	if (!device_is_ready(accel)) {
		/* Чаще всего значит: чип не ответил на I2C-адресе или
		 * WHO_AM_I != 0x33. Проверить: SDA/SCL/питание модуля,
		 * адрес в app.overlay (0x19 при AD=3V3, 0x18 при AD=GND),
		 * что CS подтянут к 3V3 (I2C-режим чипа). */
		LOG_ERR("Акселерометр %s не готов: нет связи по I2C "
			"или неверный адрес", accel->name);
		return -ENODEV;
	}

	/* Диапазон ±2g: гравитация (1g) и небольшие ускорения — с
	 * запасом; при ±2g у чипа максимальная чувствительность.
	 * Атрибут задаётся в м/с², поэтому 2g переводим хелпером. */
	(void)sensor_g_to_ms2(2, &attr);
	ret = sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ,
			      SENSOR_ATTR_FULL_SCALE, &attr);
	if (ret < 0) {
		LOG_ERR("Не удалось задать диапазон ±2g: %d", ret);
		return ret;
	}

	/* ODR по умолчанию 10 Гц (= BAD6E003 default). Период опроса
	 * задаётся конфигурацией (по умолчанию 1 с); чем ниже ODR,
	 * тем меньше ток самого чипа. Значение по умолчанию может
	 * быть перекрыто конфигом из NVS (gatt_badge.c, commit). */
	attr.val1 = 10;
	attr.val2 = 0;
	ret = sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ,
			      SENSOR_ATTR_SAMPLING_FREQUENCY, &attr);
	if (ret < 0) {
		LOG_ERR("Не удалось задать ODR 10 Гц: %d", ret);
		return ret;
	}

	LOG_INF("[accel] LIS2DH12 готов: ±2g, ODR 10 Гц, "
		"опрос 1 с (до загрузки конфига)");

	/* Запускаем поток опроса (создан в приостановленном виде). */
	k_thread_start(accel_poll_tid);

	return 0;
}
