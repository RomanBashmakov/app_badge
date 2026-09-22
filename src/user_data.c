/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Пользовательские данные (ТЗ 4.3): littlefs /data + PSA AES-256-GCM.
 * Подробнее — user_data.h.
 */

#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include <psa/crypto.h>

#include "event_log.h"
#include "user_data.h"

LOG_MODULE_REGISTER(user_data, CONFIG_LOG_DEFAULT_LEVEL);

/* ------------------------------------------------------------------ *
 *  Константы формата чанков                                            *
 * ------------------------------------------------------------------ */

#define UD_KEY_LEN		32	/* AES-256 */
#define UD_NONCE_LEN		12	/* 96-бит GCM nonce */
#define UD_TAG_LEN		16	/* GCM-тег */
#define UD_AAD_LEN		6	/* magic+версия+алгоритм из заголовка */
#define UD_CHUNK_PLAIN_MAX	4096	/* макс. открытого текста в чанке */
#define UD_CIPHER_MAX		(UD_CHUNK_PLAIN_MAX + UD_TAG_LEN)

#define UD_MOUNT		"/data"
#define UD_SETTINGS_KEY		"sec/aeskey"

/* Заголовок чанка (18 Б): nonce уникален для каждого чанка (случайный,
 * 96 бит — вероятность повтора при наших объёмах записи ничтожна),
 * хранится открыто — это не секрет, секрет только ключ. */
struct ud_chunk_hdr {
	uint8_t magic[4];		/* "BDAT" */
	uint8_t version;		/* формат: 1 */
	uint8_t alg;			/* 1 = AES-256-GCM */
	uint8_t nonce[UD_NONCE_LEN];
} __packed;

BUILD_ASSERT(sizeof(struct ud_chunk_hdr) == 4 + 1 + 1 + UD_NONCE_LEN);

#define UD_HDR_MAGIC		{ 'B', 'D', 'A', 'T' }
#define UD_HDR_VERSION		1
#define UD_HDR_ALG_AES256_GCM	1

/* ------------------------------------------------------------------ *
 *  Состояние модуля                                                    *
 * ------------------------------------------------------------------ */

/* Буферы чанка: статические (не в стеке — вызовы могут идти из потоков
 * с ограниченным стеком), доступ под мьютексом. */
static uint8_t io_plain[UD_CHUNK_PLAIN_MAX];
static uint8_t io_cipher[UD_CIPHER_MAX];
static K_MUTEX_DEFINE(ud_lock);

static uint8_t ud_key_raw[UD_KEY_LEN];
static bool ud_key_loaded;	/* ключ найден в NVS */
static bool ud_ready;

/* PSA-дескриптор ключа (volatile: живёт в RAM, при старте импортируется
 * из NVS или генерится заново). */
static mbedtls_svc_key_id_t ud_key;

/* ------------------------------------------------------------------ *
 *  Ключ: NVS (settings sec/aeskey) ↔ PSA (volatile key)                *
 * ------------------------------------------------------------------ */

static int sec_settings_set(const char *name, size_t len,
			    settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "aeskey") != 0) {
		return -ENOENT;
	}

	if (len != UD_KEY_LEN) {
		LOG_ERR("sec/aeskey: длина %zu (ожидалось %u)", len,
			UD_KEY_LEN);
		return -EINVAL;
	}

	if (read_cb(cb_arg, ud_key_raw, UD_KEY_LEN) != UD_KEY_LEN) {
		return -EINVAL;
	}

	ud_key_loaded = true;
	return 0;
}

/* Поддерево "sec" отдельное от "badge" (gatt_badge.c): у settings один
 * статический обработчик на имя поддерева. */
SETTINGS_STATIC_HANDLER_DEFINE(sec, "sec", NULL, sec_settings_set,
			       NULL, NULL);

static void key_attrs_init(psa_key_attributes_t *attrs)
{
	psa_set_key_type(attrs, PSA_KEY_TYPE_AES);
	psa_set_key_bits(attrs, 256);
	psa_set_key_usage_flags(attrs, PSA_KEY_USAGE_ENCRYPT |
					PSA_KEY_USAGE_DECRYPT);
	psa_set_key_algorithm(attrs, PSA_ALG_GCM);
	/* lifetime по умолчанию — volatile: ключ живёт в RAM, при старте
	 * импортируется из NVS. */
}

