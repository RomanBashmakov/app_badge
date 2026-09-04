/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Реализация работы с кнопкой sw0 и светодиодом led0.
 *
 * Подробнее см. button.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "button.h"

LOG_MODULE_REGISTER(button, CONFIG_LOG_DEFAULT_LEVEL);

/* ------------------------------------------------------------------ *
 *  Devicetree-узлы и константы                                        *
 * ------------------------------------------------------------------ */

/* sw0 = boot_button на PH3 (GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN). */
#define SW0_NODE DT_ALIAS(sw0)
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(SW0_NODE),
	     "Не найден узел кнопки (alias sw0) в devicetree. "
	     "Проверьте DTS платы: узел gpio-keys с алиасом sw0.");

/* Светодиод led0 (PE4). */
#define LED0_NODE DT_ALIAS(led0)

#define BUTTON_DEBOUNCE_MS 20

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* Callback активности: вызывается при каждом нажатии (для сброса
 * таймера сна в main.c).
 */
static void (*activity_callback)(void);

/* Debounce work. */
static struct k_work_delayable debounce_work;

static struct gpio_callback button_cb;

/* ------------------------------------------------------------------ *
 *  Обработка нажатий (антидребезг + LED + activity)                    *
 * ------------------------------------------------------------------ */

static void button_debounce_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int val = gpio_pin_get_dt(&button);
	if (val < 0) {
		LOG_ERR("Не удалось прочитать состояние кнопки: %d", val);
		return;
	}

	if (val) {
		gpio_pin_set_dt(&led, 1);
		LOG_INF("[sw0] нажата");
		/* Уведомляем main() о активности для сброса таймера сна. */
		if (activity_callback) {
			activity_callback();
		}
	} else {
		gpio_pin_set_dt(&led, 0);
		LOG_INF("[sw0] отжата");
	}
}

static void button_pressed(const struct device *dev, struct gpio_callback *cb,
			   uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_work_reschedule(&debounce_work, K_MSEC(BUTTON_DEBOUNCE_MS));
}

/* ------------------------------------------------------------------ *
 *  Публичные функции                                                  *
 * ------------------------------------------------------------------ */

void button_set_activity_callback(void (*callback)(void))
{
	activity_callback = callback;
}

int button_init(void)
{
	int ret;

	if (!device_is_ready(button.port)) {
		LOG_ERR("GPIO-устройство кнопки %s не готово",
			button.port->name);
		return -ENODEV;
	}

	if (!device_is_ready(led.port)) {
		LOG_ERR("GPIO-устройство светодиода %s не готово",
			led.port->name);
		return -ENODEV;
	}

	LOG_INF("[sw0] настройка: порт=%s pin=%d",
		button.port->name, button.pin);

	/* Светодиод: выход, погашен. */
	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Не удалось настроить светодиод: %d", ret);
		return ret;
	}

	/* Кнопка: вход. */
	ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("gpio_pin_configure_dt() failed: %d", ret);
		return ret;
	}

	/* Прерывание по обоим фронтам (нажатие/отжатие). */
	ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);
	if (ret < 0) {
		LOG_ERR("gpio_pin_interrupt_configure_dt() failed: %d", ret);
		return ret;
	}

	k_work_init_delayable(&debounce_work, button_debounce_handler);

	gpio_init_callback(&button_cb, button_pressed, BIT(button.pin));
	ret = gpio_add_callback(button.port, &button_cb);
	if (ret < 0) {
		LOG_ERR("gpio_add_callback() failed: %d", ret);
		return ret;
	}

	LOG_INF("[sw0] готова: прерывания + антидребезг %d мс",
		BUTTON_DEBOUNCE_MS);
	LOG_INF("Светодиод led0 готов: зажигается по нажатию");
	return 0;
}
