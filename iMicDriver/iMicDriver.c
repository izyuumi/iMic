/*
 *  iMicDriver.c
 *  iMicDriver – CoreAudio AudioServerPlugin virtual input device
 *
 *  Reads audio from an mmap'd ring buffer at /tmp/imic_audio_buffer
 *  and presents it as a mono Float32 48 kHz input device.
 */

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdatomic.h>
#include <os/log.h>

/* ──────────────────────────────────────────────────────────────────────
   Constants
   ────────────────────────────────────────────────────────────────────── */

#define kPlugInObjectID             ((AudioObjectID)1)   /* kAudioObjectPlugInObject */
#define kDeviceObjectID             ((AudioObjectID)2)
#define kStreamObjectID             ((AudioObjectID)3)

#define kSampleRate                 48000.0
#define kBufferFrameSize            512
#define kRingBufferFrameCount       65536
#define kZeroTimeStampPeriod        kRingBufferFrameCount

#define kRingBufferHeaderSize       16                       /* 2 x UInt64 */
#define kRingBufferDataSize         (kRingBufferFrameCount * sizeof(Float32))
#define kRingBufferTotalSize        (kRingBufferHeaderSize + kRingBufferDataSize)

#define kRingBufferPath             "/tmp/imic_audio_buffer"

/*
 * These properties are defined in AudioHardware.h (client-side) but not in
 * AudioHardwareBase.h which is all that AudioServerPlugIn.h pulls in.
 * The HAL still queries them from plugins, so we define them here by fourCC.
 */
#ifndef kAudioDevicePropertyBufferFrameSize
#define kAudioDevicePropertyBufferFrameSize       'fsiz'
#endif
#ifndef kAudioDevicePropertyBufferFrameSizeRange
#define kAudioDevicePropertyBufferFrameSizeRange   'fsz#'
#endif

static const char *kDeviceName          = "iMic";
static const char *kDeviceUID           = "iMic-VirtualDevice";
static const char *kDeviceModelUID      = "iMic-Model";
static const char *kManufacturerName    = "yumi.to";

/* ──────────────────────────────────────────────────────────────────────
   Ring-buffer header layout (shared with the writer process)
   ────────────────────────────────────────────────────────────────────── */

typedef struct {
    volatile UInt64 writeIndex;   /* offset 0  – updated by writer */
    volatile UInt64 readIndex;    /* offset 8  – updated by reader (us) */
} RingBufferHeader;

/* ──────────────────────────────────────────────────────────────────────
   Driver state  (singleton – one instance per plugin load)
   ────────────────────────────────────────────────────────────────────── */

typedef struct {
    AudioServerPlugInDriverInterface   *vtablePtr;   /* first field: single-indirection pointer */
    AudioServerPlugInDriverInterface    vtable;       /* the actual vtable struct */
    AudioServerPlugInHostRef            host;
    UInt32                              refCount;

    /* IO state */
    Boolean     ioIsRunning;
    UInt64      anchorHostTime;
    UInt64      tickCount;         /* incremented every ZeroTimeStamp call */

    /* mmap ring buffer */
    void       *ringBufferMap;     /* mmap pointer (NULL when not mapped) */
    int         ringBufferFD;
} iMicDriver;

static iMicDriver gDriver;

/* ──────────────────────────────────────────────────────────────────────
   Helpers
   ────────────────────────────────────────────────────────────────────── */

static os_log_t imic_log(void)
{
    static os_log_t log = NULL;
    if (!log) log = os_log_create("to.yumi.iMicDriver", "Driver");
    return log;
}

#define LOG_ERR(fmt, ...)  os_log_error(imic_log(), fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) os_log_info(imic_log(),  fmt, ##__VA_ARGS__)

static void mapRingBuffer(void)
{
    if (gDriver.ringBufferMap) return;

    int fd = open(kRingBufferPath, O_RDWR);
    if (fd < 0) return;

    void *map = mmap(NULL, kRingBufferTotalSize, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }

    gDriver.ringBufferFD  = fd;
    gDriver.ringBufferMap = map;
    LOG_INFO("Ring buffer mapped at %p", map);
}

