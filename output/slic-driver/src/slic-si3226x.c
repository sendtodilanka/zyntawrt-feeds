// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Silicon Labs Si3226x / Si3228x SLIC driver for ZyntaWRT / AW1000
 *
 * Target platform : Arcadyan AW1000 (IPQ807x AP-HK09, machid 0x8010008)
 * SPI bus         : spi@78b8000 (BLSP QUP SPI3)
 * Chip select     : CS0
 * SPI max freq    : 960 kHz  — CONFIRMED from stock firmware hk09 DTB
 * Reset GPIO      : GPIO46, active-high
 * IRQ GPIO        : GPIO47, IRQ_TYPE_EDGE_FALLING
 *
 * Compatible strings (as found in stock firmware hk09 DTB):
 *   "siliconlab,si32xxx"   — primary (used by slic.ko to bind)
 *   "silabs,si3226x"       — generic fallback
 *
 * -----------------------------------------------------------------------
 * ProSLIC API dependency
 * -----------------------------------------------------------------------
 * This driver calls into the Silicon Labs ProSLIC API (v5.6.4L or later).
 * The API source files must be placed in src/proslic_api/ alongside this
 * file and are NOT distributed here (proprietary / registration required
 * from Skyworks).  A compatible free-software reference is available at:
 *   jameshilliard/WECB-VZ-GPL  proslic_api-v5.6.4L/
 *
 * Required API source files (add to src/proslic_api/):
 *   si_voice.c, si3226x_intf.c, si3226x_userdef.c, proslic.c
 *
 * -----------------------------------------------------------------------
 * PCM / TDM dependency
 * -----------------------------------------------------------------------
 * The IPQ8074 PCM TDM driver (CONFIG_SND_SOC_IPQ_PCM_TDM) is required.
 * It is absent from upstream OpenWrt — a kernel patch sourced from
 * Qualcomm QSDK (git.codelinaro.org/clo/qsdk) must be applied first.
 * See: docs/patches/ipq-pcm-tdm.md
 *
 * Stock firmware mapping:
 *   IPQ PCM DMA TX ch 6 (stereo port 3) → Si3226x PCM RX (voice from SoC)
 *   IPQ PCM DMA RX ch 1 (stereo port 0) → Si3226x PCM TX (voice to SoC)
 *
 * Date: 2026-03-30
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/poll.h>
#include <linux/wait.h>

/* ---- ProSLIC API type definitions (from si_voice.h / si_voice_datatypes.h) */

typedef unsigned char  uInt8;
typedef unsigned short uInt16;
typedef unsigned int   uInt32;
typedef int            int32;

/* SI3226X_TYPE = 5 (from si_voice.h) */
#define SI3226X_TYPE  5

/* LF_* linefeed status codes (from proslic.h) */
#define LF_OPEN        0
#define LF_FWD_ACTIVE  1
#define LF_FWD_OHT     2
#define LF_TIP_OPEN    3
#define LF_RINGING     4
#define LF_REV_ACTIVE  5
#define LF_REV_OHT     6
#define LF_RING_OPEN   7

/* ---- ioctl interface for userspace (voiced daemon) ---- */
#define PROSLIC_IOC_MAGIC	'P'
#define PROSLIC_RING_ON		_IO(PROSLIC_IOC_MAGIC, 1)
#define PROSLIC_RING_OFF	_IO(PROSLIC_IOC_MAGIC, 2)
#define PROSLIC_GET_HOOK_STATUS	_IOR(PROSLIC_IOC_MAGIC, 3, int)
#define PROSLIC_GET_DTMF	_IOR(PROSLIC_IOC_MAGIC, 4, int)
#define PROSLIC_PLAY_TONE	_IOW(PROSLIC_IOC_MAGIC, 5, int)
#define PROSLIC_SET_LINEFEED	_IOW(PROSLIC_IOC_MAGIC, 6, int)

/* Si3226x hook status register (reg 4, LCRRTP / LOOP_STAT) */
#define SI3226X_REG_LOOP_STAT	4
#define SI3226X_HOOK_OFF	0x01	/* bit 0: loop closure */

/* Si3226x DTMF decode register (reg 24, TONDTMF) */
#define SI3226X_REG_TONDTMF	24
#define SI3226X_DTMF_VALID	0x10	/* bit 4: valid digit */
#define SI3226X_DTMF_DIGIT_MASK	0x0f

/* Tone IDs for PLAY_TONE ioctl */
#define TONE_NONE		0
#define TONE_DIAL		1
#define TONE_RINGBACK		2
#define TONE_BUSY		3
#define TONE_CONGESTION		4

/* Si3226x tone generator oscillator registers */
#define SI3226X_REG_OCON	48	/* Oscillator control */
#define SI3226X_REG_OMODE	49	/* Oscillator mode */

/*
 * ProSLIC API control interface callback typedefs.
 * These match the signatures defined in si_voice_ctrl.h.
 */
typedef int (*ctrl_Reset_fptr)(void *hCtrl, int reset);
typedef int (*ctrl_WriteRegister_fptr)(void *hCtrl, uInt8 channel,
				       uInt8 reg, uInt8 val);
