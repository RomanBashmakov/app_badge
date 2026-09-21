#!/usr/bin/env bash
#
# jlink.sh — отладка бейджа через J-Link: прошивка, RTT-лог, сброс.
#
# Всё выполняется в постоянном docker-контейнере badge-probe (образ
# zephyr-build): там есть openocd 0.12 из zephyr-sdk, который нужен
# для J-Link V13 (host openocd 0.11 слишком стар). Конфиг таргета —
# scripts/openocd_jlink_wb55.cfg (SWD 200 кГц — выше на этой разводке
# чтения «сыплются»).
#
# Использование (из каталога app_badge):
#   ./scripts/jlink.sh flash          # прошить приложение (MCUboot-
#                                      #   подписанный build/zephyr/
#                                      #   app_update.bin в slot0)
#   ./scripts/jlink.sh flashboot      # прошить MCUboot (build_mcuboot/
#                                      #   zephyr/zephyr.bin в 0x08000000;
#                                      # один раз после сборки бутлоадера)
#   ./scripts/jlink.sh rtt [сек]      # RTT-лог в терминале (default 60 с)
#   ./scripts/jlink.sh rtt 0          # бесконечно (выход — Ctrl+C)
#   ./scripts/jlink.sh reset          # перезапуск платы (через RTT-сессию)
#   ./scripts/jlink.sh stop           # остановить openocd-сессию
#
# Раскладка флеша (weact_stm32wb55_core, партии в DTS платы):
#   0x08000000 MCUboot (48 КБ) · 0x0800C000 slot0 (400 КБ) ·
#   0x08070000 slot1 (400 КБ) · 0x080D4000 scratch (16 КБ) ·
#   0x080D8000 storage/NVS (8 КБ). Без MCUboot в 0x08000000 приложение
#   в slot0 НЕ стартует (некому его загрузить и проверить подпись).
#
# Примечания:
#   - rtt поднимает openocd-сервер (порт 9999 = RTT ch1, 4444 = telnet)
#     и стримит лог; АДРЕС RTT-блока берётся из ELF автоматически.
#   - перед flash запущенная rtt-сессия сама глушится (пробник один).

set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
WS_DIR="$(cd "$APP_DIR/.." && pwd)"
IMAGE="${ZW_IMAGE:-zephyrprojectrtos/zephyr-build:latest}"
NAME=badge-probe
OO=/opt/toolchains/zephyr-sdk-1.0.1/hosttools/sysroots/x86_64-pokysdk-linux/usr/bin/openocd
SCR=/opt/toolchains/zephyr-sdk-1.0.1/hosttools/sysroots/x86_64-pokysdk-linux/usr/share/openocd/scripts
CFG=/workdir/app_badge/scripts/openocd_jlink_wb55.cfg
APP_BIN=/workdir/app_badge/build/app_badge/zephyr/zephyr.signed.bin
SLOT0=0x0800C000
MCUBOOT_BIN=/workdir/app_badge/build/mcuboot/zephyr/zephyr.bin
BOOT=0x08000000

ensure_container() {
	if ! docker ps --format '{{.Names}}' | grep -qx "$NAME"; then
		docker rm -f "$NAME" 2>/dev/null || true
		echo "Запускаю контейнер $NAME (образ $IMAGE)..."
		docker run -d --name "$NAME" --privileged \
			-v /dev/bus/usb:/dev/bus/usb \
			-v "$WS_DIR":/workdir \
			-p 127.0.0.1:9999:9999 -p 127.0.0.1:4444:4444 \
			"$IMAGE" sleep infinity
	fi
}

rtt_addr() {
	# Адрес control block _SEGGER_RTT из ELF (может меняться между сборками)
	nm "$APP_DIR/build/app_badge/zephyr/zephyr.elf" | awk '/ B _SEGGER_RTT$/{print "0x"$1}'
}

case "${1:-}" in
flash)
	ensure_container
	if [ ! -f "$APP_DIR/build/app_badge/zephyr/zephyr.signed.bin" ]; then
		echo "Нет build/app_badge/zephyr/zephyr.signed.bin — сначала сборка приложения." >&2
		exit 1
	fi
	docker exec "$NAME" pkill openocd 2>/dev/null || true
	sleep 1
	docker exec "$NAME" bash -c \
		"$OO -s $SCR -f $CFG -c 'init' -c 'reset halt' \
		 -c 'program $APP_BIN verify $SLOT0' \
		 -c 'reset run' -c 'shutdown'"
	;;

flashboot)
	ensure_container
	if [ ! -f "$APP_DIR/build/mcuboot/zephyr/zephyr.bin" ]; then
		echo "Нет build/mcuboot/zephyr/zephyr.bin — сначала сборка MCUboot (см. README)." >&2
		exit 1
	fi
	docker exec "$NAME" pkill openocd 2>/dev/null || true
	sleep 1
	docker exec "$NAME" bash -c \
		"$OO -s $SCR -f $CFG -c 'init' -c 'reset halt' \
		 -c 'program $MCUBOOT_BIN verify $BOOT' \
		 -c 'reset run' -c 'shutdown'"
	;;

rtt)
	ensure_container
	ADDR="$(rtt_addr)"
	if [ -z "$ADDR" ]; then
		echo "Не найден _SEGGER_RTT в build/app_badge/zephyr/zephyr.elf — сначала сборка." >&2
		exit 1
	fi
	docker exec "$NAME" pkill openocd 2>/dev/null || true
	sleep 1
	docker exec -d "$NAME" bash -c \
		"$OO -s $SCR -f $CFG -c 'bindto 0.0.0.0' -c 'init' \
		 -c 'rtt setup $ADDR 0x1000 \"SEGGER RTT\"' \
		 -c 'rtt start' -c 'rtt server start 9999 1' \
		 > /tmp/rtt_openocd.log 2>&1"
	sleep 4
	T="${2:-60}"
	echo "=== RTT ch1 (логи), RTT-блок $ADDR, ${T} c (Ctrl+C — прервать) ==="
	if [ "$T" = "0" ]; then
		nc 127.0.0.1 9999 || true
	else
		timeout "$T" nc 127.0.0.1 9999 || true
	fi
	;;

reset)
	ensure_container
	(printf 'reset run\n'; sleep 1) | nc -w 2 127.0.0.1 4444 >/dev/null 2>&1 \
		|| echo "RTT-сессия не запущена — сначала: ./scripts/jlink.sh rtt"
	;;

stop)
	ensure_container
	docker exec "$NAME" pkill openocd 2>/dev/null || true
	echo "openocd остановлен (контейнер $NAME оставлен)"
	;;

*)
	echo "Использование: $0 flash | flashboot | rtt [сек] | reset | stop" >&2
	exit 1
	;;
esac
