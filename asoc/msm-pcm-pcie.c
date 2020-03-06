/* Copyright (c) 2020, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */


#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/mhi.h>

#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/control.h>
#include <asm/dma.h>
#include <dsp/msm_audio_ion.h>
#include <dsp/q6adm-v2.h>

/* hw_params declarations*/
#define MIN_PLAYBACK_PERIOD_SIZE (1920)
#define MAX_PLAYBACK_PERIOD_SIZE (1920 * 2)
#define MIN_PLAYBACK_NUM_PERIODS (2)
#define MAX_PLAYBACK_NUM_PERIODS (32)

#define PERIOD_SIZE prtd->period_size

#define DRV_NAME "msm-pcm-pcie"

/* global struct definations */
struct pcm_pcie_afe_info {
	unsigned long dma_addr;
	void *vaddr;
	struct snd_pcm_substream *substream;
	uint8_t start;
	struct mutex lock;
	int prepared;
	struct hrtimer hrt;
	struct afe_audio_client *audio_client;
	bool reset_event;
	uint8_t buf_cnt;
	uint8_t period_size;
};


struct mem_info_msg {
	uint64_t addr;
	uint64_t size;
} __packed;

/* Initialization */
typedef void (*cb_fn)(uint64_t seq_num, uint64_t host_time, uint64_t device_time);

static int session_count;
static struct delayed_work poll_index_work;
static struct delayed_work sync_operation;
static struct hrtimer *playback_hrt;
static struct hrtimer *capture_hrt;
struct mhi_controller *mhi_cntrl = NULL;
struct mhi_device *mhidev = NULL;
struct mem_info_msg *pcie_mem_size;
struct snd_dma_buffer *voice_pcie_data_buf = NULL;

/* Dummy functions definations */
static int get_header_size(void) { return 40; }
static void init_header(void *mem) { }
static void time_sync_operation(struct mhi_device *dev) { }
static enum hrtimer_restart afe_hrtimer_callback(struct hrtimer *hrt){return 0;}
static enum hrtimer_restart afe_hrtimer_rec_callback(struct hrtimer *hrt){return 0;}
static void poll_dl_index(struct work_struct *work){}
static void time_sync_op(struct work_struct *work){}

/* Dummy definations */
#define NUM_UL_SLOTS 1
#define NUM_DL_SLOTS 1
#define UL_SLOT_SIZE (128 * 2)
#define DL_SLOT_SIZE (128 * 2)

static void pcm_afe_process_tx_pkt(uint32_t opcode,
		uint32_t token, uint32_t *payload,
		 void *priv)
{
	struct pcm_pcie_afe_info *prtd = priv;
	uint16_t event;

	if (prtd == NULL)
		return;

	switch (opcode) {
	case AFE_EVENT_RT_PROXY_PORT_STATUS: {
		event = (uint16_t)((0xFFFF0000 & payload[0]) >> 0x10);
			switch (event) {
			case AFE_EVENT_RTPORT_START: {
				pr_debug("%s: AFE_EVENT_RTPORT_START\n", __func__);
				prtd->start = 1;
				prtd->buf_cnt = 0;
				break;
			}
			case AFE_EVENT_RTPORT_STOP:
				pr_debug("%s: AFE_EVENT_RTPORT_STOP\n", __func__);
				prtd->start = 0;
				break;
			case AFE_EVENT_RTPORT_LOW_WM:
				pr_debug("%s: Underrun\n", __func__);
				break;
			case AFE_EVENT_RTPORT_HI_WM:
				pr_debug("%s: Overrun\n", __func__);
				break;
			default:
				break;
			}
			break;
	}
	case APR_BASIC_RSP_RESULT: {
		switch (payload[0]) {
		case AFE_PORT_DATA_CMD_RT_PROXY_PORT_WRITE_V2:
			pr_debug("%s write done\n", __func__);
			break;
		default:
			break;
		}
		break;
	}
	case RESET_EVENTS:
		prtd->reset_event = true;
		break;
	default:
		break;
	}
}

