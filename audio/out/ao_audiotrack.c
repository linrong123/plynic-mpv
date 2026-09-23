/*
 * Android AudioTrack audio output driver.
 * Copyright (C) 2018 Aman Gupta <aman@tmm1.net>
 * Copyright (C) 2012-2015 VLC authors and VideoLAN, VideoLabs
 * Authors: Thomas Guillem <thomas@gllm.fr>
 *          Ming Hu <tewilove@gmail.com>
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ao.h"
#include "internal.h"
#include "common/msg.h"
#include "audio/format.h"
#include "options/m_option.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "misc/jni.h"

static mp_static_mutex jni_static_lock = MP_STATIC_MUTEX_INITIALIZER;
static int jni_static_use_count = 0;

// Some devices return the native DEAD_OBJECT status (-EPIPE) from
// AudioTrack.write() instead of translating it to ERROR_DEAD_OBJECT.
#define ERROR_NATIVE_DEAD_OBJECT (-32)
// Consecutive failed writes after which the track is given up on.
#define MAX_WRITE_ERRORS 3

struct priv {
    jobject audiotrack;
    jint samplerate;
    jint channel_config;
    jint format;
    jint size;
    jint buffer_frames; // size of the track buffer in frames

    jobject timestamp;
    int64_t timestamp_fetched;
    bool timestamp_set;
    int timestamp_stable;
    double last_delay;

    uint32_t written_frames; /* requires uint32_t rollover semantics */
    uint32_t playhead_pos;
    uint32_t playhead_offset;
    bool reset_pending;

    void *chunk;
    int chunksize;
    jbyteArray bytearray;
    jshortArray shortarray;
    jfloatArray floatarray;
    jobject bbuf;

    bool cfg_pcm_float;
    int cfg_session_id;

    bool track_dead;    // write() failed for good, waiting for the AO reload
    int write_errors;   // consecutive failed write() calls

    bool thread_terminate;
    bool thread_created;
    mp_thread thread;
    mp_mutex lock;
    mp_cond wakeup;
};

static struct JNIByteBuffer {
    jclass clazz;
    jmethodID clear;
} ByteBuffer;
#define OFFSET(member) offsetof(struct JNIByteBuffer, member)
static const struct MPJniField ByteBuffer_mapping[] = {
    {"java/nio/ByteBuffer", NULL, MP_JNI_CLASS, OFFSET(clazz), 1},
    {"clear", "()Ljava/nio/Buffer;", MP_JNI_METHOD, OFFSET(clear), 1},
    {0},
};
#undef OFFSET

static struct JNIAudioTrack {
    jclass clazz;
    jmethodID ctor;
    jmethodID ctorV21;
    jmethodID release;
    jmethodID getState;
    jmethodID getPlayState;
    jmethodID play;
    jmethodID stop;
    jmethodID flush;
    jmethodID pause;
    jmethodID write;
    jmethodID writeFloat;
    jmethodID writeShortV23;
    jmethodID writeBufferV21;
    jmethodID getBufferSizeInFramesV23;
    jmethodID getPlaybackHeadPosition;
    jmethodID getTimestamp;
    jmethodID getLatency;
    jmethodID getMinBufferSize;
    jmethodID getNativeOutputSampleRate;
    jint STATE_INITIALIZED;
    jint PLAYSTATE_STOPPED;
    jint PLAYSTATE_PAUSED;
    jint PLAYSTATE_PLAYING;
    jint MODE_STREAM;
    jint ERROR;
    jint ERROR_BAD_VALUE;
    jint ERROR_INVALID_OPERATION;
    jint WRITE_BLOCKING;
    jint WRITE_NON_BLOCKING;
} AudioTrack;
#define OFFSET(member) offsetof(struct JNIAudioTrack, member)
static const struct MPJniField AudioTrack_mapping[] = {
    {"android/media/AudioTrack", NULL, MP_JNI_CLASS, OFFSET(clazz), 1},
    {"<init>", "(IIIIIII)V", MP_JNI_METHOD, OFFSET(ctor), 1},
    {"<init>", "(Landroid/media/AudioAttributes;Landroid/media/AudioFormat;III)V", MP_JNI_METHOD, OFFSET(ctorV21), 0},
    {"release", "()V", MP_JNI_METHOD, OFFSET(release), 1},
    {"getState", "()I", MP_JNI_METHOD, OFFSET(getState), 1},
    {"getPlayState", "()I", MP_JNI_METHOD, OFFSET(getPlayState), 1},
    {"play", "()V", MP_JNI_METHOD, OFFSET(play), 1},
    {"stop", "()V", MP_JNI_METHOD, OFFSET(stop), 1},
    {"flush", "()V", MP_JNI_METHOD, OFFSET(flush), 1},
    {"pause", "()V", MP_JNI_METHOD, OFFSET(pause), 1},
    {"write", "([BII)I", MP_JNI_METHOD, OFFSET(write), 1},
    {"write", "([FIII)I", MP_JNI_METHOD, OFFSET(writeFloat), 1},
    {"write", "([SIII)I", MP_JNI_METHOD, OFFSET(writeShortV23), 0},
    {"write", "(Ljava/nio/ByteBuffer;II)I", MP_JNI_METHOD, OFFSET(writeBufferV21), 1},
    {"getBufferSizeInFrames", "()I", MP_JNI_METHOD, OFFSET(getBufferSizeInFramesV23), 0},
    {"getTimestamp", "(Landroid/media/AudioTimestamp;)Z", MP_JNI_METHOD, OFFSET(getTimestamp), 1},
    {"getPlaybackHeadPosition", "()I", MP_JNI_METHOD, OFFSET(getPlaybackHeadPosition), 1},
    {"getLatency", "()I", MP_JNI_METHOD, OFFSET(getLatency), 1},
    {"getMinBufferSize", "(III)I", MP_JNI_STATIC_METHOD, OFFSET(getMinBufferSize), 1},
    {"getNativeOutputSampleRate", "(I)I", MP_JNI_STATIC_METHOD, OFFSET(getNativeOutputSampleRate), 1},
    {"WRITE_BLOCKING", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(WRITE_BLOCKING), 0},
    {"WRITE_NON_BLOCKING", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(WRITE_NON_BLOCKING), 0},
    {"STATE_INITIALIZED", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(STATE_INITIALIZED), 1},
    {"PLAYSTATE_STOPPED", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(PLAYSTATE_STOPPED), 1},
    {"PLAYSTATE_PAUSED", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(PLAYSTATE_PAUSED), 1},
    {"PLAYSTATE_PLAYING", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(PLAYSTATE_PLAYING), 1},
    {"MODE_STREAM", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(MODE_STREAM), 1},
    {"ERROR", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(ERROR), 1},
    {"ERROR_BAD_VALUE", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(ERROR_BAD_VALUE), 1},
    {"ERROR_INVALID_OPERATION", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(ERROR_INVALID_OPERATION), 1},
    {0}
};
#undef OFFSET

