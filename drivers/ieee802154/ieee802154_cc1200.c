/* ieee802154_cc1200.c - TI CC1200 driver */

/*
 * Copyright (c) 2017 Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_LEVEL CONFIG_SYS_LOG_IEEE802154_DRIVER_LEVEL
#define SYS_LOG_DOMAIN "dev/cc1200"
#include <logging/sys_log.h>

#include <errno.h>

#include <kernel.h>
#include <arch/cpu.h>

#include <board.h>
#include <device.h>
#include <init.h>
#include <net/net_if.h>
#include <net/net_pkt.h>

#include <misc/byteorder.h>
#include <string.h>
#include <random/rand32.h>

#include <spi.h>
#include <gpio.h>

#include <net/ieee802154_radio.h>

#include "ieee802154_cc1200.h"
#include "ieee802154_cc1200_rf.h"

#if defined(CONFIG_IEEE802154_CC1200_GPIO_SPI_CS)
static struct spi_cs_control cs_ctrl;
#endif

/* ToDo: supporting 802.15.4g will require GPIO2
 * used as CC1200_GPIO_SIG_RXFIFO_THR
 *
 * Note: GPIO3 is unused.
 */
#define CC1200_IOCFG3	CC1200_GPIO_SIG_MARC_2PIN_STATUS_0
#define CC1200_IOCFG2	CC1200_GPIO_SIG_MARC_2PIN_STATUS_1
#define CC1200_IOCFG0	CC1200_GPIO_SIG_PKT_SYNC_RXTX

/***********************
 * Debugging functions *
 **********************/
#if CONFIG_SYS_LOG_IEEE802154_DRIVER_LEVEL == 4
static void _cc1200_print_status(u8_t status)
{
	if (status == CC1200_STATUS_IDLE) {
		SYS_LOG_DBG("Idling");
	} else if (status == CC1200_STATUS_RX) {
		SYS_LOG_DBG("Receiving");
	} else if (status == CC1200_STATUS_TX) {
		SYS_LOG_DBG("Transmitting");
	} else if (status == CC1200_STATUS_FSTXON) {
		SYS_LOG_DBG("FS TX on");
	} else if (status == CC1200_STATUS_CALIBRATE) {
		SYS_LOG_DBG("Calibrating");
	} else if (status == CC1200_STATUS_SETTLING) {
		SYS_LOG_DBG("Settling");
	} else if (status == CC1200_STATUS_RX_FIFO_ERROR) {
		SYS_LOG_DBG("RX FIFO error!");
	} else if (status == CC1200_STATUS_TX_FIFO_ERROR) {
		SYS_LOG_DBG("TX FIFO error!");
	}
}
#else
#define _cc1200_print_status(...)
#endif /* CONFIG_SYS_LOG_IEEE802154_DRIVER_LEVEL */

/*********************
 * Generic functions *
 ********************/

bool _cc1200_access_reg(struct spi_config *spi, bool read, u8_t addr,
			void *data, size_t length, bool extended, bool burst)
{
	u8_t cmd_buf[2];
	struct spi_buf buf[2] = {
		{
			.buf = cmd_buf,
			.len = extended ? 2 : 1,
		},
		{
			.buf = data,
			.len = length,

		}
	};

	/*
	SYS_LOG_DBG("%s: addr 0x%02x - Data %p Length %u - %s, %s",
		    read ? "Read" : "Write", addr, data, length,
		    extended ? "extended" : "normal",
		    burst ? "burst" : "single");
	*/

	cmd_buf[0] = 0;

	if (burst) {
		cmd_buf[0] |= CC1200_ACCESS_BURST;
	}

	if (extended) {
		cmd_buf[0] |= CC1200_REG_EXTENDED_ADDRESS;
		cmd_buf[1] = addr;
	} else {
		cmd_buf[0] |= addr;
	}

	if (read) {
		cmd_buf[0] |= CC1200_ACCESS_RD;

		return (spi_transceive(spi, buf, 1, buf, 2) == 0);
	}

	/* CC1200_ACCESS_WR is 0 so no need to play with it */
	return (spi_write(spi, buf, data ? 2 : 1) == 0);
}

