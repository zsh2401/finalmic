/*
 * finalmic.driver — a minimal AudioServerPlugIn that provides one virtual
 * audio device called "finalmic".
 *
 * The device is a loopback: whatever an application plays into its output
 * streams is stored in a lock-free ring and handed back through its input
 * streams. finalmic (the mixer) plays the mixed microphone + capture card
 * audio into the output side; OBS / 抖音直播伴侣 captures the input side,
 * so the device behaves like a normal 2-in / 2-out sound card.
 *
 * Realtime notes: DoIOOperation() is called on the HAL's IO thread. It only
 * touches the preallocated ring and does no allocation, no locking and no
 * CoreFoundation work.
 */

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------- config */

enum {
    kObjectID_PlugIn       = kAudioObjectPlugInObject,
    kObjectID_Device       = 2,
    kObjectID_StreamInput  = 3,
    kObjectID_StreamOutput = 4
};

#define kRingFrames        16384u
#define kRingMask          (kRingFrames - 1u)
#define kMinBufferFrames   32u
#define kMaxBufferFrames   4096u
#define kDeviceUID         "com.zsh2401.finalmic.device"
#define kModelUID          "com.zsh2401.finalmic.model"
#define kManufacturer      "zsh2401"
#define kBundleID          "com.zsh2401.finalmic.driver"
#define kDeviceName        "finalmic"

/* ---------------------------------------------------------------- driver */

typedef struct FinalmicDriver {
    AudioServerPlugInDriverInterface *iface;  /* must stay first: the HAL treats
                                                 the object as a pointer to a
                                                 vtable pointer (COM style) */

    UInt32 refCount;
    AudioServerPlugInHostRef host;

    UInt32 running;
    UInt32 bufferFrames;
    Float64 sampleRate;

    UInt64 anchorHost;
    double anchorSample;
    UInt64 seed;
    double ticksPerFrame;

    _Atomic uint64_t writePos;
    _Atomic uint64_t readPos;
    _Atomic uint64_t underruns;
    _Atomic uint64_t overruns;

    float ring[kRingFrames * 2];
} FinalmicDriver;

static CFArrayRef g_empty_array;

static FinalmicDriver *driver_from_ref(void *ref) {
    return (FinalmicDriver *)ref;
}

/* ---------------------------------------------------------------- tracing */

static FILE *g_log;

