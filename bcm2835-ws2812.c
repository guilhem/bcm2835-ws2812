// SPDX-License-Identifier: GPL-2.0-only
/* Five TagTagTag WS2812 LEDs on BCM2835/BCM2837 GPIO13 (PWM index 1). */
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/led-class-multicolor.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "ws2812-encode.h"

#define PWM_CTL 0x00
#define PWM_STA 0x04
#define PWM_DMAC 0x08
#define PWM_FIFO 0x18
#define PWM_RNG2 0x20
#define PWM_CTL_PWEN2 BIT(8)
#define PWM_CTL_MODE2 BIT(9)
#define PWM_CTL_USEF2 BIT(13)
#define PWM_CTL_CLRF1 BIT(6)
#define PWM_STA_EMPTY BIT(1)
#define PWM_STA_WERR BIT(2)
#define PWM_STA_RERR BIT(3)
#define PWM_STA_GAPO2 BIT(5)
#define PWM_STA_BERR BIT(8)
#define PWM_ERRORS (PWM_STA_WERR | PWM_STA_RERR | PWM_STA_BERR)
#define PWM_DMAC_EN BIT(31)
#define PWM_DMAC_THRESH ((8U << 8) | 8U)
#define DMA_TIMEOUT_MS 75
#define FIFO_TIMEOUT_US 5000

struct ws2812;
struct ws2812_led {
	struct led_classdev_mc mc;
	struct mc_subled colors[3];
	struct ws2812 *ctl;
	unsigned int index;
};

struct ws2812 {
	struct device *dev;
	void __iomem *regs;
	struct clk *clk;
	struct clk *osc;
	struct dma_chan *dma;
	struct device *dma_dev;
	struct completion done;
	struct mutex lock;
	struct ws2812_led leds[WS2812_LEDS];
	const struct attribute_group *first_groups[3];
	u8 desired[WS2812_LEDS][3];
	u32 *buffer;
	dma_addr_t buffer_dma;
	size_t buffer_bytes;
	u32 rate, reset_us;
	unsigned int registered;
	bool ready, stopping;
};

static int ws2812_stop(struct ws2812 *ctl)
{
	u32 errors;
	/* SBIT2=0 and RPTL2=0 leave the idle output low. */
	writel(0, ctl->regs + PWM_DMAC);
	writel(PWM_CTL_CLRF1, ctl->regs + PWM_CTL);
	readl(ctl->regs + PWM_CTL);
	usleep_range(ctl->reset_us, ctl->reset_us + 100);
	errors = readl(ctl->regs + PWM_STA) & PWM_ERRORS;
	writel(PWM_ERRORS | PWM_STA_GAPO2, ctl->regs + PWM_STA);
	return errors ? -EIO : 0;
}

static void ws2812_dma_done(void *data)
{
	struct ws2812 *ctl = data;

	complete(&ctl->done);
}

/* Caller holds lock. Completion means FIFO drained AND reset held low. */
static int ws2812_send(struct ws2812 *ctl)
{
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;
	u32 status;
	unsigned int i;
	int ret, stop_ret;

	ws2812_stop(ctl);
	ws2812_encode(ctl->buffer, ctl->desired);
	reinit_completion(&ctl->done);
	desc = dmaengine_prep_slave_single(ctl->dma, ctl->buffer_dma,
		ctl->buffer_bytes, DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc)
		return -ENOMEM;
	desc->callback = ws2812_dma_done;
	desc->callback_param = ctl;
	cookie = dmaengine_submit(desc);
	ret = dma_submit_error(cookie);
	if (ret)
		goto stop_dma;

	/* Prefill the shared 16-word FIFO with low bits before starting PWM. */
	for (i = 0; i < 16; i++)
		writel(0, ctl->regs + PWM_FIFO);
	writel(32, ctl->regs + PWM_RNG2);
	writel(PWM_DMAC_EN | PWM_DMAC_THRESH, ctl->regs + PWM_DMAC);
	dma_async_issue_pending(ctl->dma);
	writel(PWM_CTL_MODE2 | PWM_CTL_USEF2 | PWM_CTL_PWEN2,
	       ctl->regs + PWM_CTL);

	if (!wait_for_completion_timeout(&ctl->done,
					 msecs_to_jiffies(DMA_TIMEOUT_MS))) {
		ret = -ETIMEDOUT;
		goto stop_dma;
	}
	if (dma_async_is_tx_complete(ctl->dma, cookie, NULL, NULL) != DMA_COMPLETE) {
		ret = -EIO;
		goto stop_dma;
	}

	ret = readl_poll_timeout(ctl->regs + PWM_STA, status,
				status & PWM_STA_EMPTY, 10, FIFO_TIMEOUT_US);
	if (ret)
		goto stop_dma;
	/* GAPO2 also occurs at normal EOF; it cannot classify an underrun here. */
	if (status & PWM_ERRORS) {
		ret = -EIO;
		goto stop_dma;
	}
	/* EMPTY means the last word entered the shifter, not that it left the pin. */
	usleep_range(DIV_ROUND_UP(32U * 1000000U, ctl->rate),
		     DIV_ROUND_UP(32U * 1000000U, ctl->rate) + 50);
	status = readl(ctl->regs + PWM_STA);
	ret = (status & PWM_ERRORS) ? -EIO : 0;
stop_dma:
	/* Also drains callbacks before reusing/freeing the coherent buffer. */
	stop_ret = dmaengine_terminate_sync(ctl->dma);
	if (!ret)
		ret = stop_ret;
	stop_ret = ws2812_stop(ctl);
	return ret ? ret : stop_ret;
}

