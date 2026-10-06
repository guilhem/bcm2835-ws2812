/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WS2812_ENCODE_H
#define WS2812_ENCODE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
#endif

#define WS2812_LEDS 5
#define WS2812_DATA_BITS (WS2812_LEDS * 24 * 3)
#define WS2812_DATA_WORDS ((WS2812_DATA_BITS + 31) / 32)

/* FIFO words are native-endian; the PWM serializer sends bit 31 first. */
static inline void ws2812_encode(u32 *words, const u8 rgb[WS2812_LEDS][3])
{
	unsigned int led, color, bit, symbol, pos = 0, i;
	static const unsigned int grb[] = { 1, 0, 2 };

	for (i = 0; i < WS2812_DATA_WORDS; i++)
		words[i] = 0;
	for (led = 0; led < WS2812_LEDS; led++)
		for (color = 0; color < 3; color++)
			for (bit = 0; bit < 8; bit++) {
				u8 value = rgb[led][grb[color]];

				for (symbol = 0; symbol < 3; symbol++, pos++)
					if (!symbol || (symbol == 1 &&
							(value & (1U << (7 - bit)))))
						words[pos / 32] |= 1U << (31 - pos % 32);
			}
}
#endif