static struct JNIAudioAttributes {
    jclass clazz;
    jint CONTENT_TYPE_MOVIE;
    jint CONTENT_TYPE_MUSIC;
    jint USAGE_MEDIA;
} AudioAttributes;
#define OFFSET(member) offsetof(struct JNIAudioAttributes, member)
static const struct MPJniField AudioAttributes_mapping[] = {
    {"android/media/AudioAttributes", NULL, MP_JNI_CLASS, OFFSET(clazz), 0},
    {"CONTENT_TYPE_MOVIE", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(CONTENT_TYPE_MOVIE), 0},
    {"CONTENT_TYPE_MUSIC", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(CONTENT_TYPE_MUSIC), 0},
    {"USAGE_MEDIA", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(USAGE_MEDIA), 0},
    {0}
};
#undef OFFSET

static struct JNIAudioAttributesBuilder {
    jclass clazz;
    jmethodID ctor;
    jmethodID setUsage;
    jmethodID setContentType;
    jmethodID build;
} AudioAttributesBuilder;
#define OFFSET(member) offsetof(struct JNIAudioAttributesBuilder, member)
static const struct MPJniField AudioAttributesBuilder_mapping[] = {
    {"android/media/AudioAttributes$Builder", NULL, MP_JNI_CLASS, OFFSET(clazz), 0},
    {"<init>", "()V", MP_JNI_METHOD, OFFSET(ctor), 0},
    {"setUsage", "(I)Landroid/media/AudioAttributes$Builder;", MP_JNI_METHOD, OFFSET(setUsage), 0},
    {"setContentType", "(I)Landroid/media/AudioAttributes$Builder;", MP_JNI_METHOD, OFFSET(setContentType), 0},
    {"build", "()Landroid/media/AudioAttributes;", MP_JNI_METHOD, OFFSET(build), 0},
    {0}
};
#undef OFFSET

static struct JNIAudioFormat {
    jclass clazz;
    jint ENCODING_PCM_8BIT;
    jint ENCODING_PCM_16BIT;
    jint ENCODING_PCM_FLOAT;
    jint ENCODING_IEC61937;
    jint CHANNEL_OUT_MONO;
    jint CHANNEL_OUT_STEREO;
    jint CHANNEL_OUT_FRONT_CENTER;
    jint CHANNEL_OUT_QUAD;
    jint CHANNEL_OUT_5POINT1;
    jint CHANNEL_OUT_BACK_CENTER;
    jint CHANNEL_OUT_7POINT1_SURROUND;
} AudioFormat;
#define OFFSET(member) offsetof(struct JNIAudioFormat, member)
static const struct MPJniField AudioFormat_mapping[] = {
    {"android/media/AudioFormat", NULL, MP_JNI_CLASS, OFFSET(clazz), 1},
    {"ENCODING_PCM_8BIT", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(ENCODING_PCM_8BIT), 1},
    {"ENCODING_PCM_16BIT", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(ENCODING_PCM_16BIT), 1},
    {"ENCODING_PCM_FLOAT", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(ENCODING_PCM_FLOAT), 1},
    {"ENCODING_IEC61937", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(ENCODING_IEC61937), 0},
    {"CHANNEL_OUT_MONO", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(CHANNEL_OUT_MONO), 1},
    {"CHANNEL_OUT_STEREO", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(CHANNEL_OUT_STEREO), 1},
    {"CHANNEL_OUT_FRONT_CENTER", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(CHANNEL_OUT_FRONT_CENTER), 1},
    {"CHANNEL_OUT_QUAD", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(CHANNEL_OUT_QUAD), 1},
    {"CHANNEL_OUT_5POINT1", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(CHANNEL_OUT_5POINT1), 1},
    {"CHANNEL_OUT_BACK_CENTER", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(CHANNEL_OUT_BACK_CENTER), 1},
    {"CHANNEL_OUT_7POINT1_SURROUND", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(CHANNEL_OUT_7POINT1_SURROUND), 0},
    {0}
};
#undef OFFSET