typedef uInt8 (*ctrl_ReadRegister_fptr)(void *hCtrl, uInt8 channel, uInt8 reg);
typedef int (*ctrl_WriteRAM_fptr)(void *hCtrl, uInt8 channel,
				  uInt16 addr, uInt32 val);
typedef uInt32 (*ctrl_ReadRAM_fptr)(void *hCtrl, uInt8 channel, uInt16 addr);
typedef int (*ctrl_Semaphore_fptr)(void *hCtrl, int state);
typedef int (*system_delay_fptr)(void *hTimer, int ms);
typedef int (*system_timeElapsed_fptr)(void *hTimer, uInt32 startTime,
				       uInt32 *elapsed);
typedef int (*system_getTime_fptr)(void *hTimer, uInt32 *sysTime);

/*
 * SiVoiceControlInterfaceType — wires SPI and timer callbacks into ProSLIC API.
 * Mirrors the struct in si_voice.h (ProSLIC API v5.6.4L).
 */
typedef struct {
	void				*hCtrl;
	ctrl_Reset_fptr			 Reset_fptr;
	ctrl_WriteRegister_fptr		 WriteRegister_fptr;
	ctrl_ReadRegister_fptr		 ReadRegister_fptr;
	ctrl_WriteRAM_fptr		 WriteRAM_fptr;
	ctrl_ReadRAM_fptr		 ReadRAM_fptr;
	ctrl_Semaphore_fptr		 Semaphore_fptr;
	void				*hTimer;
	system_delay_fptr		 Delay_fptr;
	system_timeElapsed_fptr		 timeElapsed_fptr;
	system_getTime_fptr		 getTime_fptr;
	int				 usermodeStatus;
} SiVoiceControlInterfaceType;

/* SiVoiceDeviceType — represents one Si3226x chip (2 channels). */
typedef struct {
	SiVoiceControlInterfaceType	*ctrlInterface;
	uInt8				 chipRev;
	uInt8				 chipType;
	uInt8				 lsRev;
	uInt8				 lsType;
	int				 usermodeStatus;
} SiVoiceDeviceType;
typedef SiVoiceDeviceType *SiVoiceDeviceType_ptr;

/* SiVoiceChanType — represents one voice channel (one RJ11 port). */
typedef struct {
	SiVoiceDeviceType_ptr		 deviceId;
	uInt8				 channel;
	uInt8				 channelType;
	int				 error;
	int				 debugMode;
	int				 channelEnable;
	uInt8				 bomOption;
} SiVoiceChanType;
typedef SiVoiceChanType *SiVoiceChanType_ptr;

/* Aliases used by si3226x_intf.h / proslic.h */
typedef SiVoiceDeviceType    ProslicDeviceType;
typedef SiVoiceChanType     *proslicChanType_ptr;
typedef SiVoiceControlInterfaceType controlInterfaceType;

/* ---- Forward declarations for ProSLIC API functions (from API source) ---- */

extern int SiVoice_createControlInterface(SiVoiceControlInterfaceType **ppCtrl);
extern int SiVoice_destroyControlInterface(SiVoiceControlInterfaceType **ppCtrl);
extern int SiVoice_createDevice(SiVoiceDeviceType **ppDev);
extern int SiVoice_destroyDevice(SiVoiceDeviceType **ppDev);
extern int SiVoice_createChannel(SiVoiceChanType_ptr *ppChan);
extern int SiVoice_destroyChannel(SiVoiceChanType_ptr *ppChan);

extern int SiVoice_setControlInterfaceCtrlObj(SiVoiceControlInterfaceType *pCtrl,
					      void *hCtrl);
extern int SiVoice_setControlInterfaceReset(SiVoiceControlInterfaceType *pCtrl,
					    ctrl_Reset_fptr fn);
extern int SiVoice_setControlInterfaceWriteRegister(SiVoiceControlInterfaceType *pCtrl,
						    ctrl_WriteRegister_fptr fn);
extern int SiVoice_setControlInterfaceReadRegister(SiVoiceControlInterfaceType *pCtrl,
						   ctrl_ReadRegister_fptr fn);
extern int SiVoice_setControlInterfaceWriteRAM(SiVoiceControlInterfaceType *pCtrl,
					       ctrl_WriteRAM_fptr fn);
extern int SiVoice_setControlInterfaceReadRAM(SiVoiceControlInterfaceType *pCtrl,
					      ctrl_ReadRAM_fptr fn);
extern int SiVoice_setControlInterfaceTimerObj(SiVoiceControlInterfaceType *pCtrl,
					       void *hTimer);
extern int SiVoice_setControlInterfaceDelay(SiVoiceControlInterfaceType *pCtrl,
					    system_delay_fptr fn);
extern int SiVoice_setControlInterfaceTimeElapsed(SiVoiceControlInterfaceType *pCtrl,
						  system_timeElapsed_fptr fn);
extern int SiVoice_setControlInterfaceGetTime(SiVoiceControlInterfaceType *pCtrl,
					      system_getTime_fptr fn);
extern int SiVoice_setControlInterfaceSemaphore(SiVoiceControlInterfaceType *pCtrl,
						ctrl_Semaphore_fptr fn);

