/*
 * finalmic — CoreAudio mixing engine (C side).
 *
 * The engine opens N input devices (the microphone and the capture card),
 * resamples each stream with a clock-drift-correcting fractional resampler,
 * mixes them into one stereo 48 kHz float stream and renders that stream to
 * a loopback output device (BlackHole / WeMeet / any virtual audio device).
 *
 * The realtime work lives entirely in C so no Go code ever runs on an audio
 * thread: input callbacks push into lock-free rings, the output callback pulls
 * and mixes. The Go layer is control plane only.
 */
#ifndef FINALMIC_ENGINE_H
#define FINALMIC_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FM_MAX_SOURCES 4
#define FM_NAME_MAX 256
#define FM_UID_MAX 256

typedef struct {
    int      index;
    uint32_t id;
    char     name[FM_NAME_MAX];
    char     uid[FM_UID_MAX];
    int      in_channels;
    int      out_channels;
    double   sample_rate;
    uint32_t transport;      /* fourcc, big endian packed */
    int      is_default_input;
    int      is_default_output;
} fm_device;

/* Enumerates every audio device. Returns the device count, or a negative
 * CoreAudio error code. */
int fm_list_devices(fm_device *out, int max);

/* Resolves a device UID (exact) to an AudioDeviceID. Returns 0 when not found. */
uint32_t fm_device_by_uid(const char *uid);

/* 1 when the device currently exists and is alive. */
int fm_device_alive(uint32_t id);

typedef struct {
    char   uid[FM_UID_MAX];
    double gain_db;
    int    highpass;
} fm_source_config;

typedef struct {
    fm_source_config sources[FM_MAX_SOURCES];
    int      num_sources;
    char     output_uid[FM_UID_MAX];
    double   sample_rate;
    int      buffer_frames;
    double   target_ms;      /* jitter buffer target, default 30 */
} fm_config;

typedef struct {
    double   peak_db[FM_MAX_SOURCES];
    double   rms_db[FM_MAX_SOURCES];
    double   out_peak_db;
    double   out_rms_db;
    double   gain_db[FM_MAX_SOURCES];
    double   master_db;
    double   fill_ms[FM_MAX_SOURCES];
    double   drift_ppm[FM_MAX_SOURCES];
    long long xruns[FM_MAX_SOURCES];
    long long drops[FM_MAX_SOURCES];
    long long frames;
    int      running;
    int      primed[FM_MAX_SOURCES];
} fm_stats;

typedef struct fm_engine fm_engine;

fm_engine *fm_engine_create(const fm_config *cfg, char *err, size_t errlen);
int        fm_engine_start(fm_engine *e, char *err, size_t errlen);
void       fm_engine_stop(fm_engine *e);
void       fm_engine_destroy(fm_engine *e);
void       fm_engine_set_gain_db(fm_engine *e, int index, double db);
void       fm_engine_set_master_db(fm_engine *e, double db);
void       fm_engine_get_stats(fm_engine *e, fm_stats *out);
void       fm_engine_set_record_path(fm_engine *e, const char *path);
int        fm_engine_record_start(fm_engine *e, char *err, size_t errlen);
void       fm_engine_record_stop(fm_engine *e);
double     fm_engine_sample_rate(fm_engine *e);

/* Captures from one device for `ms` milliseconds and reports level. Used by
 * `finalmic doctor` to prove permission and signal. */
int fm_probe_input(const char *uid, int ms, double *peak_db, double *rms_db,
                   long long *frames, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* FINALMIC_ENGINE_H */
