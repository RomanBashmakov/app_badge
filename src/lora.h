/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoRa-трансивер SX1272 (штатная подсистема Zephyr LORA,
 * backend loramac-node; распиновка — app.overlay, узел &sx1272).
 *
 * Режим: приёмник 868 МГц (BW 125 кГц, SF 7, CR 4/5),
 * непрерывный асинхронный приём (RXCONTINUOUS); пакеты
 * логируются в callback по прерыванию DIO0 (RxDone).
 */

#ifndef LORA_H
#define LORA_H

/*
 * Инициализация LoRa-модема:
 *   - проверка готовности устройства (alias lora0 -> &sx1272);
 *   - конфигурация модема: 868 МГц, BW 125 кГц, SF 7, CR 4/5, CRC;
 *   - проверка версии чипа (REG_VERSION == 0x22) и дамп ключевых
 *     регистров по SPI (низкоуровневый API loramac-node);
 *   - запуск непрерывного асинхронного приёма.
 *
 * Возвращает 0 при успехе, отрицательный код ошибки при неудаче.
 */
int lora_init(void);

#endif /* LORA_H */