extern int SiVoice_SWInitChan(SiVoiceChanType_ptr hChan, int channel,
			      int chipType, SiVoiceDeviceType *pDev,
			      SiVoiceControlInterfaceType *pCtrl);
extern int SiVoice_Reset(SiVoiceChanType_ptr pChan);

extern int Si3226x_Init(proslicChanType_ptr *hProslic, int size);
extern int Si3226x_PCMSetup(SiVoiceChanType *pProslic, int preset);
extern int Si3226x_PCMTimeSlotSetup(SiVoiceChanType *pProslic,
				    uInt16 rxcount, uInt16 txcount);
extern int Si3226x_PCMStart(proslicChanType_ptr hProslic);
extern int Si3226x_DCFeedSetup(SiVoiceChanType *pProslic, int preset);
extern int Si3226x_SetLinefeedStatus(SiVoiceChanType *pProslic, uInt8 newLF);
extern int Si3226x_ShutdownChannel(proslicChanType_ptr hProslic);

/* ---- Driver constants ---------------------------------------------------- */

#define DRIVER_NAME		"slic-si3226x"
#define SLIC_NUM_CHANNELS	2	/* Si3226x family is dual-channel;
					 * exact part (Si32261-FM2) not confirmed
					 * from firmware — family only */

/*
 * SPI clock rate used by the AW1000 stock firmware.
 * CONFIRMED from hk09 DTB: spi-max-frequency = <0xea600> = 960000 Hz.
 * Do NOT change this to 8 MHz — the Si3226x on this board runs at 960 kHz.
 */
#define SLIC_SPI_MAX_FREQ_HZ	960000

/*
 * PCM timeslot configuration (8-bit PCM, short frame).
 *
 * The IPQ8074 PCM TDM driver assigns:
 *   DMA TX ch 6 (stereo port 3) — audio from SoC to Si3226x (Si3226x RX)
 *   DMA RX ch 1 (stereo port 0) — audio from Si3226x to SoC (Si3226x TX)
 *
 * Si3226x PCM timeslot assignments (offset in bits from frame sync):
 *   Channel 0: rxcount=0,  txcount=0   (timeslot 0)
 *   Channel 1: rxcount=8,  txcount=8   (timeslot 1, 8 bits later)
 *
 * Adjust these if the IPQ PCM driver uses a different frame mapping.
 */
#define SLIC_PCM_RX_SLOT_CH0	0
#define SLIC_PCM_TX_SLOT_CH0	0
#define SLIC_PCM_SLOT_STRIDE	8	/* 8-bit PCM: next channel = +8 bits */

/* ProSLIC API preset indices (index into si3226x_userdef.c preset tables) */
#define SLIC_PCM_PRESET		0
#define SLIC_DCFEED_PRESET	0

/* ---- SPI HAL context ----------------------------------------------------- */

/*
 * slic_spi_ctx — passed to ProSLIC API as hCtrl.
 * Holds everything the SPI callbacks need without a global pointer.
 */
struct slic_spi_ctx {
	struct spi_device	*spi;
	struct gpio_desc	*reset_gpio;
};

/*
 * Si3226x SPI register access protocol (from slic.ko reverse engineering):
 *
 *   Write register:  [0x00 | (chan<<6) | (reg & 0x3F)]  [val]   (2 bytes)
 *   Read register:   [0x80 | (chan<<6) | (reg & 0x3F)]  [0x00]  (2 bytes, return byte 1)
 *
 *   Write RAM addr:  [0x07 | (chan<<6)]  [addr_hi]  [addr_lo]  [val b3] [val b2] [val b1] [val b0]
 *   Read  RAM addr:  [0x87 | (chan<<6)]  [addr_hi]  [addr_lo]  [0]*4   → returns 4 bytes
 *
 * channel bits: channel 0 = 0x00, channel 1 = 0x40
 */

static int slic_hal_reset(void *hCtrl, int reset)
{
	struct slic_spi_ctx *ctx = hCtrl;

	/* reset=1: assert (hold chip in reset); reset=0: release */
	gpiod_set_value_cansleep(ctx->reset_gpio, reset ? 1 : 0);
	msleep(10);
	return 0;
}

static int slic_hal_write_reg(void *hCtrl, uInt8 channel, uInt8 reg, uInt8 val)
{
	struct slic_spi_ctx *ctx = hCtrl;
	u8 txbuf[2];

	txbuf[0] = (channel << 6) | (reg & 0x3f);	/* WR: bit7=0 */
	txbuf[1] = val;
	return spi_write(ctx->spi, txbuf, sizeof(txbuf));
}

static uInt8 slic_hal_read_reg(void *hCtrl, uInt8 channel, uInt8 reg)
{
	struct slic_spi_ctx *ctx = hCtrl;
	u8 tx[2] = { 0x80 | (channel << 6) | (reg & 0x3f), 0x00 };
	u8 rx[2] = { 0, 0 };
	struct spi_transfer xfer = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len    = 2,
	};
	int ret;

	/* Full-duplex: Si3226x clocks data out on MISO while we clock in cmd */
	ret = spi_sync_transfer(ctx->spi, &xfer, 1);
	if (ret)
		return 0xff;
	return rx[1];
}

