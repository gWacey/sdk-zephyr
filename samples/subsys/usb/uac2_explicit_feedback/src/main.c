/*
 * Copyright (c) 2023-2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdlib.h>

#include <sample_usbd.h>
#include "feedback.h"

#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_uac2.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/logging/log.h>

#if CONFIG_HW_CODEC_CIRRUS_LOGIC
#include <zephyr/drivers/gpio.h>
#include "macros_common.h"
#include "bsp_driver_if.h"
#include "cs47l63.h"
#include "cs47l63_spec.h"
#include "cs47l63_reg_conf.h"
#include "cs47l63_comm.h"
#endif /* (CONFIG_HW_CODEC_CIRRUS_LOGIC) */

LOG_MODULE_REGISTER(uac2_sample, LOG_LEVEL_INF);

#define HEADPHONES_OUT_TERMINAL_ID UAC2_ENTITY_ID(DT_NODELABEL(out_terminal))

#if CONFIG_BOARD_NRF5340_AUDIO_DK_NRF5340_CPUAPP
#define I2S_TX DT_ALIAS(i2s_tx)
#else
#define I2S_TX DT_NODELABEL(i2s_tx)
#endif /* CONFIG_BOARD_NRF5340_AUDIO_DK_NRF5340_CPUAPP */

#define FS_SAMPLES_PER_SOF  48
#define HS_SAMPLES_PER_SOF  6
#define MAX_SAMPLES_PER_SOF MAX(FS_SAMPLES_PER_SOF, HS_SAMPLES_PER_SOF)
#define SAMPLE_FREQUENCY    (FS_SAMPLES_PER_SOF * 1000)
#define SAMPLE_BIT_WIDTH    16
#define NUMBER_OF_CHANNELS  2
#define BYTES_PER_SAMPLE    DIV_ROUND_UP(SAMPLE_BIT_WIDTH, 8)
#define BYTES_PER_SLOT      (BYTES_PER_SAMPLE * NUMBER_OF_CHANNELS)
#define MIN_BLOCK_SIZE      ((MAX_SAMPLES_PER_SOF - 1) * BYTES_PER_SLOT)
#define BLOCK_SIZE          (MAX_SAMPLES_PER_SOF * BYTES_PER_SLOT)
#define MAX_BLOCK_SIZE      ((MAX_SAMPLES_PER_SOF + 1) * BYTES_PER_SLOT)
#define MAX_VOLUME_REG_VAL  0x80

/* Absolute minimum is 5 buffers (1 actively consumed by I2S, 2nd queued as next
 * buffer, 3rd acquired by USB stack to receive data to, and 2 to handle SOF/I2S
 * offset errors), but add 2 additional buffers to prevent out of memory errors
 * when USB host decides to perform rapid terminal enable/disable cycles.
 */
#define I2S_BUFFERS_COUNT 7
K_MEM_SLAB_DEFINE_STATIC(i2s_tx_slab, ROUND_UP(MAX_BLOCK_SIZE, UDC_BUF_GRANULARITY),
			 I2S_BUFFERS_COUNT, UDC_BUF_ALIGN);

#if CONFIG_HW_CODEC_CIRRUS_LOGIC
static cs47l63_t cs47l63_driver;
#endif /* CONFIG_HW_CODEC_CIRRUS_LOGIC */

struct usb_i2s_ctx {
	const struct device *i2s_dev;
	bool terminal_enabled;
	bool i2s_started;
	bool microframes;
	/* Number of blocks written, used to determine when to start I2S.
	 * Overflows are not a problem because this variable is not necessary
	 * after I2S is started.
	 */
	uint8_t i2s_blocks_written;
	struct feedback_ctx *fb;
};

