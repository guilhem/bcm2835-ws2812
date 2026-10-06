# bcm2835-ws2812

A GPL-2.0-only Linux LED multicolor driver for the **five TagTagTag WS2812 LEDs**
on GPIO13 (PWM index 1). Targets Raspberry Pi Zero W (BCM2835, ARMv6) and Zero 2 W
(BCM2837, ARM64). It owns the entire PWM block and uses Linux pinctrl, clock and
DMAengine APIs. Linux allocates the physical DMA channel; DT selects **PWM DREQ 5**.

## Build and first light

Install a compiler, make, `device-tree-compiler`, and the headers matching the
running Raspberry Pi kernel, including its real `Module.symvers`. The kernel needs
`CONFIG_LEDS_CLASS_MULTICOLOR`, `CONFIG_DMA_BCM2835`, `CONFIG_OF`, pinctrl and the
BCM2835 clock driver. A source tree with only `modules_prepare` lacks the required
symbol versions.

```sh
make check
make KDIR=/lib/modules/$(uname -r)/build
sudo make install KDIR=/lib/modules/$(uname -r)/build
sudo depmod -a
```

Add `dtoverlay=bcm2835-ws2812` to `/boot/firmware/config.txt` and reboot. The module
is loaded through its DT alias. Disable competing PWM overlays, PWM audio and
any userspace WS281x driver first. GPIO13 must be connected to the TagTagTag chain;
this driver does not configure power supplies or level shifting.

```sh
LED=/sys/class/leds/multi:indicator-0
cat "$LED/multi_index"             # red green blue
printf '255 0 0\n' | sudo tee "$LED/multi_intensity" >/dev/null
printf '64\n' | sudo tee "$LED/brightness" >/dev/null
printf '1\n' | sudo tee "$LED/sync" >/dev/null
```

All five LEDs start black. The other paths are `multi:indicator-1` through
`multi:indicator-4`. To turn the chain off and confirm the transfer:

```sh
for i in 0 1 2 3 4; do
    printf '0\n' | sudo tee "/sys/class/leds/multi:indicator-$i/brightness" >/dev/null
done
printf '1\n' | sudo tee /sys/class/leds/multi:indicator-0/sync >/dev/null
```

## ABI

| Attribute | Contract |
| --- | --- |
| `brightness` | Standard LED master brightness, 0–255; `max_brightness` is 255. |
| `multi_intensity` | Standard LED RGB component intensities, each 0–255. |
| `multi_index` | `red green blue`; wire encoding is GRB, most significant bit first. |
| LED 0 `sync` | Write-only controller barrier; accepts `1` with an optional newline. |

Each brightness callback transmits the complete desired state. LED core writes
can run through brightness work and do not reliably report driver errors to the
writer. A successful write to **`multi:indicator-0/sync`** flushes all five LED
brightness works without holding the controller lock, then synchronously sends
the complete desired state again. It reports errors from this new transfer,
not a cached result from a previous operation. DMA completion alone is insufficient:
FIFO drain, the last serializer word and a low reset interval must also complete.
Stop concurrent LED writers/triggers before using this as a quiescence barrier.
New writes after the barrier may turn the LEDs on again.

`sync` exists in the attribute groups at LED 0's device-add event, so udev can
assign permissions alongside `brightness` and `multi_intensity`. LED 0 is
registered last, using the native multicolor group's attributes plus `sync`.
A call during probe returns `EAGAIN`; an invalid barrier value returns `EINVAL`.
Out-of-range component intensities are rejected by the driver with `ERANGE`
through `sync` (some LED core versions accept such values in `multi_intensity`).
Transfers return `ETIMEDOUT` for DMA/FIFO timeouts or `EIO` for DMA/FIFO errors.
The normal end-of-stream GAPO2 flag is not used to classify underruns;
DMA starvation and waveform integrity still require hardware qualification.
The DMA wait is 75 ms and FIFO polling is limited to 5 ms; scheduling latency can
extend elapsed wall time. DMA is terminated synchronously before buffer reuse
or release. No character device, `/dev/mem`, `/dev/vcio` or privileged daemon is
part of this interface.

Sysfs has no owner-close lifecycle: exiting or killing a writer retains the
last color. An integrating service should set every brightness to zero and
write `sync` from a systemd stop/post-stop helper. Probe sends black before
exposing LEDs. Remove/shutdown attempt a bounded black transfer and stop DMA;
a hardware failure is logged and cannot guarantee the visible chain is black.