static struct JNIAudioFormatBuilder {
    jclass clazz;
    jmethodID ctor;
    jmethodID setEncoding;
    jmethodID setSampleRate;
    jmethodID setChannelMask;
    jmethodID build;
} AudioFormatBuilder;
#define OFFSET(member) offsetof(struct JNIAudioFormatBuilder, member)
static const struct MPJniField AudioFormatBuilder_mapping[] = {
    {"android/media/AudioFormat$Builder", NULL, MP_JNI_CLASS, OFFSET(clazz), 0},
    {"<init>", "()V", MP_JNI_METHOD, OFFSET(ctor), 0},
    {"setEncoding", "(I)Landroid/media/AudioFormat$Builder;", MP_JNI_METHOD, OFFSET(setEncoding), 0},
    {"setSampleRate", "(I)Landroid/media/AudioFormat$Builder;", MP_JNI_METHOD, OFFSET(setSampleRate), 0},
    {"setChannelMask", "(I)Landroid/media/AudioFormat$Builder;", MP_JNI_METHOD, OFFSET(setChannelMask), 0},
    {"build", "()Landroid/media/AudioFormat;", MP_JNI_METHOD, OFFSET(build), 0},
    {0}
};
#undef OFFSET

static struct JNIAudioManager {
    jclass clazz;
    jint ERROR_DEAD_OBJECT;
    jint STREAM_MUSIC;
} AudioManager;
#define OFFSET(member) offsetof(struct JNIAudioManager, member)
static const struct MPJniField AudioManager_mapping[] = {
    {"android/media/AudioManager", NULL, MP_JNI_CLASS, OFFSET(clazz), 1},
    {"STREAM_MUSIC", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(STREAM_MUSIC), 1},
    {"ERROR_DEAD_OBJECT", "I", MP_JNI_STATIC_FIELD_AS_INT, OFFSET(ERROR_DEAD_OBJECT), 0},
    {0}
};
#undef OFFSET

static struct JNIAudioTimestamp {
    jclass clazz;
    jmethodID ctor;
    jfieldID framePosition;
    jfieldID nanoTime;
} AudioTimestamp;
#define OFFSET(member) offsetof(struct JNIAudioTimestamp, member)
static const struct MPJniField AudioTimestamp_mapping[] = {
    {"android/media/AudioTimestamp", NULL, MP_JNI_CLASS, OFFSET(clazz), 1},
    {"<init>", "()V", MP_JNI_METHOD, OFFSET(ctor), 1},
    {"framePosition", "J", MP_JNI_FIELD, OFFSET(framePosition), 1},
    {"nanoTime", "J", MP_JNI_FIELD, OFFSET(nanoTime), 1},
    {0}
};
#undef OFFSET

#define ENTRY(name) { &name, name ## _mapping }
static const struct {
    void *fields;
    const struct MPJniField *mapping;
} jclass_list[] = {
    ENTRY(ByteBuffer),
    ENTRY(AudioTrack),
    ENTRY(AudioAttributes),
    ENTRY(AudioAttributesBuilder),
    ENTRY(AudioFormat),
    ENTRY(AudioFormatBuilder),
    ENTRY(AudioManager),
    ENTRY(AudioTimestamp),
};
#undef ENTRY

