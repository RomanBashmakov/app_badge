/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Кнопка sw0 и светодиод led0 (упрощение coconut_vibe: одна кнопка).
 *
 * По нажатию/отжатию:
 *   - зажигается/гаснет светодиод led0;
 *   - вызывается callback активности (используется в main.c
 *     для сброса таймера сна бейджа).
 *
 * sw0 определён в базовом DTS платы WeAct STM32WB55 Core
 * (boot_button, PH3, GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN).
 */

#ifndef BUTTON_H
#define BUTTON_H

/*
 * Инициализация кнопки и светодиода:
 *   - настраивает GPIO кнопки на вход с прерыванием по обоим фронтам;
 *   - включает антидребезг (BUTTON_DEBOUNCE_MS);
 *   - настраивает светодиод led0 на выход.
 *
 * Возвращает 0 при успехе, отрицательный код ошибки при неудаче.
 */
int button_init(void);

/*
 * Регистрирует callback, вызываемый при каждом нажатии кнопки.
 */
void button_set_activity_callback(void (*callback)(void));

#endif /* BUTTON_H */