static inline u8_t *get_mac(struct device *dev)
{
	struct cc1200_context *cc1200 = dev->driver_data;

#if defined(CONFIG_IEEE802154_CC1200_RANDOM_MAC)
	u32_t *ptr = (u32_t *)(cc1200->mac_addr + 4);

	UNALIGNED_PUT(sys_rand32_get(), ptr);

	cc1200->mac_addr[7] = (cc1200->mac_addr[7] & ~0x01) | 0x02;
#else
	cc1200->mac_addr[4] = CONFIG_IEEE802154_CC1200_MAC4;
	cc1200->mac_addr[5] = CONFIG_IEEE802154_CC1200_MAC5;
	cc1200->mac_addr[6] = CONFIG_IEEE802154_CC1200_MAC6;
	cc1200->mac_addr[7] = CONFIG_IEEE802154_CC1200_MAC7;
#endif

	cc1200->mac_addr[0] = 0x00;
	cc1200->mac_addr[1] = 0x12;
	cc1200->mac_addr[2] = 0x4b;
	cc1200->mac_addr[3] = 0x00;

	return cc1200->mac_addr;
}

static u8_t get_status(struct spi_config *spi)
{
	u8_t val;

	if (_cc1200_access_reg(spi, true, CC1200_INS_SNOP,
			       &val, 1, false, false)) {
		/* See Section 3.1.2 */
		return val & CC1200_STATUS_MASK;
	}

	/* We cannot get the status, so let's assume about readyness */
	return CC1200_STATUS_CHIP_NOT_READY;
}

/******************
 * GPIO functions *
 *****************/

static inline void gpio0_int_handler(struct device *port,
				     struct gpio_callback *cb, u32_t pins)
{
	struct cc1200_context *cc1200 =
		CONTAINER_OF(cb, struct cc1200_context, rx_tx_cb);

	if (atomic_get(&cc1200->tx) == 1) {
		if (atomic_get(&cc1200->tx_start) == 0) {
			atomic_set(&cc1200->tx_start, 1);
		} else {
			atomic_set(&cc1200->tx, 0);
		}

		k_sem_give(&cc1200->tx_sync);
	} else {
		if (atomic_get(&cc1200->rx) == 1) {
			k_sem_give(&cc1200->rx_lock);
			atomic_set(&cc1200->rx, 0);
		} else {
			atomic_set(&cc1200->rx, 1);
		}
	}
}

static void enable_gpio0_interrupt(struct cc1200_context *cc1200, bool enable)
{
	if (enable) {
		gpio_pin_enable_callback(
			cc1200->gpios[CC1200_GPIO_IDX_GPIO0].dev,
			cc1200->gpios[CC1200_GPIO_IDX_GPIO0].pin);
	} else {
		gpio_pin_disable_callback(
			cc1200->gpios[CC1200_GPIO_IDX_GPIO0].dev,
			cc1200->gpios[CC1200_GPIO_IDX_GPIO0].pin);
	}
}

static void setup_gpio_callback(struct device *dev)
{
	struct cc1200_context *cc1200 = dev->driver_data;

	gpio_init_callback(&cc1200->rx_tx_cb, gpio0_int_handler,
			   BIT(cc1200->gpios[CC1200_GPIO_IDX_GPIO0].pin));
	gpio_add_callback(cc1200->gpios[CC1200_GPIO_IDX_GPIO0].dev,
			  &cc1200->rx_tx_cb);
}

/****************
 * RF functions *
 ***************/

static u8_t get_lo_divider(struct spi_config *spi)
{
	/* See Table 34  */
	return FSD_BANDSELECT(read_reg_fs_cfg(spi)) << 1;
}

static bool write_reg_freq(struct spi_config *spi, u32_t freq)
{
	u8_t freq_data[3];

	freq_data[0] = (u8_t)((freq & 0x00FF0000) >> 16);
	freq_data[1] = (u8_t)((freq & 0x0000FF00) >> 8);
	freq_data[2] = (u8_t)(freq & 0x000000FF);

	return _cc1200_access_reg(spi, false, CC1200_REG_FREQ2,
				  freq_data, 3, true, true);
}