static int AudioTrack_New(struct ao *ao)
{
    struct priv *p = ao->priv;
    JNIEnv *env = MP_JNI_GET_ENV(ao);
    jobject audiotrack = NULL;

    if (AudioTrack.ctorV21) {
        MP_VERBOSE(ao, "Using API21 initializer\n");
        jobject tmp = NULL;

        jobject format_builder = MP_JNI_NEW(AudioFormatBuilder.clazz, AudioFormatBuilder.ctor);
        MP_JNI_EXCEPTION_LOG(ao);
        tmp = MP_JNI_CALL_OBJECT(format_builder, AudioFormatBuilder.setEncoding, p->format);
        MP_JNI_LOCAL_FREEP(&tmp);
        tmp = MP_JNI_CALL_OBJECT(format_builder, AudioFormatBuilder.setSampleRate, p->samplerate);
        MP_JNI_LOCAL_FREEP(&tmp);
        tmp = MP_JNI_CALL_OBJECT(format_builder, AudioFormatBuilder.setChannelMask, p->channel_config);
        MP_JNI_LOCAL_FREEP(&tmp);
        jobject format = MP_JNI_CALL_OBJECT(format_builder, AudioFormatBuilder.build);
        MP_JNI_LOCAL_FREEP(&format_builder);

        jobject attr_builder = MP_JNI_NEW(AudioAttributesBuilder.clazz, AudioAttributesBuilder.ctor);
        MP_JNI_EXCEPTION_LOG(ao);
        tmp = MP_JNI_CALL_OBJECT(attr_builder, AudioAttributesBuilder.setUsage, AudioAttributes.USAGE_MEDIA);
        MP_JNI_LOCAL_FREEP(&tmp);
        if (ao->set_media_role) {
            jint content_type = (ao->init_flags & AO_INIT_MEDIA_ROLE_MUSIC) ?
                AudioAttributes.CONTENT_TYPE_MUSIC : AudioAttributes.CONTENT_TYPE_MOVIE;
            tmp = MP_JNI_CALL_OBJECT(attr_builder, AudioAttributesBuilder.setContentType, content_type);
            MP_JNI_LOCAL_FREEP(&tmp);
        }
        jobject attr = MP_JNI_CALL_OBJECT(attr_builder, AudioAttributesBuilder.build);
        MP_JNI_LOCAL_FREEP(&attr_builder);

        audiotrack = MP_JNI_NEW(
            AudioTrack.clazz,
            AudioTrack.ctorV21,
            attr,
            format,
            p->size,
            AudioTrack.MODE_STREAM,
            p->cfg_session_id
        );

        MP_JNI_LOCAL_FREEP(&format);
        MP_JNI_LOCAL_FREEP(&attr);
    } else {
        MP_VERBOSE(ao, "Using legacy initializer\n");
        audiotrack = MP_JNI_NEW(
            AudioTrack.clazz,
            AudioTrack.ctor,
            AudioManager.STREAM_MUSIC,
            p->samplerate,
            p->channel_config,
            p->format,
            p->size,
            AudioTrack.MODE_STREAM,
            p->cfg_session_id
        );
    }
    if (MP_JNI_EXCEPTION_LOG(ao) < 0 || !audiotrack) {
        MP_FATAL(ao, "AudioTrack Init failed\n");
        return -1;
    }

    if (MP_JNI_CALL_INT(audiotrack, AudioTrack.getState) != AudioTrack.STATE_INITIALIZED) {
        MP_JNI_CALL_VOID(audiotrack, AudioTrack.release);
        MP_JNI_EXCEPTION_LOG(ao);
        MP_JNI_LOCAL_FREEP(&audiotrack);
        MP_ERR(ao, "AudioTrack.getState failed\n");
        return -1;
    }

    if (AudioTrack.getBufferSizeInFramesV23) {
        int bufferSize = MP_JNI_CALL_INT(audiotrack, AudioTrack.getBufferSizeInFramesV23);
        if (bufferSize > 0) {
            MP_VERBOSE(ao, "AudioTrack.getBufferSizeInFrames = %d\n", bufferSize);
            ao->device_buffer = bufferSize;
            p->buffer_frames = bufferSize;
        }
    }

    p->audiotrack = (*env)->NewGlobalRef(env, audiotrack);
    MP_JNI_LOCAL_FREEP(&audiotrack);
    if (!p->audiotrack)
        return -1;

    return 0;
}

static uint32_t AudioTrack_getPlaybackHeadPosition(struct ao *ao)
{
    struct priv *p = ao->priv;
    if (!p->audiotrack)
        return 0;
    JNIEnv *env = MP_JNI_GET_ENV(ao);
    uint32_t pos = 0;
    int64_t now = mp_raw_time_ns();
    int state = MP_JNI_CALL_INT(p->audiotrack, AudioTrack.getPlayState);

    int stable_count = 20;
    int64_t wait = p->timestamp_stable < stable_count ? 50000000 : 3000000000;

    if (state == AudioTrack.PLAYSTATE_PLAYING && p->format != AudioFormat.ENCODING_IEC61937 &&
        (p->timestamp_fetched == 0 || now - p->timestamp_fetched >= wait)) {
        if (!p->timestamp_fetched)
            p->timestamp_stable = 0;

        int64_t time1 = MP_JNI_GET_LONG(p->timestamp, AudioTimestamp.nanoTime);
        if (MP_JNI_CALL_BOOL(p->audiotrack, AudioTrack.getTimestamp, p->timestamp)) {
            p->timestamp_set = true;
            p->timestamp_fetched = now;
            if (p->timestamp_stable < stable_count) {
                uint32_t fpos = 0xFFFFFFFFL & MP_JNI_GET_LONG(p->timestamp, AudioTimestamp.framePosition);
                int64_t time2 = MP_JNI_GET_LONG(p->timestamp, AudioTimestamp.nanoTime);
                //MP_VERBOSE(ao, "getTimestamp: fpos= %u / time= %"PRId64" / now= %"PRId64" / stable= %d\n", fpos, time2, now, p->timestamp_stable);
                if (time1 != time2 && time2 != 0 && fpos != 0) {
                    p->timestamp_stable++;
                }
            }
        }
    }

    /* AudioTrack's framePosition and playbackHeadPosition return a signed integer,
     * but documentation states it should be interpreted as a 32-bit unsigned integer.
     */
    if (p->timestamp_set) {
        pos = 0xFFFFFFFFL & MP_JNI_GET_LONG(p->timestamp, AudioTimestamp.framePosition);
        uint32_t fpos = pos;
        int64_t time = MP_JNI_GET_LONG(p->timestamp, AudioTimestamp.nanoTime);
        if (time == 0)
            fpos = pos = 0;
        if (fpos != 0 && time != 0 && state == AudioTrack.PLAYSTATE_PLAYING) {
            double diff = (double)(now - time) / 1e9;
            pos += diff * ao->samplerate;
        }
        //MP_VERBOSE(ao, "position = %u via getTimestamp (state = %d / fpos= %u / time= %"PRId64")\n", pos, state, fpos, time);
    } else {
        pos = 0xFFFFFFFFL & MP_JNI_CALL_INT(p->audiotrack, AudioTrack.getPlaybackHeadPosition);
        //MP_VERBOSE(ao, "playbackHeadPosition = %u (reset_pending=%d)\n", pos, p->reset_pending);
    }


    if (p->format == AudioFormat.ENCODING_IEC61937) {
        if (p->reset_pending) {
            // after a flush(), playbackHeadPosition will not reset to 0 right away.
            // sometimes, it will never reset at all.
            // save the initial offset after the reset, to subtract it going forward.
            if (p->playhead_offset == 0)
                p->playhead_offset = pos;
            p->reset_pending = false;
            MP_VERBOSE(ao, "IEC/playbackHead offset = %d\n", pos);
        }

        // usually shortly after a flush(), playbackHeadPosition will reset to 0.
        // clear out the position and offset to avoid regular "rollover" below
        if (pos == 0 && p->playhead_offset != 0) {
            MP_VERBOSE(ao, "IEC/playbackHeadPosition %d -> %d (flush)\n", p->playhead_pos, pos);
            p->playhead_offset = 0;
            p->playhead_pos = 0;
        }

        // sometimes on a new AudioTrack instance, playbackHeadPosition will reset
        // to 0 shortly after playback starts for no reason.
        if (pos == 0 && p->playhead_pos != 0) {
            MP_VERBOSE(ao, "IEC/playbackHeadPosition %d -> %d (reset)\n", p->playhead_pos, pos);
            p->playhead_offset = 0;
            p->playhead_pos = 0;
            p->written_frames = 0;
        }
    }

    p->playhead_pos = pos;
    return p->playhead_pos - p->playhead_offset;
}

