/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Кольцевой журнал событий (ТЗ 4.2) на FCB во внешней FLASH.
 * Подробнее — event_log.h.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/fs/fcb.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/timeutil.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "event_log.h"

LOG_MODULE_REGISTER(event_log, CONFIG_LOG_DEFAULT_LEVEL);

/* ------------------------------------------------------------------ *
 *  FCB на партиции journal (128 КБ = 32 сектора W25Q32 по 4 КБ)        *
 * ------------------------------------------------------------------ */

/* В этом дереве Zephyr (v4.4.99) старый FLASH_AREA_ID() убран: id партиции
 * даёт PARTITION_ID(label) — см. include/zephyr/storage/flash_map.h. */
#define JOURNAL_FA_ID		PARTITION_ID(journal_partition)
#define JOURNAL_SECTORS_MAX	32

/* Запись ТЗ 4.2: ровно 10 байт. Сериализуем вручную (LE), packed-структура
 * не нужна и не используется — не зависим от раскладки компилятора. */
#define RECORD_LEN		10
#define RECORD_CRC_POLY		0x07	/* CRC-8-CCITT */
#define RECORD_CRC_INIT		0x00

static struct flash_sector journal_sectors[JOURNAL_SECTORS_MAX];

static struct fcb journal_fcb = {
	.f_magic = 0x4A524E4C,		/* "JRNL" */
	.f_version = 1,			/* версия формата записей */
	.f_scratch_cnt = 0,		/* все сектора под данные: при заполнении
					 * FCB ротирует (стирает старейший) */
	.f_sectors = journal_sectors,
	.f_sector_cnt = 0,		/* заполнит event_log_init() */
};

static bool journal_ready;

/* ------------------------------------------------------------------ *
 *  Очередь событий: write может зваться из ISR (напр. LoRa DIO0) —     *
 *  запись во флеш (стирание при ротации, ожидание готовности чипа)     *
 *  выполняется только из контекста потока, в system workqueue.         *
 * ------------------------------------------------------------------ */

struct pending_event {
	uint8_t type;
	uint32_t data;
};

K_MSGQ_DEFINE(evt_q, sizeof(struct pending_event), 16, 4);

static struct k_work flush_work;

static uint32_t dropped_events;

/* ------------------------------------------------------------------ *
 *  Метка времени: RTC (UNIX-сек), фолбэк — uptime                      *
 * ------------------------------------------------------------------ */

uint32_t event_log_timestamp(void)
{
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(rtc))
	const struct device *rtc = DEVICE_DT_GET(DT_NODELABEL(rtc));
	struct rtc_time rt;

	if (device_is_ready(rtc) && rtc_get_time(rtc, &rt) == 0) {
		/* rtc_time 1-к-1 отображается на struct tm; timegm даёт
		 * UNIX-сек независимо от эпохи старта RTC (без синхронизации
		 * значения монотонны, календарно фиктивны). */
		return (uint32_t)timeutil_timegm(rtc_time_to_tm(&rt));
	}
#endif
	return (uint32_t)(k_uptime_get() / 1000);
}

/* ------------------------------------------------------------------ *
 *  Сериализация записи (10 Б: ts LE32 | type | data LE32 | CRC8)       *
 * ------------------------------------------------------------------ */

static void record_serialize(uint32_t ts, uint8_t type, uint32_t data,
			     uint8_t raw[RECORD_LEN])
{
	sys_put_le32(ts, &raw[0]);
	raw[4] = type;
	sys_put_le32(data, &raw[5]);
	raw[9] = crc8(raw, RECORD_LEN - 1, RECORD_CRC_POLY, RECORD_CRC_INIT,
		      false);
}

static bool record_crc_ok(const uint8_t raw[RECORD_LEN])
{
	return raw[9] == crc8(raw, RECORD_LEN - 1, RECORD_CRC_POLY,
			      RECORD_CRC_INIT, false);
}

/* ------------------------------------------------------------------ *
 *  Запись в FCB (вызывается только из system workqueue)                *
 * ------------------------------------------------------------------ */