/* ------------------------------------------------------------------ *
 *  Чанки: файлы /data/u<id>_<seq>.bd                                   *
 * ------------------------------------------------------------------ */

static void chunk_path(char *path, size_t size, enum user_data_id id,
		       unsigned seq)
{
	snprintf(path, size, UD_MOUNT "/u%u_%06u.bd", (unsigned)id, seq);
}

/* Зашифровать и записать чанк. Вызывается под ud_lock. */
static int chunk_write(enum user_data_id id, unsigned seq,
		       const uint8_t *plain, size_t plain_len)
{
	struct ud_chunk_hdr hdr = {
		.magic = UD_HDR_MAGIC,
		.version = UD_HDR_VERSION,
		.alg = UD_HDR_ALG_AES256_GCM,
	};
	char path[32];
	struct fs_file_t file;
	size_t cipher_len;
	psa_status_t st;
	int rc;

	__ASSERT(plain_len <= UD_CHUNK_PLAIN_MAX, "чанк больше буфера");

	/* Случайный nonce на чанк: повтор nonce при одном ключе для GCM
	 * фатален; 96 случайных бит при наших объёмах записи — запас
	 * колоссальный (парадокс дней рождения наступил бы при 2^48). */
	st = psa_generate_random(hdr.nonce, UD_NONCE_LEN);
	if (st != PSA_SUCCESS) {
		return -EIO;
	}

	st = psa_aead_encrypt(ud_key, PSA_ALG_GCM,
			      hdr.nonce, UD_NONCE_LEN,
			      (const uint8_t *)&hdr, UD_AAD_LEN,
			      plain, plain_len,
			      io_cipher, sizeof(io_cipher), &cipher_len);
	if (st != PSA_SUCCESS) {
		LOG_ERR("psa_aead_encrypt: %d", (int)st);
		return -EIO;
	}

	chunk_path(path, sizeof(path), id, seq);

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_WRITE | FS_O_CREATE);
	if (rc != 0) {
		LOG_ERR("fs_open(%s): %d", path, rc);
		return rc;
	}

	rc = fs_write(&file, &hdr, sizeof(hdr));
	if (rc == sizeof(hdr)) {
		rc = fs_write(&file, io_cipher, cipher_len);
	}
	if (rc < 0) {
		LOG_ERR("fs_write(%s): %d", path, rc);
	}

	fs_close(&file);
	return rc < 0 ? rc : 0;
}

/* Прочитать и расшифровать чанк в io_plain. Вызывается под ud_lock.
 * -ENOENT — чанк с таким seq не существует (конец цепочки). */
static int chunk_read(enum user_data_id id, unsigned seq, size_t *plain_len)
{
	struct ud_chunk_hdr hdr;
	char path[32];
	struct fs_file_t file;
	size_t total = 0;
	psa_status_t st;
	int rc;

	chunk_path(path, sizeof(path), id, seq);

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_READ);
	if (rc == -ENOENT) {
		return -ENOENT;
	}
	if (rc != 0) {
		LOG_ERR("fs_open(%s): %d", path, rc);
		return rc;
	}

	rc = fs_read(&file, &hdr, sizeof(hdr));
	if (rc != sizeof(hdr)) {
		LOG_ERR("Чанк %s: заголовок короткий (%d)", path, rc);
		rc = -EIO;
		goto out;
	}

	/* Заголовок связан с тегом через AAD: подделка ломает тег. */
	if (memcmp(hdr.magic, (uint8_t[4])UD_HDR_MAGIC, 4) != 0 ||
	    hdr.version != UD_HDR_VERSION || hdr.alg != UD_HDR_ALG_AES256_GCM) {
		LOG_ERR("Чанк %s: неизвестный формат", path);
		rc = -EINVAL;
		goto out;
	}

	while (total < sizeof(io_cipher)) {
		ssize_t n = fs_read(&file, io_cipher + total,
				    sizeof(io_cipher) - total);

		if (n <= 0) {
			break;
		}
		total += (size_t)n;
	}

	st = psa_aead_decrypt(ud_key, PSA_ALG_GCM,
			      hdr.nonce, UD_NONCE_LEN,
			      (const uint8_t *)&hdr, UD_AAD_LEN,
			      io_cipher, total,
			      io_plain, sizeof(io_plain), plain_len);
	if (st != PSA_SUCCESS) {
		/* Неверный тег: повреждение/подделка данных или чужой ключ. */
		LOG_ERR("psa_aead_decrypt(%s): %d", path, (int)st);
		rc = -EILSEQ;
		goto out;
	}

	rc = 0;
