/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Реализация GATT-сервисов данных жетона. Подробнее см. gatt_badge.h
 * и контракт android_badge/PROTOCOL.md.
 */

#include <errno.h>
#include <limits.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/bas.h>

#include <zephyr/settings/settings.h>

#include "accel.h"
#include "event_log.h"
#include "gatt_badge.h"

LOG_MODULE_REGISTER(gatt_badge, CONFIG_LOG_DEFAULT_LEVEL);

/* ------------------------------------------------------------------ *
 *  Конфигурация (BAD6E003): 12 байт LE, см. PROTOCOL.md                *
 * ------------------------------------------------------------------ */

#define BADGE_CFG_SIZE    12
#define BADGE_CFG_KEY     "badge/cfg"	/* ключ settings/NVS */
#define BADGE_CFG_SUBTREE "badge"	/* settings-поддерево */

struct badge_cfg {
	uint32_t owner_id;			/* 0..2^32-1 */
	uint16_t accel_odr_hz;			/* 1..200 (снап к ODR LIS2DH12) */
	uint16_t wake_threshold_mg;		/* 1..2000 (применение — этап 2) */
	uint16_t activity_threshold_mg;		/* 1..2000 (применение — этап 2) */
	uint16_t telemetry_period_s;		/* 1..3600 */
};

#define BADGE_CFG_DEFAULT {			\
	.owner_id = 0,				\
	.accel_odr_hz = 10,			\
	.wake_threshold_mg = 150,		\
	.activity_threshold_mg = 60,		\
	.telemetry_period_s = 1,		\
}

/* Поддерживаемые ODR LIS2DH12 в нормальном режиме (драйвер требует
 * ТОЧНОЕ совпадение, произвольное значение из конфига снаппим к
 * ближайшему поддерживаемому). */
static const uint16_t accel_odr_supported[] = { 1, 10, 25, 50, 100, 200 };

static uint16_t snap_odr(uint16_t hz)
{
	uint16_t best = accel_odr_supported[0];
	int best_diff = INT_MAX;

	for (size_t i = 0; i < ARRAY_SIZE(accel_odr_supported); i++) {
		int diff = (int)hz - (int)accel_odr_supported[i];

		if (diff < 0) {
			diff = -diff;
		}
		if (diff < best_diff) {
			best_diff = diff;
			best = accel_odr_supported[i];
		}
	}
	return best;
}

/* Текущая конфигурация. Доступ из BT RX-потока (read/write GATT) и из
 * system workqueue (применение/сохранение) — защита мьютексом
 * (K_MUTEX_DEFINE — статическая инициализация, k_mutex_init не нужен). */
static struct badge_cfg cfg = BADGE_CFG_DEFAULT;
static K_MUTEX_DEFINE(cfg_lock);
static bool cfg_save_pending;

/* Кэш последних замеров (для read-характеристик). */
static int16_t last_accel_mg[3];	/* X, Y, Z, милли-g */
static int16_t last_temp_c01;		/* 0.01 °C */

/* BLE-стек поднялся (bt_ready в ble.c → gatt_badge_bt_ready()).
 * До этого notify не отправляем — стека ещё нет (bt_is_enabled() в
 * этой версии Zephyr отсутствует, ведём флаг сами). */
static atomic_t bt_ready_flag = ATOMIC_INIT(0);

static void cfg_serialize(const struct badge_cfg *c, uint8_t raw[BADGE_CFG_SIZE])
{
	sys_put_le32(c->owner_id, &raw[0]);
	sys_put_le16(c->accel_odr_hz, &raw[4]);
	sys_put_le16(c->wake_threshold_mg, &raw[6]);
	sys_put_le16(c->activity_threshold_mg, &raw[8]);
	sys_put_le16(c->telemetry_period_s, &raw[10]);
}