static void pcm_afe_process_rx_pkt(uint32_t opcode,
		uint32_t token, uint32_t *payload,
		 void *priv)
{
	struct pcm_pcie_afe_info *prtd = priv;
	uint16_t event;

	if (prtd == NULL)
		return;

	switch (opcode) {
	case AFE_EVENT_RT_PROXY_PORT_STATUS: {
		event = (uint16_t)((0xFFFF0000 & payload[0]) >> 0x10);
		switch (event) {
		case AFE_EVENT_RTPORT_START: {
			pr_debug("%s: AFE_EVENT_RTPORT_START \n", __func__);
			prtd->start = 1;
			prtd->buf_cnt = 0;
			break;
		}
		case AFE_EVENT_RTPORT_STOP:
			pr_debug("%s: AFE_EVENT_RTPORT_STOP \n", __func__);
			prtd->start = 0;
			break;
		case AFE_EVENT_RTPORT_LOW_WM:
			pr_debug("%s: Underrun\n", __func__);
			break;
		case AFE_EVENT_RTPORT_HI_WM:
			pr_debug("%s: Overrun\n", __func__);
			break;
		default:
			pr_err("%s: event %d\n", __func__, opcode);
			break;
		}
		break;
	}
	case APR_BASIC_RSP_RESULT: {
		switch (payload[0]) {
		case AFE_PORT_DATA_CMD_RT_PROXY_PORT_READ_V2:
			pr_debug("%s :Read done\n", __func__);
			break;
		default:
			break;
		}
		break;
	}
	case RESET_EVENTS:
		prtd->reset_event = true;
		break;
	default:
		break;
	}
}

static int msm_afe_playback_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct pcm_pcie_afe_info *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *dai = rtd->cpu_dai;
	struct pcm_pcie_afe_info *prtd_tx = container_of(playback_hrt, struct pcm_pcie_afe_info, hrt);
	int ret = 0;

	pr_debug("%s\n", __func__);
	ret = afe_register_get_events(dai->id,
				      pcm_afe_process_tx_pkt, prtd_tx);
	if (ret < 0) {
		pr_err("afe-pcm:register for events failed\n");
		return ret;
	}
	prtd->prepared++;
	return ret;
}

static int msm_afe_capture_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct pcm_pcie_afe_info *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *dai = rtd->cpu_dai;
	struct pcm_pcie_afe_info *prtd_rx = container_of(capture_hrt, struct pcm_pcie_afe_info, hrt);
	int ret = 0;

	pr_debug("%s\n", __func__);
	ret = afe_register_get_events(dai->id,
				      pcm_afe_process_rx_pkt, prtd_rx);
	if (ret < 0) {
		pr_err("afe-pcm:register for events failed\n");
		return ret;
	}
	prtd->prepared++;
	return 0;
}

static struct snd_pcm_hardware msm_pcm_pcie_hw_params = {
	.info =                 ( SNDRV_PCM_INFO_INTERLEAVED),
	.formats =              SNDRV_PCM_FMTBIT_S16_LE,
	.rates =                (SNDRV_PCM_RATE_8000 |
				 SNDRV_PCM_RATE_16000 |
				 SNDRV_PCM_RATE_48000),
	.rate_min =             8000,
	.rate_max =             48000,
	.channels_min =         1,
	.channels_max =         2,
	.buffer_bytes_max =     MAX_PLAYBACK_PERIOD_SIZE * MAX_PLAYBACK_NUM_PERIODS,
	.period_bytes_min =     MIN_PLAYBACK_PERIOD_SIZE,
	.period_bytes_max =     MAX_PLAYBACK_PERIOD_SIZE,
	.periods_min =          MIN_PLAYBACK_NUM_PERIODS,
	.periods_max =          MAX_PLAYBACK_NUM_PERIODS,
	.fifo_size =            0,
};