static double AudioTrack_getLatency(struct ao *ao)
{
    JNIEnv *env = MP_JNI_GET_ENV(ao);
    struct priv *p = ao->priv;
    if (!p->audiotrack)
        return 0;

    uint32_t playhead = AudioTrack_getPlaybackHeadPosition(ao);
    // Signed: a position extrapolated from a timestamp runs past what was
    // written when the track underruns. Nothing is buffered then, and the
    // timestamp needs refreshing.
    int32_t diff = (int32_t)(p->written_frames - playhead);
    if (diff < 0) {
        p->timestamp_fetched = 0;
        diff = 0;
    }
    double delay = diff / (double)(ao->samplerate);
    if (!p->timestamp_set &&
        p->format != AudioFormat.ENCODING_IEC61937)
    {
        // The head position counts what the mixer has consumed, so the output
        // latency behind the track is missing. getLatency() is that latency
        // plus the whole track buffer, which written - head already covers:
        // add only the part beyond the track buffer.
        double latency = MP_JNI_CALL_INT(p->audiotrack, AudioTrack.getLatency) / 1000.0;
        delay += MPMAX(0, latency - p->buffer_frames / (double)(ao->samplerate));
    }
    if (delay > 2.0) {
        // Not plausible with a track buffer of a fraction of a second. Poll a
        // new timestamp, and keep the last estimate meanwhile: returning 0
        // would make the audio clock (and the video synced to it) jump.
        MP_DBG(ao, "getLatency: written=%u playhead=%u delay=%f, ignored\n",
               p->written_frames, playhead, delay);
        p->timestamp_fetched = 0;
        return p->last_delay;
    }
    p->last_delay = delay;
    return delay;
}

static int AudioTrack_write(struct ao *ao, int len)
{
    struct priv *p = ao->priv;
    if (!p->audiotrack)
        return -1;
    JNIEnv *env = MP_JNI_GET_ENV(ao);
    void *buf = p->chunk;

    jint ret;
    if (p->format == AudioFormat.ENCODING_IEC61937) {
        (*env)->SetShortArrayRegion(env, p->shortarray, 0, len / 2, buf);
        if (MP_JNI_EXCEPTION_LOG(ao) < 0) return -1;
        ret = MP_JNI_CALL_INT(p->audiotrack, AudioTrack.writeShortV23, p->shortarray, 0, len / 2, AudioTrack.WRITE_BLOCKING);
        if (MP_JNI_EXCEPTION_LOG(ao) < 0) return -1;
        if (ret > 0) ret *= 2;

    } else if (AudioTrack.writeBufferV21) {
        // reset positions for reading
        jobject bbuf = MP_JNI_CALL_OBJECT(p->bbuf, ByteBuffer.clear);
        if (MP_JNI_EXCEPTION_LOG(ao) < 0) return -1;
        MP_JNI_LOCAL_FREEP(&bbuf);
        ret = MP_JNI_CALL_INT(p->audiotrack, AudioTrack.writeBufferV21, p->bbuf, len, AudioTrack.WRITE_BLOCKING);
        if (MP_JNI_EXCEPTION_LOG(ao) < 0) return -1;

    } else if (p->format == AudioFormat.ENCODING_PCM_FLOAT) {
        (*env)->SetFloatArrayRegion(env, p->floatarray, 0, len / sizeof(float), buf);
        if (MP_JNI_EXCEPTION_LOG(ao) < 0) return -1;
        ret = MP_JNI_CALL_INT(p->audiotrack, AudioTrack.writeFloat, p->floatarray, 0, len / sizeof(float), AudioTrack.WRITE_BLOCKING);
        if (MP_JNI_EXCEPTION_LOG(ao) < 0) return -1;
        if (ret > 0) ret *= sizeof(float);

    } else {
        (*env)->SetByteArrayRegion(env, p->bytearray, 0, len, buf);
        if (MP_JNI_EXCEPTION_LOG(ao) < 0) return -1;
        ret = MP_JNI_CALL_INT(p->audiotrack, AudioTrack.write, p->bytearray, 0, len);
        if (MP_JNI_EXCEPTION_LOG(ao) < 0) return -1;
    }

    return ret;
}