/* See Section 9.12 - RF programming
 *
 * The given formula in datasheet cannot be simply applied here, where CPU
 * limits us to unsigned integers of 32 bits. Instead, "slicing" it to
 * parts that fits in such limit is a solution which is applied below.
 *
 * The original formula being (freqoff is neglegted):
 * Freq = ( RF * Lo_Div * 2^16 ) / Xtal
 *
 * RF and Xtal are, from here, expressed in KHz.
 *
 * It first calculates the targeted RF with given ChanCenterFreq0, channel
 * spacing and the channel number.
 *
 * The calculation will slice the targeted RF by multiple of 10:
 * 10^n where n is in [5, 3]. The rest, below 1000, is taken at once.
 * Let's take the 434000 KHz RF for instance:
 * it will be "sliced" in 3 parts: 400000, 30000, 4000.
 * Or the 169406 KHz RF, 4 parts: 100000, 60000, 9000, 406.
 *
 * This permits also to play with Xtal to keep the result big enough to avoid
 * losing precision. A factor - growing as much as Xtal decrease -  is then
 * applied to get to the proper result. Which one is rounded to the nearest
 * integer, again to get a bit better precision.
 *
 * In the end, this algorithm below works for all the supported bands by CC1200.
 * User does not need to pass anything extra besides the nominal settings: no
 * pre-computed part or else.
 */
static u32_t rf_evaluate_freq_setting(struct cc1200_context *cc1200, u32_t chan)
{
	u32_t xtal = CONFIG_IEEE802154_CC1200_XOSC;
	u32_t mult_10 = 100000;
	u32_t factor = 1;
	u32_t freq = 0;
	u32_t rf, lo_div;

	rf = cc1200->rf_settings->chan_center_freq0 +
		((chan * (u32_t)cc1200->rf_settings->channel_spacing) / 10);
	lo_div = get_lo_divider(&cc1200->spi);

	SYS_LOG_DBG("Calculating freq for %u KHz RF (%u)", rf, lo_div);

	while (rf > 0) {
		u32_t hz, freq_tmp, rst;

		if (rf < 1000) {
			hz = rf;
		} else {
			hz = rf / mult_10;
			hz *= mult_10;
		}

		if (hz < 1000) {
			freq_tmp = (hz * lo_div * 65536) / xtal;
		} else {
			freq_tmp = ((hz * lo_div) / xtal) * 65536;
		}

		rst = freq_tmp % factor;
		freq_tmp /= factor;

		if (factor > 1 && (rst/(factor/10)) > 5) {
			freq_tmp++;
		}

		freq += freq_tmp;

		factor *= 10;
		mult_10 /= 10;
		xtal /= 10;
		rf -= hz;
	}

	SYS_LOG_DBG("FREQ is 0x%06X", freq);

	return freq;
}

static bool
rf_install_settings(struct device *dev,
		    const struct cc1200_rf_registers_set *rf_settings)
{
	struct cc1200_context *cc1200 = dev->driver_data;

	if (!_cc1200_access_reg(&cc1200->spi, false, CC1200_REG_SYNC3,
				(void *)rf_settings->registers,
				CC1200_RF_NON_EXT_SPACE_REGS, false, true) ||
	    !_cc1200_access_reg(&cc1200->spi, false, CC1200_REG_IF_MIX_CFG,
				(void *)rf_settings->registers +
				CC1200_RF_NON_EXT_SPACE_REGS,
				CC1200_RF_EXT_SPACE_REGS, true, true) ||
	    !write_reg_pkt_len(&cc1200->spi, 0xFF)) {
		SYS_LOG_ERR("Could not install RF settings");
		return false;
	}

	cc1200->rf_settings = rf_settings;

	return true;
}