static int slic_hal_write_ram(void *hCtrl, uInt8 channel, uInt16 addr, uInt32 val)
{
	struct slic_spi_ctx *ctx = hCtrl;
	u8 txbuf[7];

	txbuf[0] = 0x07 | (channel << 6);		/* WR RAM: bit7=0, reg=0x07 */
	txbuf[1] = (addr >> 8) & 0xff;
	txbuf[2] = addr & 0xff;
	txbuf[3] = (val >> 24) & 0xff;
	txbuf[4] = (val >> 16) & 0xff;
	txbuf[5] = (val >>  8) & 0xff;
	txbuf[6] = val & 0xff;
	return spi_write(ctx->spi, txbuf, sizeof(txbuf));
}

static uInt32 slic_hal_read_ram(void *hCtrl, uInt8 channel, uInt16 addr)
{
	struct slic_spi_ctx *ctx = hCtrl;
	u8 tx[3] = {
		0x87 | (channel << 6),			/* RD RAM: bit7=1, reg=0x07 */
		(addr >> 8) & 0xff,
		addr & 0xff
	};
	u8 rx[4] = { 0, 0, 0, 0 };
	int ret;

	ret = spi_write_then_read(ctx->spi, tx, sizeof(tx), rx, sizeof(rx));
	if (ret)
		return 0xffffffffU;
	return ((uInt32)rx[0] << 24) | ((uInt32)rx[1] << 16) |
	       ((uInt32)rx[2] << 8)  |  (uInt32)rx[3];
}

static int slic_hal_delay(void *hTimer, int ms)
{
	(void)hTimer;
	msleep(ms);
	return 0;
}

static int slic_hal_time_elapsed(void *hTimer, uInt32 startTime,
				 uInt32 *elapsed)
{
	uInt32 now = (uInt32)jiffies_to_msecs(jiffies);

	(void)hTimer;
	if (elapsed)
		*elapsed = now - startTime;
	return 0;
}

static int slic_hal_get_time(void *hTimer, uInt32 *sysTime)
{
	(void)hTimer;
	if (sysTime)
		*sysTime = (uInt32)jiffies_to_msecs(jiffies);
	return 0;
}

static int slic_hal_semaphore(void *hCtrl, int state)
{
	/* No semaphore needed — Linux SPI bus locking handles concurrency */
	(void)hCtrl;
	(void)state;
	return 0;
}

/* ---- ProSLIC init -------------------------------------------------------- */

/*
 * slic_si3226x_dev — allocated per chip, stored in spi driver data.
 */
struct slic_si3226x {
	struct spi_device		*spi;
	struct gpio_desc		*reset_gpio;
	int				 irq;

	/* ProSLIC API objects */
	struct slic_spi_ctx		 spi_ctx;
	SiVoiceControlInterfaceType	*ctrl_intf;
	SiVoiceDeviceType		*proslic_dev;
	SiVoiceChanType_ptr		 chan[SLIC_NUM_CHANNELS];

	/* chardev / ioctl interface */
	struct miscdevice		 miscdev[SLIC_NUM_CHANNELS];
	char				 devname[SLIC_NUM_CHANNELS][16];
	wait_queue_head_t		 hook_wq;
	int				 hook_event;
};

/* Global pointer table — needed so file_operations can find struct from minor */
static struct slic_si3226x *slic_devs[SLIC_NUM_CHANNELS];
static DEFINE_MUTEX(slic_devs_lock);

/*
 * si3226x_proslic_init - Full ProSLIC API initialisation sequence.
 *
 * Called from probe() after GPIO and IRQ setup.
 * Implements the standard Si3226x bring-up sequence:
 *   1. Allocate ProSLIC API objects
 *   2. Wire SPI/GPIO callbacks into control interface
 *   3. Software-initialise each channel object
 *   4. Hardware reset
 *   5. Si3226x_Init() — patch download, calibration, DCDC power-up
 *   6. PCM timeslot setup
 *   7. DC feed configuration
 *   8. PCM start
 *   9. Set linefeed active
 */
