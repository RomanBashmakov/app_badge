/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Бейдж: BLE + LoRa SX1272 + кнопка sw0 + светодиод led0.
 *
 * Архитектура (слияние coconut_vibe и zephyr_tutorial):
 *   - кнопка/LED: прерывание EXTI + антидребезг (button.c);
 *   - BLE: реклама + подключение, сон/пробуждение (ble.c);
 *   - LoRa: непрерывный приём 868 МГц, пакеты в callback (lora.c);
 *   - PM: CPU1 (M4) входит в STOP2 при idle.
 *
 * Энергосбережение:
 *   1) CONFIG_PM + CONFIG_TICKLESS_KERNEL: CPU1 входит в STOP2
 *      при idle. Пробуждение — IPCC (BLE от CPU2) и EXTI: кнопка
 *      (PH3) и DIO0 трансивера (PC13). LoRa-приём в сне продолжается.
 *   2) BLE connection parameters: радио CPU2 просыпается раз в ~4 сек
 *      (после подключения, параметры в prj.conf).
 *   3) Программный сон: через SLEEP_TIMEOUT_SEC без нажатий кнопки
 *      останавливается BLE-реклама. LoRa-приём НЕ останавливается —
 *      входящий LoRa-пакет будит бейдж. Нажатие кнопки будит BLE.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ble.h"
#include "button.h"
#include "lora.h"

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

/* Таймаут сна: 10 секунд без активности → останавливаем BLE-рекламу. */
#define SLEEP_TIMEOUT_SEC 10

/* Work для отложенного засыпания.
 * Сбрасывается при каждом нажатии кнопки (activity_callback).
 */
static void sleep_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sleep_work, sleep_work_handler);

/* Флаг состояния сна. */
static bool is_sleeping;

/* Вызывается при каждом нажатии кнопки (из button.c).
 * Сбрасывает таймер сна и будит BLE, если спит.
 */
static void on_button_activity(void)
{
	/* Сбрасываем таймер засыпания. */
	k_work_reschedule(&sleep_work, K_SECONDS(SLEEP_TIMEOUT_SEC));

	/* Если спали — просыпаемся. */
	if (is_sleeping) {
		is_sleeping = false;
		ble_wake();
	}
}

/* Вызывается по истечению SLEEP_TIMEOUT_SEC без активности. */
static void sleep_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!is_sleeping) {
		is_sleeping = true;
		ble_sleep();
	}
}

int main(void)
{
	int ret;

	LOG_INF("Старт бейджа \"%s\"", CONFIG_BT_DEVICE_NAME);

	/* --- Кнопка sw0 + светодиод led0 --- */
	ret = button_init();
	if (ret < 0) {
		LOG_ERR("Инициализация кнопки/светодиода не удалась: %d", ret);
	}

	/* Регистрируем callback активности для сброса таймера сна. */
	button_set_activity_callback(on_button_activity);

	/* --- LoRa SX1272: приёмник 868 МГц --- */
	ret = lora_init();
	if (ret < 0) {
		LOG_ERR("Инициализация LoRa не удалась: %d", ret);
	}

	/* Даём RTT-вьюеру время вывести логи инициализации,
	 * прежде чем BLE начнёт спамить HCI-сообщениями.
	 */
	LOG_INF("Жду 3 сек перед BLE init...");
	k_sleep(K_SECONDS(3));
	LOG_INF("Запускаю BLE init");

	/* --- Bluetooth LE --- */
	ret = ble_init();
	if (ret < 0) {
		LOG_ERR("Инициализация Bluetooth не удалась: %d", ret);
	}

	/* Запускаем таймер сна: через 10 сек без нажатий → засыпаем. */
	k_work_reschedule(&sleep_work, K_SECONDS(SLEEP_TIMEOUT_SEC));

	/* Основной поток больше не нужен: вся логика (кнопка, LoRa-приём,
	 * BLE) живёт в прерываниях и системной workqueue. Уходим в вечный
	 * сон: при CONFIG_PM + TICKLESS_KERNEL CPU1 войдёт в STOP2 и будет
	 * пробуждаться только IPCC (BLE от CPU2), EXTI кнопки (PH3) и
	 * EXTI DIO0 трансивера (PC13, входящий LoRa-пакет).
	 */
	k_sleep(K_FOREVER);

	return 0;
}