static void uninit_jni(struct ao *ao)
{
    mp_mutex_lock(&jni_static_lock);
    jni_static_use_count--;
    if (jni_static_use_count == 0) {
        JNIEnv *env = MP_JNI_GET_ENV(ao);
        for (int i = 0; i < MP_ARRAY_SIZE(jclass_list); i++) {
            mp_jni_reset_jfields(env, jclass_list[i].fields,
                                jclass_list[i].mapping, 1, ao->log);
        }
    }
    mp_mutex_unlock(&jni_static_lock);
}

static int init_jni(struct ao *ao)
{
    mp_mutex_lock(&jni_static_lock);
    JNIEnv *env = MP_JNI_GET_ENV(ao);
    if (jni_static_use_count == 0) {
        for (int i = 0; i < MP_ARRAY_SIZE(jclass_list); i++) {
            if (mp_jni_init_jfields(env, jclass_list[i].fields,
                                    jclass_list[i].mapping, 1, ao->log) < 0) {
                goto error;
            }
        }
    }
    jni_static_use_count++;
    mp_mutex_unlock(&jni_static_lock);
    return 0;

error:
    for (int i = 0; i < MP_ARRAY_SIZE(jclass_list); i++) {
        mp_jni_reset_jfields(env, jclass_list[i].fields,
                            jclass_list[i].mapping, 1, ao->log);
    }
    mp_mutex_unlock(&jni_static_lock);
    return -1;
}

static MP_THREAD_VOID ao_thread(void *arg)
{
    struct ao *ao = arg;
    struct priv *p = ao->priv;
    JNIEnv *env = MP_JNI_GET_ENV(ao);
    mp_thread_set_name("ao/audiotrack");
    mp_mutex_lock(&p->lock);
    while (!p->thread_terminate) {
        int state = AudioTrack.PLAYSTATE_PAUSED;
        if (p->audiotrack && !p->track_dead) {
            state = MP_JNI_CALL_INT(p->audiotrack, AudioTrack.getPlayState);
        }
        if (state == AudioTrack.PLAYSTATE_PLAYING) {
            int read_samples = p->chunksize / ao->sstride;
            int64_t ts = mp_time_ns();
            ts += MP_TIME_S_TO_NS(read_samples / (double)(ao->samplerate));
            ts += MP_TIME_S_TO_NS(AudioTrack_getLatency(ao));
            int samples = ao_read_data(ao, &p->chunk, read_samples, ts, NULL, false, false);
            if (!samples) {
                // Underrun or EOF (or a pause/reset is on its way). Nothing
                // tells this thread when the core refills the buffer, so poll,
                // but don't spin: that would take CPU time from the decoder
                // that is late already. 10 ms is well within the >= 75 ms
                // the track buffer holds.
                mp_cond_timedwait(&p->wakeup, &p->lock, MP_TIME_MS_TO_NS(10));
                continue;
            }
            int ret = AudioTrack_write(ao, samples * ao->sstride);
            if (ret >= 0) {
                p->written_frames += ret / ao->sstride;
                p->write_errors = 0;
            } else if (ret == AudioManager.ERROR_DEAD_OBJECT ||
                       ret == ERROR_NATIVE_DEAD_OBJECT ||
                       ++p->write_errors >= MAX_WRITE_ERRORS)
            {
                // The track is gone for good. A direct or offloaded output
                // (multichannel PCM or passthrough over HDMI, some phones'
                // direct PCM path) is not restored by AudioTrack when the
                // route changes (BT/HDMI hotplug, audioserver restart), and
                // the new route may need a different format anyway. Stop
                // feeding it and let the core rebuild the AO, which
                // renegotiates the format and starts it like any other.
                MP_WARN(ao, "AudioTrack.write failed with %d, reloading the "
                        "audio output\n", ret);
                p->track_dead = true;
                ao_request_reload(ao);
            } else {
                MP_ERR(ao, "AudioTrack.write failed with %d\n", ret);
                mp_cond_timedwait(&p->wakeup, &p->lock, MP_TIME_MS_TO_NS(20));
            }
        } else {
            mp_cond_timedwait(&p->wakeup, &p->lock, MP_TIME_MS_TO_NS(300));
        }
    }
    mp_mutex_unlock(&p->lock);
    MP_THREAD_RETURN();
}

static void uninit(struct ao *ao)
{
    struct priv *p = ao->priv;
    JNIEnv *env = MP_JNI_GET_ENV(ao);
    if (p->audiotrack) {
        MP_JNI_CALL_VOID(p->audiotrack, AudioTrack.stop);
        MP_JNI_EXCEPTION_LOG(ao);
        MP_JNI_CALL_VOID(p->audiotrack, AudioTrack.flush);
        MP_JNI_EXCEPTION_LOG(ao);
    }

    mp_mutex_lock(&p->lock);
    p->thread_terminate = true;
    mp_cond_signal(&p->wakeup);
    mp_mutex_unlock(&p->lock);

    if (p->thread_created)
        mp_thread_join(p->thread);

    if (p->audiotrack) {
        MP_JNI_CALL_VOID(p->audiotrack, AudioTrack.release);
        MP_JNI_EXCEPTION_LOG(ao);
        MP_JNI_GLOBAL_FREEP(&p->audiotrack);
    }

    MP_JNI_GLOBAL_FREEP(&p->bytearray);

    MP_JNI_GLOBAL_FREEP(&p->shortarray);

    MP_JNI_GLOBAL_FREEP(&p->floatarray);

    MP_JNI_GLOBAL_FREEP(&p->bbuf);

    MP_JNI_GLOBAL_FREEP(&p->timestamp);

    mp_cond_destroy(&p->wakeup);
    mp_mutex_destroy(&p->lock);

    uninit_jni(ao);
}

