/**
 * OpenAL cross platform audio library
 * Copyright (C) 1999-2007 by authors.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 *  Boston, MA  02111-1307, USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#ifdef HAVE_DLFCN_H
#include <dlfcn.h>
#endif
#include "alMain.h"
#include "AL/al.h"
#include "AL/alc.h"

#include <alsa/asoundlib.h>


typedef struct {
    snd_pcm_t *pcmHandle;
    snd_pcm_format_t format;
    int killNow;
    ALvoid *thread;
} alsa_data;

typedef struct {
    ALCchar *name;
    int card, dev;
} DevMap;

static void *alsa_handle;
#define MAKE_FUNC(f) static typeof(f) * p##f
MAKE_FUNC(snd_strerror);
MAKE_FUNC(snd_pcm_open);
MAKE_FUNC(snd_pcm_close);
MAKE_FUNC(snd_pcm_nonblock);
MAKE_FUNC(snd_pcm_frames_to_bytes);
MAKE_FUNC(snd_pcm_hw_params_malloc);
MAKE_FUNC(snd_pcm_hw_params_free);
MAKE_FUNC(snd_pcm_hw_params_any);
MAKE_FUNC(snd_pcm_hw_params_set_access);
MAKE_FUNC(snd_pcm_hw_params_set_format);
MAKE_FUNC(snd_pcm_hw_params_set_channels);
MAKE_FUNC(snd_pcm_hw_params_set_periods_near);
MAKE_FUNC(snd_pcm_hw_params_set_rate_near);
MAKE_FUNC(snd_pcm_hw_params_set_rate);
MAKE_FUNC(snd_pcm_hw_params_set_buffer_size_near);
MAKE_FUNC(snd_pcm_hw_params_set_buffer_size_min);
MAKE_FUNC(snd_pcm_hw_params);
MAKE_FUNC(snd_pcm_prepare);
MAKE_FUNC(snd_pcm_start);
MAKE_FUNC(snd_pcm_resume);
MAKE_FUNC(snd_pcm_state);
MAKE_FUNC(snd_pcm_avail_update);
MAKE_FUNC(snd_pcm_areas_silence);
MAKE_FUNC(snd_pcm_mmap_begin);
MAKE_FUNC(snd_pcm_mmap_commit);
MAKE_FUNC(snd_pcm_drain);
MAKE_FUNC(snd_pcm_info_malloc);
MAKE_FUNC(snd_pcm_info_free);
MAKE_FUNC(snd_pcm_info_set_device);
MAKE_FUNC(snd_pcm_info_set_subdevice);
MAKE_FUNC(snd_pcm_info_set_stream);
MAKE_FUNC(snd_pcm_info_get_name);
MAKE_FUNC(snd_ctl_pcm_next_device);
MAKE_FUNC(snd_ctl_pcm_info);
MAKE_FUNC(snd_ctl_open);
MAKE_FUNC(snd_ctl_close);
MAKE_FUNC(snd_ctl_card_info_malloc);
MAKE_FUNC(snd_ctl_card_info_free);
MAKE_FUNC(snd_ctl_card_info);
MAKE_FUNC(snd_ctl_card_info_get_name);
MAKE_FUNC(snd_card_next);
#undef MAKE_FUNC

#define MAX_DEVICES 16
#define MAX_ALL_DEVICES 32

static DevMap allDevNameMap[MAX_ALL_DEVICES];
static ALCchar *alsaDeviceList[MAX_DEVICES];
static ALCchar *alsaCaptureDeviceList[MAX_DEVICES];

static int xrun_recovery(snd_pcm_t *handle, int err)
{
    if (err == -EPIPE)
    {    /* under-run */
        err = psnd_pcm_prepare(handle);
        if(err >= 0)
            err = psnd_pcm_start(handle);
        if (err < 0)
            AL_PRINT("prepare failed: %s\n", psnd_strerror(err));
    }
    else if (err == -ESTRPIPE)
    {
        while ((err = psnd_pcm_resume(handle)) == -EAGAIN)
            usleep(1);       /* wait until the suspend flag is released */
        if (err < 0)
        {
            err = psnd_pcm_prepare(handle);
            if(err >= 0)
                err = psnd_pcm_start(handle);
            if (err < 0)
                AL_PRINT("prepare failed: %s\n", psnd_strerror(err));
        }
    }
    return err;
}


