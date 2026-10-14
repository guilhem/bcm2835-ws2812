/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../ws2812-encode.h"

#define BIT(n) (1U << (n))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define DMA_MEM_TO_DEV 1
#define DMA_PREP_INTERRUPT 1
#define DMA_CTRL_ACK 2
#define msecs_to_jiffies(ms) (ms)
#define dma_submit_error(cookie) ((cookie) < 0 ? (cookie) : 0)
#define dma_async_is_tx_complete(chan, cookie, last, used) \
	dmaengine_tx_status(chan, cookie, NULL)
#define readl_poll_timeout(addr, value, cond, delay, timeout) \
	((value) = fake_poll(addr, delay, timeout), (cond) ? 0 : -ETIMEDOUT)

typedef int dma_cookie_t;
typedef size_t dma_addr_t;
enum dma_status { DMA_COMPLETE, DMA_IN_PROGRESS };
struct dma_tx_state { dma_cookie_t last, used; u32 residue; };
struct device;
struct dma_chan;
struct completion { bool done; };
struct dma_async_tx_descriptor { void (*callback)(void *); void *callback_param; };
struct ws2812 {
	struct device *dev;
	char *regs;
	struct dma_chan *dma;
	struct completion done;
	u8 desired[WS2812_LEDS][3];
	u32 *buffer;
	dma_addr_t buffer_dma;
	size_t buffer_bytes;
	u32 rate, reset_us;
};

static u32 readl(const void *addr);
static void writel(u32 value, void *addr);
static void usleep_range(unsigned int min, unsigned int max);
static void complete(struct completion *done);
static void reinit_completion(struct completion *done);
static struct dma_async_tx_descriptor *dmaengine_prep_slave_single(
	struct dma_chan *chan, dma_addr_t addr, size_t bytes, int dir, int flags);
static dma_cookie_t dmaengine_submit(struct dma_async_tx_descriptor *desc);
static void dma_async_issue_pending(struct dma_chan *chan);
static unsigned long wait_for_completion_timeout(struct completion *done,
	unsigned long timeout);
static enum dma_status dmaengine_tx_status(struct dma_chan *chan,
	dma_cookie_t cookie, struct dma_tx_state *state);
static int dmaengine_terminate_sync(struct dma_chan *chan);
static u32 fake_poll(const void *addr, unsigned int delay, unsigned int timeout);
static void dev_err(struct device *dev, const char *format, ...);

#include "ws2812-transfer-under-test.h"

static u32 regs[10];
static struct dma_async_tx_descriptor descriptor;
static enum { SUCCESS, DMA_TIMEOUT, FIFO_TIMEOUT } scenario;
static unsigned int prefill, terminations, diagnostics, clears;
static bool readback, reset_held, dma_live;
static char diagnostic[256];

static u32 readl(const void *addr)
{
	if (addr == &regs[PWM_CTL / 4] && !(regs[PWM_CTL / 4] & PWM_CTL_PWEN2))
		readback = true;
	return *(const u32 *)addr;
}

static void writel(u32 value, void *addr)
{
	if (addr == &regs[PWM_CTL / 4]) {
		if (value & PWM_CTL_CLRF1) {
			assert(value == (PWM_CTL_MODE2 | PWM_CTL_USEF2 | PWM_CTL_CLRF1));
			assert(regs[PWM_DMAC / 4] == 0);
			prefill = 0;
			readback = reset_held = false;
			clears++;
		} else if (value & PWM_CTL_PWEN2) {
			assert(value == (PWM_CTL_MODE2 | PWM_CTL_USEF2 | PWM_CTL_PWEN2));
			assert(prefill == 16 && dma_live);
		}
		value &= ~PWM_CTL_CLRF1;
	} else if (addr == &regs[PWM_FIFO / 4]) {
		assert(regs[PWM_CTL / 4] == (PWM_CTL_MODE2 | PWM_CTL_USEF2));
		assert(regs[PWM_DMAC / 4] == 0 && reset_held);
		assert(value == 0 && prefill < 16);
		prefill++;
	} else if (addr == &regs[PWM_STA / 4]) {
		regs[PWM_STA / 4] &= ~value;
		return;
	}
	*(u32 *)addr = value;
}

static void usleep_range(unsigned int min, unsigned int max)
{
	assert(max >= min);
	if (!(regs[PWM_CTL / 4] & PWM_CTL_PWEN2)) {
		assert(readback && min == 300 && max == 400);
		reset_held = true;
	}
}

static void complete(struct completion *done) { done->done = true; }
static void reinit_completion(struct completion *done) { done->done = false; }

static struct dma_async_tx_descriptor *dmaengine_prep_slave_single(
	struct dma_chan *chan, dma_addr_t addr, size_t bytes, int dir, int flags)
{
	(void)chan;
	assert(addr == 123 && bytes > WS2812_DATA_WORDS * sizeof(u32));
	assert(dir == DMA_MEM_TO_DEV && flags == (DMA_PREP_INTERRUPT | DMA_CTRL_ACK));
	return &descriptor;
}