static int rf_calibrate(struct spi_config *spi)
{
	if (!instruct_scal(spi)) {
		SYS_LOG_ERR("Could not calibrate RF");
		return -EIO;
	}

	k_busy_wait(5 * USEC_PER_MSEC);

	/* We need to re-enable RX as SCAL shuts off the freq synth */
	if (!instruct_sidle(spi) ||
	    !instruct_sfrx(spi) ||
	    !instruct_srx(spi)) {
		SYS_LOG_ERR("Could not switch to RX");
		return -EIO;
	}

	k_busy_wait(10 * USEC_PER_MSEC);

	_cc1200_print_status(get_status(spi));

	return 0;
}

/****************
 * TX functions *
 ***************/

static inline bool write_txfifo(struct spi_config *spi,
				void *data, size_t length)
{
	return _cc1200_access_reg(spi, false,
				  CC1200_REG_TXFIFO,
				  data, length, false, true);
}

/****************
 * RX functions *
 ***************/

static inline bool read_rxfifo(struct spi_config *spi,
			       void *data, size_t length)
{
	return _cc1200_access_reg(spi, true,
				  CC1200_REG_RXFIFO,
				  data, length, false, true);
}

static inline u8_t get_packet_length(struct spi_config *spi)
{
	u8_t len;

	if (_cc1200_access_reg(spi, true, CC1200_REG_RXFIFO,
			       &len, 1, false, true)) {
		return len;
	}

	return 0;
}

static inline bool verify_rxfifo_validity(struct spi_config *spi, u8_t pkt_len)
{
	/* packet should be at least 3 bytes as a ACK */
	if (pkt_len < 3 ||
	    read_reg_num_rxbytes(spi) > (pkt_len + CC1200_FCS_LEN)) {
		return false;
	}

	return true;
}

static inline bool read_rxfifo_content(struct spi_config *spi,
				       struct net_buf *frag, u8_t len)
{

	if (!read_rxfifo(spi, frag->data, len) ||
	    (get_status(spi) == CC1200_STATUS_RX_FIFO_ERROR)) {
		return false;
	}

	net_buf_add(frag, len);

	return true;
}

static inline bool verify_crc(struct spi_config *spi, struct net_pkt *pkt)
{
	u8_t fcs[2];

	if (!read_rxfifo(spi, fcs, 2)) {
		return false;
	}

	if (!(fcs[1] & CC1200_FCS_CRC_OK)) {
		return false;
	}

	net_pkt_set_ieee802154_rssi(pkt, fcs[0]);
	net_pkt_set_ieee802154_lqi(pkt, fcs[1] & CC1200_FCS_LQI_MASK);

	return true;
}

static void cc1200_rx(struct device *dev)
{
	struct cc1200_context *cc1200 = dev->driver_data;
	struct net_buf *pkt_frag;
	struct net_pkt *pkt;
	u8_t pkt_len;

	while (1) {
		pkt = NULL;

		k_sem_take(&cc1200->rx_lock, K_FOREVER);

		if (get_status(&cc1200->spi) == CC1200_STATUS_RX_FIFO_ERROR) {
			SYS_LOG_ERR("Fifo error");
			goto flush;
		}

		pkt_len = get_packet_length(&cc1200->spi);
		if (!verify_rxfifo_validity(&cc1200->spi, pkt_len)) {
			SYS_LOG_ERR("Invalid frame");
			goto flush;
		}

		pkt = net_pkt_get_reserve_rx(0, K_NO_WAIT);
		if (!pkt) {
			SYS_LOG_ERR("No free pkt available");
			goto flush;
		}

		pkt_frag = net_pkt_get_frag(pkt, K_NO_WAIT);
		if (!pkt_frag) {
			SYS_LOG_ERR("No free frag available");
			goto flush;
		}

		net_pkt_frag_insert(pkt, pkt_frag);

		if (!read_rxfifo_content(&cc1200->spi, pkt_frag, pkt_len)) {
			SYS_LOG_ERR("No content read");
			goto flush;
		}

		if (!verify_crc(&cc1200->spi, pkt)) {
			SYS_LOG_ERR("Bad packet CRC");
			goto out;
		}

		if (ieee802154_radio_handle_ack(cc1200->iface, pkt) == NET_OK) {
			SYS_LOG_DBG("ACK packet handled");
			goto out;
		}

		SYS_LOG_DBG("Caught a packet (%u)", pkt_len);

		if (net_recv_data(cc1200->iface, pkt) < 0) {
			SYS_LOG_DBG("Packet dropped by NET stack");
			goto out;
		}

		net_analyze_stack("CC1200 Rx Fiber stack",
				  K_THREAD_STACK_BUFFER(cc1200->rx_stack),
				  K_THREAD_STACK_SIZEOF(cc1200->rx_stack));
		continue;
flush:
		SYS_LOG_DBG("Flushing RX");
		instruct_sidle(&cc1200->spi);
		instruct_sfrx(&cc1200->spi);
		instruct_srx(&cc1200->spi);
out:
		if (pkt) {
			net_pkt_unref(pkt);
		}

	}
}