static int si3226x_proslic_init(struct slic_si3226x *priv)
{
	struct device *dev = &priv->spi->dev;
	int i, ret;

	/* ------------------------------------------------------------------ */
	/* Step 1 — Allocate ProSLIC API objects                               */
	/* ------------------------------------------------------------------ */

	ret = SiVoice_createControlInterface(&priv->ctrl_intf);
	if (ret) {
		dev_err(dev, "SiVoice_createControlInterface failed: %d\n", ret);
		return -ENOMEM;
	}

	ret = SiVoice_createDevice(&priv->proslic_dev);
	if (ret) {
		dev_err(dev, "SiVoice_createDevice failed: %d\n", ret);
		goto err_free_ctrl;
	}

	for (i = 0; i < SLIC_NUM_CHANNELS; i++) {
		ret = SiVoice_createChannel(&priv->chan[i]);
		if (ret) {
			dev_err(dev, "SiVoice_createChannel(%d) failed: %d\n",
				i, ret);
			goto err_free_chans;
		}
	}

	/* ------------------------------------------------------------------ */
	/* Step 2 — Wire SPI and GPIO callbacks into control interface         */
	/* ------------------------------------------------------------------ */

	priv->spi_ctx.spi        = priv->spi;
	priv->spi_ctx.reset_gpio = priv->reset_gpio;

	SiVoice_setControlInterfaceCtrlObj(priv->ctrl_intf, &priv->spi_ctx);
	SiVoice_setControlInterfaceReset(priv->ctrl_intf, slic_hal_reset);
	SiVoice_setControlInterfaceWriteRegister(priv->ctrl_intf,
						 slic_hal_write_reg);
	SiVoice_setControlInterfaceReadRegister(priv->ctrl_intf,
						slic_hal_read_reg);
	SiVoice_setControlInterfaceWriteRAM(priv->ctrl_intf, slic_hal_write_ram);
	SiVoice_setControlInterfaceReadRAM(priv->ctrl_intf, slic_hal_read_ram);
	SiVoice_setControlInterfaceTimerObj(priv->ctrl_intf, NULL);
	SiVoice_setControlInterfaceDelay(priv->ctrl_intf, slic_hal_delay);
	SiVoice_setControlInterfaceTimeElapsed(priv->ctrl_intf,
					       slic_hal_time_elapsed);
	SiVoice_setControlInterfaceGetTime(priv->ctrl_intf, slic_hal_get_time);
	SiVoice_setControlInterfaceSemaphore(priv->ctrl_intf, slic_hal_semaphore);

	/* ------------------------------------------------------------------ */
	/* Step 3 — Software-initialise each channel (no SPI yet)             */
	/* ------------------------------------------------------------------ */

	for (i = 0; i < SLIC_NUM_CHANNELS; i++) {
		ret = SiVoice_SWInitChan(priv->chan[i], i, SI3226X_TYPE,
					 priv->proslic_dev, priv->ctrl_intf);
		if (ret) {
			dev_err(dev, "SiVoice_SWInitChan(%d) failed: %d\n",
				i, ret);
			goto err_free_chans;
		}
	}

	/* ------------------------------------------------------------------ */
	/* Step 4 — Hardware reset (>500 ms for full DCDC settle)             */
	/* Uses GPIO46 active-high reset confirmed from hk09 DTB.             */
	/* ------------------------------------------------------------------ */

	ret = SiVoice_Reset(priv->chan[0]);
	if (ret) {
		dev_err(dev, "SiVoice_Reset failed: %d\n", ret);
		goto err_free_chans;
	}

	/* ------------------------------------------------------------------ */
	/* Step 5 — Full chip init: patch download, calibration, DCDC         */
	/* ------------------------------------------------------------------ */

	ret = Si3226x_Init(priv->chan, SLIC_NUM_CHANNELS);
	if (ret) {
		dev_err(dev, "Si3226x_Init failed: %d\n", ret);
		goto err_free_chans;
	}

	/* ------------------------------------------------------------------ */
	/* Step 6 — PCM timeslot setup                                         */
	/*                                                                     */
	/* IPQ8074 PCM TDM DMA assignments (stock firmware):                  */
	/*   DMA TX ch 6 (stereo port 3) → Si3226x PCM RX                    */
	/*   DMA RX ch 1 (stereo port 0) → Si3226x PCM TX                    */
	/*                                                                     */
	/* Si3226x PCM timeslots (8-bit linear PCM, short frame):             */
	/*   channel 0: rx=0,  tx=0   (timeslot 0)                            */
	/*   channel 1: rx=8,  tx=8   (timeslot 1, offset by 8 bits)          */
	/* ------------------------------------------------------------------ */

	for (i = 0; i < SLIC_NUM_CHANNELS; i++) {
		uInt16 rx_slot = SLIC_PCM_RX_SLOT_CH0 + i * SLIC_PCM_SLOT_STRIDE;
		uInt16 tx_slot = SLIC_PCM_TX_SLOT_CH0 + i * SLIC_PCM_SLOT_STRIDE;

		ret = Si3226x_PCMSetup(priv->chan[i], SLIC_PCM_PRESET);
		if (ret) {
			dev_err(dev, "Si3226x_PCMSetup(%d) failed: %d\n",
				i, ret);
			goto err_free_chans;
		}

		ret = Si3226x_PCMTimeSlotSetup(priv->chan[i], rx_slot, tx_slot);
		if (ret) {
			dev_err(dev, "Si3226x_PCMTimeSlotSetup(%d) failed: %d\n",
				i, ret);
			goto err_free_chans;
		}
	}

	/* ------------------------------------------------------------------ */
	/* Step 7 — DC feed configuration (sets VBAT ~57V, current limits)   */
	/* ------------------------------------------------------------------ */

	for (i = 0; i < SLIC_NUM_CHANNELS; i++) {
		ret = Si3226x_DCFeedSetup(priv->chan[i], SLIC_DCFEED_PRESET);
		if (ret) {
			dev_err(dev, "Si3226x_DCFeedSetup(%d) failed: %d\n",
				i, ret);
			goto err_free_chans;
		}
	}

	/* ------------------------------------------------------------------ */
	/* Step 8 — Start PCM clocks (enables TDM interface)                  */
	/* ------------------------------------------------------------------ */

	for (i = 0; i < SLIC_NUM_CHANNELS; i++) {
		ret = Si3226x_PCMStart(priv->chan[i]);
		if (ret) {
			dev_err(dev, "Si3226x_PCMStart(%d) failed: %d\n",
				i, ret);
			goto err_free_chans;
		}
	}

	/* ------------------------------------------------------------------ */
	/* Step 9 — Set forward-active linefeed (normal talk state)           */
	/* ------------------------------------------------------------------ */

	for (i = 0; i < SLIC_NUM_CHANNELS; i++) {
		ret = Si3226x_SetLinefeedStatus(priv->chan[i], LF_FWD_ACTIVE);
		if (ret) {
			dev_err(dev, "Si3226x_SetLinefeedStatus(%d) failed: %d\n",
				i, ret);
			goto err_free_chans;
		}
	}

	dev_info(dev, "Si3226x ProSLIC: %d channels initialised, PCM active\n",
		 SLIC_NUM_CHANNELS);
	return 0;

err_free_chans:
	for (i = SLIC_NUM_CHANNELS - 1; i >= 0; i--) {
		if (priv->chan[i]) {
			SiVoice_destroyChannel(&priv->chan[i]);
			priv->chan[i] = NULL;
		}
	}
	SiVoice_destroyDevice(&priv->proslic_dev);
err_free_ctrl:
	SiVoice_destroyControlInterface(&priv->ctrl_intf);
	return ret;
}