static void cfg_deserialize(const uint8_t raw[BADGE_CFG_SIZE], struct badge_cfg *c)
{
	c->owner_id = sys_get_le32(&raw[0]);
	c->accel_odr_hz = sys_get_le16(&raw[4]);
	c->wake_threshold_mg = sys_get_le16(&raw[6]);
	c->activity_threshold_mg = sys_get_le16(&raw[8]);
	c->telemetry_period_s = sys_get_le16(&raw[10]);
}

static bool cfg_validate(const struct badge_cfg *c)
{
	return c->accel_odr_hz >= 1 && c->accel_odr_hz <= 200 &&
	       c->wake_threshold_mg >= 1 && c->wake_threshold_mg <= 2000 &&
	       c->activity_threshold_mg >= 1 && c->activity_threshold_mg <= 2000 &&
	       c->telemetry_period_s >= 1 && c->telemetry_period_s <= 3600;
}

/* ------------------------------------------------------------------ *
 *  Применение конфигурации                                             *
 *                                                                     *
 *  I2C (sensor_attr_set) и запись NVS не выполняем в BT RX-потоке —   *
 *  отложено в system workqueue. Вызывается:                            *
 *    - после записи BAD6E003 (write GATT);                             *
 *    - после settings_load() (commit-обработчик, загрузка из NVS).     *
 * ------------------------------------------------------------------ */

static void apply_cfg_work_handler(struct k_work *work);

static K_WORK_DEFINE(apply_cfg_work, apply_cfg_work_handler);

static void apply_cfg_work_handler(struct k_work *work)
{
	struct badge_cfg c;
	bool need_save;

	ARG_UNUSED(work);

	k_mutex_lock(&cfg_lock, K_FOREVER);
	c = cfg;
	need_save = cfg_save_pending;
	cfg_save_pending = false;
	k_mutex_unlock(&cfg_lock);

	if (accel_set_odr(c.accel_odr_hz) == 0) {
		LOG_INF("ODR %u Гц: применится в потоке опроса", c.accel_odr_hz);
	}
	accel_set_telemetry_period(c.telemetry_period_s);

	if (need_save) {
		uint8_t raw[BADGE_CFG_SIZE];
		int ret;

		cfg_serialize(&c, raw);
		ret = settings_save_one(BADGE_CFG_KEY, raw, sizeof(raw));
		if (ret) {
			LOG_ERR("Сохранение конфига в NVS не удалось: %d", ret);
		} else {
			LOG_INF("Конфиг сохранён в NVS (%s)", BADGE_CFG_KEY);
		}
	}
}

/* ------------------------------------------------------------------ *
 *  settings/NVS: поддерево "badge", ключ "cfg"                         *
 * ------------------------------------------------------------------ */

static int badge_settings_set(const char *name, size_t len,
			      settings_read_cb read_cb, void *cb_arg)
{
	uint8_t raw[BADGE_CFG_SIZE];
	struct badge_cfg c;
	ssize_t n;

	ARG_UNUSED(len);

	if (strcmp(name, "cfg") != 0) {
		return -ENOENT;
	}

	n = read_cb(cb_arg, raw, sizeof(raw));
	if (n != sizeof(raw)) {
		LOG_ERR("badge/cfg в NVS: неверная длина %zd (ожидалось %u)",
			(uint32_t)n, BADGE_CFG_SIZE);
		return -EINVAL;
	}

	cfg_deserialize(raw, &c);
	if (!cfg_validate(&c)) {
		LOG_WRN("badge/cfg в NVS не прошёл валидацию — игнорирую");
		return -EINVAL;
	}

	k_mutex_lock(&cfg_lock, K_FOREVER);
	cfg = c;
	k_mutex_unlock(&cfg_lock);

	LOG_INF("Конфиг загружен из NVS: owner=%u odr=%u wake=%u act=%u "
		"period=%u",
		(uint32_t)c.owner_id, c.accel_odr_hz, c.wake_threshold_mg,
		c.activity_threshold_mg, c.telemetry_period_s);
	return 0;
}