/********************
 * Radio device API *
 *******************/
static enum ieee802154_hw_caps cc1200_get_capabilities(struct device *dev)
{
	return IEEE802154_HW_FCS | IEEE802154_HW_SUB_GHZ;
}

static int cc1200_cca(struct device *dev)
{
	struct cc1200_context *cc1200 = dev->driver_data;

	if (atomic_get(&cc1200->rx) == 0) {
		u8_t status = read_reg_rssi0(&cc1200->spi);

		if (!(status & CARRIER_SENSE) &&
		    (status & CARRIER_SENSE_VALID)) {
			return 0;
		}
	}

	SYS_LOG_WRN("Busy");

	return -EBUSY;
}

static int cc1200_set_channel(struct device *dev, u16_t channel)
{
	struct cc1200_context *cc1200 = dev->driver_data;

	/* Unlike usual 15.4 chips, cc1200 is closer to a bare metal radio modem
	 * and thus does not provide any means to select a channel directly, but
	 * requires instead that one calculates and configures the actual
	 * targeted frequency for the requested channel.
	 *
	 * See rf_evaluate_freq_setting() above.
	 */

	if (atomic_get(&cc1200->rx) == 0) {
		u32_t freq = rf_evaluate_freq_setting(cc1200, channel);

		if (!write_reg_freq(&cc1200->spi, freq) ||
		    rf_calibrate(&cc1200->spi)) {
			SYS_LOG_ERR("Could not set channel %u", channel);
			return -EIO;
		}
	}

	return 0;
}

static int cc1200_set_txpower(struct device *dev, s16_t dbm)
{
	struct cc1200_context *cc1200 = dev->driver_data;
	u8_t pa_power_ramp;

	SYS_LOG_DBG("%d dbm", dbm);

	/* See Section 7.1 */
	dbm = ((dbm + 18) * 2) - 1;
	if ((dbm <= 3) || (dbm >= 64)) {
		SYS_LOG_ERR("Unhandled value");
		return -EINVAL;
	}

	pa_power_ramp = read_reg_pa_cfg1(&cc1200->spi) & ~PA_POWER_RAMP_MASK;
	pa_power_ramp |= ((u8_t) dbm) & PA_POWER_RAMP_MASK;

	if (!write_reg_pa_cfg1(&cc1200->spi, pa_power_ramp)) {
		SYS_LOG_ERR("Could not proceed");
		return -EIO;
	}

	return 0;
}

static int cc1200_tx(struct device *dev,
		     struct net_pkt *pkt,
		     struct net_buf *frag)
{
	struct cc1200_context *cc1200 = dev->driver_data;
	u8_t *frame = frag->data - net_pkt_ll_reserve(pkt);
	u8_t len = net_pkt_ll_reserve(pkt) + frag->len;
	bool status = false;

	SYS_LOG_DBG("%p (%u)", frag, len);

	/* ToDo:
	 * Supporting 802.15.4g will require to loop in pkt's frags
	 * depending on len value, this will also take more time.
	 */