static void unmapRingBuffer(void)
{
    if (gDriver.ringBufferMap) {
        munmap(gDriver.ringBufferMap, kRingBufferTotalSize);
        gDriver.ringBufferMap = NULL;
    }
    if (gDriver.ringBufferFD >= 0) {
        close(gDriver.ringBufferFD);
        gDriver.ringBufferFD = -1;
    }
}

/* ──────────────────────────────────────────────────────────────────────
   Format helpers
   ────────────────────────────────────────────────────────────────────── */

static AudioStreamBasicDescription makeStreamFormat(void)
{
    AudioStreamBasicDescription fmt = {0};
    fmt.mSampleRate       = kSampleRate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat
                          | kAudioFormatFlagsNativeEndian
                          | kAudioFormatFlagIsPacked;
    fmt.mBitsPerChannel   = 32;
    fmt.mChannelsPerFrame = 1;
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerFrame    = sizeof(Float32);
    fmt.mBytesPerPacket   = sizeof(Float32);
    return fmt;
}

/* ──────────────────────────────────────────────────────────────────────
   IUnknown
   ────────────────────────────────────────────────────────────────────── */

static HRESULT iMic_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface)
{
    CFUUIDRef requestedUUID = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    CFUUIDRef pluginTypeUUID = CFUUIDGetConstantUUIDWithBytes(NULL,
        0x44,0x3A,0xBA,0xB8, 0xE7,0xB3, 0x49,0x1A,
        0xB9,0x85, 0xBE,0xB9,0x18,0x70,0x30,0xDB);
    CFUUIDRef iUnknownUUID = CFUUIDGetConstantUUIDWithBytes(NULL,
        0x00,0x00,0x00,0x00, 0x00,0x00, 0x00,0x00,
        0xC0,0x00, 0x00,0x00,0x00,0x00,0x00,0x46);

    Boolean match = CFEqual(requestedUUID, pluginTypeUUID) ||
                    CFEqual(requestedUUID, iUnknownUUID);
    CFRelease(requestedUUID);

    if (!match) {
        *outInterface = NULL;
        return E_NOINTERFACE;
    }

    gDriver.refCount++;
    *outInterface = &gDriver.vtablePtr;
    return S_OK;
}

static ULONG iMic_AddRef(void *inDriver)
{
    return ++gDriver.refCount;
}

static ULONG iMic_Release(void *inDriver)
{
    if (gDriver.refCount > 0) gDriver.refCount--;
    return gDriver.refCount;
}

/* ──────────────────────────────────────────────────────────────────────
   Initialize
   ────────────────────────────────────────────────────────────────────── */

static OSStatus iMic_Initialize(AudioServerPlugInDriverRef inDriver,
                                AudioServerPlugInHostRef inHost)
{
    gDriver.host = inHost;
    LOG_INFO("iMicDriver initialized");
    return kAudioHardwareNoError;
}

/* ──────────────────────────────────────────────────────────────────────
   CreateDevice / DestroyDevice (not used – we publish statically)
   ────────────────────────────────────────────────────────────────────── */

static OSStatus iMic_CreateDevice(AudioServerPlugInDriverRef inDriver,
                                  CFDictionaryRef inDescription,
                                  const AudioServerPlugInClientInfo *inClientInfo,
                                  AudioObjectID *outDeviceObjectID)
{
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus iMic_DestroyDevice(AudioServerPlugInDriverRef inDriver,
                                   AudioObjectID inDeviceObjectID)
{
    return kAudioHardwareUnsupportedOperationError;
}

/* ──────────────────────────────────────────────────────────────────────
   Client management
   ────────────────────────────────────────────────────────────────────── */

static OSStatus iMic_AddDeviceClient(AudioServerPlugInDriverRef inDriver,
                                     AudioObjectID inDeviceObjectID,
                                     const AudioServerPlugInClientInfo *inClientInfo)
{
    return kAudioHardwareNoError;
}

static OSStatus iMic_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver,
                                        AudioObjectID inDeviceObjectID,
                                        const AudioServerPlugInClientInfo *inClientInfo)
{
    return kAudioHardwareNoError;
}