static void fm_log(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void fourcc(UInt32 v, char out[5]) {
    out[0] = (char)((v >> 24) & 0xff);
    out[1] = (char)((v >> 16) & 0xff);
    out[2] = (char)((v >> 8) & 0xff);
    out[3] = (char)(v & 0xff);
    out[4] = 0;
}

/* --------------------------------------------------------------- helpers */

static OSStatus put_u32(UInt32 *ioSize, void *out, UInt32 inSize, UInt32 v) {
    if (ioSize) *ioSize = sizeof(UInt32);
    if (!out) return noErr;
    if (inSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
    *(UInt32 *)out = v;
    return noErr;
}

static OSStatus put_f64(UInt32 *ioSize, void *out, UInt32 inSize, Float64 v) {
    if (ioSize) *ioSize = sizeof(Float64);
    if (!out) return noErr;
    if (inSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
    *(Float64 *)out = v;
    return noErr;
}

static OSStatus put_cf(UInt32 *ioSize, void *out, UInt32 inSize, CFTypeRef v) {
    if (ioSize) *ioSize = sizeof(CFTypeRef);
    if (!out) return noErr;
    if (inSize < sizeof(CFTypeRef)) return kAudioHardwareBadPropertySizeError;
    *(CFTypeRef *)out = v;
    return noErr;
}

static OSStatus put_ids(UInt32 *ioSize, void *out, UInt32 inSize,
                        const AudioObjectID *ids, UInt32 n) {
    UInt32 bytes = n * (UInt32)sizeof(AudioObjectID);
    if (ioSize) *ioSize = bytes;
    if (!out) return noErr;
    if (inSize < bytes) return kAudioHardwareBadPropertySizeError;
    memcpy(out, ids, bytes);
    return noErr;
}

static void fill_asbd(AudioStreamBasicDescription *asbd, Float64 rate) {
    memset(asbd, 0, sizeof(*asbd));
    asbd->mSampleRate       = rate;
    asbd->mFormatID         = kAudioFormatLinearPCM;
    asbd->mFormatFlags      = kAudioFormatFlagsNativeFloatPacked |
                              kAudioFormatFlagIsNonInterleaved;
    asbd->mChannelsPerFrame = 2;
    asbd->mBitsPerChannel   = 32;
    asbd->mFramesPerPacket  = 1;
    asbd->mBytesPerFrame    = 4;
    asbd->mBytesPerPacket   = 4;
}

static OSStatus put_asbd(UInt32 *ioSize, void *out, UInt32 inSize, Float64 rate) {
    if (ioSize) *ioSize = sizeof(AudioStreamBasicDescription);
    if (!out) return noErr;
    if (inSize < sizeof(AudioStreamBasicDescription))
        return kAudioHardwareBadPropertySizeError;
    fill_asbd((AudioStreamBasicDescription *)out, rate);
    return noErr;
}

static OSStatus put_ranged_asbd(UInt32 *ioSize, void *out, UInt32 inSize, Float64 rate) {
    if (ioSize) *ioSize = sizeof(AudioStreamRangedDescription);
    if (!out) return noErr;
    if (inSize < sizeof(AudioStreamRangedDescription))
        return kAudioHardwareBadPropertySizeError;
    AudioStreamRangedDescription *d = (AudioStreamRangedDescription *)out;
    memset(d, 0, sizeof(*d));
    fill_asbd(&d->mFormat, rate);
    d->mSampleRateRange.mMinimum = rate;
    d->mSampleRateRange.mMaximum = rate;
    return noErr;
}

/* ------------------------------------------------------------- properties */

static Boolean is_input_stream(AudioObjectID obj) { return obj == kObjectID_StreamInput; }

static OSStatus get_stream_configuration(UInt32 *ioSize, void *out, UInt32 inSize,
                                         AudioObjectPropertyScope scope) {
    AudioBufferList *abl = (AudioBufferList *)out;
    if (ioSize) *ioSize = sizeof(AudioBufferList);
    if (!out) return noErr;
    if (inSize < sizeof(AudioBufferList)) return kAudioHardwareBadPropertySizeError;
    abl->mNumberBuffers = 1;
    abl->mBuffers[0].mNumberChannels = 2;
    abl->mBuffers[0].mDataByteSize = 0;
    abl->mBuffers[0].mData = NULL;
    (void)scope;
    return noErr;
}

static OSStatus property_data(FinalmicDriver *d, AudioObjectID obj,
                              const AudioObjectPropertyAddress *addr,
                              UInt32 inSize, UInt32 *ioSize, void *out,
                              UInt32 qualSize, const void *qual) {
    if (!d || !addr) return kAudioHardwareBadObjectError;

    UInt32 scope = addr->mScope;
    switch (obj) {
    case kObjectID_PlugIn:
        switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
            return put_u32(ioSize, out, inSize, kAudioObjectClassID);
        case kAudioObjectPropertyClass:
            return put_u32(ioSize, out, inSize, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner:
            return put_u32(ioSize, out, inSize, kAudioObjectSystemObject);
        case kAudioObjectPropertyName:
            return put_cf(ioSize, out, inSize, CFSTR(kDeviceName));
        case kAudioObjectPropertyManufacturer:
            return put_cf(ioSize, out, inSize, CFSTR(kManufacturer));
        case kAudioObjectPropertyOwnedObjects: {
            AudioObjectID ids[1] = {kObjectID_Device};
            return put_ids(ioSize, out, inSize, ids, 1);
        }
        case kAudioObjectPropertyCustomPropertyInfoList:
            return put_cf(ioSize, out, inSize, g_empty_array);
        case kAudioPlugInPropertyBoxList:
        case kAudioPlugInPropertyClockDeviceList: {
            AudioObjectID none = 0;
            return put_ids(ioSize, out, inSize, &none, 0);
        }
        case kAudioPlugInPropertyTranslateUIDToBox:
        case kAudioPlugInPropertyTranslateUIDToClockDevice:
            return put_u32(ioSize, out, inSize, 0);
        case kAudioPlugInPropertyBundleID:
            return put_cf(ioSize, out, inSize, CFSTR(kBundleID));
        case kAudioPlugInPropertyDeviceList: {
            AudioObjectID ids[1] = {kObjectID_Device};
            return put_ids(ioSize, out, inSize, ids, 1);
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            AudioObjectID id = 0;
            if (qual && qualSize >= sizeof(CFStringRef)) {
                CFStringRef uid = *(CFStringRef *)qual;
                if (uid && CFStringGetCStringPtr(uid, kCFStringEncodingUTF8)) {
                    if (CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo)
                        id = kObjectID_Device;
                } else if (uid && CFEqual(uid, CFSTR(kDeviceUID))) {
                    id = kObjectID_Device;
                }
            }
            return put_u32(ioSize, out, inSize, id);
        }
        default:
            return kAudioHardwareUnknownPropertyError;
        }

    case kObjectID_Device:
        switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
            return put_u32(ioSize, out, inSize, kAudioObjectClassID);
        case kAudioObjectPropertyClass:
            return put_u32(ioSize, out, inSize, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner:
            return put_u32(ioSize, out, inSize, kObjectID_PlugIn);
        case kAudioObjectPropertyName:
            return put_cf(ioSize, out, inSize, CFSTR(kDeviceName));
        case kAudioObjectPropertyManufacturer:
            return put_cf(ioSize, out, inSize, CFSTR(kManufacturer));
        case kAudioObjectPropertyOwnedObjects: {
            AudioObjectID ids[2] = {kObjectID_StreamInput, kObjectID_StreamOutput};
            if (scope == kAudioObjectPropertyScopeInput)
                return put_ids(ioSize, out, inSize, ids, 1);
            if (scope == kAudioObjectPropertyScopeOutput)
                return put_ids(ioSize, out, inSize, &ids[1], 1);
            return put_ids(ioSize, out, inSize, ids, 2);
        }
        case kAudioDevicePropertyConfigurationApplication:
            return put_cf(ioSize, out, inSize, CFSTR("com.zsh2401.finalmic"));
        case kAudioDevicePropertyDeviceUID:
            return put_cf(ioSize, out, inSize, CFSTR(kDeviceUID));
        case kAudioDevicePropertyModelUID:
            return put_cf(ioSize, out, inSize, CFSTR(kModelUID));
        case kAudioDevicePropertyTransportType:
            return put_u32(ioSize, out, inSize, kAudioDeviceTransportTypeVirtual);
        case kAudioDevicePropertyRelatedDevices: {
            AudioObjectID none = 0;
            return put_ids(ioSize, out, inSize, &none, 0);
        }
        case kAudioDevicePropertyClockDomain:
            return put_u32(ioSize, out, inSize, 0);
        case kAudioDevicePropertyDeviceIsAlive:
            return put_u32(ioSize, out, inSize, 1);
        case kAudioDevicePropertyDeviceIsRunning:
            return put_u32(ioSize, out, inSize, d->running ? 1 : 0);
        case kAudioDevicePropertyDeviceIsRunningSomewhere:
            return put_u32(ioSize, out, inSize, d->running ? 1 : 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            return put_u32(ioSize, out, inSize, 1);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            return put_u32(ioSize, out, inSize, 1);
        case kAudioDevicePropertyIsHidden:
            return put_u32(ioSize, out, inSize, 0);
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            UInt32 ch[2] = {1, 2};
            if (ioSize) *ioSize = sizeof(ch);
            if (!out) return noErr;
            if (inSize < sizeof(ch)) return kAudioHardwareBadPropertySizeError;
            memcpy(out, ch, sizeof(ch));
            return noErr;
        }
        case kAudioDevicePropertyNominalSampleRate:
            return put_f64(ioSize, out, inSize, d->sampleRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            AudioValueRange r = {d->sampleRate, d->sampleRate};
            if (ioSize) *ioSize = sizeof(r);
            if (!out) return noErr;
            if (inSize < sizeof(r)) return kAudioHardwareBadPropertySizeError;
            memcpy(out, &r, sizeof(r));
            return noErr;
        }
        case kAudioDevicePropertyBufferFrameSize:
            return put_u32(ioSize, out, inSize, d->bufferFrames);
        case kAudioDevicePropertyBufferFrameSizeRange: {
            AudioValueRange r = {kMinBufferFrames, kMaxBufferFrames};
            if (ioSize) *ioSize = sizeof(r);
            if (!out) return noErr;
            if (inSize < sizeof(r)) return kAudioHardwareBadPropertySizeError;
            memcpy(out, &r, sizeof(r));
            return noErr;
        }
        case kAudioDevicePropertyZeroTimeStampPeriod:
            /* The HAL models the clock as zero time stamps spaced this many
             * frames apart; keep it in sync with the ring buffer. */
            return put_u32(ioSize, out, inSize, kRingFrames);
        case kAudioDevicePropertyLatency:
            return put_u32(ioSize, out, inSize, 0);
        case kAudioDevicePropertySafetyOffset:
            return put_u32(ioSize, out, inSize, 0);
        case kAudioDevicePropertyStreams: {
            AudioObjectID ids[1];
            if (scope == kAudioObjectPropertyScopeInput)
                ids[0] = kObjectID_StreamInput;
            else if (scope == kAudioObjectPropertyScopeOutput)
                ids[0] = kObjectID_StreamOutput;
            else
                return put_ids(ioSize, out, inSize, ids, 0);
            return put_ids(ioSize, out, inSize, ids, 1);
        }
        case kAudioDevicePropertyStreamConfiguration:
            if (scope != kAudioObjectPropertyScopeInput &&
                scope != kAudioObjectPropertyScopeOutput)
                return kAudioHardwareBadObjectError;
            return get_stream_configuration(ioSize, out, inSize, scope);
        case kAudioObjectPropertyControlList: {
            AudioObjectID none = 0;
            return put_ids(ioSize, out, inSize, &none, 0);
        }
        case kAudioObjectPropertyCustomPropertyInfoList:
            return put_cf(ioSize, out, inSize, g_empty_array);
        case kAudioObjectPropertyElementName:
            return put_cf(ioSize, out, inSize, CFSTR("finalmic"));
        case kAudioObjectPropertyElementCategoryName:
            return put_cf(ioSize, out, inSize, CFSTR("finalmic"));
        case kAudioObjectPropertyElementNumberName:
            return put_u32(ioSize, out, inSize, 0);
        case kAudioObjectPropertyFirmwareVersion:
            return put_cf(ioSize, out, inSize, CFSTR("1.0.0"));
        case kAudioObjectPropertyModelName:
            return put_cf(ioSize, out, inSize, CFSTR(kDeviceName));
        case kAudioObjectPropertySerialNumber:
            return put_cf(ioSize, out, inSize, CFSTR("finalmic-0001"));
        case kAudioDevicePropertyPreferredChannelLayout: {
            /* Stereo: two channels, left/right. */
            if (ioSize) *ioSize = sizeof(AudioChannelLayout) + sizeof(AudioChannelDescription);
            if (!out) return noErr;
            if (inSize < sizeof(AudioChannelLayout) + sizeof(AudioChannelDescription))
                return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *layout = (AudioChannelLayout *)out;
            memset(layout, 0, *ioSize);
            layout->mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
            layout->mChannelBitmap = 0;
            layout->mNumberChannelDescriptions = 0;
            return noErr;
        }
        default:
            return kAudioHardwareUnknownPropertyError;
        }

    case kObjectID_StreamInput:
    case kObjectID_StreamOutput:
        switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
            return put_u32(ioSize, out, inSize, kAudioObjectClassID);
        case kAudioObjectPropertyClass:
            return put_u32(ioSize, out, inSize, kAudioStreamClassID);
        case kAudioObjectPropertyOwner:
            return put_u32(ioSize, out, inSize, kObjectID_Device);
        case kAudioObjectPropertyName:
            return put_cf(ioSize, out, inSize,
                          is_input_stream(obj) ? CFSTR("finalmic Input")
                                               : CFSTR("finalmic Output"));
        case kAudioObjectPropertyOwnedObjects: {
            AudioObjectID none = 0;
            return put_ids(ioSize, out, inSize, &none, 0);
        }
        case kAudioObjectPropertyCustomPropertyInfoList:
            return put_cf(ioSize, out, inSize, g_empty_array);
        case kAudioObjectPropertyElementName:
            return put_cf(ioSize, out, inSize,
                          is_input_stream(obj) ? CFSTR("finalmic Input")
                                               : CFSTR("finalmic Output"));
        case kAudioObjectPropertyElementCategoryName:
            return put_cf(ioSize, out, inSize, CFSTR("finalmic"));
        case kAudioObjectPropertyElementNumberName:
            return put_u32(ioSize, out, inSize, 0);
        case kAudioStreamPropertyIsActive:
            return put_u32(ioSize, out, inSize, 1);
        case kAudioStreamPropertyDirection:
            return put_u32(ioSize, out, inSize, is_input_stream(obj) ? 1 : 0);
        case kAudioStreamPropertyTerminalType:
            return put_u32(ioSize, out, inSize,
                           is_input_stream(obj) ? kAudioStreamTerminalTypeMicrophone
                                                : kAudioStreamTerminalTypeSpeaker);
        case kAudioStreamPropertyStartingChannel:
            return put_u32(ioSize, out, inSize, 1);
        case kAudioStreamPropertyLatency:
            return put_u32(ioSize, out, inSize, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            return put_asbd(ioSize, out, inSize, d->sampleRate);
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            return put_ranged_asbd(ioSize, out, inSize, d->sampleRate);
        default:
            return kAudioHardwareUnknownPropertyError;
        }

    default:
        return kAudioHardwareBadObjectError;
    }
}

/* ------------------------------------------------------------ IUnknown */

static int uuid_equal(REFIID in, CFUUIDRef constant) {
    CFUUIDBytes a = in;
    CFUUIDBytes b = CFUUIDGetUUIDBytes(constant);
    return memcmp(&a, &b, sizeof(CFUUIDBytes)) == 0;
}

static HRESULT STDMETHODCALLTYPE drv_QueryInterface(void *inDriver, REFIID inUUID,
                                                    LPVOID *outInterface) {
    if (!outInterface) return E_POINTER;
    FinalmicDriver *d = driver_from_ref((AudioServerPlugInDriverRef)inDriver);
    if (!d) return E_POINTER;
    if (uuid_equal(inUUID, kAudioServerPlugInTypeUUID) ||
        uuid_equal(inUUID, kAudioServerPlugInDriverInterfaceUUID) ||
        uuid_equal(inUUID, IUnknownUUID)) {
        d->refCount++;
        *outInterface = inDriver;
        return S_OK;
    }
    *outInterface = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE drv_AddRef(void *inDriver) {
    FinalmicDriver *d = driver_from_ref((AudioServerPlugInDriverRef)inDriver);
    return d ? ++d->refCount : 0;
}

static ULONG STDMETHODCALLTYPE drv_Release(void *inDriver) {
    FinalmicDriver *d = driver_from_ref((AudioServerPlugInDriverRef)inDriver);
    if (!d) return 0;
    if (d->refCount > 0) d->refCount--;
    return d->refCount;
}

/* ------------------------------------------------------------ lifecycle */

static OSStatus STDMETHODCALLTYPE drv_Initialize(AudioServerPlugInDriverRef inDriver,
                                                 AudioServerPlugInHostRef inHost) {
    FinalmicDriver *d = driver_from_ref(inDriver);
    if (!d) return kAudioHardwareBadObjectError;
    d->host = inHost;
    if (!g_log) g_log = fopen("/tmp/finalmic-driver.log", "a");
    fm_log("initialize");

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double nsPerTick = (double)tb.numer / (double)tb.denom;
    double nsPerFrame = 1e9 / d->sampleRate;
    d->ticksPerFrame = nsPerFrame / nsPerTick;

    d->bufferFrames = 512;
    d->running = 0;
    d->seed = 1;
    atomic_store(&d->writePos, 0);
    atomic_store(&d->readPos, 0);
    return noErr;
}

static OSStatus STDMETHODCALLTYPE drv_CreateDevice(AudioServerPlugInDriverRef inDriver,
                                                   CFDictionaryRef inDescription,
                                                   const AudioServerPlugInClientInfo *inClientInfo,
                                                   AudioObjectID *outDeviceObjectID) {
    (void)inDriver; (void)inDescription; (void)inClientInfo;
    if (outDeviceObjectID) *outDeviceObjectID = 0;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus STDMETHODCALLTYPE drv_DestroyDevice(AudioServerPlugInDriverRef inDriver,
                                                    AudioObjectID inDeviceObjectID) {
    (void)inDriver; (void)inDeviceObjectID;
    return noErr;
}

static OSStatus STDMETHODCALLTYPE drv_AddDeviceClient(AudioServerPlugInDriverRef inDriver,
                                                      AudioObjectID inDeviceObjectID,
                                                      const AudioServerPlugInClientInfo *inClientInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientInfo;
    return noErr;
}

static OSStatus STDMETHODCALLTYPE drv_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver,
                                                         AudioObjectID inDeviceObjectID,
                                                         const AudioServerPlugInClientInfo *inClientInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientInfo;
    return noErr;
}

static OSStatus STDMETHODCALLTYPE drv_PerformDeviceConfigurationChange(
        AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
        UInt64 inChangeAction, void *inChangeInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo;
    return noErr;
}

static OSStatus STDMETHODCALLTYPE drv_AbortDeviceConfigurationChange(
        AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
        UInt64 inChangeAction, void *inChangeInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo;
    return noErr;
}

/* ------------------------------------------------------------ properties */

static Boolean STDMETHODCALLTYPE drv_HasProperty(AudioServerPlugInDriverRef inDriver,
                                                 AudioObjectID inObjectID,
                                                 pid_t inClientProcessID,
                                                 const AudioObjectPropertyAddress *inAddress) {
    (void)inClientProcessID;
    FinalmicDriver *d = driver_from_ref(inDriver);
    if (!d || !inAddress) return false;
    UInt32 size = 0;
    return property_data(d, inObjectID, inAddress, 0, &size, NULL, 0, NULL) == noErr;
}

static OSStatus STDMETHODCALLTYPE drv_IsPropertySettable(AudioServerPlugInDriverRef inDriver,
                                                         AudioObjectID inObjectID,
                                                         pid_t inClientProcessID,
                                                         const AudioObjectPropertyAddress *inAddress,
                                                         Boolean *outIsSettable) {
    (void)inDriver; (void)inClientProcessID;
    if (!outIsSettable || !inAddress) return kAudioHardwareBadObjectError;
    *outIsSettable = false;
    if (inObjectID != kObjectID_Device) return noErr;
    if (inAddress->mSelector == kAudioDevicePropertyBufferFrameSize ||
        inAddress->mSelector == kAudioDevicePropertyNominalSampleRate)
        *outIsSettable = true;
    return noErr;
}

static OSStatus STDMETHODCALLTYPE drv_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver,
                                                          AudioObjectID inObjectID,
                                                          pid_t inClientProcessID,
                                                          const AudioObjectPropertyAddress *inAddress,
                                                          UInt32 inQualifierDataSize,
                                                          const void *inQualifierData,
                                                          UInt32 *outDataSize) {
    (void)inClientProcessID;
    if (!outDataSize) return kAudioHardwareBadObjectError;
    FinalmicDriver *d = driver_from_ref(inDriver);
    OSStatus st = property_data(d, inObjectID, inAddress, 0, outDataSize, NULL,
                                inQualifierDataSize, inQualifierData);
    if (inAddress) {
        char sel[5], sc[5];
        fourcc(inAddress->mSelector, sel);
        fourcc(inAddress->mScope, sc);
        fm_log("size obj=%u %s/%s/%u -> %d (%u)", inObjectID, sel, sc,
               inAddress->mElement, (int)st, outDataSize ? *outDataSize : 0);
    }
    return st;
}

static OSStatus STDMETHODCALLTYPE drv_GetPropertyData(AudioServerPlugInDriverRef inDriver,
                                                      AudioObjectID inObjectID,
                                                      pid_t inClientProcessID,
                                                      const AudioObjectPropertyAddress *inAddress,
                                                      UInt32 inQualifierDataSize,
                                                      const void *inQualifierData,
                                                      UInt32 inDataSize,
                                                      UInt32 *outDataSize,
                                                      void *outData) {
    (void)inClientProcessID;
    FinalmicDriver *d = driver_from_ref(inDriver);
    OSStatus st = property_data(d, inObjectID, inAddress, inDataSize, outDataSize, outData,
                                inQualifierDataSize, inQualifierData);
    if (inAddress) {
        char sel[5], sc[5];
        fourcc(inAddress->mSelector, sel);
        fourcc(inAddress->mScope, sc);
        fm_log("get  obj=%u %s/%s/%u in=%u -> %d", inObjectID, sel, sc,
               inAddress->mElement, inDataSize, (int)st);
    }
    return st;
}

static OSStatus STDMETHODCALLTYPE drv_SetPropertyData(AudioServerPlugInDriverRef inDriver,
                                                      AudioObjectID inObjectID,
                                                      pid_t inClientProcessID,
                                                      const AudioObjectPropertyAddress *inAddress,
                                                      UInt32 inQualifierDataSize,
                                                      const void *inQualifierData,
                                                      UInt32 inDataSize,
                                                      const void *inData) {
    (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;
    FinalmicDriver *d = driver_from_ref(inDriver);
    if (!d || !inAddress || !inData) return kAudioHardwareBadObjectError;
    if (inObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;

    if (inAddress->mSelector == kAudioDevicePropertyBufferFrameSize) {
        if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        UInt32 v = *(const UInt32 *)inData;
        if (v < kMinBufferFrames) v = kMinBufferFrames;
        if (v > kMaxBufferFrames) v = kMaxBufferFrames;
        d->bufferFrames = v;
        return noErr;
    }
    if (inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inDataSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        Float64 v = *(const Float64 *)inData;
        if (v < 47999.0 || v > 48001.0) return kAudioHardwareUnsupportedOperationError;
        return noErr;
    }
    return kAudioHardwareUnknownPropertyError;
}

/* ------------------------------------------------------------------- IO */

static OSStatus STDMETHODCALLTYPE drv_StartIO(AudioServerPlugInDriverRef inDriver,
                                              AudioObjectID inDeviceObjectID,
                                              UInt32 inClientID) {
    (void)inClientID;
    FinalmicDriver *d = driver_from_ref(inDriver);
    if (!d || inDeviceObjectID != kObjectID_Device)
        return kAudioHardwareBadObjectError;

    /* Pre-fill two buffers of silence so the capture side never starts dry. */
    uint64_t fill = (uint64_t)d->bufferFrames * 2u;
    if (fill > kRingFrames / 4) fill = kRingFrames / 4;
    memset(d->ring, 0, sizeof(d->ring));
    atomic_store(&d->readPos, 0);
    atomic_store(&d->writePos, fill);
    atomic_store(&d->underruns, 0);
    atomic_store(&d->overruns, 0);

    fm_log("startIO bufferFrames=%u", (unsigned)d->bufferFrames);
    d->anchorHost = mach_absolute_time();
    d->anchorSample = 0.0;
    d->seed++;
    d->running = 1;
    return noErr;
}

static OSStatus STDMETHODCALLTYPE drv_StopIO(AudioServerPlugInDriverRef inDriver,
                                             AudioObjectID inDeviceObjectID,
                                             UInt32 inClientID) {
    (void)inClientID;
    FinalmicDriver *d = driver_from_ref(inDriver);
    if (!d || inDeviceObjectID != kObjectID_Device)
        return kAudioHardwareBadObjectError;
    d->running = 0;
    return noErr;
}

static OSStatus STDMETHODCALLTYPE drv_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver,
                                                       AudioObjectID inDeviceObjectID,
                                                       UInt32 inClientID,
                                                       Float64 *outSampleTime,
                                                       UInt64 *outHostTime,
                                                       UInt64 *outSeed) {
    (void)inClientID;
    FinalmicDriver *d = driver_from_ref(inDriver);
    if (!d || inDeviceObjectID != kObjectID_Device)
        return kAudioHardwareBadObjectError;
    if (!outSampleTime || !outHostTime || !outSeed)
        return kAudioHardwareBadObjectError;

    UInt64 now = mach_absolute_time();
    if (now < d->anchorHost) now = d->anchorHost;
    double frames = (double)(now - d->anchorHost) / d->ticksPerFrame;
    if (frames < 0.0) frames = 0.0;
    /* Zero time stamps advance in steps of kRingFrames, matching
     * kAudioDevicePropertyZeroTimeStampPeriod. */
    double period = (double)kRingFrames;
    double zero = floor(frames / period) * period;
    *outSampleTime = d->anchorSample + zero;
    *outHostTime = d->anchorHost + (UInt64)(zero * d->ticksPerFrame);
    *outSeed = d->seed;
    return noErr;
}

static OSStatus STDMETHODCALLTYPE drv_WillDoIOOperation(AudioServerPlugInDriverRef inDriver,
                                                        AudioObjectID inDeviceObjectID,
                                                        UInt32 inClientID,
                                                        UInt32 inOperationID,
                                                        Boolean *outWillDo,
                                                        Boolean *outWillDoInPlace) {
    (void)inDriver; (void)inClientID;
    if (inDeviceObjectID != kObjectID_Device)
        return kAudioHardwareBadObjectError;
    if (!outWillDo) return kAudioHardwareBadObjectError;
    *outWillDo = (inOperationID == kAudioServerPlugInIOOperationReadInput ||
                  inOperationID == kAudioServerPlugInIOOperationWriteMix);
    if (outWillDoInPlace) *outWillDoInPlace = true;
    return noErr;
}

static OSStatus STDMETHODCALLTYPE drv_BeginIOOperation(AudioServerPlugInDriverRef inDriver,
                                                       AudioObjectID inDeviceObjectID,
                                                       UInt32 inClientID,
                                                       UInt32 inOperationID,
                                                       UInt32 inIOBufferFrameSize,
                                                       const AudioServerPlugInIOCycleInfo *inIOCycleInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    return noErr;
}

static OSStatus STDMETHODCALLTYPE drv_EndIOOperation(AudioServerPlugInDriverRef inDriver,
                                                     AudioObjectID inDeviceObjectID,
                                                     UInt32 inClientID,
                                                     UInt32 inOperationID,
                                                     UInt32 inIOBufferFrameSize,
                                                     const AudioServerPlugInIOCycleInfo *inIOCycleInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    return noErr;
}

/* Sample access that handles both interleaved and non-interleaved layouts. */
static inline float *sample_ptr(AudioBufferList *abl, UInt32 frame, UInt32 ch) {
    if (abl->mNumberBuffers == 1) {
        float *base = (float *)abl->mBuffers[0].mData;
        return base + frame * abl->mBuffers[0].mNumberChannels + ch;
    }
    float *base = (float *)abl->mBuffers[ch].mData;
    return base + frame;
}

static OSStatus STDMETHODCALLTYPE drv_DoIOOperation(AudioServerPlugInDriverRef inDriver,
                                                    AudioObjectID inDeviceObjectID,
                                                    AudioObjectID inStreamObjectID,
                                                    UInt32 inClientID,
                                                    UInt32 inOperationID,
                                                    UInt32 inIOBufferFrameSize,
                                                    const AudioServerPlugInIOCycleInfo *inIOCycleInfo,
                                                    void *ioMainBuffer,
                                                    void *ioSecondaryBuffer) {
    (void)inClientID; (void)inStreamObjectID; (void)inIOCycleInfo; (void)ioSecondaryBuffer;
    FinalmicDriver *d = driver_from_ref(inDriver);
    if (!d || !ioMainBuffer || inDeviceObjectID != kObjectID_Device)
        return kAudioHardwareBadObjectError;
    AudioBufferList *abl = (AudioBufferList *)ioMainBuffer;
    if (abl->mNumberBuffers == 0) return noErr;

    UInt32 n = inIOBufferFrameSize;
    if (n > kRingFrames / 2) n = kRingFrames / 2;

    if (inOperationID == kAudioServerPlugInIOOperationWriteMix) {
        uint64_t w = atomic_load_explicit(&d->writePos, memory_order_relaxed);
        uint64_t r = atomic_load_explicit(&d->readPos, memory_order_acquire);
        if (w - r > (uint64_t)(kRingFrames - n)) {
            atomic_fetch_add(&d->overruns, 1);
            return noErr;
        }
        for (UInt32 i = 0; i < n; i++) {
            uint32_t idx = (uint32_t)((w + i) & kRingMask);
            d->ring[idx * 2 + 0] = *sample_ptr(abl, i, 0);
            d->ring[idx * 2 + 1] = *sample_ptr(abl, i, 1);
        }
        atomic_store_explicit(&d->writePos, w + n, memory_order_release);
        return noErr;
    }

    if (inOperationID == kAudioServerPlugInIOOperationReadInput) {
        uint64_t w = atomic_load_explicit(&d->writePos, memory_order_acquire);
        uint64_t r = atomic_load_explicit(&d->readPos, memory_order_relaxed);
        uint64_t avail = w - r;
        if (avail < (uint64_t)n) {
            atomic_fetch_add(&d->underruns, 1);
            for (UInt32 i = 0; i < n; i++) {
                *sample_ptr(abl, i, 0) = 0.0f;
                *sample_ptr(abl, i, 1) = 0.0f;
            }
            return noErr;
        }
        for (UInt32 i = 0; i < n; i++) {
            uint32_t idx = (uint32_t)((r + i) & kRingMask);
            *sample_ptr(abl, i, 0) = d->ring[idx * 2 + 0];
            *sample_ptr(abl, i, 1) = d->ring[idx * 2 + 1];
        }
        atomic_store_explicit(&d->readPos, r + n, memory_order_release);
        return noErr;
    }

    return noErr;
}

/* ---------------------------------------------------------------- export */

__attribute__((visibility("default")))
void *FinalmicDriver_Create(CFAllocatorRef allocator, CFUUIDRef typeUUID) {
    (void)allocator;
    if (!typeUUID || !CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) return NULL;

    static AudioServerPlugInDriverInterface vtable;
    static int vtable_ready = 0;
    if (!g_empty_array) g_empty_array = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
    if (!vtable_ready) {
        vtable._reserved = NULL;
        vtable.QueryInterface = drv_QueryInterface;
        vtable.AddRef = drv_AddRef;
        vtable.Release = drv_Release;
        vtable.Initialize = drv_Initialize;
        vtable.CreateDevice = drv_CreateDevice;
        vtable.DestroyDevice = drv_DestroyDevice;
        vtable.AddDeviceClient = drv_AddDeviceClient;
        vtable.RemoveDeviceClient = drv_RemoveDeviceClient;
        vtable.PerformDeviceConfigurationChange = drv_PerformDeviceConfigurationChange;
        vtable.AbortDeviceConfigurationChange = drv_AbortDeviceConfigurationChange;
        vtable.HasProperty = drv_HasProperty;
        vtable.IsPropertySettable = drv_IsPropertySettable;
        vtable.GetPropertyDataSize = drv_GetPropertyDataSize;
        vtable.GetPropertyData = drv_GetPropertyData;
        vtable.SetPropertyData = drv_SetPropertyData;
        vtable.StartIO = drv_StartIO;
        vtable.StopIO = drv_StopIO;
        vtable.GetZeroTimeStamp = drv_GetZeroTimeStamp;
        vtable.WillDoIOOperation = drv_WillDoIOOperation;
        vtable.BeginIOOperation = drv_BeginIOOperation;
        vtable.DoIOOperation = drv_DoIOOperation;
        vtable.EndIOOperation = drv_EndIOOperation;
        vtable_ready = 1;
    }

    FinalmicDriver *d = (FinalmicDriver *)calloc(1, sizeof(FinalmicDriver));
    if (!d) return NULL;
    d->iface = &vtable;
    d->refCount = 1;
    d->sampleRate = 48000.0;
    d->bufferFrames = 512;
    return d;
}
