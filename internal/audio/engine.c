/*
 * finalmic — CoreAudio mixing engine.
 *
 * Signal path
 *   input device (N ch, any rate) --AUHAL--> client 48 kHz float
 *     -> lock-free ring -> fractional reader (clock drift corrected)
 *     -> per-source gain / high-pass -> stereo sum -> soft clip
 *     -> output device (loopback virtual mic) + optional WAV recorder
 *
 * All realtime code is C. Go never runs on the audio threads.
 */
#include "engine.h"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FM_MAX_FRAMES       4096
#define FM_RING_SECONDS     2.0
#define FM_TARGET_MS_DEF    30.0
#define FM_DEFAULT_RATE     48000.0
#define FM_HPF_HZ           18.0
#define FM_SOFTCLIP_KNEE    0.85f   /* transparent below ~ -1.4 dBFS */
#define FM_DRIFT_KP         0.010
#define FM_DRIFT_SLEW       0.05
#define FM_DRIFT_CLAMP      0.010    /* +/- 1000 ppm */

/* ------------------------------------------------------------------ utils */

static void cfstr(CFStringRef s, char *out, size_t n) {
    out[0] = 0;
    if (s) CFStringGetCString(s, out, (CFIndex)n, kCFStringEncodingUTF8);
}

static int ascii_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ci_equal(const char *a, const char *b) {
    while (*a && *b) {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static void set_err(char *err, size_t len, const char *fmt, ...) {
    if (!err || !len) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, len, fmt, ap);
    va_end(ap);
}

static int get_prop_u32(AudioObjectID obj, AudioObjectPropertySelector sel,
                        AudioObjectPropertyScope scope, UInt32 *v) {
    AudioObjectPropertyAddress a = {sel, scope, kAudioObjectPropertyElementMain};
    UInt32 sz = sizeof(*v);
    return AudioObjectGetPropertyData(obj, &a, 0, NULL, &sz, v);
}

static int get_channels(AudioDeviceID dev, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a = {
        kAudioDevicePropertyStreamConfiguration, scope, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &size) != noErr || size == 0) return 0;
    AudioBufferList *bl = (AudioBufferList *)malloc(size);
    if (!bl) return 0;
    int ch = 0;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, bl) == noErr) {
        for (UInt32 i = 0; i < bl->mNumberBuffers; i++)
            ch += (int)bl->mBuffers[i].mNumberChannels;
    }
    free(bl);
    return ch;
}

static double get_rate(AudioDeviceID dev) {
    Float64 rate = 0;
    AudioObjectPropertyAddress a = {
        kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};
    UInt32 sz = sizeof(rate);
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &rate) != noErr) return 0;
    return rate;
}

static void get_name(AudioDeviceID dev, char *out, size_t n) {
    CFStringRef name = NULL;
    AudioObjectPropertyAddress a = {
        kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};
    UInt32 sz = sizeof(name);
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &name) == noErr && name) {
        cfstr(name, out, n);
        CFRelease(name);
    } else {
        snprintf(out, n, "device %u", (unsigned)dev);
    }
}

static void get_uid(AudioDeviceID dev, char *out, size_t n) {
    CFStringRef uid = NULL;
    AudioObjectPropertyAddress a = {
        kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};
    UInt32 sz = sizeof(uid);
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &uid) == noErr && uid) {
        cfstr(uid, out, n);
        CFRelease(uid);
    } else {
        out[0] = 0;
    }
}

static AudioDeviceID default_device(int input) {
    AudioObjectPropertySelector sel = input ? kAudioHardwarePropertyDefaultInputDevice
                                            : kAudioHardwarePropertyDefaultOutputDevice;
    AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    AudioDeviceID dev = 0;
    UInt32 sz = sizeof(dev);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &dev) != noErr)
        return 0;
    return dev;
}

/* ------------------------------------------------------------------ devices */

int fm_list_devices(fm_device *out, int max) {
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size) != noErr)
        return -1;
    int count = (int)(size / sizeof(AudioDeviceID));
    if (count <= 0) return 0;
    AudioDeviceID *ids = (AudioDeviceID *)malloc(size);
    if (!ids) return -1;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids) != noErr) {
        free(ids);
        return -1;
    }
    AudioDeviceID din = default_device(1), dout = default_device(0);
    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        fm_device d;
        memset(&d, 0, sizeof(d));
        d.index = n;
        d.id = ids[i];
        get_name(ids[i], d.name, sizeof(d.name));
        get_uid(ids[i], d.uid, sizeof(d.uid));
        d.in_channels = get_channels(ids[i], kAudioDevicePropertyScopeInput);
        d.out_channels = get_channels(ids[i], kAudioDevicePropertyScopeOutput);
        d.sample_rate = get_rate(ids[i]);
        get_prop_u32(ids[i], kAudioDevicePropertyTransportType,
                     kAudioObjectPropertyScopeGlobal, &d.transport);
        d.is_default_input = (ids[i] == din);
        d.is_default_output = (ids[i] == dout);
        out[n++] = d;
    }
    free(ids);
    return n;
}

uint32_t fm_device_by_uid(const char *uid) {
    fm_device list[64];
    int n = fm_list_devices(list, 64);
    for (int i = 0; i < n; i++)
        if (uid && uid[0] && ci_equal(list[i].uid, uid)) return list[i].id;
    return 0;
}