/* ──────────────────────────────────────────────────────────────────────
   Configuration change
   ────────────────────────────────────────────────────────────────────── */

static OSStatus iMic_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                      AudioObjectID inDeviceObjectID,
                                                      UInt64 inChangeAction,
                                                      void *inChangeInfo)
{
    return kAudioHardwareNoError;
}

static OSStatus iMic_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                    AudioObjectID inDeviceObjectID,
                                                    UInt64 inChangeAction,
                                                    void *inChangeInfo)
{
    return kAudioHardwareNoError;
}

/* ──────────────────────────────────────────────────────────────────────
   Property helpers
   ────────────────────────────────────────────────────────────────────── */

static Boolean objectOwnsProperty(AudioObjectID inObjectID,
                                  const AudioObjectPropertyAddress *inAddress)
{
    switch (inObjectID) {
        case kPlugInObjectID:
        case kDeviceObjectID:
        case kStreamObjectID:
            return true;
        default:
            return false;
    }
}

/* ──────────────────────────────────────────────────────────────────────
   HasProperty
   ────────────────────────────────────────────────────────────────────── */

static Boolean iMic_HasProperty(AudioServerPlugInDriverRef inDriver,
                                AudioObjectID inObjectID,
                                pid_t inClientPID,
                                const AudioObjectPropertyAddress *inAddress)
{
    switch (inObjectID)
    {
        /* ── PlugIn ── */
        case kPlugInObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyManufacturer:
                case kAudioPlugInPropertyDeviceList:
                case kAudioPlugInPropertyTranslateUIDToDevice:
                case kAudioPlugInPropertyResourceBundle:
                    return true;
            }
            break;

        /* ── Device ── */
        case kDeviceObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyManufacturer:
                case kAudioObjectPropertyName:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioDevicePropertyDeviceUID:
                case kAudioDevicePropertyModelUID:
                case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyDeviceIsRunning:
                case kAudioDevicePropertyStreams:
                case kAudioDevicePropertyNominalSampleRate:
                case kAudioDevicePropertyAvailableNominalSampleRates:
                case kAudioDevicePropertyLatency:
                case kAudioDevicePropertySafetyOffset:
                case kAudioDevicePropertyTransportType:
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                case kAudioDevicePropertyBufferFrameSize:
                case kAudioDevicePropertyBufferFrameSizeRange:
                case kAudioDevicePropertyZeroTimeStampPeriod:
                case kAudioDevicePropertyClockDomain:
                    return true;
            }
            break;

        /* ── Stream ── */
        case kStreamObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioStreamPropertyIsActive:
                case kAudioStreamPropertyDirection:
                case kAudioStreamPropertyTerminalType:
                case kAudioStreamPropertyStartingChannel:
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat:
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats:
                case kAudioStreamPropertyLatency:
                    return true;
            }
            break;
    }
    return false;
}

/* ──────────────────────────────────────────────────────────────────────
   IsPropertySettable
   ────────────────────────────────────────────────────────────────────── */

static OSStatus iMic_IsPropertySettable(AudioServerPlugInDriverRef inDriver,
                                        AudioObjectID inObjectID,
                                        pid_t inClientPID,
                                        const AudioObjectPropertyAddress *inAddress,
                                        Boolean *outIsSettable)
{
    *outIsSettable = false;
    return kAudioHardwareNoError;
}

/* ──────────────────────────────────────────────────────────────────────
   GetPropertyDataSize
   ────────────────────────────────────────────────────────────────────── */

