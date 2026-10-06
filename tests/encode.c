/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ws2812-encode.h"

static unsigned int bit_at(const u32 *words, unsigned int bit)
{
	return (words[bit / 32] >> (31 - bit % 32)) & 1U;
}

int main(void)
{
	u8 rgb[WS2812_LEDS][3] = {{0}};
	u32 words[WS2812_DATA_WORDS + 1];
	unsigned int value, led, color, bit, pos;
	const unsigned int grb[] = {1, 0, 2};

	/* Exhaust all byte values, every LED and wire color, including word crossings. */
	for (value = 0; value <= 255; value++) {
		for (led = 0; led < WS2812_LEDS; led++)
			for (color = 0; color < 3; color++)
				rgb[led][color] = (u8)(value + led * 31 + color * 67);
		memset(words, 0xff, sizeof(words));
		ws2812_encode(words, (const u8 (*)[3])rgb);
		assert(words[WS2812_DATA_WORDS] == UINT32_MAX);
		pos = 0;
		for (led = 0; led < WS2812_LEDS; led++)
			for (color = 0; color < 3; color++)
				for (bit = 0; bit < 8; bit++, pos += 3) {
					assert(bit_at(words, pos) == 1);
					assert(bit_at(words, pos + 1) ==
					       ((rgb[led][grb[color]] >> (7 - bit)) & 1U));
					assert(bit_at(words, pos + 2) == 0);
				}
		for (; pos < WS2812_DATA_WORDS * 32; pos++)
			assert(bit_at(words, pos) == 0);
	}
	memset(rgb, 0, sizeof(rgb));
	ws2812_encode(words, (const u8 (*)[3])rgb);
	assert(words[0] == 0x92492492U);
	puts("encoder: 256 patterns, all five LEDs, GRB, MSB first, padding and bounds OK");
	return 0;
}