	if (!instruct_sidle(&cc1200->spi) ||
	    !instruct_sfrx(&cc1200->spi) ||
	    !instruct_sftx(&cc1200->spi) ||
	    !instruct_sfstxon(&cc1200->spi)) {
		SYS_LOG_ERR("Cannot switch to TX mode");
		goto out;
	}

	if (!write_txfifo(&cc1200->spi, &len, CC1200_PHY_HDR_LEN) ||
	    !write_txfifo(&cc1200->spi, frame, len) ||
	    read_reg_num_txbytes(&cc1200->spi) != (len + CC1200_PHY_HDR_LEN)) {
		SYS_LOG_ERR("Cannot fill-in TX fifo");
		goto out;
	}

	atomic_set(&cc1200->tx, 1);
	atomic_set(&cc1200->tx_start, 0);

	if (!instruct_stx(&cc1200->spi)) {
		SYS_LOG_ERR("Cannot start transmission");
		goto out;
	}

	/* Wait for SYNC to be sent */
	k_sem_take(&cc1200->tx_sync, 100);
	if (atomic_get(&cc1200->tx_start) == 1) {
		/* Now wait for the packet to be fully sent */
		k_sem_take(&cc1200->tx_sync, 100);
	}

out:
	_cc1200_print_status(get_status(&cc1200->spi));

	if (atomic_get(&cc1200->tx) == 1 &&
	    read_reg_num_txbytes(&cc1200->spi) != 0) {
		SYS_LOG_ERR("TX Failed");

		atomic_set(&cc1200->tx_start, 0);
		instruct_sftx(&cc1200->spi);
		status = false;
	} else {
		status = true;
	}

	atomic_set(&cc1200->tx, 0);

	/* Get back to RX */
	instruct_srx(&cc1200->spi);

	return status ? 0 : -EIO;
}

static int cc1200_start(struct device *dev)
{
	struct cc1200_context *cc1200 = dev->driver_data;

	if (!instruct_sidle(&cc1200->spi) ||
	    !instruct_sftx(&cc1200->spi) ||
	    !instruct_sfrx(&cc1200->spi) ||
	    rf_calibrate(&cc1200->spi)) {
		SYS_LOG_ERR("Could not proceed");
		return -EIO;
	}

	enable_gpio0_interrupt(cc1200, true);

	_cc1200_print_status(get_status(&cc1200->spi));

	return 0;
}

static int cc1200_stop(struct device *dev)
{
	struct cc1200_context *cc1200 = dev->driver_data;

	enable_gpio0_interrupt(cc1200, false);

	if (!instruct_spwd(&cc1200->spi)) {
		SYS_LOG_ERR("Could not proceed");
		return -EIO;
	}

	return 0;
}

static u16_t cc1200_get_channel_count(struct device *dev)
{
	struct cc1200_context *cc1200 = dev->driver_data;

	return cc1200->rf_settings->channel_limit;
}

/******************
 * Initialization *
 *****************/

static int power_on_and_setup(struct device *dev)
{
	struct cc1200_context *cc1200 = dev->driver_data;

	if (!instruct_sres(&cc1200->spi)) {
		SYS_LOG_ERR("Cannot reset");
		return -EIO;
	}

	if (!rf_install_settings(dev, &cc1200_rf_settings)) {
		return -EIO;
	}

	if (!write_reg_iocfg3(&cc1200->spi, CC1200_IOCFG3) ||
	    !write_reg_iocfg2(&cc1200->spi, CC1200_IOCFG2) ||
	    !write_reg_iocfg0(&cc1200->spi, CC1200_IOCFG0)) {
		SYS_LOG_ERR("Cannot configure GPIOs");
		return -EIO;
	}

	setup_gpio_callback(dev);

	return rf_calibrate(&cc1200->spi);
}