out:
	fs_close(&file);
	return rc;
}

/* Сколько чанков в цепочке блока. Вызывается под ud_lock. */
static unsigned chunks_count(enum user_data_id id)
{
	char path[32];
	struct fs_dirent ent;
	unsigned seq = 0;

	while (true) {
		chunk_path(path, sizeof(path), id, seq);
		if (fs_stat(path, &ent) != 0) {
			return seq;
		}
		seq++;
	}
}

/* Стереть цепочку чанков блока. Вызывается под ud_lock. */
static int erase_chunks(enum user_data_id id)
{
	char path[32];
	unsigned seq = 0;
	int rc;

	while (true) {
		chunk_path(path, sizeof(path), id, seq);
		rc = fs_unlink(path);
		if (rc == -ENOENT) {
			return 0;
		}
		if (rc != 0) {
			LOG_ERR("fs_unlink(%s): %d", path, rc);
			return rc;
		}
		seq++;
	}
}

/* ------------------------------------------------------------------ *
 *  Публичный API (ТЗ 4.3)                                             *
 * ------------------------------------------------------------------ */

int user_data_write(enum user_data_id id, const void *buf, size_t len)
{
	size_t off = 0;
	unsigned seq = 0;
	int rc;

	if (!ud_ready) {
		return -ENODEV;
	}

	k_mutex_lock(&ud_lock, K_FOREVER);

	/* Перезапись: старая цепочка стирается, новая с seq=0. */
	rc = erase_chunks(id);
	while (off < len && rc == 0) {
		size_t n = MIN(UD_CHUNK_PLAIN_MAX, len - off);

		rc = chunk_write(id, seq++, (const uint8_t *)buf + off, n);
		off += n;
	}

	k_mutex_unlock(&ud_lock);

	if (rc == 0 && len > 0) {
		event_log_write(EV_DATA_WRITE, id);
	}
	return rc;
}

int user_data_append(enum user_data_id id, const void *buf, size_t len)
{
	size_t off = 0;
	unsigned seq;
	int rc = 0;

	if (!ud_ready) {
		return -ENODEV;
	}

	k_mutex_lock(&ud_lock, K_FOREVER);

	/* Дозапись (форма 100): новые чанки в конец цепочки. */
	seq = chunks_count(id);
	while (off < len && rc == 0) {
		size_t n = MIN(UD_CHUNK_PLAIN_MAX, len - off);

		rc = chunk_write(id, seq++, (const uint8_t *)buf + off, n);
		off += n;
	}

	k_mutex_unlock(&ud_lock);

	if (rc == 0 && len > 0) {
		event_log_write(EV_DATA_WRITE, id);
	}
	return rc;
}

int user_data_read(enum user_data_id id, void *buf, size_t buf_size,
		   size_t *out_len)
{
	size_t total = 0;
	int rc = 0;

	if (!ud_ready) {
		return -ENODEV;
	}

	k_mutex_lock(&ud_lock, K_FOREVER);

	for (unsigned seq = 0; rc == 0; seq++) {
		size_t n;

		rc = chunk_read(id, seq, &n);
		if (rc == -ENOENT) {
			rc = 0;		/* конец цепочки */
			break;
		}
		if (rc != 0) {
			break;
		}

		if (total + n > buf_size) {
			rc = -ENOMEM;
			break;
		}
		memcpy((uint8_t *)buf + total, io_plain, n);
		total += n;
	}

	k_mutex_unlock(&ud_lock);

	if (rc == 0 && out_len != NULL) {
		*out_len = total;
	}
	return rc;
}

int user_data_erase(enum user_data_id id)
{
	int rc;

	if (!ud_ready) {
		return -ENODEV;
	}

	k_mutex_lock(&ud_lock, K_FOREVER);
	rc = erase_chunks(id);
	k_mutex_unlock(&ud_lock);

	return rc;
}

/* ------------------------------------------------------------------ *
 *  Инициализация                                                       *
 * ------------------------------------------------------------------ */