static int mhi_transfer(void)
{
	int ret = 0;
	int dma_buf_size = get_header_size() + (NUM_UL_SLOTS * UL_SLOT_SIZE) + (NUM_DL_SLOTS * DL_SLOT_SIZE);

	pr_debug("%s: Enter \n", __func__);

	pcie_mem_size = kzalloc(sizeof(*pcie_mem_size), GFP_KERNEL);
	if (!pcie_mem_size) {
		ret = -ENOMEM;
		return ret;
	}
	pcie_mem_size->addr = voice_pcie_data_buf->addr;
	pcie_mem_size->size = dma_buf_size;
	pr_debug("voice_pcie_data_buf-area = %pK, size=%x\n", voice_pcie_data_buf->area, dma_buf_size);
	pr_debug("voice_pcie_data_buf-addr = %pK, size=%x\n", voice_pcie_data_buf->addr, dma_buf_size);
	ret = mhi_queue_transfer(mhidev, DMA_TO_DEVICE, pcie_mem_size,
				 sizeof(struct mem_info_msg), MHI_EOT);
	if (ret) {
		pr_err("Failed to transfer mem and size to MHI chan, ret %d\n", ret);
		return ret;
	}
	pr_debug("%s: Exit \n", __func__);
	return ret;
}

static int mhi_setup(void)
{
	int ret = 0;

	pr_debug("%s: Enter \n", __func__);
	time_sync_operation(mhidev);

	ret = mhi_prepare_for_transfer(mhidev);
	if (ret) {
		pr_err("Failed to start MHI chan, ret %d\n", ret);
		return ret;
	}
	pr_debug("%s: Exit \n", __func__);

	return ret;
}

static int msm_afe_pcie_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct pcm_pcie_afe_info *prtd = NULL;
	int dma_buf_size = get_header_size() + (NUM_UL_SLOTS * UL_SLOT_SIZE) + (NUM_DL_SLOTS * DL_SLOT_SIZE);

	pr_debug("%s\n", __func__);
	prtd = kzalloc(sizeof(struct pcm_pcie_afe_info), GFP_KERNEL);
	if (prtd == NULL)
		return -ENOMEM;
	pr_debug("prtd %pK\n", prtd);

	mutex_init(&prtd->lock);

	mutex_lock(&prtd->lock);

	prtd->substream = substream;
	runtime->private_data = prtd;
	prtd->audio_client = q6afe_audio_client_alloc(prtd);
	if (!prtd->audio_client) {
		pr_err("%s: Could not allocate memory\n", __func__);
		mutex_unlock(&prtd->lock);
		kfree(prtd);
		return -ENOMEM;
	}

	memset(voice_pcie_data_buf->area, 0, dma_buf_size);
	init_header(voice_pcie_data_buf->area);
	runtime->hw = msm_pcm_pcie_hw_params;

	/*init timer*/
	hrtimer_init(&prtd->hrt, CLOCK_MONOTONIC, HRTIMER_MODE_REL);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		pr_debug("prtd rx %pK\n", prtd);
		prtd->hrt.function = afe_hrtimer_callback;
		playback_hrt = &(prtd->hrt);
		if (mhidev != NULL)
			mhi_transfer();
	} else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		pr_debug("prtd tx %pK\n", prtd);
		prtd->hrt.function = afe_hrtimer_rec_callback;
		capture_hrt = &(prtd->hrt);
	}

	mutex_unlock(&prtd->lock);
	prtd->reset_event = false;
	pr_debug("%s: Exit return 0\n", __func__);
	return 0;
}

