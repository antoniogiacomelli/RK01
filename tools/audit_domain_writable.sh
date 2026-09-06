#!/bin/sh
# SPDX-License-Identifier: Apache-2.0

set -u

if [ "$#" -eq 0 ]; then
    exit 0
fi

: "${OBJDUMP:=objdump}"

for obj in "$@"; do
    if [ ! -f "$obj" ]; then
        continue
    fi

    if ! sections="$("$OBJDUMP" -h "$obj" 2>/dev/null)"; then
        printf '%s\n' "rk-domain-audit: warning: cannot inspect $obj"
        continue
    fi

    printf '%s\n' "$sections" | awk -v obj="$obj" '
        $1 ~ /^[0-9]+$/ {
            name = $2
            size = $3
            if (size ~ /^0+$/) {
                next
            }
            if (name ~ /^(\.data|\.bss|\.sdata|\.sbss)(\.|$)/) {
                printf("rk-domain-audit: warning: %s has writable section %s size 0x%s\n",
                       obj, name, size)
            }
        }'

    "$OBJDUMP" -t "$obj" 2>/dev/null | awk -v obj="$obj" '
        /\*COM\*/ {
            printf("rk-domain-audit: warning: %s has COMMON symbol %s\n",
                   obj, $NF)
        }'
done