static OSStatus iMic_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver,
                                         AudioObjectID inObjectID,
                                         pid_t inClientPID,
                                         const AudioObjectPropertyAddress *inAddress,
                                         UInt32 inQualifierDataSize,
                                         const void *inQualifierData,
                                         UInt32 *outDataSize)
{
    switch (inObjectID)
    {
        /* ── PlugIn ── */
        case kPlugInObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID);
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyManufacturer:
                    *outDataSize = sizeof(CFStringRef);
                    return kAudioHardwareNoError;
                case kAudioPlugInPropertyDeviceList:
                    *outDataSize = sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                case kAudioPlugInPropertyTranslateUIDToDevice:
                    *outDataSize = sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                case kAudioPlugInPropertyResourceBundle:
                    *outDataSize = sizeof(CFStringRef);
                    return kAudioHardwareNoError;
            }
            break;

        /* ── Device ── */
        case kDeviceObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID);
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyManufacturer:
                case kAudioObjectPropertyName:
                case kAudioDevicePropertyDeviceUID:
                case kAudioDevicePropertyModelUID:
                    *outDataSize = sizeof(CFStringRef);
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyOwnedObjects:
                case kAudioDevicePropertyStreams:
                    /* Only report stream for input scope */
                    if (inAddress->mScope == kAudioObjectPropertyScopeGlobal ||
                        inAddress->mScope == kAudioObjectPropertyScopeInput) {
                        *outDataSize = sizeof(AudioObjectID);
                    } else {
                        *outDataSize = 0;
                    }
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyDeviceIsRunning:
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                    *outDataSize = sizeof(UInt32);
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyNominalSampleRate:
                    *outDataSize = sizeof(Float64);
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyAvailableNominalSampleRates:
                    *outDataSize = sizeof(AudioValueRange);
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyLatency:
                case kAudioDevicePropertySafetyOffset:
                case kAudioDevicePropertyBufferFrameSize:
                case kAudioDevicePropertyZeroTimeStampPeriod:
                case kAudioDevicePropertyClockDomain:
                    *outDataSize = sizeof(UInt32);
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyBufferFrameSizeRange:
                    *outDataSize = sizeof(AudioValueRange);
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyTransportType:
                    *outDataSize = sizeof(UInt32);
                    return kAudioHardwareNoError;
            }
            break;

        /* ── Stream ── */
        case kStreamObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID);
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                case kAudioStreamPropertyIsActive:
                case kAudioStreamPropertyDirection:
                case kAudioStreamPropertyTerminalType:
                case kAudioStreamPropertyStartingChannel:
                    *outDataSize = sizeof(UInt32);
                    return kAudioHardwareNoError;
                case kAudioStreamPropertyLatency:
                    *outDataSize = sizeof(UInt32);
                    return kAudioHardwareNoError;
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat:
                    *outDataSize = sizeof(AudioStreamBasicDescription);
                    return kAudioHardwareNoError;
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats:
                    *outDataSize = sizeof(AudioStreamRangedDescription);
                    return kAudioHardwareNoError;
            }
            break;
    }

    return kAudioHardwareUnknownPropertyError;
}

/* ──────────────────────────────────────────────────────────────────────
   GetPropertyData
   ────────────────────────────────────────────────────────────────────── */