static int journal_append(uint32_t ts, uint8_t type, uint32_t data)
{
	uint8_t raw[RECORD_LEN];
	struct fcb_entry loc;
	int rc;

	record_serialize(ts, type, data, raw);

	rc = fcb_append(&journal_fcb, RECORD_LEN, &loc);
	if (rc == -ENOSPC) {
		/* Кольцо заполнено — ротируем (стирается старейший сектор)
		 * и повторяем. */
		rc = fcb_rotate(&journal_fcb);
		if (rc != 0) {
			return rc;
		}
		rc = fcb_append(&journal_fcb, RECORD_LEN, &loc);
		if (rc != 0) {
			return rc;
		}
	} else if (rc != 0) {
		return rc;
	}

	rc = flash_area_write(journal_fcb.fap, FCB_ENTRY_FA_DATA_OFF(loc),
			      raw, RECORD_LEN);
	if (rc != 0) {
		return rc;
	}

	return fcb_append_finish(&journal_fcb, &loc);
}

static void journal_flush_handler(struct k_work *work)
{
	struct pending_event ev;

	ARG_UNUSED(work);

	while (k_msgq_get(&evt_q, &ev, K_NO_WAIT) == 0) {
		int rc = journal_append(event_log_timestamp(), ev.type,
					ev.data);

		if (rc != 0) {
			LOG_ERR("Запись события 0x%02x в журнал: %d",
				ev.type, rc);
		}
	}

	if (dropped_events != 0) {
		LOG_WRN("Потеряно событий (очередь полна): %u",
			dropped_events);
		dropped_events = 0;
	}
}

void event_log_write(uint8_t type, uint32_t data)
{
	struct pending_event ev = { .type = type, .data = data };

	if (!journal_ready) {
		return;
	}

	/* K_NO_WAIT: из ISR блокироваться нельзя; при переполнении очереди
	 * событие теряется (счётчик сбросов выводится при следующем flush). */
	if (k_msgq_put(&evt_q, &ev, K_NO_WAIT) != 0) {
		dropped_events++;
		return;
	}

	k_work_submit(&flush_work);
}

/* ------------------------------------------------------------------ *
 *  Дамп журнала (fcb_walk идёт от старейшей записи к новой)            *
 * ------------------------------------------------------------------ */

static const char *event_type_str(uint8_t type)
{
	switch (type) {
	case EV_BOOT:		return "boot";
	case EV_BUTTON:		return "button";
	case EV_FLASH_ERR:	return "flash_err";
	case EV_DATA_WRITE:	return "data_write";
	case EV_LORA_RX:	return "lora_rx";
	case EV_BLE_CONN:	return "ble_conn";
	case EV_BLE_DISC:	return "ble_disc";
	default:		return "?";
	}
}

struct dump_ctx {
	const struct shell *sh;
	uint32_t total;		/* всего валидных записей */
	uint32_t idx;		/* индекс текущей (от старейшей) */
	uint32_t skip;		/* сколько пропустить с начала */
	uint32_t printed;
	uint32_t corrupt;
};

static int dump_walk_cb(struct fcb_entry_ctx *loc_ctx, void *arg)
{
	struct dump_ctx *ctx = arg;
	uint8_t raw[RECORD_LEN];

	if (flash_area_read(loc_ctx->fap, FCB_ENTRY_FA_DATA_OFF(loc_ctx->loc),
			    raw, RECORD_LEN) != 0) {
		ctx->corrupt++;
		ctx->idx++;
		return 0;
	}

	if (!record_crc_ok(raw)) {
		ctx->corrupt++;
		ctx->idx++;
		return 0;
	}

	if (ctx->idx >= ctx->skip && ctx->sh != NULL) {
		uint32_t ts = sys_get_le32(&raw[0]);
		uint32_t data = sys_get_le32(&raw[5]);

		shell_print(ctx->sh,
			    "[%u] ts=%u type=0x%02x (%s) data=0x%08x",
			    ctx->idx, ts, raw[4],
			    event_type_str(raw[4]), data);
		ctx->printed++;
	}

	ctx->idx++;
	ctx->total++;
	return 0;
}