int fm_device_alive(uint32_t id) {
    if (!id) return 0;
    UInt32 alive = 0;
    if (get_prop_u32((AudioDeviceID)id, kAudioDevicePropertyDeviceIsAlive,
                     kAudioObjectPropertyScopeGlobal, &alive) != noErr)
        return 0;
    return alive ? 1 : 0;
}

/* --------------------------------------------------------------- dsp bits */

static inline float softclip(float x) {
    float a = fabsf(x);
    if (a <= FM_SOFTCLIP_KNEE) return x;
    float over = (a - FM_SOFTCLIP_KNEE) / (1.0f - FM_SOFTCLIP_KNEE);
    float y = FM_SOFTCLIP_KNEE + (1.0f - FM_SOFTCLIP_KNEE) * tanhf(over);
    return x < 0 ? -y : y;
}

static inline float ring_at(const float *ring, uint32_t mask, uint64_t frame, int ch) {
    return ring[((uint32_t)frame & mask) * 2 + ch];
}

/* 4-point Catmull-Rom interpolation; ratio stays within ~+/-1000 ppm of unity
 * so this is effectively transparent. */
static inline void read_stereo(const float *ring, uint32_t mask, double pos,
                               float *l, float *r) {
    uint64_t i1 = (uint64_t)pos;
    double t = pos - (double)i1;
    uint64_t i0 = i1 > 0 ? i1 - 1 : 0;
    double t2 = t * t, t3 = t2 * t;
    double a = -0.5 * t3 + t2 - 0.5 * t;
    double b = 1.5 * t3 - 2.5 * t2 + 1.0;
    double c = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
    double d = 0.5 * t3 - 0.5 * t2;
    *l = (float)(a * ring_at(ring, mask, i0, 0) + b * ring_at(ring, mask, i1, 0) +
                 c * ring_at(ring, mask, i1 + 1, 0) + d * ring_at(ring, mask, i1 + 2, 0));
    *r = (float)(a * ring_at(ring, mask, i0, 1) + b * ring_at(ring, mask, i1, 1) +
                 c * ring_at(ring, mask, i1 + 1, 1) + d * ring_at(ring, mask, i1 + 2, 1));
}

static inline float onepole_hp(float x, double *z1, double *z2, double a) {
    double y = a * (*z2 + (double)x - *z1);
    *z1 = x;
    *z2 = y;
    return (float)y;
}

static void atomic_max(_Atomic double *p, double v) {
    double cur = atomic_load_explicit(p, memory_order_relaxed);
    while (v > cur) {
        if (atomic_compare_exchange_weak_explicit(p, &cur, v, memory_order_relaxed,
                                                  memory_order_relaxed))
            break;
    }
}

/* ---------------------------------------------------------------- sources */

typedef struct fm_engine fm_engine;

typedef struct {
    fm_engine *engine;
    AudioUnit  au;
    AudioDeviceID dev;
    int        channels;
    int        running;
    char       name[FM_NAME_MAX];
    char       uid[FM_UID_MAX];

    float     *ring;
    uint32_t   cap_frames;
    uint32_t   mask;
    _Atomic uint64_t w;          /* frames written (absolute) */
    _Atomic double   rp;         /* fractional read position (absolute) */

    double     ratio;
    double     gain_db;
    _Atomic double gain_tgt;
    double     gain_cur;
    int        highpass;
    double     hp_a;
    double     hp_x[2], hp_y[2];

    int        primed;
    double     target_fill;
    double     fill_ms;
    double     drift_ppm;
    uint64_t   xruns;
    uint64_t   drops;
    uint64_t   render_errs;
    _Atomic double peak_acc;
    _Atomic double ss_acc;
    _Atomic long long samples_acc;

    float     *scratch[2];
    AudioBufferList *abl;
} fm_source;

typedef struct {
    FILE      *f;
    pthread_t  thread;
    int        thread_started;
    int        stop;
    float     *ring;
    uint32_t   cap;
    uint32_t   mask;
    _Atomic uint64_t w;
    _Atomic uint64_t r;
    long long  written;
    char       path[512];
    pthread_mutex_t mu;
} fm_recorder;

struct fm_engine {
    fm_source src[FM_MAX_SOURCES];
    int       nsrc;

    AudioUnit     out_au;
    AudioDeviceID out_dev;
    char          out_name[FM_NAME_MAX];
    int           out_channels;
    int           running;

    double   rate;
    int      buffer_frames;
    double   target_ms;
    uint32_t target_fill;
    double   master_gain;

    _Atomic double out_peak;
    _Atomic double out_ss;
    _Atomic long long out_samples;

    fm_recorder rec;
    int         recording;
};

/* ------------------------------------------------------------- input side */

static OSStatus input_callback(void *ref, AudioUnitRenderActionFlags *flags,
                               const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes,
                               AudioBufferList *io_data) {
    (void)io_data;
    fm_source *s = (fm_source *)ref;
    if (!s || !s->running) return noErr;
    if (nframes > FM_MAX_FRAMES) nframes = FM_MAX_FRAMES;

    for (int c = 0; c < s->channels; c++)
        s->abl->mBuffers[c].mDataByteSize = nframes * (UInt32)sizeof(float);

    OSStatus st = AudioUnitRender(s->au, flags, ts, bus, nframes, s->abl);
    if (st != noErr) {
        s->render_errs++;
        return noErr;
    }

    uint64_t w = atomic_load_explicit(&s->w, memory_order_relaxed);
    double rp = atomic_load_explicit(&s->rp, memory_order_relaxed);
    if ((double)w - rp > (double)(s->cap_frames - nframes)) {
        s->drops++;
        return noErr;
    }

    const float *b0 = (const float *)s->abl->mBuffers[0].mData;
    const float *b1 = s->channels > 1 ? (const float *)s->abl->mBuffers[1].mData : b0;
    float *ring = s->ring;
    uint32_t mask = s->mask;
    for (UInt32 i = 0; i < nframes; i++) {
        uint32_t idx = (uint32_t)((w + i) & mask);
        ring[idx * 2 + 0] = b0[i];
        ring[idx * 2 + 1] = b1[i];
    }
    atomic_store_explicit(&s->w, w + nframes, memory_order_release);
    return noErr;
}