static OSStatus iMic_GetPropertyData(AudioServerPlugInDriverRef inDriver,
                                     AudioObjectID inObjectID,
                                     pid_t inClientPID,
                                     const AudioObjectPropertyAddress *inAddress,
                                     UInt32 inQualifierDataSize,
                                     const void *inQualifierData,
                                     UInt32 inDataSize,
                                     UInt32 *outDataSize,
                                     void *outData)
{
    switch (inObjectID)
    {
        /* ── PlugIn ── */
        case kPlugInObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    *outDataSize = sizeof(AudioClassID);
                    *((AudioClassID *)outData) = kAudioObjectClassID;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID);
                    *((AudioClassID *)outData) = kAudioPlugInClassID;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID);
                    *((AudioObjectID *)outData) = kAudioObjectUnknown;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyManufacturer:
                    *outDataSize = sizeof(CFStringRef);
                    *((CFStringRef *)outData) = CFSTR("yumi.to");
                    return kAudioHardwareNoError;

                case kAudioPlugInPropertyDeviceList:
                    *outDataSize = sizeof(AudioObjectID);
                    *((AudioObjectID *)outData) = kDeviceObjectID;
                    return kAudioHardwareNoError;

                case kAudioPlugInPropertyTranslateUIDToDevice: {
                    CFStringRef uid = *((CFStringRef *)inQualifierData);
                    *outDataSize = sizeof(AudioObjectID);
                    if (uid && CFStringCompare(uid, CFSTR("iMic-VirtualDevice"),
                                               0) == kCFCompareEqualTo) {
                        *((AudioObjectID *)outData) = kDeviceObjectID;
                    } else {
                        *((AudioObjectID *)outData) = kAudioObjectUnknown;
                    }
                    return kAudioHardwareNoError;
                }

                case kAudioPlugInPropertyResourceBundle:
                    *outDataSize = sizeof(CFStringRef);
                    *((CFStringRef *)outData) = CFSTR("");
                    return kAudioHardwareNoError;
            }
            break;

        /* ── Device ── */
        case kDeviceObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    *outDataSize = sizeof(AudioClassID);
                    *((AudioClassID *)outData) = kAudioObjectClassID;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID);
                    *((AudioClassID *)outData) = kAudioDeviceClassID;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID);
                    *((AudioObjectID *)outData) = kPlugInObjectID;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyManufacturer:
                    *outDataSize = sizeof(CFStringRef);
                    *((CFStringRef *)outData) = CFSTR("yumi.to");
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyName:
                    *outDataSize = sizeof(CFStringRef);
                    *((CFStringRef *)outData) = CFSTR("iMic");
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyDeviceUID:
                    *outDataSize = sizeof(CFStringRef);
                    *((CFStringRef *)outData) = CFSTR("iMic-VirtualDevice");
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyModelUID:
                    *outDataSize = sizeof(CFStringRef);
                    *((CFStringRef *)outData) = CFSTR("iMic-Model");
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyOwnedObjects:
                case kAudioDevicePropertyStreams:
                    if (inAddress->mScope == kAudioObjectPropertyScopeGlobal ||
                        inAddress->mScope == kAudioObjectPropertyScopeInput) {
                        *outDataSize = sizeof(AudioObjectID);
                        *((AudioObjectID *)outData) = kStreamObjectID;
                    } else {
                        *outDataSize = 0;
                    }
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyDeviceIsAlive:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 1;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyDeviceIsRunning:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = gDriver.ioIsRunning ? 1 : 0;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyNominalSampleRate:
                    *outDataSize = sizeof(Float64);
                    *((Float64 *)outData) = kSampleRate;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyAvailableNominalSampleRates: {
                    *outDataSize = sizeof(AudioValueRange);
                    AudioValueRange range = { kSampleRate, kSampleRate };
                    memcpy(outData, &range, sizeof(range));
                    return kAudioHardwareNoError;
                }

                case kAudioDevicePropertyLatency:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 0;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertySafetyOffset:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 0;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyTransportType:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = kAudioDeviceTransportTypeVirtual;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                    *outDataSize = sizeof(UInt32);
                    if (inAddress->mScope == kAudioObjectPropertyScopeInput ||
                        inAddress->mScope == kAudioObjectPropertyScopeGlobal) {
                        *((UInt32 *)outData) = 1;
                    } else {
                        *((UInt32 *)outData) = 0;
                    }
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 0;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyBufferFrameSize:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = kBufferFrameSize;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyBufferFrameSizeRange: {
                    *outDataSize = sizeof(AudioValueRange);
                    AudioValueRange range = { kBufferFrameSize, kBufferFrameSize };
                    memcpy(outData, &range, sizeof(range));
                    return kAudioHardwareNoError;
                }

                case kAudioDevicePropertyZeroTimeStampPeriod:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = kZeroTimeStampPeriod;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyClockDomain:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 0;
                    return kAudioHardwareNoError;
            }
            break;

        /* ── Stream ── */
        case kStreamObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    *outDataSize = sizeof(AudioClassID);
                    *((AudioClassID *)outData) = kAudioObjectClassID;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID);
                    *((AudioClassID *)outData) = kAudioStreamClassID;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID);
                    *((AudioObjectID *)outData) = kDeviceObjectID;
                    return kAudioHardwareNoError;

                case kAudioStreamPropertyIsActive:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 1;
                    return kAudioHardwareNoError;

                case kAudioStreamPropertyDirection:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 1;  /* 1 = input */
                    return kAudioHardwareNoError;

                case kAudioStreamPropertyTerminalType:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = kAudioStreamTerminalTypeMicrophone;
                    return kAudioHardwareNoError;

                case kAudioStreamPropertyStartingChannel:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 1;
                    return kAudioHardwareNoError;

                case kAudioStreamPropertyLatency:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 0;
                    return kAudioHardwareNoError;

                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat: {
                    *outDataSize = sizeof(AudioStreamBasicDescription);
                    AudioStreamBasicDescription fmt = makeStreamFormat();
                    memcpy(outData, &fmt, sizeof(fmt));
                    return kAudioHardwareNoError;
                }

                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats: {
                    *outDataSize = sizeof(AudioStreamRangedDescription);
                    AudioStreamRangedDescription ranged = {0};
                    ranged.mFormat = makeStreamFormat();
                    ranged.mSampleRateRange.mMinimum = kSampleRate;
                    ranged.mSampleRateRange.mMaximum = kSampleRate;
                    memcpy(outData, &ranged, sizeof(ranged));
                    return kAudioHardwareNoError;
                }
            }
            break;
    }

    return kAudioHardwareUnknownPropertyError;
}