static void uac2_terminal_update_cb(const struct device *dev, uint8_t terminal,
				    bool enabled, bool microframes,
				    void *user_data)
{
	struct usb_i2s_ctx *ctx = user_data;

	/* This sample has only one terminal therefore the callback can simply
	 * ignore the terminal variable.
	 */
	__ASSERT_NO_MSG(terminal == HEADPHONES_OUT_TERMINAL_ID);

	ctx->microframes = microframes;

	ctx->terminal_enabled = enabled;
	if (ctx->i2s_started && !enabled) {
		i2s_trigger(ctx->i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
		ctx->i2s_started = false;
		ctx->i2s_blocks_written = 0;
		feedback_reset_ctx(ctx->fb, ctx->microframes);
	}
}

static void *uac2_get_recv_buf(const struct device *dev, uint8_t terminal,
			       uint16_t size, void *user_data)
{
	ARG_UNUSED(dev);
	struct usb_i2s_ctx *ctx = user_data;
	void *buf = NULL;
	int ret;

	if (terminal == HEADPHONES_OUT_TERMINAL_ID) {
		__ASSERT_NO_MSG(size <= MAX_BLOCK_SIZE);

		if (!ctx->terminal_enabled) {
			LOG_ERR("Buffer request on disabled terminal");
			return NULL;
		}

		ret = k_mem_slab_alloc(&i2s_tx_slab, &buf, K_NO_WAIT);
		if (ret != 0) {
			buf = NULL;
		}
	}

	return buf;
}

static void uac2_data_recv_cb(const struct device *dev, uint8_t terminal,
			      void *buf, uint16_t size, void *user_data)
{
	struct usb_i2s_ctx *ctx = user_data;
	int ret;

	if (!ctx->terminal_enabled) {
		k_mem_slab_free(&i2s_tx_slab, buf);
		return;
	}

	if (!size) {
		/* Zero fill to keep I2S going. If this is transient error, then
		 * this is probably best we can do. Otherwise, host will likely
		 * either disable terminal (or the cable will be disconnected)
		 * which will stop I2S.
		 */
		if (USBD_SUPPORTS_HIGH_SPEED && ctx->microframes) {
			size = HS_SAMPLES_PER_SOF * BYTES_PER_SLOT;
		} else {
			size = FS_SAMPLES_PER_SOF * BYTES_PER_SLOT;
		}
		memset(buf, 0, size);
	}

	LOG_DBG("Received %d data to input terminal %d", size, terminal);

	ret = i2s_write(ctx->i2s_dev, buf, size);
	if (ret < 0) {
		ctx->i2s_started = false;
		ctx->i2s_blocks_written = 0;
		feedback_reset_ctx(ctx->fb, ctx->microframes);

		/* Most likely underrun occurred, prepare I2S restart */
		i2s_trigger(ctx->i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);

		ret = i2s_write(ctx->i2s_dev, buf, size);
		if (ret < 0) {
			/* Drop data block, will try again on next frame */
			k_mem_slab_free(&i2s_tx_slab, buf);
		}
	}

	if (ret == 0) {
		ctx->i2s_blocks_written++;
	}
}

static void uac2_buf_release_cb(const struct device *dev, uint8_t terminal,
				void *buf, void *user_data)
{
	/* This sample does not send audio data so this won't be called */
}

/* Variables for debug use to facilitate simple how feedback value affects
 * audio data rate experiments. These debug variables can also be used to
 * determine how well the feedback regulator deals with errors. The values
 * are supposed to be modified by debugger.
 *
 * Setting use_hardcoded_feedback to true, essentially bypasses the feedback
 * regulator and makes host send hardcoded_feedback samples every 16384 SOFs
 * (when operating at Full-Speed).
 *
 * The feedback at Full-Speed is Q10.14 value. For 48 kHz audio sample rate,
 * there are nominally 48 samples every SOF. The corresponding value is thus
 * 48 << 14. Such feedback value would result in host sending always 48 samples.
 * Now, if we want to receive more samples (because 1 ms according to audio
 * sink is shorter than 1 ms according to USB Host 500 ppm SOF timer), then
 * the feedback value has to be increased. The fractional part is 14-bit wide
 * and therefore increment by 1 means 1 additional sample every 2**14 SOFs.
 * (48 << 14) + 1 therefore results in host sending 48 samples 16383 times and
 * 49 samples 1 time during every 16384 SOFs.
 *
 * Similarly, if we want to receive less samples (because 1 ms according to
 * audio signk is longer than 1 ms according to USB Host), then the feedback
 * value has to be decreased. (48 << 14) - 1 therefore results in host sending
 * 48 samples 16383 times and 47 samples 1 time during every 16384 SOFs.
 *
 * If the feedback value differs by more than 1 (i.e. LSB), then the +1/-1
 * samples packets are generally evenly distributed. For example feedback value
 * (48 << 14) + (1 << 5) results in 48 samples 511 times and 49 samples 1 time
 * during every 512 SOFs.
 *
 * For High-Speed above changes slightly, because the feedback format is Q16.16
 * and microframes are used. The 48 kHz audio sample rate is achieved by sending
 * 6 samples every SOF (microframe). The nominal value is the average number of
 * samples to send every microframe and therefore for 48 kHz the nominal value
 * is (6 << 16).
 */
static volatile bool use_hardcoded_feedback;
static volatile uint32_t hardcoded_feedback = (48 << 14) + 1;

static uint32_t uac2_feedback_cb(const struct device *dev, uint8_t terminal,
				 void *user_data)
{
	/* Sample has only one UAC2 instance with one terminal so both can be
	 * ignored here.
	 */
	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);
	struct usb_i2s_ctx *ctx = user_data;

	if (use_hardcoded_feedback) {
		return hardcoded_feedback;
	} else {
		return feedback_value(ctx->fb);
	}
}