int user_data_init(void)
{
	psa_key_attributes_t attrs;
	psa_status_t st;
	int rc;

	st = psa_crypto_init();
	if (st != PSA_SUCCESS) {
		LOG_ERR("psa_crypto_init: %d", (int)st);
		return -EIO;
	}

	/* Загружаем ключ из NVS до BLE: BT позже вызовет settings_load()
	 * повторно — обработчики идемпотентны, это штатно. */
	rc = settings_subsys_init();
	if (rc == 0) {
		rc = settings_load();
	}
	if (rc != 0) {
		/* Не фатально, но если ключ в NVS был — данные станут
		 * нечитаемыми (будет сгенерирован новый ключ). */
		LOG_WRN("settings_load: %d — ключ будет сгенерирован заново",
			rc);
	}

	if (ud_key_loaded) {
		key_attrs_init(&attrs);
		st = psa_import_key(&attrs, ud_key_raw, UD_KEY_LEN, &ud_key);
		if (st != PSA_SUCCESS) {
			LOG_ERR("psa_import_key: %d", (int)st);
			return -EIO;
		}
		LOG_INF("Ключ пользовательских данных загружен из NVS (%s)",
			UD_SETTINGS_KEY);
	} else {
		/* Первый старт: ключ = 32 случайных байта RNG, сохраняем в NVS.
		 * psa_generate_key() требует WANT_KEY_GENERATE (мы его не
		 * включали — экономия), поэтому генерим сырой ключ через
		 * psa_generate_random() и импортируем как volatile-ключ. */
		st = psa_generate_random(ud_key_raw, UD_KEY_LEN);
		if (st != PSA_SUCCESS) {
			LOG_ERR("psa_generate_random: %d", (int)st);
			return -EIO;
		}

		key_attrs_init(&attrs);
		st = psa_import_key(&attrs, ud_key_raw, UD_KEY_LEN, &ud_key);
		if (st != PSA_SUCCESS) {
			LOG_ERR("psa_import_key: %d", (int)st);
			return -EIO;
		}

		rc = settings_save_one(UD_SETTINGS_KEY, ud_key_raw,
				       UD_KEY_LEN);
		if (rc != 0) {
			LOG_ERR("Сохранение ключа в NVS: %d", rc);
			return rc;
		}
		LOG_INF("Сгенерирован новый ключ (RNG) и сохранён в NVS (%s)",
			UD_SETTINGS_KEY);
	}

	ud_ready = true;
	LOG_INF("Пользовательские данные готовы: " UD_MOUNT
		", AES-256-GCM, чанки ≤%u Б", UD_CHUNK_PLAIN_MAX);
	return 0;
}

/* ------------------------------------------------------------------ *
 *  Shell: udtest — FCT-тест 6 (ТЗ): write/read тест-паттерна           *
 *  с полным AES-256-GCM roundtrip (запись → чтение → сверка).          *
 *  Пишет в блок «произвольные данные» (UD_CUSTOM_BASE, ТЗ 4.3).        *
 * ------------------------------------------------------------------ */
#ifdef CONFIG_SHELL

static int cmd_udtest(const struct shell *sh, size_t argc, char **argv)
{
	static const uint8_t pattern[] = "W25Q32 selftest: 0123456789 ABCDEF";
	uint8_t buf[sizeof(pattern)];
	size_t len = 0;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!ud_ready) {
		/* Диагностика: если init при старте упал (его лог мог
		 * дропнуться при переполнении лог-хранилища) — повторяем
		 * здесь и показываем точную причину. */
		rc = user_data_init();
		if (rc != 0) {
			shell_error(sh, "user_data_init: %d", rc);
			return rc;
		}
	}

	rc = user_data_write(UD_CUSTOM_BASE, pattern, sizeof(pattern));
	if (rc != 0) {
		shell_error(sh, "user_data_write: %d", rc);
		return rc;
	}

	rc = user_data_read(UD_CUSTOM_BASE, buf, sizeof(buf), &len);
	if (rc != 0) {
		shell_error(sh, "user_data_read: %d", rc);
		return rc;
	}

	if (len != sizeof(pattern) || memcmp(buf, pattern, len) != 0) {
		shell_error(sh, "паттерн не совпал (len=%zu, ожидалось %zu)",
			    len, sizeof(pattern));
		return -EIO;
	}

	shell_print(sh, "OK: %zu Б write→AES-GCM→read→совпало "
			"(файл /data/u%u_000000.bd)",
			len, (unsigned)UD_CUSTOM_BASE);
	return 0;
}

SHELL_CMD_REGISTER(udtest, NULL,
		   "FCT 6: тест-паттерн пользовательских данных (AES-GCM)",
		   cmd_udtest);

#endif /* CONFIG_SHELL */


