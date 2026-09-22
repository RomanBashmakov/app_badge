/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Пользовательские данные жетона (ТЗ 4.3) во внешней FLASH с шифрованием.
 *
 * Хранилище: партиция userdata W25Q32, littlefs (/data, автомаунт fstab).
 * Каждый блок — цепочка чанков-файлов /data/u<id>_<seq>.bd по ≤4 КБ:
 * чанк = заголовок (magic/версия/алгоритм/nonce) + AES-256-GCM шифротекст
 * + 16-байтовый тег. Заголовок (первые 6 Б) — AAD: подделка заголовка
 * ломает тег. Дозапись (форма 100) = новый чанк — износ флеша равномерный,
 * сбой питания портит максимум последний чанк.
 *
 * Крипто: PSA Crypto (mbedTLS), AES-256-GCM. Ключ генерится аппаратным
 * RNG при первом старте и хранится в NVS внутренней FLASH (settings
 * sec/aeskey). Выбор стека и модель угроз —
 * Badge_Obsidian/Работа/Криптозащита пользовательских данных.md.
 *
 * Доступ по ТЗ — только после аутентификации сканера через SE: сейчас
 * API доступен только коду жетона (BLE-чтение — фаза 2), ключ наружу
 * не отдаётся.
 */

#ifndef USER_DATA_H
#define USER_DATA_H

#include <stddef.h>
#include <stdint.h>

/* Блоки пользовательских данных (ТЗ 4.3). */
enum user_data_id {
	UD_BLOOD = 0,		/* группа крови + резус, ≤ 16 Б */
	UD_ALLERGIES,		/* аллергии, ≤ 256 Б */
	UD_CHRONIC,		/* хронические заболевания, ≤ 256 Б */
	UD_FORMA100,		/* форма 100, ≤ 64 КБ, многократная дозапись */
	UD_GUN_LICENSE,		/* оружейный аттестат, ≤ 4 КБ */

	/* Произвольные данные: UD_CUSTOM_BASE + n (n < 2^24). */
	UD_CUSTOM_BASE = 32,
};

/* Инициализация: psa_crypto_init() + загрузка/генерация ключа.
 * Вызывать из main() до первого обращения и до ble_init(). */
int user_data_init(void);

/* Перезаписать блок (старое содержимое стирается).
 * Большие данные автоматически режутся на чанки по 4 КБ. */
int user_data_write(enum user_data_id id, const void *buf, size_t len);

/* Дописать в конец блока (форма 100) — новые чанки в конец цепочки. */
int user_data_append(enum user_data_id id, const void *buf, size_t len);

/* Прочитать блок целиком (все чанки по порядку).
 * out_len — фактическая длина; -ENOMEM если buf_size мало. */
int user_data_read(enum user_data_id id, void *buf, size_t buf_size,
		   size_t *out_len);

/* Стереть блок (все чанки). */
int user_data_erase(enum user_data_id id);

#endif /* USER_DATA_H */