static int msm_afe_pcie_close(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime;
	struct snd_dma_buffer *dma_buf;
	struct pcm_pcie_afe_info *prtd;
	struct snd_soc_pcm_runtime *rtd = NULL;
	struct snd_soc_dai *dai = NULL;
	int dir = IN;
	int ret = 0;

	pr_debug("%s\n", __func__);
	if (substream == NULL) {
		pr_err("substream is NULL\n");
		return -EINVAL;
	}
	rtd = substream->private_data;
	dai = rtd->cpu_dai;
	runtime = substream->runtime;
	prtd = runtime->private_data;

	mutex_lock(&prtd->lock);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		dir = IN;
		cancel_delayed_work(&sync_operation);
		cancel_delayed_work(&poll_index_work);
		session_count--;
		ret =  afe_unregister_get_events(dai->id);
		if (ret < 0)
			pr_err("AFE unregister for events failed\n");
	} else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		dir = OUT;
		session_count--;
		ret =  afe_unregister_get_events(dai->id);
		if (ret < 0)
			pr_err("AFE unregister for events failed\n");
	}

	hrtimer_cancel(&prtd->hrt);

	ret = afe_cmd_memory_unmap(afe_req_mmap_handle(prtd->audio_client));
	if (ret < 0)
		pr_err("AFE memory unmap failed\n");

	pr_debug("release all buffer\n");
	dma_buf = &substream->dma_buffer;
	if (dma_buf == NULL) {
		pr_err("dma_buf is NULL\n");
		goto done;
	}

	if ((!session_count) && (mhidev != NULL)) {
		mhi_device_put(mhidev, MHI_VOTE_DEVICE); 
	}
	if (dma_buf->area)
		dma_buf->area = NULL;
	q6afe_audio_client_buf_free_contiguous(dir, prtd->audio_client);
done:
	pr_debug("%s: dai->id =%x\n", __func__, dai->id);
	q6afe_audio_client_free(prtd->audio_client);
	mutex_unlock(&prtd->lock);
	prtd->prepared--;
	kfree(prtd);
	runtime->private_data = NULL;
	return 0;
}

static int msm_afe_pcie_prepare(struct snd_pcm_substream *substream)
{
	int ret = 0;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct pcm_pcie_afe_info *prtd = runtime->private_data;

	pr_debug("%s\n", __func__);
	if (prtd->prepared)
		return 0;

	mutex_lock(&prtd->lock);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
                session_count++;
		ret = msm_afe_playback_prepare(substream);
	} else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		session_count++;
		ret = msm_afe_capture_prepare(substream);
	}
	mutex_unlock(&prtd->lock);

	if((session_count == 2) && (mhidev != NULL)) {
		mhi_device_get_sync(mhidev, MHI_VOTE_DEVICE);
	}
	return ret;
}

static int msm_afe_pcie_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_dma_buffer *dma_buf = &substream->dma_buffer;
	struct pcm_pcie_afe_info *prtd = runtime->private_data;
	struct afe_audio_buffer *buf;
	int dir, rc;

	pr_debug("%s\n", __func__);
	mutex_lock(&prtd->lock);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		dir = IN;
	else
		dir = OUT;

	rc = q6afe_audio_client_buf_alloc_contiguous(dir,
		     prtd->audio_client,
		     params_buffer_bytes(params) / params_periods(params),
		     params_periods(params));

	pr_debug("params_buffer_bytes(params) = %d\n",
		 (params_buffer_bytes(params)));
	pr_debug("params_periods(params) = %d\n",
		 (params_periods(params)));
	pr_debug("params_periodsize(params) = %d\n",
		 (params_buffer_bytes(params) / params_periods(params)));

	prtd->buf_cnt = 0;
	prtd->period_size = params_periods(params);
	if (rc < 0) {
		pr_err("Audio Start: Buffer Allocation failed rc = %d\n", rc);
		mutex_unlock(&prtd->lock);
		return -ENOMEM;
	}
	buf = prtd->audio_client->port[dir].buf;

	if (buf == NULL || buf[0].data == NULL) {
		mutex_unlock(&prtd->lock);
		return -ENOMEM;
	}

	pr_debug("%s:buf = %pK\n", __func__, buf);
	dma_buf->dev.type = SNDRV_DMA_TYPE_DEV;
	dma_buf->dev.dev = substream->pcm->card->dev;
	dma_buf->private_data = NULL;
	dma_buf->area = buf[0].data;
	dma_buf->addr = buf[0].phys;

	dma_buf->bytes = params_buffer_bytes(params);

	if (!dma_buf->area) {
		pr_err("%s:MSM AFE physical memory allocation failed\n",
		       __func__);
		mutex_unlock(&prtd->lock);
		return -ENOMEM;
	}

	memset(dma_buf->area, 0, params_buffer_bytes(params));

	prtd->dma_addr = (phys_addr_t) dma_buf->addr;
	prtd->vaddr =(void *)dma_buf->area;
	mutex_unlock(&prtd->lock);
	snd_pcm_set_runtime_buffer(substream, &substream->dma_buffer);

	rc = afe_memory_map(dma_buf->addr, dma_buf->bytes, prtd->audio_client);
	pr_debug("%s mem map handle is 0x%x\n", __func__, afe_req_mmap_handle(prtd->audio_client));
	if (rc < 0)
		pr_err("fail to map memory to DSP\n");
	return rc;
}