static void si3226x_proslic_shutdown(struct slic_si3226x *priv)
{
	int i;

	for (i = 0; i < SLIC_NUM_CHANNELS; i++) {
		if (priv->chan[i]) {
			Si3226x_SetLinefeedStatus(priv->chan[i], LF_OPEN);
			Si3226x_ShutdownChannel(priv->chan[i]);
			SiVoice_destroyChannel(&priv->chan[i]);
			priv->chan[i] = NULL;
		}
	}
	if (priv->proslic_dev) {
		SiVoice_destroyDevice(&priv->proslic_dev);
		priv->proslic_dev = NULL;
	}
	if (priv->ctrl_intf) {
		SiVoice_destroyControlInterface(&priv->ctrl_intf);
		priv->ctrl_intf = NULL;
	}
}

/* ---- chardev / ioctl interface ------------------------------------------ */

/*
 * Map minor number back to channel and device.
 * proslic0 = chip 0 channel 0, proslic1 = chip 0 channel 1.
 */
static int slic_get_channel(struct file *filp)
{
	struct miscdevice *md = filp->private_data;
	const char *name = md->name;

	/* devname is "proslicN" — channel = last digit */
	return name[strlen(name) - 1] - '0';
}

static struct slic_si3226x *slic_get_dev(struct file *filp)
{
	int ch = slic_get_channel(filp);

	if (ch < 0 || ch >= SLIC_NUM_CHANNELS)
		return NULL;
	return slic_devs[ch];
}

static int slic_cdev_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int slic_cdev_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static unsigned int slic_cdev_poll(struct file *filp, poll_table *wait)
{
	struct slic_si3226x *priv = slic_get_dev(filp);
	unsigned int mask = 0;

	if (!priv)
		return POLLERR;

	poll_wait(filp, &priv->hook_wq, wait);
	if (priv->hook_event) {
		mask |= POLLIN | POLLRDNORM;
		priv->hook_event = 0;
	}
	return mask;
}