static ALuint ALSAProc(ALvoid *ptr)
{
    ALCdevice *pDevice = (ALCdevice*)ptr;
    alsa_data *data = (alsa_data*)pDevice->ExtraData;
    const snd_pcm_channel_area_t *areas = NULL;
    snd_pcm_sframes_t avail, commitres;
    snd_pcm_uframes_t offset, frames;
    char *WritePtr;
    int WriteCnt;
    int err;

    while(!data->killNow)
    {
        snd_pcm_state_t state = psnd_pcm_state(data->pcmHandle);
        if(state == SND_PCM_STATE_XRUN)
        {
            err = xrun_recovery(data->pcmHandle, -EPIPE);
            if (err < 0)
            {
                AL_PRINT("XRUN recovery failed: %s\n", psnd_strerror(err));
                break;
            }
        }
        else if (state == SND_PCM_STATE_SUSPENDED)
        {
            err = xrun_recovery(data->pcmHandle, -ESTRPIPE);
            if (err < 0)
            {
                AL_PRINT("SUSPEND recovery failed: %s\n", psnd_strerror(err));
                break;
            }
        }

        avail = psnd_pcm_avail_update(data->pcmHandle);
        if(avail < 0)
        {
            err = xrun_recovery(data->pcmHandle, avail);
            if (err < 0)
            {
                AL_PRINT("available update failed: %s\n", psnd_strerror(err));
                break;
            }
        }

        // make sure there's frames to process
        if(avail == 0)
        {
            usleep(1000);
            continue;
        }

        // it is possible that contiguous areas are smaller, thus we use a loop
        while (avail > 0)
        {
            frames = avail;

            err = psnd_pcm_mmap_begin(data->pcmHandle, &areas, &offset, &frames);
            if (err < 0)
            {
                err = xrun_recovery(data->pcmHandle, err);
                if (err < 0)
                    AL_PRINT("mmap begin error: %s\n", psnd_strerror(err));
                break;
            }

            SuspendContext(NULL);
            WritePtr = (char*)areas->addr + (offset * areas->step / 8);
            WriteCnt = psnd_pcm_frames_to_bytes(data->pcmHandle, frames);
            aluMixData(pDevice->Context, WritePtr, WriteCnt, pDevice->Format);
            ProcessContext(NULL);

            commitres = psnd_pcm_mmap_commit(data->pcmHandle, offset, frames);
            if (commitres < 0 || (commitres-frames) != 0)
            {
                AL_PRINT("mmap commit error: %s\n",
                         psnd_strerror(commitres >= 0 ? -EPIPE : commitres));
                break;
            }

            avail -= frames;
        }
    }

    return 0;
}

static void fill_silence(snd_pcm_t *pcmHandle, snd_pcm_format_t alsaFormat, int channels)
{
    const snd_pcm_channel_area_t *areas = NULL;
    snd_pcm_sframes_t avail, commitres;
    snd_pcm_uframes_t offset, frames;
    int err;

    avail = psnd_pcm_avail_update(pcmHandle);
    if(avail < 0)
    {
        err = xrun_recovery(pcmHandle, avail);
        if (err < 0)
        {
            AL_PRINT("available update failed: %s\n", psnd_strerror(err));
            return;
        }
    }

    // it is possible that contiguous areas are smaller, thus we use a loop
    while (avail > 0)
    {
        frames = avail;

        err = psnd_pcm_mmap_begin(pcmHandle, &areas, &offset, &frames);
        if (err < 0)
        {
            err = xrun_recovery(pcmHandle, err);
            if (err < 0)
            {
                AL_PRINT("mmap begin error: %s\n", psnd_strerror(err));
                break;
            }
            continue;
        }

        psnd_pcm_areas_silence(areas, offset, channels, frames, alsaFormat);

        commitres = psnd_pcm_mmap_commit(pcmHandle, offset, frames);
        if (commitres < 0 || (commitres-frames) != 0)
        {
            AL_PRINT("mmap commit error: %s\n",
                     psnd_strerror(commitres >= 0 ? -EPIPE : commitres));
            break;
        }

        avail -= frames;
    }
}