static int badge_settings_commit(void)
{
	/* settings_load() вызывается в bt_ready() (ble.c); применяем
	 * загруженную конфигурацию отложенно, вне BT RX-потока. */
	k_work_submit(&apply_cfg_work);
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(badge, BADGE_CFG_SUBTREE, NULL,
				badge_settings_set, badge_settings_commit, NULL);

/* ------------------------------------------------------------------ *
 *  ESS 0x181A: Temperature 0x2A6E (sint16, 0.01 °C, LE, notify)        *
 *                                                                     *
 *  Готового ESS-сервиса в Zephyr нет — используем SIG-UUID напрямую.   *
 * ------------------------------------------------------------------ */

/* Подписки клиентов отслеживает сам стек (CCC): bt_gatt_notify(NULL,…)
 * рассылает только подписанным и возвращает -ENOTCONN без подписчиков.
 * Обработчики cfg_changed ниже — только диагностика в логах. */

static void accel_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	LOG_INF("Подписка BAD6E002 (аксель): %s",
		(value & BT_GATT_CCC_NOTIFY) ? "вкл" : "выкл");
}

static void temp_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	LOG_INF("Подписка ESS 0x2A6E (температура): %s",
		(value & BT_GATT_CCC_NOTIFY) ? "вкл" : "выкл");
}

static ssize_t read_temp(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	uint8_t raw[2];

	k_mutex_lock(&cfg_lock, K_FOREVER);
	sys_put_le16((uint16_t)last_temp_c01, raw);
	k_mutex_unlock(&cfg_lock);

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 raw, sizeof(raw));
}

BT_GATT_SERVICE_DEFINE(ess_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_16(BT_UUID_ESS_VAL)),
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(BT_UUID_TEMPERATURE_VAL),
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ,
		read_temp, NULL, NULL),
	BT_GATT_CCC(temp_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* ------------------------------------------------------------------ *
 *  Кастомный сервис BAD6E001-1234-5678-ABCD-EF0123456789               *
 *    BAD6E002 — акселерометр (notify, 6 байт int16 mg LE);            *
 *    BAD6E003 — конфигурация (read + write with response, 12 байт).   *
 *  Байты 128-битного UUID в BT_UUID_DECLARE_128 — little-endian.       *
 * ------------------------------------------------------------------ */

/* База UUID для BT_UUID_DECLARE_128: массив — ПОЛНАЯ реверсия MSB-порядка
 * строки xxxx-1234-5678-ABCD-EF0123456789 (LSB первым; как в in-tree
 * mcumgr: 8D53DC1D-1DB7-4CD3-868B-8A527460AA84 = 84 aa 60 74 52 8a …).
 * Итог: bad6e00X-1234-5678-abcd-ef0123456789 (проверено bt_uuid_to_str). */
#define BADGE_UUID_BASE_LE \
	0x89, 0x67, 0x45, 0x23, 0x01, 0xef, 0xcd, 0xab, 0x78, 0x56, 0x34, 0x12
/* Первая группа строки (bad6e00X) идёт В КОНЦЕ массива. */
#define BADGE_SVC_UUID   BADGE_UUID_BASE_LE, 0x01, 0xe0, 0xd6, 0xba
#define BADGE_ACCEL_UUID BADGE_UUID_BASE_LE, 0x02, 0xe0, 0xd6, 0xba
#define BADGE_CFG_UUID   BADGE_UUID_BASE_LE, 0x03, 0xe0, 0xd6, 0xba
#define BADGE_JOURNAL_UUID BADGE_UUID_BASE_LE, 0x04, 0xe0, 0xd6, 0xba

static ssize_t read_accel(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  void *buf, uint16_t len, uint16_t offset)
{
	uint8_t raw[6];

	k_mutex_lock(&cfg_lock, K_FOREVER);
	sys_put_le16((uint16_t)last_accel_mg[0], &raw[0]);
	sys_put_le16((uint16_t)last_accel_mg[1], &raw[2]);
	sys_put_le16((uint16_t)last_accel_mg[2], &raw[4]);
	k_mutex_unlock(&cfg_lock);

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 raw, sizeof(raw));
}