/* ------------------------------------------------------------ output side */

static void mix_source(fm_engine *e, fm_source *s, float *out, int n) {
    uint64_t w = atomic_load_explicit(&s->w, memory_order_acquire);
    double rp = atomic_load_explicit(&s->rp, memory_order_relaxed);
    double fill = (double)w - rp;

    /* Smooth gain changes so live tweaks never click. */
    s->gain_cur += (atomic_load_explicit(&s->gain_tgt, memory_order_relaxed) - s->gain_cur) * 0.08;

    if (!s->primed) {
        double prefill = s->target_fill;
        if (fill >= prefill) {
            rp = (double)w - prefill;
            atomic_store_explicit(&s->rp, rp, memory_order_relaxed);
            s->primed = 1;
        } else {
            s->fill_ms = fill * 1000.0 / e->rate;
            atomic_store_explicit(&s->peak_acc, 0.0, memory_order_relaxed);
            atomic_store_explicit(&s->ss_acc, 0.0, memory_order_relaxed);
            atomic_store_explicit(&s->samples_acc, 0, memory_order_relaxed);
            return;
        }
    }

    if (fill < (double)n * 1.02 + 16.0) {
        /* Underrun: the reader caught the writer. Drop the partial buffer and
         * re-prime; this costs ~15 ms of silence instead of a stutter loop. */
        s->xruns++;
        s->primed = 0;
        s->ratio = 1.0;
        atomic_store_explicit(&s->rp, (double)w, memory_order_relaxed);
        atomic_store_explicit(&s->peak_acc, 0.0, memory_order_relaxed);
        atomic_store_explicit(&s->ss_acc, 0.0, memory_order_relaxed);
        atomic_store_explicit(&s->samples_acc, 0, memory_order_relaxed);
        return;
    }

    double step = s->ratio;
    double start = rp;
    float gain = (float)s->gain_cur;
    double hp_a = s->hp_a;
    int hp = s->highpass;
    double peak = 0.0, ss = 0.0;

    for (int i = 0; i < n; i++) {
        float l, r;
        read_stereo(s->ring, s->mask, start + (double)i * step, &l, &r);
        l *= gain;
        r *= gain;
        if (hp) {
            l = onepole_hp(l, &s->hp_x[0], &s->hp_y[0], hp_a);
            r = onepole_hp(r, &s->hp_x[1], &s->hp_y[1], hp_a);
        }
        out[i * 2 + 0] += l;
        out[i * 2 + 1] += r;
        double al = fabs((double)l);
        if (al > peak) peak = al;
        double ar = fabs((double)r);
        if (ar > peak) peak = ar;
        ss += (double)l * l + (double)r * r;
    }

    rp = start + (double)n * step;
    atomic_store_explicit(&s->rp, rp, memory_order_relaxed);

    /* Compensate for the frames the producer adds while this block plays, so
     * the estimate matches the fill at the next callback. */
    double newfill = (double)w - rp + (double)n;
    if (newfill > (double)s->cap_frames * 0.9) {
        /* Writer was faster than the reader for a long time; resync. */
        s->drops++;
        rp = (double)w - s->target_fill;
        atomic_store_explicit(&s->rp, rp, memory_order_relaxed);
        newfill = s->target_fill + (double)n;
    }

    /* Clock drift control: P-loop on buffer occupancy, slewed per block. */
    double err = (newfill - s->target_fill) / (s->target_fill > 0 ? s->target_fill : 1.0);
    double corr = FM_DRIFT_KP * err;
    if (corr > FM_DRIFT_CLAMP) corr = FM_DRIFT_CLAMP;
    if (corr < -FM_DRIFT_CLAMP) corr = -FM_DRIFT_CLAMP;
    double target_ratio = 1.0 + corr;
    s->ratio += (target_ratio - s->ratio) * FM_DRIFT_SLEW;

    s->fill_ms = newfill * 1000.0 / e->rate;
    s->drift_ppm = (s->ratio - 1.0) * 1e6;

    atomic_max(&s->peak_acc, peak);
    atomic_fetch_add_explicit(&s->ss_acc, ss, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->samples_acc, (long long)n * 2, memory_order_relaxed);
}

static void recorder_push(fm_recorder *r, const float *data, int frames) {
    if (!r->f) return;
    uint64_t w = atomic_load_explicit(&r->w, memory_order_relaxed);
    uint64_t rd = atomic_load_explicit(&r->r, memory_order_relaxed);
    if ((double)w - (double)rd > (double)(r->cap - (uint32_t)frames)) return; /* drop */
    for (int i = 0; i < frames; i++) {
        uint32_t idx = (uint32_t)((w + (uint64_t)i) & r->mask);
        r->ring[idx * 2 + 0] = data[i * 2 + 0];
        r->ring[idx * 2 + 1] = data[i * 2 + 1];
    }
    atomic_store_explicit(&r->w, w + (uint64_t)frames, memory_order_release);
}