static int ws2812_desired(struct ws2812_led *led, enum led_brightness brightness)
{
	unsigned int color, intensity[3];

	for (color = 0; color < 3; color++) {
		intensity[color] = READ_ONCE(led->colors[color].intensity);
		if (intensity[color] > LED_FULL)
			return -ERANGE;
	}
	for (color = 0; color < 3; color++) {
		led->colors[color].brightness =
			DIV_ROUND_CLOSEST((unsigned int)brightness * intensity[color], LED_FULL);
		led->ctl->desired[led->index][color] = led->colors[color].brightness;
	}
	return 0;
}

static int ws2812_brightness(struct led_classdev *cdev, enum led_brightness value)
{
	struct ws2812_led *led = container_of(lcdev_to_mccdev(cdev),
					     struct ws2812_led, mc);
	struct ws2812 *ctl = led->ctl;
	int ret;

	mutex_lock(&ctl->lock);
	if (ctl->stopping) {
		/* Unregister flushes a final LED_OFF after quiesce sent black. */
		ret = value == LED_OFF ? 0 : -ESHUTDOWN;
		goto out;
	}
	ret = ws2812_desired(led, value);
	if (!ret)
		ret = ws2812_send(ctl);
out:
	mutex_unlock(&ctl->lock);
	return ret;
}

static ssize_t sync_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct ws2812_led *led = container_of(lcdev_to_mccdev(cdev),
					     struct ws2812_led, mc);
	struct ws2812 *ctl = led->ctl;
	unsigned int i;
	int ret;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	/* Class attributes exist before led_init_core() finishes during device_add. */
	if (!smp_load_acquire(&ctl->ready))
		return -EAGAIN;
	/* No controller or led_access lock here: callbacks need the controller. */
	for (i = 0; i < WS2812_LEDS; i++)
		flush_work(&ctl->leds[i].mc.led_cdev.set_brightness_work);

	mutex_lock(&ctl->lock);
	if (ctl->stopping) {
		ret = -ESHUTDOWN;
		goto out;
	}
	/* Resnapshot all desired values, including invalid writes rejected by work. */
	for (i = 0; i < WS2812_LEDS; i++) {
		ret = ws2812_desired(&ctl->leds[i],
			READ_ONCE(ctl->leds[i].mc.led_cdev.brightness));
		if (ret)
			goto out;
	}
	ret = ws2812_send(ctl);