static ssize_t read_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			void *buf, uint16_t len, uint16_t offset)
{
	uint8_t raw[BADGE_CFG_SIZE];

	k_mutex_lock(&cfg_lock, K_FOREVER);
	cfg_serialize(&cfg, raw);
	k_mutex_unlock(&cfg_lock);

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 raw, sizeof(raw));
}

static ssize_t write_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 const void *buf, uint16_t len, uint16_t offset,
			 uint8_t flags)
{
	const uint8_t *raw = buf;
	struct badge_cfg c;
	uint16_t snapped;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
		/* Фиксированный формат — long write не поддерживаем. */
		return BT_GATT_ERR(EINVAL);
	}

	if (offset != 0 || len != BADGE_CFG_SIZE) {
		LOG_WRN("write BAD6E003: offset=%u len=%u (ожидалось 0/%u)",
			offset, len, BADGE_CFG_SIZE);
		return BT_GATT_ERR(EINVAL);
	}

	cfg_deserialize(raw, &c);
	if (!cfg_validate(&c)) {
		LOG_WRN("write BAD6E003: значения вне диапазона "
			"(odr=%u wake=%u act=%u period=%u)",
			c.accel_odr_hz, c.wake_threshold_mg,
			c.activity_threshold_mg, c.telemetry_period_s);
		return BT_GATT_ERR(EINVAL);
	}

	/* ODR — только дискретные значения LIS2DH12. */
	snapped = snap_odr(c.accel_odr_hz);
	if (snapped != c.accel_odr_hz) {
		LOG_INF("ODR %u Гц снапнут к поддерживаемому %u Гц",
			c.accel_odr_hz, snapped);
		c.accel_odr_hz = snapped;
	}

	k_mutex_lock(&cfg_lock, K_FOREVER);
	cfg = c;
	cfg_save_pending = true;
	k_mutex_unlock(&cfg_lock);

	/* Успешный write-response = значения приняты; применение и
	 * сохранение в NVS — отложенно (system workqueue). */
	k_work_submit(&apply_cfg_work);

	LOG_INF("Новый конфиг: owner=%u odr=%u wake=%u act=%u period=%u",
		(uint32_t)c.owner_id, c.accel_odr_hz, c.wake_threshold_mg,
		c.activity_threshold_mg, c.telemetry_period_s);
	return len;
}

/* Значения характеристик в таблицах сервисов (для notify):
 * [0]=primary, [1]=declaration, [2]=значение характеристики. */
#define ACCEL_VAL_ATTR (&badge_svc.attrs[2])
#define TEMP_VAL_ATTR  (&ess_svc.attrs[2])
/* BAD6E004 в badge_svc после конфигурационной: [4]=decl cfg, [5]=cfg,
 * [6]=decl journal, [7]=значение journal. */
#define JOURNAL_VAL_ATTR (&badge_svc.attrs[7])

/* ------------------------------------------------------------------ *
 *  Журнал событий по BLE (BAD6E004, фаза 2)                             *
 *                                                                     *
 *  Write-запрос, 5 Б: [op u8][arg u32 LE]                              *
 *    op 0x01 — дамп журнала с записи №arg (0 = с старейшей)            *
 *    op 0x02 — очистить журнал                                         *
 *  Notify-ответы (только подписанным на BAD6E004):                     *
 *    op 0x01 чанк: [op][total u32][count u8][more u8][count×10 Б]      *
 *                  — записи ровно с запрошенного arg; count адаптивен  *
 *                  к MTU клиента (1 запись при MTU 23, 23 при 247);    *
 *                  more=1 → клиент запрашивает следующий чанк          *
 *    op 0x02 очистка завершена: [op][total u32 = 0]                    *
 *    op 0x03 живой хвост: [op][10 Б записи] — каждое новое событие     *
 *  total в каждом чанке: уменьшился = ротация журнала стёрла старейшие *
 *  секторы, индексы сместились → клиент перезапускает дамп с 0.        *
 * ------------------------------------------------------------------ */

