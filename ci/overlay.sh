#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
[ "$#" -eq 1 ] || { echo "usage: $0 vendor.dtb" >&2; exit 2; }
mkdir -p build
fdtoverlay -i "$1" -o build/overlay-test.dtb bcm2835-ws2812.dtbo
pwm=$(fdtget -t s build/overlay-test.dtb /__symbols__ pwm)
[ "$(fdtget -t s build/overlay-test.dtb "$pwm" compatible)" = guilhem,bcm2835-ws2812 ]
[ "$(fdtget -t s build/overlay-test.dtb "$pwm" dma-names)" = tx ]
[ "$(fdtget -t s build/overlay-test.dtb "$pwm" status)" = okay ]
[ "$(fdtget -t s build/overlay-test.dtb "$pwm" clock-names)" = 'pwm osc' ]
[ "$(fdtget -t u build/overlay-test.dtb "$pwm" clock-frequency)" = 2400000 ]
[ "$(fdtget -t u build/overlay-test.dtb "$pwm" reset-us)" = 300 ]
# Splitting the two numeric DT cells is intentional.
# shellcheck disable=SC2046
set -- $(fdtget -t u build/overlay-test.dtb "$pwm" dmas)
[ "$#" -eq 2 ] && [ "$2" -eq 5 ]
i=0
while [ "$i" -lt 5 ]; do
    [ "$(fdtget -t s build/overlay-test.dtb "$pwm/led@$i" label)" = "multi:indicator-$i" ]
    [ "$(fdtget -t u build/overlay-test.dtb "$pwm/led@$i" reg)" = "$i" ]
    i=$((i + 1))
done
echo "overlay: exclusive PWM, DREQ 5, clocks and five stable LED labels OK"
