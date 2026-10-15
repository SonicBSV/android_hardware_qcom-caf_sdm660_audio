/*
 * Copyright (c) 2013 - 2016, The Linux Foundation. All rights reserved.
 * Optimized for ASUS X00TD/X01BD (NXP TFA8974 + Kernel 4.19 + Android 13/14)
 */

#define LOG_TAG "audio_hw_tfa98xx_feedback"
#define LOG_NDDEBUG 0

#include <errno.h>
#include <math.h>
#include <cutils/log.h>
#include <fcntl.h>
#include <dirent.h>
#include "audio_hw.h"
#include "platform.h"
#include "platform_api.h"
#include <sys/stat.h>
#include <stdlib.h>
#include <cutils/properties.h>
#include "audio_extn.h"

#ifdef TFA98XX_ENABLED

/* 
 * FIXED BUFFER TIMINGS FOR KERNEL 4.19:
 * 512 period size prevents XRUN/Underrun while keeping DSP latency minimal.
 */
static struct pcm_config pcm_config_tfa98xx_fb = {
    .channels = 2,
    .rate = 48000,
    .period_size = 512,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = 0,
    .stop_threshold = INT_MAX,
    .avail_min = 0,
};

static struct pcm *pcm_fb = NULL;

static bool can_enable_feedback_on_device(snd_device_t snd_device)
{
    bool ret = false;

    /* 
     * КРИТИЧЕСКИЙ ФИКС: Включены ВСЕ аппаратные и защищенные режимы динамиков
     * ASUS X00TD/X01BD (включая громкую связь и звонки с контролем батареи VBAT).
     */
    switch (snd_device) {
    case SND_DEVICE_OUT_SPEAKER:
    case SND_DEVICE_OUT_SPEAKER_REVERSE:
    case SND_DEVICE_OUT_SPEAKER_PROTECTED:
    case SND_DEVICE_OUT_SPEAKER_PROTECTED_VBAT:
    case SND_DEVICE_OUT_SPEAKER_VBAT:
    case SND_DEVICE_OUT_SPEAKER_EXTERNAL_1:
    case SND_DEVICE_OUT_SPEAKER_EXTERNAL_2:
    case SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES_EXTERNAL_1:
    case SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES_EXTERNAL_2:
    case SND_DEVICE_OUT_SPEAKER_AND_ANC_HEADSET:
    case SND_DEVICE_OUT_SPEAKER_AND_HDMI:
    case SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES:
    case SND_DEVICE_OUT_SPEAKER_AND_LINE:
    case SND_DEVICE_OUT_SPEAKER_AND_USB_HEADSET:
    case SND_DEVICE_OUT_VOICE_SPEAKER:
    case SND_DEVICE_OUT_VOICE_SPEAKER_VBAT:
    case SND_DEVICE_OUT_VOICE_SPEAKER_PROTECTED:
    case SND_DEVICE_OUT_VOICE_SPEAKER_PROTECTED_VBAT:
    case SND_DEVICE_OUT_VOICE_SPEAKER_2:
    case SND_DEVICE_OUT_VOICE_SPEAKER_2_VBAT:
    case SND_DEVICE_OUT_VOICE_SPEAKER_2_PROTECTED:
    case SND_DEVICE_OUT_VOICE_SPEAKER_2_PROTECTED_VBAT:
        ret = true;
        break;
    default:
        ret = false;
        break;
    }

    return ret;
}

int audio_extn_tfa98xx_start_feedback(struct audio_device *adev, 
                                      snd_device_t snd_device)
{
    struct audio_usecase *uc_info_tx = NULL;
    int32_t pcm_dev_tx_id = -1, ret = 0;

    if (!can_enable_feedback_on_device(snd_device))
        return 0;

    ALOGV("%s: Entry for snd_device: %d", __func__, snd_device);

    if (!adev) {
        ALOGE("%s: Invalid audio device handle", __func__);
        return -EINVAL;
    }

    if (!pcm_fb) {
        uc_info_tx = (struct audio_usecase *)calloc(1, sizeof(struct audio_usecase));
        if (!uc_info_tx) {
            ALOGE("%s: Failed to allocate memory for VI-Feedback usecase", __func__);
            return -ENOMEM;
        }

        uc_info_tx->id = USECASE_AUDIO_SPKR_CALIB_TX;
        uc_info_tx->type = PCM_CAPTURE;
        uc_info_tx->in_snd_device = SND_DEVICE_IN_CAPTURE_VI_FEEDBACK;
        uc_info_tx->out_snd_device = SND_DEVICE_NONE;
        list_init(&uc_info_tx->device_list);

        list_add_tail(&adev->usecase_list, &uc_info_tx->list);

        enable_snd_device(adev, SND_DEVICE_IN_CAPTURE_VI_FEEDBACK);
        enable_audio_route(adev, uc_info_tx);

        pcm_dev_tx_id = platform_get_pcm_device_id(uc_info_tx->id, PCM_CAPTURE);
        if (pcm_dev_tx_id < 0) {
            ALOGE("%s: Invalid pcm device for usecase (%d)",
                  __func__, uc_info_tx->id);
            ret = -ENODEV;
            goto exit;
        }

        pcm_fb = pcm_open(adev->snd_card,
                          pcm_dev_tx_id,
                          PCM_IN, &pcm_config_tfa98xx_fb);
        if (pcm_fb && !pcm_is_ready(pcm_fb)) {
            ALOGE("%s: pcm_open error: %s", __func__, pcm_get_error(pcm_fb));
            ret = -EIO;
            goto exit;
        }

        if (pcm_start(pcm_fb) < 0) {
            ALOGE("%s: pcm_start for VI feedback failed: %s", __func__, pcm_get_error(pcm_fb));
            ret = -EINVAL;
            goto exit;
        }

        ALOGI("%s: TFA8974 VI-Feedback successfully started on PCM device %d", 
              __func__, pcm_dev_tx_id);
    }

    return 0;

exit:
    if (ret) {
        if (pcm_fb) {
            pcm_close(pcm_fb);
            pcm_fb = NULL;
        }

        disable_snd_device(adev, SND_DEVICE_IN_CAPTURE_VI_FEEDBACK);
        if (uc_info_tx) {
            list_remove(&uc_info_tx->list);
            disable_audio_route(adev, uc_info_tx);
            free(uc_info_tx);
        }
    } 

    ALOGV("%s: Exit with ret: %d", __func__, ret);
    return ret;
}

void audio_extn_tfa98xx_stop_feedback(struct audio_device *adev, 
                                      snd_device_t snd_device)
{
    struct audio_usecase *uc_info_tx;

    if (!can_enable_feedback_on_device(snd_device))
        return;

    ALOGV("%s: Entry for snd_device: %d", __func__, snd_device);

    uc_info_tx = get_usecase_from_list(adev, USECASE_AUDIO_SPKR_CALIB_TX);

    if (pcm_fb) {
        /* Явный останов перед закрытием для предотвращения сбоя кольцевого буфера */
        pcm_stop(pcm_fb);
        pcm_close(pcm_fb);
        pcm_fb = NULL;
    }

    disable_snd_device(adev, SND_DEVICE_IN_CAPTURE_VI_FEEDBACK);
    if (uc_info_tx) {
        list_remove(&uc_info_tx->list);
        disable_audio_route(adev, uc_info_tx);
        free(uc_info_tx);
    }

    ALOGI("%s: TFA8974 VI-Feedback cleanly stopped", __func__);
    ALOGV("%s: Exit", __func__);
}

#endif /* TFA98XX_ENABLED */