#define JR_OP_DUMP_REQ    0x01
#define JR_OP_CLEAR_REQ   0x02
#define JR_OP_CHUNK       0x01
#define JR_OP_CLEARED     0x02
#define JR_OP_LIVE        0x03
#define JR_REQ_LEN        5	/* op u8 + arg u32 */

/* Чанк: [op][total u32][count u8][more u8] + записи по 10 Б.
 * Записей при ATT MTU 247: (247-3-7)/10 = 23. */
#define JR_CHUNK_HDR_LEN  7
#define JR_MAX_COUNT      23

static uint8_t jr_tx[JR_CHUNK_HDR_LEN + JR_MAX_COUNT * EV_LOG_RECORD_LEN];
/* Отдельный буфер live-пакета: журнал-чанк собирается тем же system
 * workqueue'ом (jr_work), но живой хвост уходит из flush_work —
 * workqueue один, вызовы сериализованы; отдельный буфер — от греха. */
static uint8_t jr_live[1 + EV_LOG_RECORD_LEN];

struct jr_request {
	uint8_t op;
	uint32_t arg;
	uint16_t mtu;		/* ATT MTU клиента на момент запроса */
};

static struct jr_request jr_req;
static struct k_work jr_work;

/* ATT MTU текущего соединения (у жетона оно одно). Прямой геттера
 * (bt_gatt_get_att_mtu) в этом дереве Zephyr нет — трекаем exchange
 * через bt_gatt_cb.att_mtu_updated; до обмена действует дефолт 23
 * (чанк = 1 запись), после 247 — 23 записи. */
static atomic_t att_mtu_val = ATOMIC_INIT(23);

static void journal_att_mtu_updated(struct bt_conn *conn, uint16_t tx,
				    uint16_t rx)
{
	ARG_UNUSED(conn);

	uint16_t eff = MIN(tx, rx);

	atomic_set(&att_mtu_val, (atomic_val_t)eff);
	LOG_INF("ATT MTU: %u", eff);
}

/* non-const: bt_gatt_cb_register() вешает структуру в свой список
 * (мутит поле node) — API требует именно struct bt_gatt_cb *. */
static struct bt_gatt_cb journal_gatt_cb = {
	.att_mtu_updated = journal_att_mtu_updated,
};

static void journal_ccc_changed(const struct bt_gatt_attr *attr,
				uint16_t value)
{
	ARG_UNUSED(attr);

	LOG_INF("Подписка BAD6E004 (журнал): %s",
		(value & BT_GATT_CCC_NOTIFY) ? "вкл" : "выкл");
}

static void journal_notify(const struct bt_gatt_attr *attr,
			   const void *data, uint16_t len)
{
	if (!atomic_get(&bt_ready_flag)) {
		return;
	}

	int err = bt_gatt_notify(NULL, attr, data, len);

	if (err && err != -ENOTCONN) {
		LOG_WRN("notify BAD6E004 не удался: %d", err);
	}
}

/* journal_work_handler и journal_live_cb — ниже, ПОСЛЕ
 * BT_GATT_SERVICE_DEFINE(badge_svc): используют JOURNAL_VAL_ATTR. */