/* ------------------------------------------------------------------ *
 *  Shell: journal dump [N] | journal clear                             *
 * ------------------------------------------------------------------ */

#ifdef CONFIG_SHELL

static int cmd_journal_dump(const struct shell *sh, size_t argc, char **argv)
{
	struct dump_ctx ctx = { .sh = NULL, .skip = 0 };
	uint32_t n = 20;
	int rc;

	if (!journal_ready) {
		shell_error(sh, "Журнал не инициализирован (FLASH?)");
		return -ENODEV;
	}

	if (argc == 3) {
		/* argc: argv[0]="dump", argv[1]=N, argv[2]="journal"
		 * (shell передаёт аргументы от вложенной команды к корню). */
		n = (uint32_t)strtoul(argv[1], NULL, 0);
		if (n == 0) {
			n = 1;
		}
	}

	/* Проход 1: посчитать валидные записи. */
	rc = fcb_walk(&journal_fcb, NULL, dump_walk_cb, &ctx);
	if (rc != 0) {
		shell_error(sh, "fcb_walk: %d", rc);
		return rc;
	}

	shell_print(sh, "Журнал: %u записей (повреждено CRC: %u), "
			"секторов: %u. Последние %u:",
			ctx.total, ctx.corrupt, journal_fcb.f_sector_cnt,
			MIN(n, ctx.total));

	if (ctx.total == 0) {
		return 0;
	}

	/* Проход 2: печать последних n (пропускаем первые total-n). */
	uint32_t total = ctx.total;

	ctx.idx = 0;
	ctx.total = 0;
	ctx.corrupt = 0;
	ctx.printed = 0;
	ctx.sh = sh;
	ctx.skip = (total > n) ? (total - n) : 0;

	rc = fcb_walk(&journal_fcb, NULL, dump_walk_cb, &ctx);
	if (rc != 0) {
		shell_error(sh, "fcb_walk (проход 2): %d", rc);
	}

	return rc;
}

static int cmd_journal_clear(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!journal_ready) {
		shell_error(sh, "Журнал не инициализирован (FLASH?)");
		return -ENODEV;
	}

	rc = fcb_clear(&journal_fcb);
	if (rc != 0) {
		shell_error(sh, "fcb_clear: %d", rc);
		return rc;
	}

	shell_print(sh, "Журнал стёрт");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_journal,
	/* mandatory=2: "dump" + родительское "journal" (shell передаёт их
	 * в argv), optional=1: N. Хэндлер: argc==3 значит N задан. */
	SHELL_CMD_ARG(dump, NULL,
		      "Последние N записей (умолчание 20): journal dump [N]",
		      cmd_journal_dump, 2, 1),
	SHELL_CMD(clear, NULL, "Стереть журнал (fcb_clear)", cmd_journal_clear),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(journal, &sub_journal,
		   "Журнал событий (внешняя FLASH, ТЗ 4.2)", NULL);

#endif /* CONFIG_SHELL */

/* ------------------------------------------------------------------ *
 *  Инициализация                                                       *
 * ------------------------------------------------------------------ */

int event_log_init(void)
{
	uint32_t cnt = ARRAY_SIZE(journal_sectors);
	int rc;

	rc = flash_area_get_sectors(JOURNAL_FA_ID, &cnt, journal_sectors);
	if (rc != 0 && rc != -ENOMEM) {
		LOG_ERR("Геометрия партиции journal: %d", rc);
		return rc;
	}
	journal_fcb.f_sector_cnt = cnt;

	rc = fcb_init(JOURNAL_FA_ID, &journal_fcb);
	if (rc != 0) {
		LOG_ERR("FCB init на партиции journal: %d", rc);
		return rc;
	}

	k_work_init(&flush_work, journal_flush_handler);
	journal_ready = true;

	/* Оценка ёмкости: 10 Б записи + накладные FCB (~6 Б на запись). */
	LOG_INF("Журнал событий готов: %u секторов × 4 КБ, ёмкость ~%u записей",
		journal_fcb.f_sector_cnt,
		journal_fcb.f_sector_cnt * 4096u / 16u);

	return 0;
}