static int msm_afe_pcie_trigger(struct snd_pcm_substream *substream, int cmd)
{
	int ret = 0;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct pcm_pcie_afe_info *prtd = runtime->private_data;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		prtd->start = 1;
		pr_debug("%s: SNDRV_PCM_TRIGGER_START\n", __func__);
		schedule_delayed_work(&poll_index_work, msecs_to_jiffies(1));
		schedule_delayed_work(&sync_operation, msecs_to_jiffies(100));
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		pr_debug("%s: SNDRV_PCM_TRIGGER_STOP\n", __func__);
		prtd->start = 0;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static const struct snd_pcm_ops msm_pcie_ops = {
	.open           = msm_afe_pcie_open,
	.close          = msm_afe_pcie_close,
	.hw_params	= msm_afe_pcie_hw_params,
	.prepare        = msm_afe_pcie_prepare,
	.trigger	= msm_afe_pcie_trigger,
};

static int mhi_audio_probe(struct mhi_device *mhi_dev, const struct mhi_device_id *mhi_id)
{
	int ret = 0;
	int dma_buf_size = get_header_size() + (NUM_UL_SLOTS * UL_SLOT_SIZE) + (NUM_DL_SLOTS * DL_SLOT_SIZE);

	pr_debug("%s Enter\n", __func__);
	voice_pcie_data_buf = devm_kzalloc(&mhi_dev->dev, sizeof(struct snd_dma_buffer), GFP_KERNEL);
	if (!voice_pcie_data_buf)
		return -ENOMEM;

	voice_pcie_data_buf->dev.type = SNDRV_DMA_TYPE_DEV;
	voice_pcie_data_buf->dev.dev = &mhi_dev->dev;
	voice_pcie_data_buf->private_data = NULL;
	voice_pcie_data_buf->area = dma_alloc_attrs(mhi_dev->dev.parent, dma_buf_size, &voice_pcie_data_buf->addr, GFP_KERNEL, DMA_ATTR_FORCE_CONTIGUOUS);
	if (!voice_pcie_data_buf->area) {
		pr_err("%s: DMA buffer allocation failed\n", __func__);
		return -ENOMEM;
	}
	pr_debug("%s: 1 dma_alloc_attrs success PA :0x%lx\n",
		 __func__, (unsigned long)voice_pcie_data_buf->addr);
	pr_debug("%s: 1 dma_alloc_attrs success VA :0x%lx\n",
		 __func__, (unsigned long)voice_pcie_data_buf->area);
	INIT_DELAYED_WORK(&poll_index_work, poll_dl_index);
	INIT_DELAYED_WORK(&sync_operation, time_sync_op);

	mhidev = mhi_dev;
	mhi_cntrl = mhi_dev->mhi_cntrl;

	if (mhidev != NULL)
		mhi_setup();

	pr_debug("%s Exit\n", __func__);
	return ret;
}

static void mhi_audio_remove(struct mhi_device *mhi_dev)
{
	int dma_buf_size = get_header_size() + (NUM_UL_SLOTS * UL_SLOT_SIZE) + (NUM_DL_SLOTS * DL_SLOT_SIZE);

	pr_debug("%s\n", __func__);
	/*free dma buffers;*/
	dma_free_attrs(mhi_dev->dev.parent, dma_buf_size, voice_pcie_data_buf->area,
			voice_pcie_data_buf->addr, DMA_ATTR_FORCE_CONTIGUOUS);
	return;
}

static int msm_asoc_pcm_new(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_card *card = rtd->card->snd_card;

	if (!card->dev->coherent_dma_mask)
		card->dev->coherent_dma_mask = DMA_BIT_MASK(32);
	return 0;
}

static int msm_asoc_pcie_probe(struct snd_soc_platform *platform)
{
	pr_debug("%s\n", __func__);
	return 0;
}

static struct snd_soc_platform_driver msm_soc_platform = {
	.ops		= &msm_pcie_ops,
	.pcm_new	= msm_asoc_pcm_new,
	.probe		= msm_asoc_pcie_probe,
};

static void mhi_audio_ul_xfer_cb(struct mhi_device *mhi_dev, struct mhi_result *mhi_result)
{
	pr_debug("%s\n", __func__);
	return;
}

static void mhi_audio_dl_xfer_cb(struct mhi_device *mhi_dev, struct mhi_result *mhi_result)
{
	pr_debug("%s\n", __func__);
	return;
}

static const struct mhi_device_id mhi_audio_match_table[] = {
	{ .chan = "AUDIO_VOICE_0" },
	{}
};

static struct mhi_driver mhi_audio_driver = {
	.id_table = mhi_audio_match_table,
	.probe = mhi_audio_probe,
	.remove = mhi_audio_remove,
	.ul_xfer_cb = mhi_audio_ul_xfer_cb,
	.dl_xfer_cb = mhi_audio_dl_xfer_cb,
	.driver = {
		.name = "msm-pcm-pcie",
		.owner = THIS_MODULE,
	}
};

static int msm_pcm_pcie_probe(struct platform_device *pdev)
{
	int ret = 0;

	pr_debug("%s: dev name %s\n", __func__, dev_name(&pdev->dev));
	ret = mhi_driver_register(&mhi_audio_driver);
	if (ret) {
		pr_err("%s: mhi_driver_register failed with ret = %d",
			__func__, ret);
		return -EPROBE_DEFER;
	}
	return snd_soc_register_platform(&pdev->dev,
				   &msm_soc_platform);
}

static int msm_pcm_pcie_remove(struct platform_device *pdev)
{
	pr_debug("%s\n", __func__);
	snd_soc_unregister_platform(&pdev->dev);
	mhi_driver_unregister(&mhi_audio_driver);
	return 0;
}
static const struct of_device_id msm_pcm_pcie_dt_match[] = {
	{.compatible = "qcom,msm-pcm-pcie"},
	{}
};
MODULE_DEVICE_TABLE(of, msm_pcm_pcie_dt_match);

static struct platform_driver msm_pcm_pcie_driver = {
	.driver = {
		.name = "msm-pcm-pcie",
		.owner = THIS_MODULE,
		.of_match_table = msm_pcm_pcie_dt_match,
	},
	.probe = msm_pcm_pcie_probe,
	.remove = msm_pcm_pcie_remove,
};

int __init msm_soc_pcie_init(void)
{
	pr_debug("%s\n", __func__);
	return platform_driver_register(&msm_pcm_pcie_driver);
}

void msm_soc_pcie_exit(void)
{
	pr_debug("%s\n", __func__);
	platform_driver_unregister(&msm_pcm_pcie_driver);
}

MODULE_DESCRIPTION("PCIe PCM module platform driver");
MODULE_LICENSE("GPL v2");