static OSStatus output_callback(void *ref, AudioUnitRenderActionFlags *flags,
                                const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes,
                                AudioBufferList *io) {
    (void)flags; (void)ts; (void)bus;
    fm_engine *e = (fm_engine *)ref;
    if (!e || !e->running || io->mNumberBuffers == 0) return noErr;
    if (nframes > FM_MAX_FRAMES) nframes = FM_MAX_FRAMES;

    float *out = (float *)io->mBuffers[0].mData;
    if (!out) return noErr;
    memset(out, 0, (size_t)nframes * 2 * sizeof(float));

    for (int i = 0; i < e->nsrc; i++) mix_source(e, &e->src[i], out, (int)nframes);

    double peak = 0.0, ss = 0.0;
    for (UInt32 i = 0; i < nframes * 2; i++) {
        float v = softclip(out[i]);
        out[i] = v;
        double a = fabs((double)v);
        if (a > peak) peak = a;
        ss += (double)v * v;
    }
    atomic_max(&e->out_peak, peak);
    atomic_fetch_add_explicit(&e->out_ss, ss, memory_order_relaxed);
    atomic_fetch_add_explicit(&e->out_samples, (long long)nframes * 2, memory_order_relaxed);

    if (e->recording) recorder_push(&e->rec, out, (int)nframes);
    return noErr;
}

/* --------------------------------------------------------------- recorder */

