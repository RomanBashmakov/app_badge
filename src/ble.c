/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bluetooth LE бейджа (упрощение coconut_vibe: без HID-профиля).
 *
 * Модуль реализует:
 *   - рекламу (connectable) с именем устройства из CONFIG_BT_DEVICE_NAME;
 *   - обработку подключений/отключений и безопасности;
 *   - программный «сон»: остановку рекламы для экономии тока.
 *
 * Имя устройства задаётся в prj.conf: CONFIG_BT_DEVICE_NAME="Badge".
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/settings/settings.h>

#include "ble.h"

LOG_MODULE_REGISTER(ble, CONFIG_LOG_DEFAULT_LEVEL);

/* Активное подключение. */
static struct bt_conn *default_conn;

/* Флаг режима сна: блокирует перезапуск рекламы в disconnected(). */
static bool is_sleeping;

/* ------------------------------------------------------------------ *
 *  Реклама и callbacks подключения                                    *
 * ------------------------------------------------------------------ */

/* Медленная реклама для снижения энергопотребления.
 * BT_LE_ADV_CONN_FAST_1 рекламирует каждые 30-60 мс — радио активно часто.
 * Медленный режим: интервал ~1.0-1.2 сек → радио просыпается редко.
 */
#define BT_LE_ADV_CONN_LOW_POWER \
	BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, \
			BT_GAP_ADV_SLOW_INT_MIN, \
			BT_GAP_ADV_SLOW_INT_MAX, \
			NULL)

/* Advertising data: флаги LE. */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS,
		      (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};

/* Scan response: полное имя устройства из CONFIG_BT_DEVICE_NAME. */
static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE,
		CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* Work для отложенного перезапуска рекламы после disconnect.
 * Нельзя вызывать bt_le_adv_start() прямо из callback disconnected(),
 * т.к. он выполняется в RX-потоке HCI до освобождения буферов -> -ENOMEM.
 */
static void adv_restart_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(adv_restart_work, adv_restart_work_handler);

static void adv_restart_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (default_conn != NULL) {
		/* Уже успели подключиться заново — реклама не нужна. */
		return;
	}

	int err = bt_le_adv_start(BT_LE_ADV_CONN_LOW_POWER,
				  ad, ARRAY_SIZE(ad),
				  sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Перезапуск рекламы не удался: %d", err);
	} else {
		LOG_INF("Реклама перезапущена");
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("Подключение не удалось: err 0x%02x %s",
			err, bt_hci_err_to_str(err));
		return;
	}

	if (default_conn) {
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	LOG_INF("Bluetooth подключён %s", bt_conn_dst_str(conn));

	default_conn = bt_conn_ref(conn);

	/* Запрашиваем шифрование (задел под канал данных бейджа). */
	if (bt_conn_set_security(conn, BT_SECURITY_L2)) {
		LOG_WRN("Не удалось задать уровень безопасности");
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Bluetooth отключён, причина 0x%02x %s",
		reason, bt_hci_err_to_str(reason));

	if (default_conn == conn) {
		bt_conn_unref(default_conn);
		default_conn = NULL;
	}

	/* Не перезапускаем рекламу в режиме сна.
	 * Перезапуск выполняем с задержкой, чтобы вызвать bt_le_adv_start()
	 * вне RX-потока HCI (иначе падает с -ENOMEM).
	 */
	if (is_sleeping) {
		return;
	}

	k_work_reschedule(&adv_restart_work, K_MSEC(20));
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	if (!err) {
		LOG_INF("Безопасность: уровень %u", level);
	} else {
		LOG_WRN("Сбой безопасности: уровень %u, err %s (%d)",
			level, bt_security_err_to_str(err), err);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

static void bt_ready(int err)
{
	if (err) {
		LOG_ERR("Bluetooth init failed: %d", err);
		return;
	}

	LOG_INF("Bluetooth инициализирован");

	/* При CONFIG_BT_SETTINGS=y стек не генерирует ID-адрес сам: он ожидает,
	 * что адрес и ключи bonding'а загружены из NVS. Без этого вызова
	 * bt_le_adv_start() падает с -EAGAIN (-11) и в логе видно:
	 *   "No ID address. App must call settings_load()".
	 * Обработчики настроек BT регистрируются внутри bt_enable(), поэтому
	 * settings_load() вызываем здесь, в bt_ready(), ДО старта рекламы.
	 */
	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		int sret = settings_load();
		if (sret) {
			LOG_ERR("settings_load() failed: %d", sret);
		}
	}

	/* Запускаем рекламу: подключаемую (CONNECTABLE) + scan response. */
	err = bt_le_adv_start(BT_LE_ADV_CONN_LOW_POWER,
			      ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Запуск рекламы не удался: %d", err);
		return;
	}

	LOG_INF("Реклама запущена: устройство видно как \"%s\"",
		CONFIG_BT_DEVICE_NAME);
}

/* ------------------------------------------------------------------ *
 *  Публичные функции                                                  *
 * ------------------------------------------------------------------ */

void ble_sleep(void)
{
	/* Устанавливаем флаг сна ДО disconnect, чтобы disconnected()
	 * callback не запустил рекламу заново.
	 */
	is_sleeping = true;

	/* Отключаем активное подключение, если есть.
	 * Радио CPU2 работает пока есть conn, поэтому рвём его.
	 * disconnected() callback сработает, но не перезапустит рекламу
	 * из-за флага is_sleeping.
	 */
	if (default_conn) {
		bt_conn_disconnect(default_conn,
				   BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}

	/* Отменяем отложенный перезапуск рекламы (если был запланирован). */
	k_work_cancel_delayable(&adv_restart_work);

	/* Останавливаем рекламу — радио CPU2 прекращает передачу пакетов.
	 * Это основной источник экономии тока в режиме сна.
	 */
	int err = bt_le_adv_stop();
	if (err) {
		LOG_ERR("bt_le_adv_stop() failed: %d", err);
	} else {
		LOG_INF("BLE засыпает: реклама остановлена");
	}
}

void ble_wake(void)
{
	/* Сбрасываем флаг сна. */
	is_sleeping = false;

	if (default_conn != NULL) {
		return;
	}

	/* Запускаем рекламу. Быстрый режим (FAST_1) для быстрого
	 * переподключения. После подключения хост применит медленные
	 * параметры из CONFIG_BT_PERIPHERAL_PREF_*.
	 */
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
				  ad, ARRAY_SIZE(ad),
				  sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("bt_le_adv_start() failed: %d", err);
	} else {
		LOG_INF("BLE проснулся: реклама запущена");
	}
}

int ble_init(void)
{
	int err = bt_enable(bt_ready);
	if (err) {
		LOG_ERR("Bluetooth init failed: %d", err);
	}
	return err;
}