static void uac2_sof(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev);
	struct usb_i2s_ctx *ctx = user_data;

	if (ctx->i2s_started) {
		feedback_process(ctx->fb);
	}

	/* We want to maintain 3 SOFs delay, i.e. samples received during SOF n
	 * should be on I2S during SOF n+3. This provides enough wiggle room
	 * for software scheduling that effectively eliminates "buffers not
	 * provided in time" problem.
	 *
	 * ">= 2" translates into 3 SOFs delay because the timeline is:
	 * USB SOF n
	 *   OUT DATA0 n received from host
	 * USB SOF n+1
	 *   DATA0 n is available to UDC driver (See Universal Serial Bus
	 *   Specification Revision 2.0 5.12.5 Data Prebuffering) and copied
	 *   to I2S buffer before SOF n+2; i2s_blocks_written = 1
	 *   OUT DATA0 n+1 received from host
	 * USB SOF n+2
	 *   DATA0 n+1 is copied; i2s_block_written = 2
	 *   OUT DATA0 n+2 received from host
	 * USB SOF n+3
	 *   This function triggers I2S start
	 *   DATA0 n+2 is copied; i2s_block_written is no longer relevant
	 *   OUT DATA0 n+3 received from host
	 */
	if (!ctx->i2s_started && ctx->terminal_enabled &&
	    ctx->i2s_blocks_written >= 2) {
		i2s_trigger(ctx->i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
		ctx->i2s_started = true;
		feedback_start(ctx->fb, ctx->i2s_blocks_written, ctx->microframes);
	}
}

static struct uac2_ops usb_audio_ops = {
	.sof_cb = uac2_sof,
	.terminal_update_cb = uac2_terminal_update_cb,
	.get_recv_buf = uac2_get_recv_buf,
	.data_recv_cb = uac2_data_recv_cb,
	.buf_release_cb = uac2_buf_release_cb,
	.feedback_cb = uac2_feedback_cb,
};

static struct usb_i2s_ctx main_ctx;

#if CONFIG_HW_CODEC_CIRRUS_LOGIC
/**
 * @brief Write to multiple registers in CS47L63.
 */
static int cs47l63_comm_reg_conf_write(const uint32_t config[][2], uint32_t num_of_regs)
{
	int ret;
	uint32_t reg;
	uint32_t value;

	for (int i = 0; i < num_of_regs; i++) {
		reg = config[i][0];
		value = config[i][1];

		if (reg == SPI_BUSY_WAIT) {
			LOG_DBG("Busy waiting instead of writing to CS47L63");
			/* Wait for us defined in value */
			k_busy_wait(value);
		} else {
			ret = cs47l63_write_reg(&cs47l63_driver, reg, value);
			if (ret) {
				printk("Failed to write to CS47L63 reg 0x%08X: %d\n", reg, ret);
				return ret;
			}
		}
	}

	return 0;
}

