/* Offline smoke test for the finalmic HAL plug-in: loads the bundle, fakes the
 * host interface, checks the property model and the loopback ring. */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  ok   %s\n", msg); \
    else { printf("  FAIL %s\n", msg); failures++; } \
} while (0)

static OSStatus h_changed(AudioServerPlugInHostRef h, AudioObjectID o, UInt32 n,
                          const AudioObjectPropertyAddress *a) {
    (void)h; (void)o; (void)n; (void)a; return noErr;
}
static OSStatus h_copy(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef *d) {
    (void)h; (void)k; (void)d; return kAudioHardwareUnknownPropertyError;
}
static OSStatus h_write(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef d) {
    (void)h; (void)k; (void)d; return noErr;
}
static OSStatus h_delete(AudioServerPlugInHostRef h, CFStringRef k) {
    (void)h; (void)k; return noErr;
}
static OSStatus h_request(AudioServerPlugInHostRef h, AudioObjectID o, UInt64 a, void *i) {
    (void)h; (void)o; (void)a; (void)i; return noErr;
}

typedef AudioServerPlugInDriverInterface *VT;

static UInt32 get_u32(AudioServerPlugInDriverRef d, AudioObjectID obj,
                      AudioObjectPropertySelector sel) {
    AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 v = 0, size = sizeof(v);
    OSStatus st = (*d)->GetPropertyData(d, obj, 0, &a, 0, NULL, sizeof(v), &size, &v);
    if (st != noErr) return 0xFFFFFFFFu;
    return v;
}

static CFStringRef get_cf(AudioServerPlugInDriverRef d, AudioObjectID obj,
                          AudioObjectPropertySelector sel) {
    AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    CFStringRef v = NULL;
    UInt32 size = sizeof(v);
    (*d)->GetPropertyData(d, obj, 0, &a, 0, NULL, sizeof(v), &size, &v);
    return v;
}