out:
	mutex_unlock(&ctl->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(sync);
static struct attribute *ws2812_sync_attrs[] = { &dev_attr_sync.attr, NULL };
static const struct attribute_group ws2812_sync_group = { .attrs = ws2812_sync_attrs };

static void ws2812_quiesce(struct ws2812 *ctl)
{
	int ret;

	mutex_lock(&ctl->lock);
	if (!ctl->stopping) {
		ctl->stopping = true;
		memset(ctl->desired, 0, sizeof(ctl->desired));
		ret = ws2812_send(ctl);
		if (ret)
			dev_warn(ctl->dev, "black frame failed: %d\n", ret);
	}
	mutex_unlock(&ctl->lock);
}

static void ws2812_unregister(struct ws2812 *ctl)
{
	/* LED 0 is registered last. Its sysfs callbacks drain before the others go. */
	while (ctl->registered) {
		unsigned int index = (--ctl->registered + 1) % WS2812_LEDS;

		led_classdev_multicolor_unregister(&ctl->leds[index].mc);
	}
}

static int ws2812_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ws2812 *ctl;
	struct resource *res;
	struct device_node *child;
	struct dma_slave_config config = { .direction = DMA_MEM_TO_DEV,
		.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES, .dst_maxburst = 1 };
	unsigned long rate;
	unsigned int i, color;
	u32 index, seen = 0;
	int ret;

	if (!of_machine_is_compatible("brcm,bcm2835") &&
	    !of_machine_is_compatible("brcm,bcm2837"))
		return -ENODEV;
	ctl = devm_kzalloc(dev, sizeof(*ctl), GFP_KERNEL);
	if (!ctl)
		return -ENOMEM;
	ctl->dev = dev;
	ctl->rate = 2400000;
	ctl->reset_us = 300;
	if (of_find_property(dev->of_node, "clock-frequency", NULL)) {
		ret = of_property_read_u32(dev->of_node, "clock-frequency", &ctl->rate);
		if (ret)
			return ret;
	}
	if (of_find_property(dev->of_node, "reset-us", NULL)) {
		ret = of_property_read_u32(dev->of_node, "reset-us", &ctl->reset_us);
		if (ret)
			return ret;
	}
	if (ctl->rate < 2000000 || ctl->rate > 3000000 ||
	    ctl->reset_us < 300 || ctl->reset_us > 1000)
		return dev_err_probe(dev, -EINVAL, "invalid timing\n");
	mutex_init(&ctl->lock);
	init_completion(&ctl->done);
	platform_set_drvdata(pdev, ctl);

	for_each_available_child_of_node(dev->of_node, child) {
		struct ws2812_led *led;
		const char *label;
		char expected[32];

		ret = of_property_read_u32(child, "reg", &index);
		if (ret || index >= WS2812_LEDS || seen & BIT(index)) {
			ret = -EINVAL;
			goto put_child;
		}
		led = &ctl->leds[index];
		ret = of_property_read_string(child, "label", &label);
		snprintf(expected, sizeof(expected), "multi:indicator-%u", index);
		if (ret || strcmp(expected, label)) {
			ret = -EINVAL;
			goto put_child;
		}
		led->ctl = ctl;
		led->index = index;
		led->mc.num_colors = 3;
		led->mc.subled_info = led->colors;
		led->mc.led_cdev.name = label;
		led->mc.led_cdev.max_brightness = LED_FULL;
		led->mc.led_cdev.color = LED_COLOR_ID_MULTI;
		led->mc.led_cdev.brightness_set_blocking = ws2812_brightness;
		led->mc.led_cdev.flags = LED_REJECT_NAME_CONFLICT;
		for (color = 0; color < 3; color++) {
			static const unsigned int colors[] = {
				LED_COLOR_ID_RED, LED_COLOR_ID_GREEN, LED_COLOR_ID_BLUE
			};

			led->colors[color].color_index = colors[color];
			led->colors[color].channel = color;
			led->colors[color].intensity = LED_FULL;
		}
		seen |= BIT(index);
	}
	if (seen != GENMASK(WS2812_LEDS - 1, 0))
		return dev_err_probe(dev, -EINVAL, "exactly five LED children required\n");

	ctl->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(ctl->regs))
		return PTR_ERR(ctl->regs);
	ret = PTR_ERR_OR_ZERO(devm_pinctrl_get_select_default(dev));
	if (ret)
		return dev_err_probe(dev, ret, "pinctrl unavailable\n");
	ctl->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(ctl->clk))
		return dev_err_probe(dev, PTR_ERR(ctl->clk), "clock unavailable\n");
	ctl->osc = devm_clk_get(dev, "osc");
	if (IS_ERR(ctl->osc))
		return dev_err_probe(dev, PTR_ERR(ctl->osc), "oscillator unavailable\n");
	ret = clk_set_parent(ctl->clk, ctl->osc);
	if (ret)
		return dev_err_probe(dev, ret, "fixed clock parent unavailable\n");
	/* Check again after rate selection: CCF may choose a different parent. */
	ret = clk_set_rate_exclusive(ctl->clk, ctl->rate);
	if (ret)
		return dev_err_probe(dev, ret, "clock rate unavailable\n");
	if (!clk_is_match(clk_get_parent(ctl->clk), ctl->osc)) {
		ret = -EINVAL;
		goto put_clock;
	}
	rate = clk_get_rate(ctl->clk);
	if (rate < ctl->rate - ctl->rate / 100 || rate > ctl->rate + ctl->rate / 100) {
		ret = -ERANGE;
		goto put_clock;
	}
	ctl->rate = rate;
	ret = clk_prepare_enable(ctl->clk);
	if (ret)
		goto put_clock;
	ws2812_stop(ctl);
	ctl->dma = dma_request_chan(dev, "tx");
	if (IS_ERR(ctl->dma)) {
		ret = dev_err_probe(dev, PTR_ERR(ctl->dma), "DMA unavailable\n");
		goto disable_clock;
	}
	ctl->dma_dev = dmaengine_get_dma_device(ctl->dma);
	/*
	 * Slave config uses the CPU physical FIFO resource, as bcm2835 I2S/SPI do.
	 * bcm2835-dma translates it through dma_map_resource (older kernels:
	 * phys_to_dma) using DT dma-ranges. Do NOT pretranslate it to 0x7e20c018:
	 * that would apply the bus translation twice on BCM2835 and BCM2837.
	 */
	config.dst_addr = res->start + PWM_FIFO;
	ret = dmaengine_slave_config(ctl->dma, &config);
	if (ret)
		goto release_dma;
	/* Zero suffix includes a complete latch interval while the serializer runs. */
	ctl->buffer_bytes = (WS2812_DATA_WORDS +
		DIV_ROUND_UP(DIV_ROUND_UP(ctl->rate * ctl->reset_us, 1000000U), 32U)) * sizeof(u32);
	ctl->buffer = dma_alloc_coherent(ctl->dma_dev, ctl->buffer_bytes,
					 &ctl->buffer_dma, GFP_KERNEL);
	if (!ctl->buffer) {
		ret = -ENOMEM;
		goto release_dma;
	}
	memset(ctl->buffer, 0, ctl->buffer_bytes);
	ret = ws2812_send(ctl);
	if (ret)
		goto free_buffer;

	/* 1..4 first; capture the native multicolor group, then add sync to LED 0. */
	for (i = 1; i <= WS2812_LEDS; i++) {
		struct led_classdev *cdev = &ctl->leds[i % WS2812_LEDS].mc.led_cdev;

		if (i < WS2812_LEDS) {
			ret = led_classdev_multicolor_register(dev, &ctl->leds[i].mc);
		} else {
			ctl->first_groups[0] = ctl->leds[1].mc.led_cdev.groups[0];
			ctl->first_groups[1] = &ws2812_sync_group;
			cdev->groups = ctl->first_groups;
			cdev->flags |= LED_MULTI_COLOR;
			ret = led_classdev_register(dev, cdev);
		}
		if (ret)
			goto unregister_leds;
		ctl->registered++;
	}
	/* Publish completed LED/work initialization to early sync callers. */
	smp_store_release(&ctl->ready, true);
	dev_info(dev, "five RGB LEDs, PWM1 GPIO13, %u Hz, reset %u us\n",
		 ctl->rate, ctl->reset_us);
	return 0;

