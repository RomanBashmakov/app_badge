/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Работа с LoRa-трансивером SX1272 по SPI через штатную подсистему
 * Zephyr (CONFIG_LORA + backend loramac-node).
 *
 * Перенос работоспособного кода из zephyr_tutorial/src/main.c
 * в модуль приложения:
 *   1. Получает LoRa-устройство из devicetree (alias lora0 -> &sx1272).
 *   2. Конфигурирует модем на 868 МГц, BW 125 кГц, SF 7, CR 4/5, приём.
 *   3. Считывает ключевые регистры SX1272 напрямую через SPI с помощью
 *      низкоуровневого API модуля loramac-node (struct Radio_s).
 *   4. Запускает непрерывный асинхронный приём (RXCONTINUOUS).
 *
 * Приём не останавливается в программном сне бейджа: DIO0 (PC13, EXTI)
 * будит CPU1 из STOP2, поэтому LoRa остаётся каналом пробуждения.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/logging/log.h>

/* Низкоуровневый API модуля loramac-node: Radio.Read()/Write() поверх SPI.
 * Путь к radio.h добавлен в CMakeLists.txt (ZEPHYR_BASE/../modules/...).
 */
#include <radio.h>

/* Адреса регистров SX1272 (LoRa-режим).
 * Источник: modules/lib/loramac-node/src/radio/sx1272/sx1272Regs-LoRa.h
 */
#define SX1272_REG_LR_OPMODE         0x01
#define SX1272_REG_LR_FRFMSB         0x06
#define SX1272_REG_LR_FRFMID         0x07
#define SX1272_REG_LR_FRFLSB         0x08
#define SX1272_REG_LR_PACONFIG       0x09
#define SX1272_REG_LR_LNA            0x0C
#define SX1272_REG_LR_MODEMCONFIG1   0x1D
#define SX1272_REG_LR_MODEMCONFIG2   0x1E
#define SX1272_REG_LR_SYNCWORD       0x39
#define SX1272_REG_LR_VERSION        0x42

/* Ожидаемое значение REG_VERSION: 0x22 для SX1272 */
#define SX1272_VERSION_ID            0x22

#define LORA_NODE DT_ALIAS(lora0)
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(LORA_NODE),
	     "Не найден узел LoRa (alias lora0) в devicetree. "
	     "Проверьте app.overlay: узел sx1272 с compatible = \"semtech,sx1272\".");

LOG_MODULE_REGISTER(lora, CONFIG_LOG_DEFAULT_LEVEL);

/* ------------------------------------------------------------------ *
 *  Callback асинхронного приёма пакетов                               *
 *  (имя не должно совпадать с typedef lora_recv_cb из lora.h)         *
 * ------------------------------------------------------------------ */
static void on_lora_packet_recv(const struct device *dev, uint8_t *data,
				uint16_t size, int16_t rssi, int8_t snr,
				void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	LOG_INF("LoRa RX: RSSI=%d dBm, SNR=%d dB, %u байт", rssi, snr, size);
	LOG_HEXDUMP_INF(data, size, "payload");
}

/* ------------------------------------------------------------------ *
 *  Вывод значений ключевых регистров SX1272 по SPI                    *
 * ------------------------------------------------------------------ */
static void sx1272_dump_registers(void)
{
	/* Radio глобально определена в zephyr/drivers/lora/loramac-node/sx127x.c */
	const uint8_t version = Radio.Read(SX1272_REG_LR_VERSION);

	LOG_INF("=== Регистры SX1272 (чтение по SPI) ===");
	LOG_INF("REG_VERSION       (0x42) = 0x%02x  (%s)", version,
		version == SX1272_VERSION_ID ? "SX1272 OK" : "НЕВЕРНЫЙ ID!");

	LOG_INF("REG_OPMODE        (0x01) = 0x%02x",
		Radio.Read(SX1272_REG_LR_OPMODE));

	/* Частота: frf = MSB<<16 | MID<<8 | LSB; F_rf = frf * 32MHz / 2^19 */
	uint32_t frf = ((uint32_t)Radio.Read(SX1272_REG_LR_FRFMSB) << 16) |
		       ((uint32_t)Radio.Read(SX1272_REG_LR_FRFMID) << 8) |
		       ((uint32_t)Radio.Read(SX1272_REG_LR_FRFLSB));
	uint32_t freq_hz = (uint32_t)((uint64_t)frf * 32000000ULL >> 19);
	LOG_INF("REG_FRF MSB/MID/LSB (0x06..08): frf=0x%06x -> %u Гц",
		frf, freq_hz);

	LOG_INF("REG_PACONFIG      (0x09) = 0x%02x",
		Radio.Read(SX1272_REG_LR_PACONFIG));
	LOG_INF("REG_LNA           (0x0C) = 0x%02x",
		Radio.Read(SX1272_REG_LR_LNA));
	LOG_INF("REG_MODEMCONFIG1  (0x1D) = 0x%02x",
		Radio.Read(SX1272_REG_LR_MODEMCONFIG1));
	LOG_INF("REG_MODEMCONFIG2  (0x1E) = 0x%02x",
		Radio.Read(SX1272_REG_LR_MODEMCONFIG2));
	LOG_INF("REG_SYNCWORD      (0x39) = 0x%02x",
		Radio.Read(SX1272_REG_LR_SYNCWORD));
}

/* ------------------------------------------------------------------ *
 *  Публичные функции                                                  *
 * ------------------------------------------------------------------ */

int lora_init(void)
{
	const struct device *const lora_dev = DEVICE_DT_GET(LORA_NODE);
	struct lora_modem_config config = {0};
	int ret;

	if (!device_is_ready(lora_dev)) {
		LOG_ERR("LoRa-устройство %s не готово", lora_dev->name);
		return -ENODEV;
	}

	/* --- Конфигурация модема: 868 МГц, приёмник --- */
	config.frequency = 868000000;          /* Европа: 863–870 МГц */
	config.bandwidth = BW_125_KHZ;
	config.datarate = SF_7;
	config.coding_rate = CR_4_5;
	config.preamble_len = 8;
	config.tx_power = 14;                  /* не используется в режиме RX */
	config.iq_inverted = false;
	config.public_network = false;         /* приватный sync word (0x12) */
	config.packet_crc_disable = false;     /* CRC включён */
	config.tx = false;                     /* режим приёма */

	ret = lora_config(lora_dev, &config);
	if (ret < 0) {
		LOG_ERR("lora_config() failed: %d", ret);
		return ret;
	}
	LOG_INF("Модем сконфигурирован: 868 МГц, BW125, SF7, CR4/5, RX");

	/* --- Чтение регистров SX1272 после конфигурации --- */
	sx1272_dump_registers();

	const uint8_t version = Radio.Read(SX1272_REG_LR_VERSION);
	if (version != SX1272_VERSION_ID) {
		LOG_WRN("SX1272 не отвечает корректно (version=0x%02x, "
			"ожидалось 0x%02x) — проверьте питание/SPI/пины",
			version, SX1272_VERSION_ID);
	}

	/* --- Запуск непрерывного асинхронного приёма --- */
	ret = lora_recv_async(lora_dev, on_lora_packet_recv, NULL);
	if (ret < 0) {
		LOG_ERR("lora_recv_async() failed: %d", ret);
		return ret;
	}
	LOG_INF("Непрерывный приём (RXCONTINUOUS) запущен");

	return 0;
}