static UInt32 scope_streams(AudioServerPlugInDriverRef d, AudioObjectPropertyScope scope,
                            AudioObjectID *out, UInt32 max) {
    AudioObjectPropertyAddress a = {kAudioDevicePropertyStreams, scope,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if ((*d)->GetPropertyDataSize(d, 2, 0, &a, 0, NULL, &size) != noErr) return 0;
    UInt32 n = size / sizeof(AudioObjectID);
    if (n > max) n = max;
    (*d)->GetPropertyData(d, 2, 0, &a, 0, NULL, size, &size, out);
    return n;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "build/finalmic.driver/Contents/MacOS/finalmic";
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    void *(*create)(CFAllocatorRef, CFUUIDRef) =
        (void *(*)(CFAllocatorRef, CFUUIDRef))dlsym(lib, "FinalmicDriver_Create");
    if (!create) { printf("factory symbol missing\n"); return 1; }

    AudioServerPlugInHostInterface host;
    memset(&host, 0, sizeof host);
    host.PropertiesChanged = h_changed;
    host.CopyFromStorage = h_copy;
    host.WriteToStorage = h_write;
    host.DeleteFromStorage = h_delete;
    host.RequestDeviceConfigurationChange = h_request;
    AudioServerPlugInDriverRef drv = (AudioServerPlugInDriverRef)create(NULL, kAudioServerPlugInTypeUUID);
    if (!drv) { printf("factory returned NULL\n"); return 1; }

    VT vt = *drv;
    CHECK(vt->Initialize(drv, &host) == noErr, "Initialize");
    CHECK(vt->AddRef(drv) >= 2, "AddRef");
    CHECK(vt->Release(drv) >= 1, "Release");

    printf("\n[plugin]\n");
    AudioObjectPropertyAddress a = {kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    CHECK(vt->GetPropertyDataSize(drv, 1, 0, &a, 0, NULL, &size) == noErr && size == 4,
          "device list size");
    AudioObjectID dev = 0;
    vt->GetPropertyData(drv, 1, 0, &a, 0, NULL, sizeof(dev), &size, &dev);
    CHECK(dev == 2, "device id == 2");

    printf("\n[device]\n");
    CHECK(get_u32(drv, 2, kAudioObjectPropertyClass) == kAudioDeviceClassID, "class is device");
    CHECK(get_u32(drv, 2, kAudioDevicePropertyTransportType) == kAudioDeviceTransportTypeVirtual,
          "transport is virtual");
    CHECK(get_u32(drv, 2, kAudioDevicePropertyDeviceIsAlive) == 1, "is alive");
    CHECK(get_u32(drv, 2, kAudioDevicePropertyDeviceCanBeDefaultDevice) == 1, "can be default");
    CFStringRef name = get_cf(drv, 2, kAudioObjectPropertyName);
    char nb[128] = {0};
    if (name) CFStringGetCString(name, nb, sizeof nb, kCFStringEncodingUTF8);
    CHECK(strcmp(nb, "finalmic") == 0, "device name is finalmic");
    CFStringRef uid = get_cf(drv, 2, kAudioDevicePropertyDeviceUID);
    char ub[128] = {0};
    if (uid) CFStringGetCString(uid, ub, sizeof ub, kCFStringEncodingUTF8);
    CHECK(strstr(ub, "finalmic") != NULL, "device uid");

    AudioObjectID ins[4], outs[4];
    UInt32 n_in = scope_streams(drv, kAudioObjectPropertyScopeInput, ins, 4);
    UInt32 n_out = scope_streams(drv, kAudioObjectPropertyScopeOutput, outs, 4);
    CHECK(n_in == 1 && ins[0] == 3, "one input stream (id 3)");
    CHECK(n_out == 1 && outs[0] == 4, "one output stream (id 4)");

    printf("\n[streams]\n");
    for (int i = 0; i < 2; i++) {
        AudioObjectID sid = i == 0 ? 3 : 4;
        AudioStreamBasicDescription asbd;
        AudioObjectPropertyAddress fa = {kAudioStreamPropertyVirtualFormat,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};
        UInt32 fs = sizeof(asbd);
        OSStatus st = vt->GetPropertyData(drv, sid, 0, &fa, 0, NULL, sizeof(asbd), &fs, &asbd);
        CHECK(st == noErr && asbd.mChannelsPerFrame == 2 && asbd.mSampleRate == 48000 &&
              asbd.mBitsPerChannel == 32, i == 0 ? "input 48k/2ch/32f" : "output 48k/2ch/32f");
        CHECK(get_u32(drv, sid, kAudioStreamPropertyDirection) == (UInt32)(i == 0 ? 1 : 0),
              i == 0 ? "input direction" : "output direction");
    }

    printf("\n[loopback IO]\n");
    CHECK(vt->StartIO(drv, 2, 1) == noErr, "StartIO");
    const UInt32 frames = 512;
    float wbuf[512 * 2], rbuf[512 * 2];
    AudioServerPlugInIOCycleInfo cycle;
    memset(&cycle, 0, sizeof(cycle));
    cycle.mNominalIOBufferFrameSize = frames;

    int mismatches = 0;
    for (int c = 0; c < 12; c++) {
        float base = (float)(c * 1000);
        for (UInt32 i = 0; i < frames; i++) {
            wbuf[i * 2 + 0] = base + i;
            wbuf[i * 2 + 1] = -base - i;
        }
        OSStatus st1 = vt->DoIOOperation(drv, 2, 4, 1, kAudioServerPlugInIOOperationWriteMix,
                                          frames, &cycle, wbuf, NULL);
        memset(rbuf, 0, sizeof rbuf);
        OSStatus st2 = vt->DoIOOperation(drv, 2, 3, 1, kAudioServerPlugInIOOperationReadInput,
                                          frames, &cycle, rbuf, NULL);
        if (st1 != noErr || st2 != noErr) { mismatches++; continue; }
        if (c >= 2) {
            float expect = (float)((c - 2) * 1000);
            if (rbuf[0] != expect || rbuf[(frames - 1) * 2] != expect + frames - 1 ||
                rbuf[1] != -expect) mismatches++;
        }
    }
    CHECK(mismatches == 0, "ring delivers data two buffers late, sample exact");
    vt->StopIO(drv, 2, 1);
    CHECK(get_u32(drv, 2, kAudioDevicePropertyDeviceIsRunning) == 0, "stopped");

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