put_child:
	of_node_put(child);
	return ret;
unregister_leds:
	ws2812_quiesce(ctl);
	ws2812_unregister(ctl);
free_buffer:
	dmaengine_terminate_sync(ctl->dma);
	ws2812_stop(ctl);
	dma_free_coherent(ctl->dma_dev, ctl->buffer_bytes, ctl->buffer, ctl->buffer_dma);
release_dma:
	dma_release_channel(ctl->dma);
disable_clock:
	clk_disable_unprepare(ctl->clk);
put_clock:
	clk_rate_exclusive_put(ctl->clk);
	return ret;
}

static void ws2812_remove(struct platform_device *pdev)
{
	struct ws2812 *ctl = platform_get_drvdata(pdev);

	ws2812_quiesce(ctl);
	ws2812_unregister(ctl);
	dmaengine_terminate_sync(ctl->dma);
	ws2812_stop(ctl);
	dma_free_coherent(ctl->dma_dev, ctl->buffer_bytes, ctl->buffer, ctl->buffer_dma);
	dma_release_channel(ctl->dma);
	clk_disable_unprepare(ctl->clk);
	clk_rate_exclusive_put(ctl->clk);
}

static void ws2812_shutdown(struct platform_device *pdev)
{
	ws2812_quiesce(platform_get_drvdata(pdev));
}

static const struct of_device_id ws2812_match[] = {
	{ .compatible = "guilhem,bcm2835-ws2812" },
	{ }
};
MODULE_DEVICE_TABLE(of, ws2812_match);

static struct platform_driver ws2812_driver = {
	.probe = ws2812_probe,
	.remove = ws2812_remove,
	.shutdown = ws2812_shutdown,
	.driver = { .name = "bcm2835-ws2812", .of_match_table = ws2812_match },
};
module_platform_driver(ws2812_driver);
MODULE_DESCRIPTION("TagTagTag five WS2812 LEDs using exclusive BCM2835 PWM1 and DMAengine");
MODULE_LICENSE("GPL v2");