static long slic_cdev_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	struct slic_si3226x *priv = slic_get_dev(filp);
	int ch = slic_get_channel(filp);
	int val, ret = 0;
	uInt8 regval;

	if (!priv || ch < 0 || ch >= SLIC_NUM_CHANNELS)
		return -ENODEV;
	if (!priv->chan[ch])
		return -ENODEV;

	switch (cmd) {
	case PROSLIC_RING_ON:
		ret = Si3226x_SetLinefeedStatus(priv->chan[ch], LF_RINGING);
		break;

	case PROSLIC_RING_OFF:
		ret = Si3226x_SetLinefeedStatus(priv->chan[ch], LF_FWD_ACTIVE);
		break;

	case PROSLIC_GET_HOOK_STATUS:
		regval = slic_hal_read_reg(&priv->spi_ctx, (uInt8)ch,
					   SI3226X_REG_LOOP_STAT);
		val = (regval & SI3226X_HOOK_OFF) ? 1 : 0;  /* 1=offhook */
		if (copy_to_user((int __user *)arg, &val, sizeof(val)))
			ret = -EFAULT;
		break;

	case PROSLIC_GET_DTMF:
		regval = slic_hal_read_reg(&priv->spi_ctx, (uInt8)ch,
					   SI3226X_REG_TONDTMF);
		if (regval & SI3226X_DTMF_VALID)
			val = regval & SI3226X_DTMF_DIGIT_MASK;
		else
			val = -1;
		if (copy_to_user((int __user *)arg, &val, sizeof(val)))
			ret = -EFAULT;
		break;

	case PROSLIC_PLAY_TONE:
		if (copy_from_user(&val, (int __user *)arg, sizeof(val)))
			return -EFAULT;
		switch (val) {
		case TONE_NONE:
			slic_hal_write_reg(&priv->spi_ctx, (uInt8)ch,
					   SI3226X_REG_OCON, 0x00);
			break;
		case TONE_DIAL:
			/* 350+440 Hz continuous — NA dial tone */
			slic_hal_write_reg(&priv->spi_ctx, (uInt8)ch,
					   SI3226X_REG_OMODE, 0x00);
			slic_hal_write_reg(&priv->spi_ctx, (uInt8)ch,
					   SI3226X_REG_OCON, 0x11);
			break;
		case TONE_RINGBACK:
			/* Oscillator cadenced (2s on / 4s off) */
			slic_hal_write_reg(&priv->spi_ctx, (uInt8)ch,
					   SI3226X_REG_OMODE, 0x02);
			slic_hal_write_reg(&priv->spi_ctx, (uInt8)ch,
					   SI3226X_REG_OCON, 0x11);
			break;
		case TONE_BUSY:
			/* 480+620 Hz cadenced (0.5s on / 0.5s off) */
			slic_hal_write_reg(&priv->spi_ctx, (uInt8)ch,
					   SI3226X_REG_OMODE, 0x01);
			slic_hal_write_reg(&priv->spi_ctx, (uInt8)ch,
					   SI3226X_REG_OCON, 0x11);
			break;
		case TONE_CONGESTION:
			/* Fast busy */
			slic_hal_write_reg(&priv->spi_ctx, (uInt8)ch,
					   SI3226X_REG_OMODE, 0x03);
			slic_hal_write_reg(&priv->spi_ctx, (uInt8)ch,
					   SI3226X_REG_OCON, 0x11);
			break;
		default:
			ret = -EINVAL;
		}
		break;

	case PROSLIC_SET_LINEFEED:
		if (copy_from_user(&val, (int __user *)arg, sizeof(val)))
			return -EFAULT;
		if (val < LF_OPEN || val > LF_RING_OPEN)
			return -EINVAL;
		ret = Si3226x_SetLinefeedStatus(priv->chan[ch], (uInt8)val);
		break;

	default:
		ret = -ENOTTY;
	}
	return ret;
}

