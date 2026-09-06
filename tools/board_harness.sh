#!/bin/sh
set -eu

ARCH=${ARCH:-armv7m}
PLATFORM=${PLATFORM:-stm32f401re}
BUILD_ROOT=${BUILD_ROOT:-build-board}
EXTRA_DEFINES=${EXTRA_DEFINES:-}
EXTRA_DEFS=${EXTRA_DEFS:-}
APP_EXAMPLE=${APP_EXAMPLE:-tiny}
FLASH_TOOL=${FLASH_TOOL:-openocd}
TOOLCHAIN=${TOOLCHAIN:-arm-none-eabi}
OPENOCD_CFG=${OPENOCD_CFG:-.vscode/openocd-stm32f401re-stlink.cfg}
JLINK=${JLINK:-JLinkExe}
JLINK_DEVICE=${JLINK_DEVICE:-STM32F401RE}
JLINK_IF=${JLINK_IF:-SWD}
JLINK_SPEED=${JLINK_SPEED:-4000}
MAKE_JOBS=${MAKE_JOBS:-4}
BOARD_TIMEOUT=${BOARD_TIMEOUT:-20}
BOARD_BAUD=${BOARD_BAUD:-115200}
BOARD_LOG_DIR=${BOARD_LOG_DIR:-build-board/logs}
BOARD_RESET_AFTER_FLASH=${BOARD_RESET_AFTER_FLASH:-ON}
SERIAL_PORT=${SERIAL_PORT:-}

detect_serial_port()
{
    for dev in /dev/cu.usbmodem* /dev/tty.usbmodem* /dev/cu.usbserial* /dev/tty.usbserial*
    do
        if [ -e "$dev" ]
        then
            printf '%s\n' "$dev"
            return 0
        fi
    done
    return 1
}

run_jlink_commander()
{
    script_name=$1
    shift

    script_path="${BUILD_ROOT}/${script_name}"
    mkdir -p "$BUILD_ROOT"
    : > "$script_path"

    for command in "$@"
    do
        printf '%s\n' "$command" >> "$script_path"
    done

    "$JLINK" \
        -device "$JLINK_DEVICE" \
        -if "$JLINK_IF" \
        -speed "$JLINK_SPEED" \
        -autoconnect 1 \
        -CommanderScript "$script_path"
}

reset_board_for_capture()
{
    if [ "$BOARD_RESET_AFTER_FLASH" != "ON" ]
    then
        return 0
    fi

    printf '%s\n' "board-harness: reset/run with ${FLASH_TOOL} while capture is active"
    if [ "$FLASH_TOOL" = "openocd" ]
    then
        openocd \
            -f "$OPENOCD_CFG" \
            -c "init; reset run; shutdown"
    elif [ "$FLASH_TOOL" = "st-flash" ]
    then
        st-flash reset
    elif [ "$FLASH_TOOL" = "jlink" ]
    then
        run_jlink_commander "jlink-reset-run.jlink" "r" "g" "q"
    else
        printf '%s\n' "board-harness: warning: no reset command for FLASH_TOOL=${FLASH_TOOL}"
    fi
}

halt_board_for_capture()
{
    if [ "$BOARD_RESET_AFTER_FLASH" != "ON" ]
    then
        return 0
    fi

    if [ "$FLASH_TOOL" = "openocd" ]
    then
        printf '%s\n' "board-harness: halt after flash before opening serial"
        openocd \
            -f "$OPENOCD_CFG" \
            -c "init; reset halt; shutdown"
    elif [ "$FLASH_TOOL" = "jlink" ]
    then
        printf '%s\n' "board-harness: halt after flash before opening serial"
        run_jlink_commander "jlink-reset-halt.jlink" "r" "h" "q"
    fi
}

capture_pid=""
cleanup_capture()
{
    if [ -n "$capture_pid" ] && kill -0 "$capture_pid" 2>/dev/null
    then
        kill "$capture_pid" 2>/dev/null || true
        wait "$capture_pid" 2>/dev/null || true
    fi
}
trap cleanup_capture EXIT INT TERM HUP

profile="rk01_demo-${APP_EXAMPLE}"
mkdir -p "$BOARD_LOG_DIR"
log_file="${BOARD_LOG_DIR}/${profile}-$(date +%Y%m%d-%H%M%S).log"

