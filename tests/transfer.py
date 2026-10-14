# SPDX-License-Identifier: GPL-2.0-only
"""Compile the real transfer functions with native fake MMIO/DMA, without driver hooks."""
from pathlib import Path

source = (Path(__file__).resolve().parent.parent / "bcm2835-ws2812.c").read_text()
print(source[source.index("#define PWM_CTL "):source.index("\nstruct ws2812;")])
print(source[source.index("static int ws2812_stop("):source.index("static int ws2812_desired(")])