## Device tree and timing

The overlay replaces `&pwm`'s compatible with `guilhem,bcm2835-ws2812`, retaining
its register resource and SoC bus translations. It selects GPIO13 ALT0, the
fixed oscillator as PWM parent, and PWM DREQ 5. There is no PWM-compatible
fallback: `pwm-bcm2835` must not bind or share either channel/FIFO. See the
[short binding](Documentation/binding.md).

The default bit clock is 2.4 MHz: logical 0 becomes `100`, logical 1 becomes
`110`, with range 32 in serializer mode on PWM index 1 (hardware channel 2).
A zero suffix plus a minimum 300 µs low interval provides reset/latch time.
`clock-frequency` and `reset-us` are DT tuning points; when changing the clock,
update `assigned-clock-rates` too. Rates from 2–3 MHz and resets from 300–1000 µs
are accepted only if the fixed oscillator can supply the rate within 1%.
The driver checks the actual parent after rate selection and rejects a PLL
chosen instead. Firmware must not change the oscillator/PWM clock behind Linux.

DMA memory uses the DMA controller device's coherent allocator. The slave
configuration takes the **CPU physical FIFO resource**, as the Raspberry Pi
I2S/SPI drivers do. On the locked kernels `bcm2835-dma` calls `dma_map_resource`
to translate it through DT `dma-ranges`; older current 6.12 kernels use
`phys_to_dma`. Pretranslating that value into a DMA bus address would translate
it twice. No CPU or DMA bus address is hardcoded here.

## DKMS and immutable systems

For a conventional writable Raspberry Pi OS installation:

```sh
sudo install -d /usr/src/bcm2835-ws2812-0.1.0
sudo cp bcm2835-ws2812.c ws2812-encode.h Makefile dkms.conf /usr/src/bcm2835-ws2812-0.1.0/
sudo dkms add -m bcm2835-ws2812 -v 0.1.0
sudo dkms build -m bcm2835-ws2812 -v 0.1.0
sudo dkms install -m bcm2835-ws2812 -v 0.1.0
```

DKMS installs the module, **not the overlay**: run `make overlay`, install
`bcm2835-ws2812.dtbo` in the boot overlay directory, and enable it as above.
The version in `dkms.conf` must match the source installation directory.

For an immutable/read-only image, build and install the module and overlay
**during image construction**. `KDIR`, `ARCH`, `CROSS_COMPILE`, `DESTDIR`, and
`OVERLAYDIR` are supported. Generate module dependencies in the staged root;
do not rebuild with DKMS or change the active root/boot files at runtime.
The driver writes no files, caches or persistent state. Sysfs belongs to the
kernel and remains writable independently of a read-only root filesystem.
A writable host build is not proof of operation on a read-only Raspberry Pi.

## Checks and releases

`make check` runs the portable C encoder check: all byte values, every LED/color,
GRB/MSB order, word crossings, zero padding and a buffer-boundary guard.
`make bcm2835-ws2812.dtbo` builds the standalone overlay without kernel headers.

CI locks the Raspberry Pi Linux and firmware commits in `kernels.lock.json`,
checks download SHA256s, extracts the **actual configuration from each vendor
`configs.ko` module**, prepares headers from the matching source and copies the real vendor
`Module.symvers`. Both ARMv6 and ARM64 modules must compile with modpost and
`W=1`; the overlay is applied to both locked vendor DTBs and inspected.
To reproduce, install both cross compilers, flex, bison, bc, OpenSSL/ELF development
headers, Python 3.12+ and device-tree-compiler, then run:

```sh
python3 ci/kernel.py armv6
make modules overlay KDIR="$PWD/build/armv6" ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- W=1
ci/overlay.sh build/downloads/bcm2708-rpi-zero-w.dtb
```

Use `arm64`, `ARCH=arm64` and `CROSS_COMPILE=aarch64-linux-gnu-` for the second
build. Clean module outputs before switching target architectures.
Tags `v*` publish source and two tested module/overlay archives with `SHA256SUMS`
only after **every required check succeeds**, including neither skipped nor
cancelled jobs. Binary modules are specific to the locked kernel/config; build
from source against matching headers for other Raspberry Pi kernels.

Compilation and DT checks cannot prove GPIO waveforms, DMA/FIFO recovery,
shutdown behavior or read-only device operation. See the hardware checklist in
[CONTRIBUTING.md](CONTRIBUTING.md). Those require both physical boards.