static int init(struct ao *ao)
{
    struct priv *p = ao->priv;
    JNIEnv *env = MP_JNI_GET_ENV(ao);
    if (!env)
        return -1;

    mp_mutex_init(&p->lock);
    mp_cond_init(&p->wakeup);

    if (init_jni(ao) < 0)
        return -1;

    if (af_fmt_is_spdif(ao->format)) {
        p->format = AudioFormat.ENCODING_IEC61937;
        if (!p->format || !AudioTrack.writeShortV23) {
            MP_ERR(ao, "spdif passthrough not supported by API\n");
            return -1;
        }
    } else if (ao->format == AF_FORMAT_U8) {
        p->format = AudioFormat.ENCODING_PCM_8BIT;
    } else if (p->cfg_pcm_float && af_fmt_is_float(ao->format)) {
        ao->format = AF_FORMAT_FLOAT;
        p->format = AudioFormat.ENCODING_PCM_FLOAT;
    } else {
        ao->format = AF_FORMAT_S16;
        p->format = AudioFormat.ENCODING_PCM_16BIT;
    }

    if (AudioTrack.getNativeOutputSampleRate) {
        jint samplerate = MP_JNI_CALL_STATIC_INT(
            AudioTrack.clazz,
            AudioTrack.getNativeOutputSampleRate,
            AudioManager.STREAM_MUSIC
        );
        if (MP_JNI_EXCEPTION_LOG(ao) == 0) {
            MP_VERBOSE(ao, "AudioTrack.nativeOutputSampleRate = %d\n", samplerate);
            ao->samplerate = MPMIN(samplerate, ao->samplerate);
        }
    }
    p->samplerate = ao->samplerate;

    /* https://developer.android.com/reference/android/media/AudioFormat#channelPositionMask */
    static const struct mp_chmap layouts[] = {
        {0},                                        // empty
        MP_CHMAP_INIT_MONO,                         // mono
        MP_CHMAP_INIT_STEREO,                       // stereo
        MP_CHMAP3(FL, FR, FC),                      // 3.0
        MP_CHMAP4(FL, FR, BL, BR),                  // quad
        MP_CHMAP5(FL, FR, FC, BL, BR),              // 5.0
        MP_CHMAP6(FL, FR, FC, LFE, BL, BR),         // 5.1
        MP_CHMAP7(FL, FR, FC, LFE, BL, BR, BC),     // 6.1
        MP_CHMAP8(FL, FR, FC, LFE, BL, BR, SL, SR), // 7.1
    };
    const jint layout_map[] = {
        0,
        AudioFormat.CHANNEL_OUT_MONO,
        AudioFormat.CHANNEL_OUT_STEREO,
        AudioFormat.CHANNEL_OUT_STEREO | AudioFormat.CHANNEL_OUT_FRONT_CENTER,
        AudioFormat.CHANNEL_OUT_QUAD,
        AudioFormat.CHANNEL_OUT_QUAD | AudioFormat.CHANNEL_OUT_FRONT_CENTER,
        AudioFormat.CHANNEL_OUT_5POINT1,
        AudioFormat.CHANNEL_OUT_5POINT1 | AudioFormat.CHANNEL_OUT_BACK_CENTER,
        AudioFormat.CHANNEL_OUT_7POINT1_SURROUND,
    };
    static_assert(MP_ARRAY_SIZE(layout_map) == MP_ARRAY_SIZE(layouts), "");
    if (p->format == AudioFormat.ENCODING_IEC61937) {
        p->channel_config = AudioFormat.CHANNEL_OUT_STEREO;
    } else {
        struct mp_chmap_sel sel = {0};
        for (int i = 0; i < MP_ARRAY_SIZE(layouts); i++) {
            if (layout_map[i])
                mp_chmap_sel_add_map(&sel, &layouts[i]);
        }
        if (!ao_chmap_sel_adjust(ao, &sel, &ao->channels))
            goto error;
        p->channel_config = layout_map[ao->channels.num];
        mp_assert(p->channel_config);
    }

    jint buffer_size = MP_JNI_CALL_STATIC_INT(
        AudioTrack.clazz,
        AudioTrack.getMinBufferSize,
        p->samplerate,
        p->channel_config,
        p->format
    );
    if (MP_JNI_EXCEPTION_LOG(ao) < 0 || buffer_size <= 0) {
        MP_FATAL(ao, "AudioTrack.getMinBufferSize returned an invalid size: %d", buffer_size);
        return -1;
    }

    // Choose double of the minimum buffer size suggested by the driver, but not
    // less than 75ms or more than 150ms.
    const int bps = af_fmt_to_bytes(ao->format);
    int min = 0.075 * p->samplerate * bps * ao->channels.num;
    int max = min * 2;
    min = MP_ALIGN_UP(min, bps);
    max = MP_ALIGN_UP(max, bps);
    p->size = MPCLAMP(buffer_size * 2, min, max);
    MP_VERBOSE(ao, "Setting bufferSize = %d (driver=%d, min=%d, max=%d)\n", p->size, buffer_size, min, max);
    mp_assert(p->size % bps == 0);
    ao->device_buffer = p->size / bps;
    p->buffer_frames = p->size / (bps * ao->channels.num);

    p->chunksize = p->size;
    p->chunk = talloc_size(ao, p->size);

    jobject timestamp = MP_JNI_NEW(AudioTimestamp.clazz, AudioTimestamp.ctor);
    if (MP_JNI_EXCEPTION_LOG(ao) < 0 || !timestamp) {
        MP_FATAL(ao, "AudioTimestamp could not be created\n");
        return -1;
    }
    p->timestamp = (*env)->NewGlobalRef(env, timestamp);
    MP_JNI_LOCAL_FREEP(&timestamp);

    // decide and create buffer of right type
    if (p->format == AudioFormat.ENCODING_IEC61937) {
        jshortArray shortarray = (*env)->NewShortArray(env, p->chunksize / 2);
        p->shortarray = (*env)->NewGlobalRef(env, shortarray);
        MP_JNI_LOCAL_FREEP(&shortarray);
    } else if (AudioTrack.writeBufferV21) {
        MP_VERBOSE(ao, "Using NIO ByteBuffer\n");
        jobject bbuf = (*env)->NewDirectByteBuffer(env, p->chunk, p->chunksize);
        p->bbuf = (*env)->NewGlobalRef(env, bbuf);
        MP_JNI_LOCAL_FREEP(&bbuf);
    } else if (p->format == AudioFormat.ENCODING_PCM_FLOAT) {
        jfloatArray floatarray = (*env)->NewFloatArray(env, p->chunksize / sizeof(float));
        p->floatarray = (*env)->NewGlobalRef(env, floatarray);
        MP_JNI_LOCAL_FREEP(&floatarray);
    } else {
        jbyteArray bytearray = (*env)->NewByteArray(env, p->chunksize);
        p->bytearray = (*env)->NewGlobalRef(env, bytearray);
        MP_JNI_LOCAL_FREEP(&bytearray);
    }

    /* create AudioTrack object */
    if (AudioTrack_New(ao) != 0) {
        MP_FATAL(ao, "Failed to create AudioTrack\n");
        goto error;
    }

    if (mp_thread_create(&p->thread, ao_thread, ao)) {
        MP_ERR(ao, "pthread creation failed\n");
        goto error;
    }
    p->thread_created = true;

    return 1;

error:
    uninit(ao);
    return -1;
}