static ssize_t write_journal(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len, uint16_t offset,
			     uint8_t flags)
{
	const uint8_t *raw = buf;

	ARG_UNUSED(attr);
	ARG_UNUSED(conn);

	if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
		return BT_GATT_ERR(EINVAL);
	}

	if (offset != 0 || len != JR_REQ_LEN) {
		LOG_WRN("write BAD6E004: offset=%u len=%u (ожидалось 0/%u)",
			offset, len, JR_REQ_LEN);
		return BT_GATT_ERR(EINVAL);
	}

	uint8_t op = raw[0];
	uint32_t arg = sys_get_le32(&raw[1]);

	if (op != JR_OP_DUMP_REQ && op != JR_OP_CLEAR_REQ) {
		LOG_WRN("write BAD6E004: неизвестный op 0x%02x", op);
		return BT_GATT_ERR(EINVAL);
	}

	jr_req.op = op;
	jr_req.arg = arg;
	jr_req.mtu = (uint16_t)atomic_get(&att_mtu_val);

	k_work_submit(&jr_work);
	return len;
}

BT_GATT_SERVICE_DEFINE(badge_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(BADGE_SVC_UUID)),
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(BADGE_ACCEL_UUID),
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ,
		read_accel, NULL, NULL),
	BT_GATT_CCC(accel_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(BADGE_CFG_UUID),
		BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
		read_cfg, write_cfg, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(BADGE_JOURNAL_UUID),
		BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_WRITE,
		NULL, write_journal, NULL),
	BT_GATT_CCC(journal_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* ------------------------------------------------------------------ *
 *  Обработчики BAD6E004 (после определения badge_svc: используют       *
 *  JOURNAL_VAL_ATTR).                                                  *
 * ------------------------------------------------------------------ */

/* Дамп/очистка по запросу — в system workqueue: walk FCB читает флеш
 * (десятки мс на полном журнале), BT RX-поток на это время не вешаем. */
static void journal_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (jr_req.op == JR_OP_CLEAR_REQ) {
		int rc = event_log_clear();

		if (rc != 0) {
			LOG_WRN("Очистка журнала по BLE: %d", rc);
		}

		jr_tx[0] = JR_OP_CLEARED;
		sys_put_le32(0, &jr_tx[1]);
		journal_notify(JOURNAL_VAL_ATTR, jr_tx, 5);
		return;
	}

	/* JR_OP_DUMP_REQ: ограничить чанк MTU запросившего клиента. */
	uint32_t room = (uint32_t)jr_req.mtu > 3 ? (jr_req.mtu - 3) : 20;
	uint32_t max_cnt = MIN((room - JR_CHUNK_HDR_LEN) / EV_LOG_RECORD_LEN,
			       JR_MAX_COUNT);
	uint8_t count = 0;
	bool more = false;
	uint32_t total = 0;
	size_t out_size = max_cnt * EV_LOG_RECORD_LEN;
	int rc = event_log_read_chunk(jr_req.arg, jr_tx + JR_CHUNK_HDR_LEN,
				      out_size, &count, &more, &total);

	if (rc != 0) {
		LOG_WRN("Чанк журнала по BLE (с %u): %d", jr_req.arg, rc);
		total = 0;
	}

	jr_tx[0] = JR_OP_CHUNK;
	sys_put_le32(total, &jr_tx[1]);
	jr_tx[5] = count;
	jr_tx[6] = more ? 1 : 0;

	journal_notify(JOURNAL_VAL_ATTR, jr_tx,
		       JR_CHUNK_HDR_LEN + count * EV_LOG_RECORD_LEN);
}

/* Живой хвост: регистрируется в gatt_badge_init(), вызывается из
 * flush_work (system workqueue) после каждой записи события. */
static void journal_live_cb(const uint8_t rec[EV_LOG_RECORD_LEN])
{
	jr_live[0] = JR_OP_LIVE;
	memcpy(&jr_live[1], rec, EV_LOG_RECORD_LEN);
	journal_notify(JOURNAL_VAL_ATTR, jr_live, sizeof(jr_live));
}

/* ------------------------------------------------------------------ *
 *  Публикация телеметрии (вызывается из потока опроса accel.c)         *
 * ------------------------------------------------------------------ */