static ALCboolean alsa_open_playback(ALCdevice *device, const ALCchar *deviceName)
{
    snd_pcm_uframes_t bufferSizeInFrames;
    snd_pcm_hw_params_t *p = NULL;
    unsigned int periods;
    alsa_data *data;
    char driver[64];
    char *err;
    int i;

    strncpy(driver, GetConfigValue("alsa", "default", "default"), sizeof(driver)-1);
    driver[sizeof(driver)-1] = 0;
    if(deviceName)
    {
        size_t idx;

        for(idx = 0;idx < MAX_ALL_DEVICES;idx++)
        {
            if(allDevNameMap[idx].name &&
               strcmp(deviceName, allDevNameMap[idx].name) == 0)
            {
                if(idx > 0)
                    sprintf(driver, "hw:%d,%d", allDevNameMap[idx].card, allDevNameMap[idx].dev);
                goto open_alsa;
            }
        }
        for(idx = 0;idx < MAX_DEVICES;idx++)
        {
            if(alsaDeviceList[idx] &&
               strcmp(deviceName, alsaDeviceList[idx]) == 0)
            {
                if(idx > 0)
                    sprintf(driver, "hw:%d,0", idx-1);
                goto open_alsa;
            }
        }
        return ALC_FALSE;
    }

    if(deviceName)
        strcpy(device->szDeviceName, deviceName);
    else
        strcpy(device->szDeviceName, alsaDeviceList[0]);

open_alsa:
    data = (alsa_data*)calloc(1, sizeof(alsa_data));

    i = psnd_pcm_open(&data->pcmHandle, driver, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if(i < 0)
    {
        usleep(200000);
        i = psnd_pcm_open(&data->pcmHandle, driver, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    }
    if(i >= 0)
    {
        i = psnd_pcm_nonblock(data->pcmHandle, 0);
        if(i < 0)
            psnd_pcm_close(data->pcmHandle);
    }
    if(i < 0)
    {
        free(data);
        return ALC_FALSE;
    }

    switch(device->Format)
    {
        case AL_FORMAT_MONO8:
        case AL_FORMAT_STEREO8:
        case AL_FORMAT_QUAD8:
            data->format = SND_PCM_FORMAT_U8;
            break;
        case AL_FORMAT_MONO16:
        case AL_FORMAT_STEREO16:
        case AL_FORMAT_QUAD16:
            data->format = SND_PCM_FORMAT_S16;
            break;
        default:
            data->format = SND_PCM_FORMAT_UNKNOWN;
            AL_PRINT("Unknown format?! %x\n", device->Format);
    }

    periods = GetConfigValueInt("alsa", "periods", 4);
    if((int)periods <= 0)
        periods = 4;
    bufferSizeInFrames = device->UpdateFreq;

    psnd_pcm_hw_params_malloc(&p);
#define ok(func, str) (i=(func),((i<0)?(err=(str)),0:1))
    /* start with the largest configuration space possible */
    if(!(ok(psnd_pcm_hw_params_any(data->pcmHandle, p), "any") &&
         /* set interleaved access */
         ok(psnd_pcm_hw_params_set_access(data->pcmHandle, p, SND_PCM_ACCESS_MMAP_INTERLEAVED), "set access") &&
         /* set format (implicitly sets sample bits) */
         ok(psnd_pcm_hw_params_set_format(data->pcmHandle, p, data->format), "set format") &&
         /* set channels (implicitly sets frame bits) */
         ok(psnd_pcm_hw_params_set_channels(data->pcmHandle, p, device->Channels), "set channels") &&
         /* set periods (implicitly constrains period/buffer parameters) */
         ok(psnd_pcm_hw_params_set_periods_near(data->pcmHandle, p, &periods, NULL), "set periods near") &&
         /* set rate (implicitly constrains period/buffer parameters) */
         ok(psnd_pcm_hw_params_set_rate_near(data->pcmHandle, p, &device->Frequency, NULL), "set rate near") &&
         /* set buffer size in frame units (implicitly sets period size/bytes/time and buffer time/bytes) */
         ok(psnd_pcm_hw_params_set_buffer_size_near(data->pcmHandle, p, &bufferSizeInFrames), "set buffer size near") &&
         /* install and prepare hardware configuration */
         ok(psnd_pcm_hw_params(data->pcmHandle, p), "set params")))
    {
        AL_PRINT("%s failed: %s\n", err, psnd_strerror(i));
        psnd_pcm_hw_params_free(p);
        psnd_pcm_close(data->pcmHandle);
        free(data);
        return ALC_FALSE;
    }
#undef ok
    psnd_pcm_hw_params_free(p);

    device->MaxNoOfSources = 256;
    device->UpdateFreq = bufferSizeInFrames;

    i = psnd_pcm_prepare(data->pcmHandle);
    if(i < 0)
    {
        AL_PRINT("prepare error: %s\n", psnd_strerror(i));
        psnd_pcm_close(data->pcmHandle);
        free(data);
        return ALC_FALSE;
    }

    fill_silence(data->pcmHandle, data->format, device->Channels);
    i = psnd_pcm_start(data->pcmHandle);
    if(i < 0)
    {
        AL_PRINT("start error: %s\n", psnd_strerror(i));
        psnd_pcm_close(data->pcmHandle);
        free(data);
        return ALC_FALSE;
    }

    device->ExtraData = data;
    data->thread = StartThread(ALSAProc, device);
    if(data->thread == NULL)
    {
        psnd_pcm_close(data->pcmHandle);
        device->ExtraData = NULL;
        free(data);
        return ALC_FALSE;
    }

    return ALC_TRUE;
}

static void alsa_close_playback(ALCdevice *device)
{
    alsa_data *data = (alsa_data*)device->ExtraData;
    data->killNow = 1;
    StopThread(data->thread);
    psnd_pcm_close(data->pcmHandle);

    free(data);
    device->ExtraData = NULL;
}


static ALCboolean alsa_open_capture(ALCdevice *pDevice, const ALCchar *deviceName, ALCuint frequency, ALCenum format, ALCsizei SampleSize)
{
    snd_pcm_format_t alsaFormat;
    snd_pcm_hw_params_t *p;
    unsigned int periods = 4;
    snd_pcm_uframes_t bufferSizeInFrames = SampleSize;
    alsa_data *data;
    char driver[64];
    char *err;
    int i;

    strncpy(driver, GetConfigValue("alsa", "capture", "default"), sizeof(driver)-1);
    driver[sizeof(driver)-1] = 0;
    if(deviceName)
    {
        size_t idx;

        for(idx = 0;idx < MAX_DEVICES;idx++)
        {
            if(alsaCaptureDeviceList[idx] &&
               strcmp(deviceName, alsaCaptureDeviceList[idx]) == 0)
            {
                if(idx > 0)
                    sprintf(driver, "hw:%d,0", idx-1);
                goto open_alsa;
            }
        }
        return ALC_FALSE;
    }

    if(deviceName)
        strcpy(pDevice->szDeviceName, deviceName);
    else
        strcpy(pDevice->szDeviceName, alsaCaptureDeviceList[0]);

open_alsa:
    data = (alsa_data*)calloc(1, sizeof(alsa_data));

    i = psnd_pcm_open(&data->pcmHandle, driver, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if(i < 0)
    {
        usleep(200000);
        i = psnd_pcm_open(&data->pcmHandle, driver, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    }
    if(i >= 0)
    {
        i = psnd_pcm_nonblock(data->pcmHandle, 0);
        if(i < 0)
            psnd_pcm_close(data->pcmHandle);
    }
    if(i < 0)
    {
        free(data);
        return ALC_FALSE;
    }

    switch(aluBytesFromFormat(format))
    {
        case 1:
            alsaFormat = SND_PCM_FORMAT_U8;
            break;
        case 2:
            alsaFormat = SND_PCM_FORMAT_S16;
            break;
        default:
            alsaFormat = SND_PCM_FORMAT_UNKNOWN;
            AL_PRINT("Unknown format?! %x\n", format);
    }

    psnd_pcm_hw_params_malloc(&p);
#define ok(func, str) (i=(func),((i<0)?(err=(str)),0:1))
    /* start with the largest configuration space possible */
    if(!(ok(psnd_pcm_hw_params_any(data->pcmHandle, p), "any") &&
         /* set interleaved access */
         ok(psnd_pcm_hw_params_set_access(data->pcmHandle, p, SND_PCM_ACCESS_MMAP_INTERLEAVED), "set access") &&
         /* set format (implicitly sets sample bits) */
         ok(psnd_pcm_hw_params_set_format(data->pcmHandle, p, alsaFormat), "set format") &&
         /* set channels (implicitly sets frame bits) */
         ok(psnd_pcm_hw_params_set_channels(data->pcmHandle, p, pDevice->Channels), "set channels") &&
         /* set periods (implicitly constrains period/buffer parameters) */
         ok(psnd_pcm_hw_params_set_periods_near(data->pcmHandle, p, &periods, NULL), "set periods near") &&
         /* set rate (implicitly constrains period/buffer parameters) */
         ok(psnd_pcm_hw_params_set_rate(data->pcmHandle, p, frequency, 0), "set rate") &&
         /* set buffer size in frame units (implicitly sets period size/bytes/time and buffer time/bytes) */
         ok(psnd_pcm_hw_params_set_buffer_size_min(data->pcmHandle, p, &bufferSizeInFrames), "set buffer size min") &&
         /* install and prepare hardware configuration */
         ok(psnd_pcm_hw_params(data->pcmHandle, p), "set params")))
    {
        AL_PRINT("%s failed: %s\n", err, psnd_strerror(i));
        psnd_pcm_hw_params_free(p);
        psnd_pcm_close(data->pcmHandle);
        free(data);
        return ALC_FALSE;
    }
#undef ok
    psnd_pcm_hw_params_free(p);

    i = psnd_pcm_prepare(data->pcmHandle);
    if(i < 0)
    {
        AL_PRINT("prepare error: %s\n", psnd_strerror(i));
        psnd_pcm_close(data->pcmHandle);
        free(data);
        return ALC_FALSE;
    }

    pDevice->ExtraData = data;
    return ALC_TRUE;
}

static void alsa_close_capture(ALCdevice *pDevice)
{
    alsa_data *data = (alsa_data*)pDevice->ExtraData;
    psnd_pcm_close(data->pcmHandle);

    free(data);
    pDevice->ExtraData = NULL;
}

static void alsa_start_capture(ALCdevice *pDevice)
{
    alsa_data *data = (alsa_data*)pDevice->ExtraData;
    psnd_pcm_start(data->pcmHandle);
}

static void alsa_stop_capture(ALCdevice *pDevice)
{
    alsa_data *data = (alsa_data*)pDevice->ExtraData;
    psnd_pcm_drain(data->pcmHandle);
}

static void alsa_capture_samples(ALCdevice *pDevice, ALCvoid *pBuffer, ALCuint lSamples)
{
    alsa_data *data = (alsa_data*)pDevice->ExtraData;
    const snd_pcm_channel_area_t *areas = NULL;
    snd_pcm_sframes_t frames, commitres;
    snd_pcm_uframes_t size, offset;
    int err;

    frames = psnd_pcm_avail_update(data->pcmHandle);
    if(frames < 0)
    {
        err = xrun_recovery(data->pcmHandle, frames);
        if (err < 0)
            AL_PRINT("available update failed: %s\n", psnd_strerror(err));
        else
            frames = psnd_pcm_avail_update(data->pcmHandle);
    }
    if (frames < (snd_pcm_sframes_t)lSamples)
    {
        SetALCError(ALC_INVALID_VALUE);
        return;
    }

    // it is possible that contiguous areas are smaller, thus we use a loop
    while (lSamples > 0)
    {
        char *Pointer;
        int Count;

        size = lSamples;
        err = psnd_pcm_mmap_begin(data->pcmHandle, &areas, &offset, &size);
        if (err < 0)
        {
            err = xrun_recovery(data->pcmHandle, err);
            if (err < 0)
            {
                AL_PRINT("mmap begin error: %s\n", psnd_strerror(err));
                break;
            }
            continue;
        }

        Pointer = (char*)areas->addr + (offset * areas->step / 8);
        Count = size * pDevice->FrameSize;

        memcpy(pBuffer, Pointer, Count);
        pBuffer = (char*)pBuffer + Count;

        commitres = psnd_pcm_mmap_commit(data->pcmHandle, offset, size);
        if (commitres < 0 || (commitres-size) != 0)
        {
           AL_PRINT("mmap commit error: %s\n",
                    psnd_strerror(commitres >= 0 ? -EPIPE : commitres));
           break;
        }

        lSamples -= size;
    }
}

static ALCuint alsa_available_samples(ALCdevice *pDevice)
{
    alsa_data *data = (alsa_data*)pDevice->ExtraData;
    snd_pcm_sframes_t frames = psnd_pcm_avail_update(data->pcmHandle);
    if(frames < 0)
    {
        int err = xrun_recovery(data->pcmHandle, frames);
        if (err < 0)
            AL_PRINT("available update failed: %s\n", psnd_strerror(err));
        else
            frames = psnd_pcm_avail_update(data->pcmHandle);
        if(frames < 0) /* ew.. */
            SetALCError(ALC_INVALID_DEVICE);
    }
    return max(frames, 0);
}


BackendFuncs alsa_funcs = {
    alsa_open_playback,
    alsa_close_playback,
    alsa_open_capture,
    alsa_close_capture,
    alsa_start_capture,
    alsa_stop_capture,
    alsa_capture_samples,
    alsa_available_samples
};

void alc_alsa_init(BackendFuncs *func_list)
{
    snd_ctl_t *handle;
    int card, err, dev, idx = 1;
    snd_ctl_card_info_t *info;
    snd_pcm_info_t *pcminfo;
    snd_pcm_stream_t stream = SND_PCM_STREAM_PLAYBACK;
    char name[128];
    char *str;

    *func_list = alsa_funcs;

#ifdef HAVE_DLFCN_H
    alsa_handle = dlopen("libasound.so.2", RTLD_NOW);
    if(!alsa_handle)
        return;
    dlerror();

#define LOAD_FUNC(f) do { \
    p##f = (typeof(f)*)dlsym(alsa_handle, #f); \
    if((str=dlerror()) != NULL) \
    { \
        dlclose(alsa_handle); \
        alsa_handle = NULL; \
        AL_PRINT("Could not load %s from libasound.so.2: %s\n", #f, str); \
        return; \
    } \
} while(0)
#else
    str = NULL;
    alsa_handle = NULL;
#define LOAD_FUNC(f) p##f = f
#endif

LOAD_FUNC(snd_strerror);
LOAD_FUNC(snd_pcm_open);
LOAD_FUNC(snd_pcm_close);
LOAD_FUNC(snd_pcm_nonblock);
LOAD_FUNC(snd_pcm_frames_to_bytes);
LOAD_FUNC(snd_pcm_hw_params_malloc);
LOAD_FUNC(snd_pcm_hw_params_free);
LOAD_FUNC(snd_pcm_hw_params_any);
LOAD_FUNC(snd_pcm_hw_params_set_access);
LOAD_FUNC(snd_pcm_hw_params_set_format);
LOAD_FUNC(snd_pcm_hw_params_set_channels);
LOAD_FUNC(snd_pcm_hw_params_set_periods_near);
LOAD_FUNC(snd_pcm_hw_params_set_rate_near);
LOAD_FUNC(snd_pcm_hw_params_set_rate);
LOAD_FUNC(snd_pcm_hw_params_set_buffer_size_near);
LOAD_FUNC(snd_pcm_hw_params_set_buffer_size_min);
LOAD_FUNC(snd_pcm_hw_params);
LOAD_FUNC(snd_pcm_prepare);
LOAD_FUNC(snd_pcm_start);
LOAD_FUNC(snd_pcm_resume);
LOAD_FUNC(snd_pcm_state);
LOAD_FUNC(snd_pcm_avail_update);
LOAD_FUNC(snd_pcm_areas_silence);
LOAD_FUNC(snd_pcm_mmap_begin);
LOAD_FUNC(snd_pcm_mmap_commit);
LOAD_FUNC(snd_pcm_drain);

LOAD_FUNC(snd_pcm_info_malloc);
LOAD_FUNC(snd_pcm_info_free);
LOAD_FUNC(snd_pcm_info_set_device);
LOAD_FUNC(snd_pcm_info_set_subdevice);
LOAD_FUNC(snd_pcm_info_set_stream);
LOAD_FUNC(snd_pcm_info_get_name);
LOAD_FUNC(snd_ctl_pcm_next_device);
LOAD_FUNC(snd_ctl_pcm_info);
LOAD_FUNC(snd_ctl_open);
LOAD_FUNC(snd_ctl_close);
LOAD_FUNC(snd_ctl_card_info_malloc);
LOAD_FUNC(snd_ctl_card_info_free);
LOAD_FUNC(snd_ctl_card_info);
LOAD_FUNC(snd_ctl_card_info_get_name);
LOAD_FUNC(snd_card_next);

#undef LOAD_FUNC

    psnd_ctl_card_info_malloc(&info);
    psnd_pcm_info_malloc(&pcminfo);

    card = -1;
    if(psnd_card_next(&card) < 0 || card < 0)
        AL_PRINT("no playback cards found...\n");
    else
    {
        alsaDeviceList[0] = AppendDeviceList("ALSA Software on default");
        allDevNameMap[0].name = AppendAllDeviceList("ALSA Software on default");
    }

    while (card >= 0) {
        sprintf(name, "hw:%d", card);
        if ((err = psnd_ctl_open(&handle, name, 0)) < 0) {
            AL_PRINT("control open (%i): %s\n", card, psnd_strerror(err));
            goto next_card;
        }
        if ((err = psnd_ctl_card_info(handle, info)) < 0) {
            AL_PRINT("control hardware info (%i): %s\n", card, psnd_strerror(err));
            psnd_ctl_close(handle);
            goto next_card;
        }
        if(card < MAX_DEVICES-1) {
            snprintf(name, sizeof(name), "ALSA Software on %s",
                     psnd_ctl_card_info_get_name(info));
            alsaDeviceList[card+1] = AppendDeviceList(name);
        }

        dev = -1;
        while (idx < MAX_ALL_DEVICES) {
            if (psnd_ctl_pcm_next_device(handle, &dev)<0)
                AL_PRINT("snd_ctl_pcm_next_device failed\n");
            if (dev < 0)
                break;
            psnd_pcm_info_set_device(pcminfo, dev);
            psnd_pcm_info_set_subdevice(pcminfo, 0);
            psnd_pcm_info_set_stream(pcminfo, stream);
            if ((err = psnd_ctl_pcm_info(handle, pcminfo)) < 0) {
                if (err != -ENOENT)
                    AL_PRINT("control digital audio info (%i): %s\n", card, psnd_strerror(err));
                continue;
            }
            snprintf(name, sizeof(name), "ALSA Software on %s [%s]",
                     psnd_ctl_card_info_get_name(info),
                     psnd_pcm_info_get_name(pcminfo));
            allDevNameMap[idx].name = AppendAllDeviceList(name);
            allDevNameMap[idx].card = card;
            allDevNameMap[idx].dev = dev;
            idx++;
        }
        psnd_ctl_close(handle);
next_card:
        if(psnd_card_next(&card) < 0) {
            AL_PRINT("snd_card_next failed\n");
            break;
        }
    }


    stream = SND_PCM_STREAM_CAPTURE;

    card = -1;
    if(psnd_card_next(&card) < 0 || card < 0) {
        AL_PRINT("no capture cards found...\n");
        psnd_pcm_info_free(pcminfo);
        psnd_ctl_card_info_free(info);
        return;
    }

    alsaCaptureDeviceList[0] = AppendCaptureDeviceList("ALSA Capture on default");

    while (card >= 0) {
        sprintf(name, "hw:%d", card);
        handle = NULL;
        if ((err = psnd_ctl_open(&handle, name, 0)) < 0) {
            AL_PRINT("control open (%i): %s\n", card, psnd_strerror(err));
        }
        if (err >= 0 && (err = psnd_ctl_card_info(handle, info)) < 0) {
            AL_PRINT("control hardware info (%i): %s\n", card, psnd_strerror(err));
            psnd_ctl_close(handle);
        }
        else if (err >= 0 && card < MAX_DEVICES-1)
        {
            snprintf(name, sizeof(name), "ALSA Capture on %s",
                     psnd_ctl_card_info_get_name(info));
            alsaCaptureDeviceList[card+1] = AppendCaptureDeviceList(name);
        }
        if(handle) psnd_ctl_close(handle);
        if(psnd_card_next(&card) < 0) {
            AL_PRINT("snd_card_next failed\n");
            break;
        }
    }
    psnd_pcm_info_free(pcminfo);
    psnd_ctl_card_info_free(info);
}