int hw_codec_default_conf_enable(void)
{
	int ret;

	ret = cs47l63_comm_reg_conf_write(clock_configuration, ARRAY_SIZE(clock_configuration));
	if (ret) {
		printk("Failed to configure clock of the hardware codec: %d\n", ret);
		return ret;
	}

	ret = cs47l63_comm_reg_conf_write(GPIO_configuration, ARRAY_SIZE(GPIO_configuration));
	if (ret) {
		printk("Failed to configure GPIOs of the hardware codec: %d\n", ret);
		return ret;
	}

	ret = cs47l63_comm_reg_conf_write(asp1_enable, ARRAY_SIZE(asp1_enable));
	if (ret) {
		printk("Failed to enable ASP1 of the hardware codec: %d\n", ret);
		return ret;
	}

	ret = cs47l63_comm_reg_conf_write(output_enable, ARRAY_SIZE(output_enable));
	if (ret) {
		printk("Failed to enable output of the hardware codec: %d\n", ret);
		return ret;
	}

	ret = cs47l63_write_reg(&cs47l63_driver, CS47L63_OUT1L_VOLUME_1,
				MAX_VOLUME_REG_VAL | CS47L63_OUT_VU);
	if (ret) {
		printk("Failed to set output volume: %d\n", ret);
		return ret;
	}

	/* Toggle FLL to start up CS47L63 */
	ret = cs47l63_comm_reg_conf_write(FLL_toggle, ARRAY_SIZE(FLL_toggle));
	if (ret) {
		printk("Failed to toggle FLL of the hardware codec: %d\n", ret);
		return ret;
	}

	return 0;
}

int hw_codec_soft_reset(void)
{
	int ret;

	ret = cs47l63_comm_reg_conf_write(output_disable, ARRAY_SIZE(output_disable));
	if (ret) {
		printk("Failed to disable output of the hardware codec: %d\n", ret);
		return ret;
	}

	ret = cs47l63_comm_reg_conf_write(soft_reset, ARRAY_SIZE(soft_reset));
	if (ret) {
		printk("Failed to reset the hardware codec: %d\n", ret);
		return ret;
	}

	return 0;
}

int hw_codec_init(void)
{
	int ret;

	ret = cs47l63_comm_init(&cs47l63_driver);
	if (ret) {
		printk("Failed to initialize the hardware codec: %d\n", ret);
		return ret;
	}

	/* Run a soft reset on start to make sure all registers are default values */
	ret = cs47l63_comm_reg_conf_write(soft_reset, ARRAY_SIZE(soft_reset));
	if (ret) {
		printk("Failed to reset the hardware codec: %d\n", ret);
		return ret;
	}

	cs47l63_driver.state = CS47L63_STATE_STANDBY;

	return 0;
}
#endif

int main(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(uac2_headphones));
	struct usbd_context *sample_usbd;
	struct i2s_config config;
	int ret;

	main_ctx.i2s_dev = DEVICE_DT_GET(I2S_TX);

	if (!device_is_ready(main_ctx.i2s_dev)) {
		printk("%s is not ready\n", main_ctx.i2s_dev->name);
		return 0;
	}

	config.word_size = SAMPLE_BIT_WIDTH;
	config.channels = NUMBER_OF_CHANNELS;
	config.format = I2S_FMT_DATA_FORMAT_I2S;
	config.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER;
	config.frame_clk_freq = SAMPLE_FREQUENCY;
	config.mem_slab = &i2s_tx_slab;
	config.block_size = MAX_BLOCK_SIZE;
	config.timeout = 0;

	ret = i2s_configure(main_ctx.i2s_dev, I2S_DIR_TX, &config);
	if (ret < 0) {
		printk("Failed to configure TX stream: %d\n", ret);
		return ret;
	}

	if (IS_ENABLED(CONFIG_BOARD_NRF5340_AUDIO_DK_NRF5340_CPUAPP)) {
		/* Configure the hardware codec now I2S bit-clock is running */
		ret = hw_codec_init();
		if (ret) {
			printk("Failed to initialize HW codec: %d", ret);
			return ret;
		}

		ret = hw_codec_default_conf_enable();
		if (ret) {
			printk("Failed to set default HW codec configuration: %d", ret);
			return ret;
		}
	}

	main_ctx.fb = feedback_init();

	usbd_uac2_set_ops(dev, &usb_audio_ops, &main_ctx);

	sample_usbd = sample_usbd_init_device(NULL);
	if (sample_usbd == NULL) {
		return -ENODEV;
	}

	ret = usbd_enable(sample_usbd);
	if (ret) {
		return ret;
	}

	return 0;
}