static void stop(struct ao *ao)
{
    struct priv *p = ao->priv;
    if (!p->audiotrack) {
        MP_ERR(ao, "AudioTrack does not exist to stop!\n");
        return;
    }

    JNIEnv *env = MP_JNI_GET_ENV(ao);
    // The ao thread holds the lock while it writes. pause() makes a blocked
    // write() return (and later ones not block), so take the lock after it;
    // the frames of that last write() are then counted before the reset
    // below, not after it.
    MP_JNI_CALL_VOID(p->audiotrack, AudioTrack.pause);
    MP_JNI_EXCEPTION_LOG(ao);

    mp_mutex_lock(&p->lock);
    MP_JNI_CALL_VOID(p->audiotrack, AudioTrack.flush);
    MP_JNI_EXCEPTION_LOG(ao);

    p->playhead_offset = 0;
    p->reset_pending = true;
    p->written_frames = 0;
    p->timestamp_fetched = 0;
    p->timestamp_set = false;
    p->last_delay = 0;
    mp_mutex_unlock(&p->lock);
}

static void start(struct ao *ao)
{
    struct priv *p = ao->priv;
    if (!p->audiotrack) {
        MP_ERR(ao, "AudioTrack does not exist to start!\n");
        return;
    }

    JNIEnv *env = MP_JNI_GET_ENV(ao);
    // The track is paused or new here, so the ao thread is (about to be)
    // waiting and the lock is free soon. Take it before play(): once playing,
    // the ao thread holds it for as long as it has data to write. Signal
    // under it, or the wakeup is lost if the ao thread has just seen the
    // track paused and not yet started waiting.
    mp_mutex_lock(&p->lock);
    MP_JNI_CALL_VOID(p->audiotrack, AudioTrack.play);
    MP_JNI_EXCEPTION_LOG(ao);
    mp_cond_signal(&p->wakeup);
    mp_mutex_unlock(&p->lock);
}

#define OPT_BASE_STRUCT struct priv

const struct ao_driver audio_out_audiotrack = {
    .description = "Android AudioTrack audio output",
    .name      = "audiotrack",
    .init      = init,
    .uninit    = uninit,
    .reset     = stop,
    .start     = start,
    .priv_size = sizeof(struct priv),
    .priv_defaults = &(const OPT_BASE_STRUCT) {
        .cfg_pcm_float = 1,
    },
    .options   = (const struct m_option[]) {
        {"pcm-float", OPT_BOOL(cfg_pcm_float)},
        {"session-id", OPT_INT(cfg_session_id)},
        {0}
    },
    .options_prefix = "audiotrack",
};
