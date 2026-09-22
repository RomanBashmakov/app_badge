/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Кольцевой журнал событий во внешней FLASH (W25Q32) — ТЗ 4.2.
 *
 * Запись фиксированного формата, 10 байт:
 *   timestamp 4 Б (UNIX, от RTC) | event_type 1 Б | event_data 4 Б | CRC8 1 Б
 *
 * Хранилище: FCB (Flash Circular Buffer) на партиции journal внешней
 * FLASH — append-only кольцевой буфер с CRC на каждую запись и
 * автоматической ротацией секторов (амортизация износа). При заполнении
 * стирается самый старый сектор; восстановление после сбоя питания
 * штатное (FCB ищет последнюю валидную запись).
 *
 * event_log_write() безопасна из любого контекста (в т.ч. из ISR:
 * событие ставится в очередь и пишется из system workqueue).
 *
 * Чтение: shell «journal dump [N]» / «journal clear» (RTT) и по BLE —
 * характеристика BAD6E004 (gatt_badge.c): дамп чанками + живой хвост.
 */

#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Длина записи журнала, байт (ts LE32 | type | data LE32 | CRC8). */
#define EV_LOG_RECORD_LEN 10u

/* Классификатор событий ТЗ 4.2: категории 0x01–0x36.
 *
 * Скелет: пока определены очевидные коды для готовых модулей; базовые
 * значения категорий «статусы/состояния носителя» — задел под алгоритмы
 * состояний (см. Badge_Obsidian/Работа/FLASH-хранение.md). Полный
 * классификатор из ТЗ догоним с модулями-источниками событий. */
enum badge_event {
	/* Категория «статусы носителя» 0x01–0x0F: задел */
	EV_WEARER_STATUS_BASE = 0x01,

	/* Категория «состояния носителя» 0x10–0x17: задел */
	EV_WEARER_STATE_BASE = 0x10,

	/* Категория «события устройства» 0x20–0x2E */
	EV_BOOT       = 0x20,	/* загрузка/перезапуск; data — задел: причина сброса */
	EV_BUTTON     = 0x21,	/* нажатие sw0; data = 1/0 (нажата/отжата) */
	EV_FLASH_ERR  = 0x22,	/* ошибка внешней FLASH; data = errno */
	EV_DATA_WRITE = 0x23,	/* запись пользовательских данных; data = id блока */

	/* Категория «связь» 0x30–0x36 */
	EV_LORA_RX    = 0x30,	/* LoRa-пакет; data = размер payload */
	EV_BLE_CONN   = 0x31,	/* BLE-подключение; data = 0 */
	EV_BLE_DISC   = 0x32,	/* BLE-отключение; data = причина (HCI err) */
};

/* Инициализация: FCB на партиции journal внешней FLASH.
 * Вызывать один раз из main() до первого event_log_write(). */
int event_log_init(void);

/* Записать событие (потокобезопасно, безопасно из ISR).
 * timestamp берётся из RTC (event_log_timestamp()). */
void event_log_write(uint8_t type, uint32_t data);

/* Текущая метка времени, UNIX-сек: RTC; если RTC недоступен —
 * фолбэк на uptime. Синхронизации времени пока нет: значения
 * монотонны от старта питания, календарно фиктивны до установки. */
uint32_t event_log_timestamp(void);

/* ------------------------------------------------------------------ *
 *  Чтение по BLE (BAD6E004, см. gatt_badge.c)                           *
 * ------------------------------------------------------------------ */

/* Прочитать записи [start, start+*out_count) в out (сырые 10-Б записи,
 * от старейшей к новой; индексное пространство — валидные записи,
 * повреждённые CRC пропускаются, как в shell dump).
 *
 *   out_size      — ёмкость out в байтах (кратна EV_LOG_RECORD_LEN;
 *                   чанк ограничен ещё и MTU на стороне gatt_badge);
 *   *out_count    — сколько записей положено (0, если start >= total);
 *   *out_more     — true, если за концом чанка есть ещё записи;
 *   *out_total    — всего валидных записей в журнале (мониторится
 *                   клиентом: уменьшение = ротация, индексы сместились).
 *
 * Потокобезопасно: FCB сериализуется мьютексом с записью/очисткой.
 * Возвращает 0 или -errno (-ENODEV до event_log_init()).
 */
int event_log_read_chunk(uint32_t start, uint8_t *out, size_t out_size,
			 uint8_t *out_count, bool *out_more,
			 uint32_t *out_total);

/* Стереть журнал (fcb_clear). Используется shell «journal clear» и
 * командой очистки по BLE. Возвращает 0 или -errno. */
int event_log_clear(void);

/* Колбэк «живого хвоста»: вызывается из system workqueue после каждой
 * успешной записи события с сырыми 10 байтами записи. Регистрирует
 * gatt_badge_init() — отправка BAD6E004 notify подписанному клиенту.
 * Вызов лёгкий: не блокировать (bt_gatt_notify не блокирует). */
typedef void (*event_log_live_cb_t)(const uint8_t rec[EV_LOG_RECORD_LEN]);

void event_log_set_live_cb(event_log_live_cb_t cb);

#endif /* EVENT_LOG_H */