static dma_cookie_t dmaengine_submit(struct dma_async_tx_descriptor *desc)
{
	assert(desc == &descriptor && desc->callback && desc->callback_param);
	return 42;
}

static void dma_async_issue_pending(struct dma_chan *chan)
{
	(void)chan;
	assert(prefill == 16 && !(regs[PWM_CTL / 4] & PWM_CTL_PWEN2));
	assert(regs[PWM_DMAC / 4] == (PWM_DMAC_EN | PWM_DMAC_THRESH));
	dma_live = true;
}

static unsigned long wait_for_completion_timeout(struct completion *done,
	unsigned long timeout)
{
	assert(timeout == 75 && !done->done);
	assert(regs[PWM_CTL / 4] & PWM_CTL_PWEN2);
	if (scenario == DMA_TIMEOUT) {
		regs[PWM_STA / 4] = PWM_STA_BERR | PWM_STA_GAPO2;
		return 0;
	}
	descriptor.callback(descriptor.callback_param);
	assert(done->done);
	return 1;
}

static enum dma_status dmaengine_tx_status(struct dma_chan *chan,
	dma_cookie_t cookie, struct dma_tx_state *state)
{
	(void)chan;
	assert(cookie == 42 && dma_live);
	if (state) {
		assert(state->last == 0 && state->used == 0 && state->residue == 0);
		if (scenario == DMA_TIMEOUT)
			state->residue = 128;
	}
	return scenario == DMA_TIMEOUT ? DMA_IN_PROGRESS : DMA_COMPLETE;
}

static u32 fake_poll(const void *addr, unsigned int delay, unsigned int timeout)
{
	assert(addr == &regs[PWM_STA / 4] && delay == 10 && timeout == 5000);
	regs[PWM_STA / 4] = scenario == FIFO_TIMEOUT ?
		PWM_STA_BERR | PWM_STA_GAPO2 : PWM_STA_EMPTY;
	return readl(addr);
}

static void dev_err(struct device *dev, const char *format, ...)
{
	va_list args;
	(void)dev;
	assert(scenario != SUCCESS && diagnostics == 0 && terminations == 0);
	assert(clears == 1 && dma_live && (regs[PWM_CTL / 4] & PWM_CTL_PWEN2));
	va_start(args, format);
	vsnprintf(diagnostic, sizeof(diagnostic), format, args);
	va_end(args);
	diagnostics++;
}

static int dmaengine_terminate_sync(struct dma_chan *chan)
{
	(void)chan;
	assert(dma_live && diagnostics == (scenario == SUCCESS ? 0U : 1U));
	dma_live = false;
	terminations++;
	return 0;
}

static void frame(struct ws2812 *ctl)
{
	char expected[256];
	terminations = diagnostics = clears = 0;
	assert(ws2812_send(ctl) == (scenario == SUCCESS ? 0 : -ETIMEDOUT));
	assert(terminations == 1 && clears == 2 && reset_held && !dma_live);
	assert(regs[PWM_CTL / 4] == (PWM_CTL_MODE2 | PWM_CTL_USEF2));
	assert(regs[PWM_DMAC / 4] == 0 && !(regs[PWM_STA / 4] & PWM_ERRORS));
	if (scenario != SUCCESS) {
		snprintf(expected, sizeof(expected),
			"%s timeout: PWM_CTL=%#x PWM_STA=%#x PWM_DMAC=%#x rate=2400000 Hz cookie=42 DMA_status=%d residue=%u\n",
			scenario == DMA_TIMEOUT ? "DMA completion (75 ms)" : "FIFO drain (5 ms)",
			PWM_CTL_MODE2 | PWM_CTL_USEF2 | PWM_CTL_PWEN2,
			PWM_STA_BERR | PWM_STA_GAPO2, PWM_DMAC_EN | PWM_DMAC_THRESH,
			scenario == DMA_TIMEOUT ? DMA_IN_PROGRESS : DMA_COMPLETE,
			scenario == DMA_TIMEOUT ? 128U : 0U);
		assert(strcmp(diagnostic, expected) == 0);
	}
}

int main(void)
{
	u32 buffer[WS2812_DATA_WORDS + 23] = { 0 };
	struct ws2812 ctl = { .regs = (char *)regs, .buffer = buffer,
		.buffer_dma = 123, .buffer_bytes = sizeof(buffer), .rate = 2400000,
		.reset_us = 300 };

	frame(&ctl);
	frame(&ctl);
	scenario = DMA_TIMEOUT;
	frame(&ctl);
	scenario = SUCCESS;
	frame(&ctl);
	scenario = FIFO_TIMEOUT;
	frame(&ctl);
	scenario = SUCCESS;
	frame(&ctl);
	puts("transfer: actual stop/send, first/repeated frames, disabled FIFO prefill, timeout snapshots before cleanup, DMA termination and recovery OK");
	return 0;
}