/* ──────────────────────────────────────────────────────────────────────
   SetPropertyData
   ────────────────────────────────────────────────────────────────────── */

static OSStatus iMic_SetPropertyData(AudioServerPlugInDriverRef inDriver,
                                     AudioObjectID inObjectID,
                                     pid_t inClientPID,
                                     const AudioObjectPropertyAddress *inAddress,
                                     UInt32 inQualifierDataSize,
                                     const void *inQualifierData,
                                     UInt32 inDataSize,
                                     const void *inData)
{
    /* All properties are read-only in this implementation */
    return kAudioHardwareUnsupportedOperationError;
}

/* ──────────────────────────────────────────────────────────────────────
   StartIO / StopIO
   ────────────────────────────────────────────────────────────────────── */

static OSStatus iMic_StartIO(AudioServerPlugInDriverRef inDriver,
                             AudioObjectID inDeviceObjectID,
                             UInt32 inClientID)
{
    if (inDeviceObjectID != kDeviceObjectID)
        return kAudioHardwareBadObjectError;

    gDriver.ioIsRunning    = true;
    gDriver.tickCount      = 0;
    gDriver.anchorHostTime = mach_absolute_time();

    mapRingBuffer();

    LOG_INFO("StartIO – IO started");
    return kAudioHardwareNoError;
}

static OSStatus iMic_StopIO(AudioServerPlugInDriverRef inDriver,
                            AudioObjectID inDeviceObjectID,
                            UInt32 inClientID)
{
    if (inDeviceObjectID != kDeviceObjectID)
        return kAudioHardwareBadObjectError;

    gDriver.ioIsRunning = false;

    unmapRingBuffer();

    LOG_INFO("StopIO – IO stopped");
    return kAudioHardwareNoError;
}

/* ──────────────────────────────────────────────────────────────────────
   GetZeroTimeStamp
   ────────────────────────────────────────────────────────────────────── */