static void wav_header(unsigned char *h, uint32_t data_bytes, uint32_t rate,
                       uint16_t channels) {
    uint32_t byte_rate = rate * channels * 4;
    memcpy(h + 0, "RIFF", 4);
    uint32_t riff = 36 + data_bytes;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    uint32_t fmt = 16;
    memcpy(h + 16, &fmt, 4);
    uint16_t format = 3; /* IEEE float */
    memcpy(h + 20, &format, 2);
    memcpy(h + 22, &channels, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    uint16_t block_align = (uint16_t)(channels * 4);
    memcpy(h + 32, &block_align, 2);
    uint16_t bits = 32;
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
}

static void *recorder_thread(void *arg) {
    fm_recorder *r = (fm_recorder *)arg;
    float buf[2048 * 2];
    for (;;) {
        int stop = __atomic_load_n(&r->stop, __ATOMIC_ACQUIRE);
        uint64_t w = atomic_load_explicit(&r->w, memory_order_acquire);
        uint64_t rd = atomic_load_explicit(&r->r, memory_order_relaxed);
        uint64_t avail = w - rd;
        if (avail >= 1024) {
            int n = (int)(avail > 2048 ? 2048 : avail);
            for (int i = 0; i < n; i++) {
                uint32_t idx = (uint32_t)((rd + (uint64_t)i) & r->mask);
                buf[i * 2 + 0] = r->ring[idx * 2 + 0];
                buf[i * 2 + 1] = r->ring[idx * 2 + 1];
            }
            pthread_mutex_lock(&r->mu);
            if (r->f) fwrite(buf, sizeof(float) * 2, (size_t)n, r->f);
            r->written += n;
            pthread_mutex_unlock(&r->mu);
            atomic_store_explicit(&r->r, rd + (uint64_t)n, memory_order_release);
        } else if (stop) {
            break;
        } else {
            struct timespec ts = {0, 2 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
    }
    /* final drain */
    uint64_t w = atomic_load_explicit(&r->w, memory_order_acquire);
    uint64_t rd = atomic_load_explicit(&r->r, memory_order_relaxed);
    while (w > rd) {
        int n = (int)((w - rd) > 2048 ? 2048 : (w - rd));
        for (int i = 0; i < n; i++) {
            uint32_t idx = (uint32_t)((rd + (uint64_t)i) & r->mask);
            buf[i * 2 + 0] = r->ring[idx * 2 + 0];
            buf[i * 2 + 1] = r->ring[idx * 2 + 1];
        }
        pthread_mutex_lock(&r->mu);
        if (r->f) fwrite(buf, sizeof(float) * 2, (size_t)n, r->f);
        r->written += n;
        pthread_mutex_unlock(&r->mu);
        rd += (uint64_t)n;
    }
    atomic_store_explicit(&r->r, rd, memory_order_release);

    pthread_mutex_lock(&r->mu);
    if (r->f) {
        unsigned char h[44];
        uint32_t data_bytes = (uint32_t)(r->written * 8);
        wav_header(h, data_bytes, 48000, 2);
        fseek(r->f, 0, SEEK_SET);
        fwrite(h, 1, sizeof(h), r->f);
        fclose(r->f);
        r->f = NULL;
    }
    pthread_mutex_unlock(&r->mu);
    return NULL;
}

/* ------------------------------------------------------------------ setup */

static int make_input(fm_source *s, fm_engine *e, char *err, size_t errlen) {
    AudioComponentDescription desc = {
        kAudioUnitType_Output, kAudioUnitSubType_HALOutput,
        kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) { set_err(err, errlen, "HAL output audio unit not available"); return -1; }

    OSStatus st = AudioComponentInstanceNew(comp, &s->au);
    if (st != noErr) {
        set_err(err, errlen, "AudioComponentInstanceNew(input) failed: %d", (int)st);
        return -1;
    }

    UInt32 one = 1, zero = 0;
    st = AudioUnitSetProperty(s->au, kAudioOutputUnitProperty_EnableIO,
                              kAudioUnitScope_Input, 1, &one, sizeof(one));
    if (st != noErr) {
        set_err(err, errlen, "enable input IO failed: %d", (int)st);
        return -1;
    }
    st = AudioUnitSetProperty(s->au, kAudioOutputUnitProperty_EnableIO,
                              kAudioUnitScope_Output, 0, &zero, sizeof(zero));
    if (st != noErr) {
        set_err(err, errlen, "disable input-side output IO failed: %d", (int)st);
        return -1;
    }
    st = AudioUnitSetProperty(s->au, kAudioOutputUnitProperty_CurrentDevice,
                              kAudioUnitScope_Global, 0, &s->dev, sizeof(s->dev));
    if (st != noErr) {
        set_err(err, errlen, "could not select input device %s: %d", s->name, (int)st);
        return -1;
    }

    s->channels = get_channels(s->dev, kAudioDevicePropertyScopeInput);
    if (s->channels <= 0) {
        set_err(err, errlen, "device %s has no input channels", s->name);
        return -1;
    }
    if (s->channels > 2) s->channels = 2;

    AudioStreamBasicDescription fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.mSampleRate = e->rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    fmt.mFramesPerPacket = 1;
    fmt.mChannelsPerFrame = (UInt32)s->channels;
    fmt.mBitsPerChannel = 32;
    fmt.mBytesPerFrame = 4;
    fmt.mBytesPerPacket = 4;

    st = AudioUnitSetProperty(s->au, kAudioUnitProperty_StreamFormat,
                              kAudioUnitScope_Output, 1, &fmt, sizeof(fmt));
    if (st != noErr) {
        set_err(err, errlen, "could not set 48k float format on %s: %d", s->name, (int)st);
        return -1;
    }

    UInt32 max_slice = FM_MAX_FRAMES;
    AudioUnitSetProperty(s->au, kAudioUnitProperty_MaximumFramesPerSlice,
                         kAudioUnitScope_Global, 0, &max_slice, sizeof(max_slice));

    AURenderCallbackStruct cb;
    cb.inputProc = input_callback;
    cb.inputProcRefCon = s;
    st = AudioUnitSetProperty(s->au, kAudioOutputUnitProperty_SetInputCallback,
                              kAudioUnitScope_Global, 0, &cb, sizeof(cb));
    if (st != noErr) {
        set_err(err, errlen, "could not install input callback on %s: %d", s->name, (int)st);
        return -1;
    }

    s->scratch[0] = (float *)calloc(FM_MAX_FRAMES, sizeof(float));
    s->scratch[1] = (float *)calloc(FM_MAX_FRAMES, sizeof(float));
    s->abl = (AudioBufferList *)calloc(
        1, sizeof(AudioBufferList) + sizeof(AudioBuffer) * (size_t)(s->channels - 1));
    if (!s->scratch[0] || !s->scratch[1] || !s->abl) {
        set_err(err, errlen, "out of memory");
        return -1;
    }
    s->abl->mNumberBuffers = (UInt32)s->channels;
    for (int c = 0; c < s->channels; c++) {
        s->abl->mBuffers[c].mNumberChannels = 1;
        s->abl->mBuffers[c].mData = s->scratch[c];
        s->abl->mBuffers[c].mDataByteSize = FM_MAX_FRAMES * sizeof(float);
    }

    st = AudioUnitInitialize(s->au);
    if (st != noErr) {
        set_err(err, errlen, "could not initialize input %s: %d", s->name, (int)st);
        return -1;
    }
    return 0;
}

static int make_output(fm_engine *e, char *err, size_t errlen) {
    AudioComponentDescription desc = {
        kAudioUnitType_Output, kAudioUnitSubType_HALOutput,
        kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) { set_err(err, errlen, "HAL output audio unit not available"); return -1; }

    OSStatus st = AudioComponentInstanceNew(comp, &e->out_au);
    if (st != noErr) {
        set_err(err, errlen, "AudioComponentInstanceNew(output) failed: %d", (int)st);
        return -1;
    }
    UInt32 one = 1, zero = 0;
    st = AudioUnitSetProperty(e->out_au, kAudioOutputUnitProperty_EnableIO,
                              kAudioUnitScope_Output, 0, &one, sizeof(one));
    if (st != noErr) {
        set_err(err, errlen, "enable output IO failed: %d", (int)st);
        return -1;
    }
    st = AudioUnitSetProperty(e->out_au, kAudioOutputUnitProperty_EnableIO,
                              kAudioUnitScope_Input, 1, &zero, sizeof(zero));
    if (st != noErr) {
        set_err(err, errlen, "disable output-side input IO failed: %d", (int)st);
        return -1;
    }
    st = AudioUnitSetProperty(e->out_au, kAudioOutputUnitProperty_CurrentDevice,
                              kAudioUnitScope_Global, 0, &e->out_dev, sizeof(e->out_dev));
    if (st != noErr) {
        set_err(err, errlen, "could not select output device %s: %d", e->out_name, (int)st);
        return -1;
    }

    e->out_channels = get_channels(e->out_dev, kAudioDevicePropertyScopeOutput);
    if (e->out_channels <= 0) {
        set_err(err, errlen, "device %s has no output channels", e->out_name);
        return -1;
    }

    AudioStreamBasicDescription fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.mSampleRate = e->rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagsNativeFloatPacked; /* interleaved */
    fmt.mFramesPerPacket = 1;
    fmt.mChannelsPerFrame = 2;
    fmt.mBitsPerChannel = 32;
    fmt.mBytesPerFrame = 8;
    fmt.mBytesPerPacket = 8;
    st = AudioUnitSetProperty(e->out_au, kAudioUnitProperty_StreamFormat,
                              kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));
    if (st != noErr) {
        set_err(err, errlen, "could not set 48k stereo format on %s: %d", e->out_name, (int)st);
        return -1;
    }

    UInt32 max_slice = FM_MAX_FRAMES;
    AudioUnitSetProperty(e->out_au, kAudioUnitProperty_MaximumFramesPerSlice,
                         kAudioUnitScope_Global, 0, &max_slice, sizeof(max_slice));

    AURenderCallbackStruct cb;
    cb.inputProc = output_callback;
    cb.inputProcRefCon = e;
    st = AudioUnitSetProperty(e->out_au, kAudioUnitProperty_SetRenderCallback,
                              kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    if (st != noErr) {
        set_err(err, errlen, "could not install render callback: %d", (int)st);
        return -1;
    }

    st = AudioUnitInitialize(e->out_au);
    if (st != noErr) {
        set_err(err, errlen, "could not initialize output %s: %d", e->out_name, (int)st);
        return -1;
    }
    return 0;
}

double fm_engine_sample_rate(fm_engine *e) { return e ? e->rate : 0; }

static uint32_t next_pow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

fm_engine *fm_engine_create(const fm_config *cfg, char *err, size_t errlen) {
    if (!cfg || cfg->num_sources <= 0) {
        set_err(err, errlen, "no input sources configured");
        return NULL;
    }
    if (cfg->num_sources > FM_MAX_SOURCES) {
        set_err(err, errlen, "too many sources (max %d)", FM_MAX_SOURCES);
        return NULL;
    }
    fm_engine *e = (fm_engine *)calloc(1, sizeof(fm_engine));
    if (!e) { set_err(err, errlen, "out of memory"); return NULL; }

    e->rate = cfg->sample_rate > 0 ? cfg->sample_rate : FM_DEFAULT_RATE;
    e->buffer_frames = cfg->buffer_frames > 0 ? cfg->buffer_frames : 512;
    e->target_ms = cfg->target_ms > 0 ? cfg->target_ms : FM_TARGET_MS_DEF;
    e->target_fill = (uint32_t)(e->rate * e->target_ms / 1000.0);
    e->master_gain = 1.0;
    atomic_init(&e->out_peak, 0.0);
    atomic_init(&e->out_ss, 0.0);
    atomic_init(&e->out_samples, 0);

    e->out_dev = fm_device_by_uid(cfg->output_uid);
    if (!e->out_dev) {
        set_err(err, errlen, "output device not found: %s", cfg->output_uid);
        free(e);
        return NULL;
    }
    get_name(e->out_dev, e->out_name, sizeof(e->out_name));
    if (get_channels(e->out_dev, kAudioDevicePropertyScopeOutput) <= 0) {
        set_err(err, errlen,
                "%s has no output channels; pick a virtual loopback device "
                "(BlackHole / WeMeet / Loopback)", e->out_name);
        free(e);
        return NULL;
    }

    uint32_t cap = next_pow2((uint32_t)(e->rate * FM_RING_SECONDS));
    e->nsrc = cfg->num_sources;
    for (int i = 0; i < e->nsrc; i++) {
        fm_source *s = &e->src[i];
        s->engine = e;
        s->dev = fm_device_by_uid(cfg->sources[i].uid);
        if (!s->dev) {
            set_err(err, errlen, "input device not found: %s", cfg->sources[i].uid);
            free(e);
            return NULL;
        }
        get_name(s->dev, s->name, sizeof(s->name));
        snprintf(s->uid, sizeof(s->uid), "%s", cfg->sources[i].uid);
        s->channels = get_channels(s->dev, kAudioDevicePropertyScopeInput);
        if (s->channels <= 0) {
            set_err(err, errlen, "%s has no input channels", s->name);
            free(e);
            return NULL;
        }
        if (s->channels > 2) s->channels = 2;

        s->cap_frames = cap;
        s->mask = cap - 1;
        s->ring = (float *)calloc((size_t)cap * 2, sizeof(float));
        if (!s->ring) { set_err(err, errlen, "out of memory"); free(e); return NULL; }

        atomic_init(&s->w, 0);
        atomic_init(&s->rp, 0.0);
        atomic_init(&s->peak_acc, 0.0);
        atomic_init(&s->ss_acc, 0.0);
        atomic_init(&s->samples_acc, 0);
        s->ratio = 1.0;
        s->gain_db = cfg->sources[i].gain_db;
        s->gain_cur = pow(10.0, s->gain_db / 20.0);
        atomic_init(&s->gain_tgt, s->gain_cur);
        s->highpass = cfg->sources[i].highpass;
        s->hp_a = exp(-2.0 * M_PI * FM_HPF_HZ / e->rate);
        s->target_fill = (double)e->target_fill;
        s->primed = 0;

        if (make_input(s, e, err, errlen) != 0) {
            free(e);
            return NULL;
        }
    }

    if (make_output(e, err, errlen) != 0) {
        free(e);
        return NULL;
    }

    pthread_mutex_init(&e->rec.mu, NULL);
    e->rec.cap = next_pow2((uint32_t)(e->rate * 8));
    e->rec.mask = e->rec.cap - 1;
    e->rec.ring = (float *)calloc((size_t)e->rec.cap * 2, sizeof(float));
    atomic_init(&e->rec.w, 0);
    atomic_init(&e->rec.r, 0);
    return e;
}

int fm_engine_start(fm_engine *e, char *err, size_t errlen) {
    if (!e) return -1;
    e->running = 1;
    for (int i = 0; i < e->nsrc; i++) {
        e->src[i].running = 1;
        OSStatus st = AudioOutputUnitStart(e->src[i].au);
        if (st != noErr) {
            set_err(err, errlen, "could not start capture on %s: %d", e->src[i].name, (int)st);
            e->running = 0;
            return -1;
        }
    }
    OSStatus st = AudioOutputUnitStart(e->out_au);
    if (st != noErr) {
        set_err(err, errlen, "could not start output on %s: %d", e->out_name, (int)st);
        e->running = 0;
        return -1;
    }
    return 0;
}

void fm_engine_stop(fm_engine *e) {
    if (!e) return;
    e->running = 0;
    if (e->recording) fm_engine_record_stop(e);
    if (e->out_au) AudioOutputUnitStop(e->out_au);
    for (int i = 0; i < e->nsrc; i++) {
        e->src[i].running = 0;
        if (e->src[i].au) AudioOutputUnitStop(e->src[i].au);
    }
}

void fm_engine_destroy(fm_engine *e) {
    if (!e) return;
    fm_engine_stop(e);
    if (e->out_au) {
        AudioUnitUninitialize(e->out_au);
        AudioComponentInstanceDispose(e->out_au);
    }
    for (int i = 0; i < e->nsrc; i++) {
        fm_source *s = &e->src[i];
        if (s->au) {
            AudioUnitUninitialize(s->au);
            AudioComponentInstanceDispose(s->au);
        }
        free(s->ring);
        free(s->scratch[0]);
        free(s->scratch[1]);
        free(s->abl);
    }
    free(e->rec.ring);
    pthread_mutex_destroy(&e->rec.mu);
    free(e);
}

void fm_engine_set_gain_db(fm_engine *e, int index, double db) {
    if (!e || index < 0 || index >= e->nsrc) return;
    e->src[index].gain_db = db;
    atomic_store_explicit(&e->src[index].gain_tgt, pow(10.0, db / 20.0), memory_order_relaxed);
}

static double to_db(double x) { return x > 1e-9 ? 20.0 * log10(x) : -120.0; }

void fm_engine_get_stats(fm_engine *e, fm_stats *out) {
    if (!e || !out) return;
    memset(out, 0, sizeof(*out));
    out->running = e->running;
    for (int i = 0; i < e->nsrc; i++) {
        fm_source *s = &e->src[i];
        double peak = atomic_exchange_explicit(&s->peak_acc, 0.0, memory_order_relaxed);
        double ss = atomic_exchange_explicit(&s->ss_acc, 0.0, memory_order_relaxed);
        long long n = atomic_exchange_explicit(&s->samples_acc, 0, memory_order_relaxed);
        out->peak_db[i] = to_db(peak);
        out->rms_db[i] = n > 0 ? to_db(sqrt(ss / (double)n)) : -120.0;
        out->fill_ms[i] = s->fill_ms;
        out->drift_ppm[i] = s->drift_ppm;
        out->xruns[i] = (long long)s->xruns;
        out->drops[i] = (long long)(s->drops + s->render_errs);
        out->primed[i] = s->primed;
    }
    double op = atomic_exchange_explicit(&e->out_peak, 0.0, memory_order_relaxed);
    double oss = atomic_exchange_explicit(&e->out_ss, 0.0, memory_order_relaxed);
    long long on = atomic_exchange_explicit(&e->out_samples, 0, memory_order_relaxed);
    out->out_peak_db = to_db(op);
    out->out_rms_db = on > 0 ? to_db(sqrt(oss / (double)on)) : -120.0;
    out->frames = on / 2;
}

/* --------------------------------------------------------------- recorder */

int fm_engine_record_start(fm_engine *e, char *err, size_t errlen) {
    if (!e) return -1;
    if (e->recording) return 0;
    if (!e->rec.ring) { set_err(err, errlen, "recorder not allocated"); return -1; }
    pthread_mutex_lock(&e->rec.mu);
    e->rec.f = fopen(e->rec.path, "wb");
    if (!e->rec.f) {
        set_err(err, errlen, "cannot open %s for writing", e->rec.path);
        pthread_mutex_unlock(&e->rec.mu);
        return -1;
    }
    unsigned char h[44];
    wav_header(h, 0, (uint32_t)e->rate, 2);
    fwrite(h, 1, sizeof(h), e->rec.f);
    e->rec.written = 0;
    atomic_store(&e->rec.w, 0);
    atomic_store(&e->rec.r, 0);
    __atomic_store_n(&e->rec.stop, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&e->rec.mu);
    if (pthread_create(&e->rec.thread, NULL, recorder_thread, &e->rec) != 0) {
        set_err(err, errlen, "cannot start recorder thread");
        fclose(e->rec.f);
        e->rec.f = NULL;
        return -1;
    }
    e->rec.thread_started = 1;
    e->recording = 1;
    return 0;
}

void fm_engine_record_stop(fm_engine *e) {
    if (!e || !e->recording) return;
    e->recording = 0;
    if (e->rec.thread_started) {
        __atomic_store_n(&e->rec.stop, 1, __ATOMIC_RELEASE);
        pthread_join(e->rec.thread, NULL);
        e->rec.thread_started = 0;
    }
}

void fm_engine_set_record_path(fm_engine *e, const char *path) {
    if (!e) return;
    snprintf(e->rec.path, sizeof(e->rec.path), "%s", path ? path : "");
}

/* ------------------------------------------------------------------ probe */

typedef struct {
    AudioUnit au;
    AudioDeviceID dev;
    int channels;
    float *scratch[2];
    AudioBufferList *abl;
    double peak;
    double ss;
    long long n;
    int running;
    double rate;
} probe_ctx;

static OSStatus probe_callback(void *ref, AudioUnitRenderActionFlags *flags,
                               const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes,
                               AudioBufferList *io) {
    (void)io;
    probe_ctx *p = (probe_ctx *)ref;
    if (!p->running) return noErr;
    if (nframes > FM_MAX_FRAMES) nframes = FM_MAX_FRAMES;
    for (int c = 0; c < p->channels; c++)
        p->abl->mBuffers[c].mDataByteSize = nframes * sizeof(float);
    OSStatus st = AudioUnitRender(p->au, flags, ts, bus, nframes, p->abl);
    if (st != noErr) return noErr;
    for (UInt32 i = 0; i < nframes; i++) {
        double v = 0;
        for (int c = 0; c < p->channels; c++) v += ((float *)p->abl->mBuffers[c].mData)[i];
        v /= p->channels;
        p->ss += v * v;
        if (fabs(v) > p->peak) p->peak = fabs(v);
        p->n++;
    }
    return noErr;
}

int fm_probe_input(const char *uid, int ms, double *peak_db, double *rms_db,
                   long long *frames, char *err, size_t errlen) {
    probe_ctx p;
    memset(&p, 0, sizeof(p));
    p.dev = fm_device_by_uid(uid);
    if (!p.dev) { set_err(err, errlen, "input device not found: %s", uid); return -1; }
    p.rate = get_rate(p.dev);
    p.channels = get_channels(p.dev, kAudioDevicePropertyScopeInput);
    if (p.channels <= 0) { set_err(err, errlen, "device has no input channels"); return -1; }
    if (p.channels > 2) p.channels = 2;

    AudioComponentDescription desc = {
        kAudioUnitType_Output, kAudioUnitSubType_HALOutput,
        kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) { set_err(err, errlen, "HAL output audio unit not available"); return -1; }
    OSStatus st = AudioComponentInstanceNew(comp, &p.au);
    if (st != noErr) { set_err(err, errlen, "instance new failed: %d", (int)st); return -1; }
    UInt32 one = 1, zero = 0;
    AudioUnitSetProperty(p.au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1,
                         &one, sizeof(one));
    AudioUnitSetProperty(p.au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0,
                         &zero, sizeof(zero));
    st = AudioUnitSetProperty(p.au, kAudioOutputUnitProperty_CurrentDevice,
                              kAudioUnitScope_Global, 0, &p.dev, sizeof(p.dev));
    if (st != noErr) { set_err(err, errlen, "select device failed: %d", (int)st); return -1; }

    AudioStreamBasicDescription fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.mSampleRate = p.rate > 0 ? p.rate : FM_DEFAULT_RATE;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    fmt.mFramesPerPacket = 1;
    fmt.mChannelsPerFrame = (UInt32)p.channels;
    fmt.mBitsPerChannel = 32;
    fmt.mBytesPerFrame = 4;
    fmt.mBytesPerPacket = 4;
    st = AudioUnitSetProperty(p.au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1,
                              &fmt, sizeof(fmt));
    if (st != noErr) { set_err(err, errlen, "set format failed: %d", (int)st); return -1; }

    UInt32 max_slice = FM_MAX_FRAMES;
    AudioUnitSetProperty(p.au, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                         0, &max_slice, sizeof(max_slice));

    p.scratch[0] = (float *)calloc(FM_MAX_FRAMES, sizeof(float));
    p.scratch[1] = (float *)calloc(FM_MAX_FRAMES, sizeof(float));
    p.abl = (AudioBufferList *)calloc(
        1, sizeof(AudioBufferList) + sizeof(AudioBuffer) * (size_t)(p.channels - 1));
    p.abl->mNumberBuffers = (UInt32)p.channels;
    for (int c = 0; c < p.channels; c++) {
        p.abl->mBuffers[c].mNumberChannels = 1;
        p.abl->mBuffers[c].mData = p.scratch[c];
        p.abl->mBuffers[c].mDataByteSize = FM_MAX_FRAMES * sizeof(float);
    }

    AURenderCallbackStruct cb;
    cb.inputProc = probe_callback;
    cb.inputProcRefCon = &p;
    AudioUnitSetProperty(p.au, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global,
                         0, &cb, sizeof(cb));
    st = AudioUnitInitialize(p.au);
    if (st != noErr) { set_err(err, errlen, "initialize failed: %d", (int)st); return -1; }
    p.running = 1;
    st = AudioOutputUnitStart(p.au);
    if (st != noErr) { set_err(err, errlen, "start failed: %d", (int)st); return -1; }
    struct timespec req = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&req, NULL);
    p.running = 0;
    AudioOutputUnitStop(p.au);
    AudioUnitUninitialize(p.au);
    AudioComponentInstanceDispose(p.au);
    free(p.scratch[0]);
    free(p.scratch[1]);
    free(p.abl);

    if (peak_db) *peak_db = p.n > 0 ? to_db(p.peak) : -120.0;
    if (rms_db) *rms_db = p.n > 0 ? to_db(sqrt(p.ss / (double)p.n)) : -120.0;
    if (frames) *frames = p.n;
    return 0;
}
