# Contributing

SPDX-License-Identifier: GPL-2.0-only

Keep changes limited to the two Raspberry Pi boards and five-LED TagTagTag
wiring. Preserve LED labels, RGB sysfs order and the controller-wide LED 0
`sync` contract. Use Linux resource/clock/pinctrl/DMA APIs rather than raw
physical memory access. Contributions are under GPL-2.0-only.

Run `make check`, both locked kernel builds and both vendor-DTB overlay checks.
Use `W=1`, real matching `Module.symvers`, and include kernel/toolchain versions
in the results. Inspect probe unwind, remove/shutdown and DMA cancellation:
callbacks must finish before their buffer or controller is freed. Check that
`sync` does not hold a lock while flushing brightness work and reports a newly
transmitted complete frame's result.

Hardware qualification is separate from compilation:

- Test both Zero W/ARMv6 and Zero 2 W/ARM64 with the actual TagTagTag chain.
- Scope GPIO13 at cold probe, color changes, zero/255, all LEDs, remove and reboot.
  Verify GRB/MSB order, bit rate, last FIFO word and at least 300 µs continuous low.
- Confirm `multi:indicator-0..4` and writable `brightness`/`multi_intensity` plus
  first-LED `sync` are present when udev receives `add`.
- Stop writers/triggers, write all zeros, then `sync`; scope the black frame
  and complete reset. Verify process exit does not imply an automatic blackout.
- Exercise concurrent color writes and `sync`, rapid remove/reprobe, and service
  stop while brightness work is queued. Check logs for lockups or use-after-free.
- Inject DMA/FIFO failures under a controlled kernel/debug setup. Check timeout
  reporting, cancellation, reset and successful full retransmission afterwards.
- Exercise a genuinely read-only root on first boot, including service permissions
  and stop/post-stop integration. Save volatile logs before shutdown.

Do not describe native compilation, DT application, simulation or CI as hardware
validation. Report unrun checks explicitly. When updating kernel locks, verify
the firmware's `extra/git_hash`, both actual configs, real symbols and SHA256s;
never use empty/synthetic symbol files to bypass modpost.