void gatt_badge_publish_accel(int16_t x_mg, int16_t y_mg, int16_t z_mg)
{
	uint8_t raw[6];

	sys_put_le16((uint16_t)x_mg, &raw[0]);
	sys_put_le16((uint16_t)y_mg, &raw[2]);
	sys_put_le16((uint16_t)z_mg, &raw[4]);

	k_mutex_lock(&cfg_lock, K_FOREVER);
	last_accel_mg[0] = x_mg;
	last_accel_mg[1] = y_mg;
	last_accel_mg[2] = z_mg;
	k_mutex_unlock(&cfg_lock);

	/* bt_gatt_notify(NULL, ...) сам рассылает только подписанным (CCC)
	 * и возвращает -ENOTCONN без подписчиков. Собственный флаг
	 * подписки НЕ проверяем: при реконнекте бондед-пира CCC=1
	 * восстанавливается из NVS (CONFIG_BT_SETTINGS_CCC_LAZY_LOADING),
	 * повторная запись того же значения не вызывает cfg_changed —
	 * флаг оставался бы 0 и навсегда глушил notify, хотя стек считает
	 * клиента подписанным. Стек ведёт подписки сам. */
	if (!atomic_get(&bt_ready_flag)) {
		return;
	}

	int err = bt_gatt_notify(NULL, ACCEL_VAL_ATTR, raw, sizeof(raw));

	if (err && err != -ENOTCONN) {
		LOG_WRN("notify акселя не удался: %d", err);
	}
}

void gatt_badge_publish_temp(int16_t temp_c01)
{
	uint8_t raw[2];

	sys_put_le16((uint16_t)temp_c01, raw);

	k_mutex_lock(&cfg_lock, K_FOREVER);
	last_temp_c01 = temp_c01;
	k_mutex_unlock(&cfg_lock);

	/* См. комментарий в gatt_badge_publish_accel(): без флага
	 * подписки — решение принимает bt_gatt_notify(). */
	if (!atomic_get(&bt_ready_flag)) {
		return;
	}

	int err = bt_gatt_notify(NULL, TEMP_VAL_ATTR, raw, sizeof(raw));

	if (err && err != -ENOTCONN) {
		LOG_WRN("notify температуры не удался: %d", err);
	}
}

/* ------------------------------------------------------------------ *
 *  Публичные функции                                                   *
 * ------------------------------------------------------------------ */

/* Приём замера из accel.c: публикация в BAD6E002 + ESS. */
static void on_accel_telemetry(const struct accel_telemetry *t)
{
	gatt_badge_publish_accel(t->mg[0], t->mg[1], t->mg[2]);
	gatt_badge_publish_temp(t->temp_c01);
}

int gatt_badge_init(void)
{
	/* Callback телеметрии регистрируется ДО accel_init() (до старта
	 * потока опроса), чтобы не терять замеры и не гоняться за ними. */
	accel_set_telemetry_cb(on_accel_telemetry);

	k_work_init(&jr_work, journal_work_handler);
	bt_gatt_cb_register(&journal_gatt_cb);

	/* Живой хвост журнала в BAD6E004 (см. event_log.c flush_work). */
	event_log_set_live_cb(journal_live_cb);

	LOG_INF("GATT-сервисы жетона готовы: ESS 0x181A + BAD6E001 "
		"(аксель BAD6E002, конфиг BAD6E003, журнал BAD6E004, "
		"NVS %s)", BADGE_CFG_KEY);
	return 0;
}

void gatt_badge_bt_ready(void)
{
	/* BAS: мониторинга АКБ на отладочной плате нет — заглушка 100 %
	 * (реальный источник появится с ревизией платы). */
	int ret = bt_bas_set_battery_level(100);

	if (ret) {
		LOG_WRN("bt_bas_set_battery_level: %d", ret);
	}
	atomic_set(&bt_ready_flag, 1);
	LOG_INF("BAS: уровень 100%% (заглушка, без мониторинга АКБ)");
}