static OSStatus iMic_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver,
                                      AudioObjectID inDeviceObjectID,
                                      UInt32 inClientID,
                                      Float64 *outSampleTime,
                                      UInt64 *outHostTime,
                                      UInt64 *outSeed)
{
    if (inDeviceObjectID != kDeviceObjectID)
        return kAudioHardwareBadObjectError;

    /*
     * We synthesise a clock: every kZeroTimeStampPeriod sample frames
     * we return a new zero-time-stamp.  We use mach_absolute_time to
     * keep the host-time advancing monotonically.
     */
    static mach_timebase_info_data_t sTimebase = {0};
    if (sTimebase.denom == 0) mach_timebase_info(&sTimebase);

    /* How many nanoseconds per kZeroTimeStampPeriod frames at kSampleRate */
    Float64 periodNanos = (Float64)kZeroTimeStampPeriod / kSampleRate * 1e9;

    /* Convert nanos → host ticks */
    UInt64 periodHostTicks = (UInt64)(periodNanos * (Float64)sTimebase.denom
                                                  / (Float64)sTimebase.numer);

    /*
     * Figure out which tick we are on now, based on how much wall-clock
     * time has passed since anchorHostTime.
     */
    UInt64 now = mach_absolute_time();
    UInt64 elapsed = now - gDriver.anchorHostTime;
    UInt64 currentTick = elapsed / periodHostTicks;

    *outSampleTime = (Float64)(currentTick * kZeroTimeStampPeriod);
    *outHostTime   = gDriver.anchorHostTime + currentTick * periodHostTicks;
    *outSeed       = 1;

    return kAudioHardwareNoError;
}

/* ──────────────────────────────────────────────────────────────────────
   DoIOOperation  – read from ring buffer
   ────────────────────────────────────────────────────────────────────── */

static OSStatus iMic_DoIOOperation(AudioServerPlugInDriverRef inDriver,
                                   AudioObjectID inDeviceObjectID,
                                   AudioObjectID inStreamObjectID,
                                   UInt32 inClientID,
                                   UInt32 inOperationID,
                                   UInt32 inIOBufferFrameSize,
                                   const AudioServerPlugInIOCycleInfo *inIOCycleInfo,
                                   void *ioMainBuffer,
                                   void *ioSecondaryBuffer)
{
    if (inOperationID != kAudioServerPlugInIOOperationReadInput)
        return kAudioHardwareNoError;

    Float32 *dst = (Float32 *)ioMainBuffer;

    /* If ring buffer isn't mapped, try once more, else silence */
    if (!gDriver.ringBufferMap) {
        mapRingBuffer();
    }

    if (!gDriver.ringBufferMap) {
        /* Output silence */
        memset(dst, 0, inIOBufferFrameSize * sizeof(Float32));
        return kAudioHardwareNoError;
    }

    RingBufferHeader *header = (RingBufferHeader *)gDriver.ringBufferMap;
    Float32 *samples = (Float32 *)((UInt8 *)gDriver.ringBufferMap + kRingBufferHeaderSize);

    /* Atomic read of writeIndex */
    UInt64 writeIdx = __atomic_load_n(&header->writeIndex, __ATOMIC_ACQUIRE);
    UInt64 readIdx  = __atomic_load_n(&header->readIndex,  __ATOMIC_ACQUIRE);

    UInt64 available = writeIdx - readIdx;
    UInt32 framesToRead = inIOBufferFrameSize;

    if (available < framesToRead) {
        /* Not enough data – read what we can, zero-fill the rest */
        UInt32 avail32 = (UInt32)available;
        for (UInt32 i = 0; i < avail32; i++) {
            UInt32 idx = (UInt32)((readIdx + i) % kRingBufferFrameCount);
            dst[i] = samples[idx];
        }
        memset(dst + avail32, 0, (framesToRead - avail32) * sizeof(Float32));
        readIdx += avail32;
    } else {
        for (UInt32 i = 0; i < framesToRead; i++) {
            UInt32 idx = (UInt32)((readIdx + i) % kRingBufferFrameCount);
            dst[i] = samples[idx];
        }
        readIdx += framesToRead;
    }

    /* Update readIndex atomically */
    __atomic_store_n(&header->readIndex, readIdx, __ATOMIC_RELEASE);

    return kAudioHardwareNoError;
}