printf '%s\n' "board-harness: build ${PLATFORM} ${profile}"
make "-j${MAKE_JOBS}" \
    "ARCH=${ARCH}" \
    "PLATFORM=${PLATFORM}" \
    "BUILD_ROOT=${BUILD_ROOT}" \
    "EXTRA_DEFINES=${EXTRA_DEFINES}" \
    "EXTRA_DEFS=${EXTRA_DEFS}" \
    "APP_EXAMPLE=${APP_EXAMPLE}" \
    "TOOLCHAIN=${TOOLCHAIN}"

printf '%s\n' "board-harness: flash with ${FLASH_TOOL}"
make \
    "ARCH=${ARCH}" \
    "PLATFORM=${PLATFORM}" \
    "BUILD_ROOT=${BUILD_ROOT}" \
    "EXTRA_DEFINES=${EXTRA_DEFINES}" \
    "EXTRA_DEFS=${EXTRA_DEFS}" \
    "APP_EXAMPLE=${APP_EXAMPLE}" \
    "FLASH_TOOL=${FLASH_TOOL}" \
    "TOOLCHAIN=${TOOLCHAIN}" \
    "OPENOCD_CFG=${OPENOCD_CFG}" \
    "JLINK=${JLINK}" \
    "JLINK_DEVICE=${JLINK_DEVICE}" \
    "JLINK_IF=${JLINK_IF}" \
    "JLINK_SPEED=${JLINK_SPEED}" \
    flash

halt_board_for_capture

if [ -z "$SERIAL_PORT" ]
then
    if SERIAL_PORT=$(detect_serial_port)
    then
        printf '%s\n' "board-harness: auto-detected serial ${SERIAL_PORT}"
    else
        printf '%s\n' "board-harness: no SERIAL_PORT set or auto-detected; skipping log capture"
        exit 0
    fi
fi

if stty -f "$SERIAL_PORT" "$BOARD_BAUD" cs8 -cstopb -parenb -ixon -ixoff raw -echo 2>/dev/null
then
    :
elif stty -F "$SERIAL_PORT" "$BOARD_BAUD" cs8 -cstopb -parenb -ixon -ixoff raw -echo 2>/dev/null
then
    :
else
    printf '%s\n' "board-harness: warning: could not configure ${SERIAL_PORT}; capturing anyway"
fi

printf '%s\n' "board-harness: capture ${SERIAL_PORT} for ${BOARD_TIMEOUT}s -> ${log_file}"
python3 - "$SERIAL_PORT" "$BOARD_TIMEOUT" "$log_file" "$BOARD_BAUD" <<'PY' &
import os
import select
import sys
import termios
import time

port = sys.argv[1]
timeout = float(sys.argv[2])
log_file = sys.argv[3]
baud = int(sys.argv[4])

def configure_port(fd, baud):
    baud_const = getattr(termios, "B{}".format(baud), None)
    if baud_const is None:
        return

    attrs = termios.tcgetattr(fd)
    attrs[0] &= ~(termios.IXON | termios.IXOFF | termios.IXANY |
                  termios.INLCR | termios.ICRNL | termios.IGNCR |
                  termios.BRKINT | termios.PARMRK | termios.ISTRIP)
    attrs[1] &= ~termios.OPOST
    attrs[2] &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB)
    attrs[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
    attrs[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON |
                  termios.ISIG | termios.IEXTEN)
    attrs[4] = baud_const
    attrs[5] = baud_const
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)

fd = os.open(port, os.O_RDONLY | os.O_NONBLOCK)
end = time.monotonic() + timeout

try:
    configure_port(fd, baud)
    with open(log_file, "wb") as out:
        while time.monotonic() < end:
            ready, _, _ = select.select([fd], [], [], 0.2)
            if not ready:
                continue
            data = os.read(fd, 4096)
            if not data:
                continue
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
            out.write(data)
            out.flush()
finally:
    os.close(fd)
PY
capture_pid=$!
sleep 0.2
reset_board_for_capture
wait "$capture_pid"
capture_pid=""

printf '%s\n' "board-harness: done"