static const struct file_operations slic_cdev_fops = {
	.owner		= THIS_MODULE,
	.open		= slic_cdev_open,
	.release	= slic_cdev_release,
	.poll		= slic_cdev_poll,
	.unlocked_ioctl	= slic_cdev_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static int slic_register_chardevs(struct slic_si3226x *priv)
{
	int i, ret;

	init_waitqueue_head(&priv->hook_wq);
	priv->hook_event = 0;

	for (i = 0; i < SLIC_NUM_CHANNELS; i++) {
		snprintf(priv->devname[i], sizeof(priv->devname[i]),
			 "proslic%d", i);
		priv->miscdev[i].minor = MISC_DYNAMIC_MINOR;
		priv->miscdev[i].name  = priv->devname[i];
		priv->miscdev[i].fops  = &slic_cdev_fops;
		priv->miscdev[i].mode  = 0660;

		ret = misc_register(&priv->miscdev[i]);
		if (ret) {
			dev_err(&priv->spi->dev,
				"misc_register(%s) failed: %d\n",
				priv->devname[i], ret);
			while (--i >= 0)
				misc_deregister(&priv->miscdev[i]);
			return ret;
		}

		mutex_lock(&slic_devs_lock);
		slic_devs[i] = priv;
		mutex_unlock(&slic_devs_lock);

		dev_info(&priv->spi->dev, "registered /dev/%s\n",
			 priv->devname[i]);
	}
	return 0;
}

static void slic_unregister_chardevs(struct slic_si3226x *priv)
{
	int i;

	for (i = 0; i < SLIC_NUM_CHANNELS; i++) {
		mutex_lock(&slic_devs_lock);
		slic_devs[i] = NULL;
		mutex_unlock(&slic_devs_lock);
		misc_deregister(&priv->miscdev[i]);
	}
}

/* ---- IRQ handler --------------------------------------------------------- */

static irqreturn_t slic_irq_handler(int irq, void *dev_id)
{
	struct slic_si3226x *priv = dev_id;

	/* Signal hook event to userspace poll() waiters */
	priv->hook_event = 1;
	wake_up_interruptible(&priv->hook_wq);
	return IRQ_HANDLED;
}

/* ---- SPI driver probe / remove ------------------------------------------ */

static int slic_si3226x_probe(struct spi_device *spi)
{
	struct slic_si3226x *priv;
	int ret;

	priv = devm_kzalloc(&spi->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->spi = spi;
	spi_set_drvdata(spi, priv);

	/* Enforce confirmed SPI clock limit */
	if (spi->max_speed_hz > SLIC_SPI_MAX_FREQ_HZ) {
		dev_warn(&spi->dev,
			 "DT spi-max-frequency %u Hz > driver max %u Hz; clamping\n",
			 spi->max_speed_hz, SLIC_SPI_MAX_FREQ_HZ);
		spi->max_speed_hz = SLIC_SPI_MAX_FREQ_HZ;
	}

	/* GPIO46 — active-high SLIC reset (confirmed from hk09 DTB) */
	priv->reset_gpio = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(priv->reset_gpio)) {
		dev_err(&spi->dev, "failed to get reset GPIO: %ld\n",
			PTR_ERR(priv->reset_gpio));
		return PTR_ERR(priv->reset_gpio);
	}

	/* GPIO47 — IRQ_TYPE_EDGE_FALLING (confirmed from hk09 DTB) */
	priv->irq = spi->irq;
	if (priv->irq <= 0) {
		dev_err(&spi->dev, "no IRQ specified in device tree\n");
		return -EINVAL;
	}

	/* Use 0 flags — DTS interrupts property encodes IRQ_TYPE_EDGE_FALLING.
	 * Letting the DT trigger type take effect is the correct DT-driver pattern.
	 * Specifying IRQF_TRIGGER_FALLING here would duplicate it; 0 is idiomatic. */
	ret = devm_request_irq(&spi->dev, priv->irq, slic_irq_handler,
			       0, DRIVER_NAME, priv);
	if (ret) {
		dev_err(&spi->dev, "failed to request IRQ %d: %d\n",
			priv->irq, ret);
		return ret;
	}

	/* Full ProSLIC API initialisation */
	ret = si3226x_proslic_init(priv);
	if (ret) {
		dev_err(&spi->dev, "ProSLIC init failed: %d\n", ret);
		/* Hold SLIC in reset on failure for safe hardware state */
		gpiod_set_value_cansleep(priv->reset_gpio, 1);
		return ret;
	}

	/* Register /dev/proslic0 and /dev/proslic1 misc char devices */
	ret = slic_register_chardevs(priv);
	if (ret) {
		dev_err(&spi->dev, "chardev registration failed: %d\n", ret);
		si3226x_proslic_shutdown(priv);
		gpiod_set_value_cansleep(priv->reset_gpio, 1);
		return ret;
	}

	dev_info(&spi->dev,
		 "Si3226x SLIC probed on %s CS%d @ %u Hz, IRQ %d\n",
		 dev_name(&spi->controller->dev),
		 spi_get_chipselect(spi, 0),
		 spi->max_speed_hz,
		 priv->irq);

	return 0;
}

static void slic_si3226x_remove(struct spi_device *spi)
{
	struct slic_si3226x *priv = spi_get_drvdata(spi);

	slic_unregister_chardevs(priv);
	si3226x_proslic_shutdown(priv);

	/* Assert reset to put SLIC in safe state */
	gpiod_set_value_cansleep(priv->reset_gpio, 1);

	dev_info(&spi->dev, "Si3226x SLIC removed\n");
}

/* ---- Device tree and SPI ID tables --------------------------------------- */

/*
 * Device tree compatible table.
 *
 * Primary: "siliconlab,si32xxx"  — CONFIRMED from AW1000 hk09 DTB.
 *          This is the string the vendor slic.ko binds to.
 * Generic: "silabs,si3226x"      — fallback / upstream candidate string.
 */
static const struct of_device_id slic_si3226x_of_match[] = {
	{ .compatible = "siliconlab,si32xxx" },	/* CONFIRMED — AW1000 stock DTB */
	{ .compatible = "silabs,si3226x" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, slic_si3226x_of_match);

static const struct spi_device_id slic_si3226x_spi_ids[] = {
	{ "si3226x", 0 },
	{ "si3228x", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(spi, slic_si3226x_spi_ids);

static struct spi_driver slic_si3226x_driver = {
	.driver = {
		.name		= DRIVER_NAME,
		.of_match_table	= slic_si3226x_of_match,
	},
	.id_table	= slic_si3226x_spi_ids,
	.probe		= slic_si3226x_probe,
	.remove		= slic_si3226x_remove,
};

module_spi_driver(slic_si3226x_driver);

MODULE_AUTHOR("ZyntaWRT Project");
MODULE_DESCRIPTION("Silicon Labs Si3226x/Si3228x SLIC driver for AW1000 (IPQ807x)");
MODULE_LICENSE("GPL");

/*
 * Hardware summary (AW1000 / AP-HK09, machid 0x8010008):
 *
 *   SPI controller : spi@78b8000 (BLSP QUP SPI3)
 *   Chip select    : CS0 (reg = <0>)
 *   SPI max freq   : 960000 Hz  (960 kHz) — CONFIRMED, NOT 8 MHz
 *   SPI MOSI       : GPIO50 (blsp3_spi_mosi)
 *   SPI MISO       : GPIO51 (blsp3_spi_miso)
 *   SPI CLK        : GPIO52 (blsp3_spi_clk)
 *   SPI CS0        : GPIO53 (blsp3_spi_cs0)
 *   SLIC RESET     : GPIO46, active-high
 *   SLIC IRQ       : GPIO47, IRQ_TYPE_EDGE_FALLING
 *   PCM controller : qca,ipq8074-pcm at 0x7704000
 *   PCM DRX        : GPIO33
 *   PCM DTX        : GPIO34
 *   PCM FSYNC      : GPIO35
 *   PCM PCLK       : GPIO36
 *   PCM DMA TX ch  : 6 (stereo port 3) → Si3226x RX
 *   PCM DMA RX ch  : 1 (stereo port 0) → Si3226x TX
 */