/* ──────────────────────────────────────────────────────────────────────
   BeginIOOperation / EndIOOperation  (no-ops)
   ────────────────────────────────────────────────────────────────────── */

static OSStatus iMic_BeginIOOperation(AudioServerPlugInDriverRef inDriver,
                                      AudioObjectID inDeviceObjectID,
                                      UInt32 inClientID,
                                      UInt32 inOperationID,
                                      UInt32 inIOBufferFrameSize,
                                      const AudioServerPlugInIOCycleInfo *inIOCycleInfo)
{
    return kAudioHardwareNoError;
}

static OSStatus iMic_EndIOOperation(AudioServerPlugInDriverRef inDriver,
                                    AudioObjectID inDeviceObjectID,
                                    UInt32 inClientID,
                                    UInt32 inOperationID,
                                    UInt32 inIOBufferFrameSize,
                                    const AudioServerPlugInIOCycleInfo *inIOCycleInfo)
{
    return kAudioHardwareNoError;
}

/* ──────────────────────────────────────────────────────────────────────
   Factory entry point  –  called by CoreAudio HAL
   ────────────────────────────────────────────────────────────────────── */

void *iMicDriver_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID)
{
    /* Verify this is the AudioServerPlugIn type UUID */
    CFUUIDRef pluginTypeUUID = CFUUIDGetConstantUUIDWithBytes(NULL,
        0x44,0x3A,0xBA,0xB8, 0xE7,0xB3, 0x49,0x1A,
        0xB9,0x85, 0xBE,0xB9,0x18,0x70,0x30,0xDB);

    if (!CFEqual(inRequestedTypeUUID, pluginTypeUUID))
        return NULL;

    /* Populate vtable */
    gDriver.vtable.QueryInterface                    = iMic_QueryInterface;
    gDriver.vtable.AddRef                            = iMic_AddRef;
    gDriver.vtable.Release                           = iMic_Release;
    gDriver.vtable.Initialize                        = iMic_Initialize;
    gDriver.vtable.CreateDevice                      = iMic_CreateDevice;
    gDriver.vtable.DestroyDevice                     = iMic_DestroyDevice;
    gDriver.vtable.AddDeviceClient                   = iMic_AddDeviceClient;
    gDriver.vtable.RemoveDeviceClient                = iMic_RemoveDeviceClient;
    gDriver.vtable.PerformDeviceConfigurationChange  = iMic_PerformDeviceConfigurationChange;
    gDriver.vtable.AbortDeviceConfigurationChange    = iMic_AbortDeviceConfigurationChange;
    gDriver.vtable.HasProperty                       = iMic_HasProperty;
    gDriver.vtable.IsPropertySettable                = iMic_IsPropertySettable;
    gDriver.vtable.GetPropertyDataSize               = iMic_GetPropertyDataSize;
    gDriver.vtable.GetPropertyData                   = iMic_GetPropertyData;
    gDriver.vtable.SetPropertyData                   = iMic_SetPropertyData;
    gDriver.vtable.StartIO                           = iMic_StartIO;
    gDriver.vtable.StopIO                            = iMic_StopIO;
    gDriver.vtable.GetZeroTimeStamp                  = iMic_GetZeroTimeStamp;
    gDriver.vtable.DoIOOperation                     = iMic_DoIOOperation;
    gDriver.vtable.BeginIOOperation                  = iMic_BeginIOOperation;
    gDriver.vtable.EndIOOperation                    = iMic_EndIOOperation;

    /* Set up double-indirection (COM-style interface pointer) */
    gDriver.vtablePtr = &gDriver.vtable;

    gDriver.refCount      = 1;
    gDriver.ioIsRunning   = false;
    gDriver.ringBufferMap = NULL;
    gDriver.ringBufferFD  = -1;
    gDriver.anchorHostTime = 0;
    gDriver.tickCount      = 0;

    LOG_INFO("iMicDriver_Create – plugin instantiated");
    return &gDriver.vtablePtr;
}