static int configure_spi(struct device *dev)
{
	struct cc1200_context *cc1200 = dev->driver_data;

	cc1200->spi.dev = device_get_binding(
			CONFIG_IEEE802154_CC1200_SPI_DRV_NAME);
	if (!cc1200->spi.dev) {
		SYS_LOG_ERR("Unable to get SPI device");
		return -ENODEV;
	}

	if (IS_ENABLED(CONFIG_IEEE802154_CC1200_GPIO_SPI_CS)) {
		cs_ctrl.gpio_dev = device_get_binding(
			CONFIG_IEEE802154_CC1200_GPIO_SPI_CS_DRV_NAME);
		if (!cs_ctrl.gpio_dev) {
			SYS_LOG_ERR("Unable to get GPIO SPI CS device");
			return -ENODEV;
		}

		cs_ctrl.gpio_pin = CONFIG_IEEE802154_CC1200_GPIO_SPI_CS_PIN;
		cs_ctrl.delay = 0;

		cc1200->spi.cs = &cs_ctrl;

		SYS_LOG_DBG("SPI GPIO CS configured on %s:%u",
			    CONFIG_IEEE802154_CC1200_GPIO_SPI_CS_DRV_NAME,
			    CONFIG_IEEE802154_CC1200_GPIO_SPI_CS_PIN);
	}

	cc1200->spi.operation = SPI_WORD_SET(8);
	cc1200->spi.frequency = CONFIG_IEEE802154_CC1200_SPI_FREQ;
	cc1200->spi.slave = CONFIG_IEEE802154_CC1200_SPI_SLAVE;

	return 0;
}

static int cc1200_init(struct device *dev)
{
	struct cc1200_context *cc1200 = dev->driver_data;

	atomic_set(&cc1200->tx, 0);
	atomic_set(&cc1200->tx_start, 0);
	atomic_set(&cc1200->rx, 0);
	k_sem_init(&cc1200->rx_lock, 0, 1);
	k_sem_init(&cc1200->tx_sync, 0, 1);

	cc1200->gpios = cc1200_configure_gpios();
	if (!cc1200->gpios) {
		SYS_LOG_ERR("Configuring GPIOS failed");
		return -EIO;
	}

	if (configure_spi(dev) != 0) {
		SYS_LOG_ERR("Configuring SPI failed");
		return -EIO;
	}

	SYS_LOG_DBG("GPIO and SPI configured");

	if (power_on_and_setup(dev) != 0) {
		SYS_LOG_ERR("Configuring CC1200 failed");
		return -EIO;
	}

	k_thread_create(&cc1200->rx_thread, cc1200->rx_stack,
			CONFIG_IEEE802154_CC1200_RX_STACK_SIZE,
			(k_thread_entry_t)cc1200_rx,
			dev, NULL, NULL, K_PRIO_COOP(2), 0, 0);

	SYS_LOG_INF("CC1200 initialized");

	return 0;
}

static void cc1200_iface_init(struct net_if *iface)
{
	struct device *dev = net_if_get_device(iface);
	struct cc1200_context *cc1200 = dev->driver_data;
	u8_t *mac = get_mac(dev);

	SYS_LOG_DBG("");

	net_if_set_link_addr(iface, mac, 8, NET_LINK_IEEE802154);

	cc1200->iface = iface;

	ieee802154_init(iface);
}

static struct cc1200_context cc1200_context_data;

static struct ieee802154_radio_api cc1200_radio_api = {
	.iface_api.init	= cc1200_iface_init,
	.iface_api.send	= ieee802154_radio_send,

	.get_capabilities	= cc1200_get_capabilities,
	.cca			= cc1200_cca,
	.set_channel		= cc1200_set_channel,
	.set_txpower		= cc1200_set_txpower,
	.tx			= cc1200_tx,
	.start			= cc1200_start,
	.stop			= cc1200_stop,
	.get_subg_channel_count = cc1200_get_channel_count,
};

NET_DEVICE_INIT(cc1200, CONFIG_IEEE802154_CC1200_DRV_NAME,
		cc1200_init, &cc1200_context_data, NULL,
		CONFIG_IEEE802154_CC1200_INIT_PRIO,
		&cc1200_radio_api, IEEE802154_L2,
		NET_L2_GET_CTX_TYPE(IEEE802154_L2), 125);
