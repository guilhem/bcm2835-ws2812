# Device tree binding

SPDX-License-Identifier: GPL-2.0-only

Bind to the existing BCM2835/BCM2837 PWM node. Replace its compatible completely
with `guilhem,bcm2835-ws2812`; keep its inherited `reg` and parent bus ranges.
The only supported wiring is five TagTagTag WS2812 LEDs on GPIO13/PWM index 1.

| Property | Required value |
| --- | --- |
| `compatible` | `guilhem,bcm2835-ws2812` without a PWM fallback |
| `reg` | Existing PWM block resource, inherited from the board DT |
| `status` | `okay` |
| `pinctrl-names` / `pinctrl-0` | `default` / GPIO13 ALT0, pull down |
| `clocks` / `clock-names` | PWM clock and fixed oscillator / `pwm`, `osc` |
| `assigned-clocks` | PWM clock |
| `assigned-clock-parents` | Fixed board oscillator |
| `assigned-clock-rates` | Desired serializer bit rate |
| `dmas` / `dma-names` | DMA controller with **DREQ 5** / `tx` |
| `#address-cells` / `#size-cells` | 1 / 0 for LED child addresses |
| `clock-frequency` | Optional; defaults to 2400000 Hz; accepted 2000000–3000000 Hz |
| `reset-us` | Optional; defaults to 300; accepted 300–1000 µs |

Exactly five enabled children, each with one unique `reg` in 0–4 and
`label = "multi:indicator-N"` for that index, are mandatory. `color = <8>` denotes
multicolor. Logical component order is fixed RGB; physical wire order is GRB.
Labels are ABI and must not be renamed. No child triggers are enabled at probe.

`bcm2835-ws2812-overlay.dts` is the complete reference. Do not add a second PWM
node, hardcode a physical DMA channel, or change the board's `dma-ranges`.
Use the DTB for the board actually booted. Apply this overlay before kernel
boot, not on a PWM block already bound to another driver.
