#include "OpenSoundDriver.h"
#include "OpenSoundShared.h"
#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioHardwareBase.h>
#include <CoreAudio/AudioServerPlugIn.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define kObjectID_PlugIn 1
#define kObjectID_Box 2
#define kObjectID_Device 3
#define kObjectID_Stream_Output 4
#define kObjectID_Volume_Output 5
#define kObjectID_Mute_Output 6

#define kBoxUID "de.easonwong.OpenSound.VirtualAudioRouterBox"
#define kBoxName "Virtual Audio Router"
#define kDeviceUID "de.easonwong.OpenSound.VirtualAudioRouter"
#define kDeviceModelUID "de.easonwong.OpenSound.VirtualAudioRouterModel"
#define kDeviceName "Virtual Audio Router"
#define kManufacturerName "OpenSound"
#define kDriverBundleID "de.easonwong.OpenSound.driver"

#define kZeroTimeStampPeriod 16384

static AudioServerPlugInHostRef gHostRef = NULL;
static UInt32 gRefCount = 0;
static Float64 gSampleRate = 48000.0;
static Float32 gVolume = 1.0f;
static Boolean gMute = false;
static Boolean gBoxAcquired = true;
static UInt32 gBufferFrameSize = 512;
static UInt32 gStartIOCount = 0;

static OpenSoundSharedMemory* gSharedMemory = NULL;

static mach_timebase_info_data_t gTimebaseInfo;
static UInt64 gAnchorHostTime = 0;

static void OpenSoundDriver_SetupSharedMemory(void) {
    if (gSharedMemory != NULL) {
        return;
    }
    int fd = shm_open(kOpenSoundShmName, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        return;
    }
    fchmod(fd, 0666);
    ftruncate(fd, sizeof(OpenSoundSharedMemory));
    void* addr = mmap(NULL, sizeof(OpenSoundSharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (addr != MAP_FAILED) {
        gSharedMemory = (OpenSoundSharedMemory*)addr;
        gSharedMemory->sampleRate = (uint32_t)gSampleRate;
        gSharedMemory->channels = 2;
    }
}

// MARK: - COM Interface Declarations

static HRESULT OpenSoundDriver_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface);
static ULONG OpenSoundDriver_AddRef(void* inDriver);
static ULONG OpenSoundDriver_Release(void* inDriver);
static OSStatus OpenSoundDriver_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus OpenSoundDriver_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID);
static OSStatus OpenSoundDriver_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
static OSStatus OpenSoundDriver_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus OpenSoundDriver_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus OpenSoundDriver_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo);
static OSStatus OpenSoundDriver_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo);
static Boolean OpenSoundDriver_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus OpenSoundDriver_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable);
static OSStatus OpenSoundDriver_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus OpenSoundDriver_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);
static OSStatus OpenSoundDriver_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData);
static OSStatus OpenSoundDriver_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus OpenSoundDriver_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus OpenSoundDriver_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed);
static OSStatus OpenSoundDriver_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace);
static OSStatus OpenSoundDriver_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo);
static OSStatus OpenSoundDriver_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer);
static OSStatus OpenSoundDriver_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo);

static AudioServerPlugInDriverInterface gDriverInterface = {
    NULL,
    OpenSoundDriver_QueryInterface,
    OpenSoundDriver_AddRef,
    OpenSoundDriver_Release,
    OpenSoundDriver_Initialize,
    OpenSoundDriver_CreateDevice,
    OpenSoundDriver_DestroyDevice,
    OpenSoundDriver_AddDeviceClient,
    OpenSoundDriver_RemoveDeviceClient,
    OpenSoundDriver_PerformDeviceConfigurationChange,
    OpenSoundDriver_AbortDeviceConfigurationChange,
    OpenSoundDriver_HasProperty,
    OpenSoundDriver_IsPropertySettable,
    OpenSoundDriver_GetPropertyDataSize,
    OpenSoundDriver_GetPropertyData,
    OpenSoundDriver_SetPropertyData,
    OpenSoundDriver_StartIO,
    OpenSoundDriver_StopIO,
    OpenSoundDriver_GetZeroTimeStamp,
    OpenSoundDriver_WillDoIOOperation,
    OpenSoundDriver_BeginIOOperation,
    OpenSoundDriver_DoIOOperation,
    OpenSoundDriver_EndIOOperation
};

static AudioServerPlugInDriverInterface* gDriverInterfacePtr = &gDriverInterface;

// MARK: - Factory & COM Lifecycle

void* OpenSoundDriver_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID) {
    if (!CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) {
        return NULL;
    }
    mach_timebase_info(&gTimebaseInfo);
    OpenSoundDriver_AddRef(&gDriverInterfacePtr);
    return &gDriverInterfacePtr;
}

static HRESULT OpenSoundDriver_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface) {
    CFUUIDRef requestedUUID = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    if (CFEqual(requestedUUID, IUnknownUUID) || CFEqual(requestedUUID, kAudioServerPlugInDriverInterfaceUUID)) {
        OpenSoundDriver_AddRef(inDriver);
        *outInterface = inDriver;
        CFRelease(requestedUUID);
        return S_OK;
    }
    CFRelease(requestedUUID);
    *outInterface = NULL;
    return E_NOINTERFACE;
}

static ULONG OpenSoundDriver_AddRef(void* inDriver) {
    return ++gRefCount;
}

static ULONG OpenSoundDriver_Release(void* inDriver) {
    if (gRefCount > 0) {
        --gRefCount;
    }
    return gRefCount;
}

// MARK: - Driver Management

static OSStatus OpenSoundDriver_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    gHostRef = inHost;
    mach_timebase_info(&gTimebaseInfo);
    gAnchorHostTime = mach_absolute_time();
    OpenSoundDriver_SetupSharedMemory();
    return kAudioHardwareNoError;
}

static OSStatus OpenSoundDriver_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID) {
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus OpenSoundDriver_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID) {
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus OpenSoundDriver_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo) {
    return kAudioHardwareNoError;
}

static OSStatus OpenSoundDriver_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo) {
    return kAudioHardwareNoError;
}

static OSStatus OpenSoundDriver_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo) {
    return kAudioHardwareNoError;
}

static OSStatus OpenSoundDriver_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo) {
    return kAudioHardwareNoError;
}

// MARK: - Properties

static Boolean OpenSoundDriver_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress) {
    if (inObjectID == kObjectID_PlugIn) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyManufacturer:
            case kAudioObjectPropertyOwnedObjects:
            case kAudioPlugInPropertyBundleID:
            case kAudioPlugInPropertyDeviceList:
            case kAudioPlugInPropertyTranslateUIDToDevice:
            case kAudioPlugInPropertyBoxList:
            case kAudioPlugInPropertyTranslateUIDToBox:
            case kAudioPlugInPropertyResourceBundle:
                return true;
            default:
                return false;
        }
    } else if (inObjectID == kObjectID_Box) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyModelName:
            case kAudioObjectPropertyManufacturer:
            case kAudioObjectPropertyOwnedObjects:
            case kAudioObjectPropertyIdentify:
            case kAudioObjectPropertySerialNumber:
            case kAudioObjectPropertyFirmwareVersion:
            case kAudioBoxPropertyBoxUID:
            case kAudioBoxPropertyTransportType:
            case kAudioBoxPropertyHasAudio:
            case kAudioBoxPropertyHasVideo:
            case kAudioBoxPropertyHasMIDI:
            case kAudioBoxPropertyIsProtected:
            case kAudioBoxPropertyAcquired:
            case kAudioBoxPropertyAcquisitionFailed:
            case kAudioBoxPropertyDeviceList:
            case kAudioBoxPropertyClockDeviceList:
                return true;
            default:
                return false;
        }
    } else if (inObjectID == kObjectID_Device) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyManufacturer:
            case kAudioObjectPropertyOwnedObjects:
            case kAudioObjectPropertyControlList:
            case kAudioDevicePropertyDeviceUID:
            case kAudioDevicePropertyModelUID:
            case kAudioDevicePropertyTransportType:
            case kAudioDevicePropertyRelatedDevices:
            case kAudioDevicePropertyClockDomain:
            case kAudioDevicePropertyStreams:
            case kAudioDevicePropertyNominalSampleRate:
            case kAudioDevicePropertyAvailableNominalSampleRates:
            case kAudioDevicePropertyDeviceIsAlive:
            case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceIsRunningSomewhere:
            case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            case kAudioDevicePropertySafetyOffset:
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertyBufferFrameSize:
            case kAudioDevicePropertyBufferFrameSizeRange:
            case kAudioDevicePropertyPreferredChannelsForStereo:
            case kAudioDevicePropertyZeroTimeStampPeriod:
            case kAudioDevicePropertyClockAlgorithm:
            case kAudioDevicePropertyClockIsStable:
                return true;
            default:
                return false;
        }
    } else if (inObjectID == kObjectID_Stream_Output) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioObjectPropertyOwnedObjects:
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
            default:
                return false;
        }
    } else if (inObjectID == kObjectID_Volume_Output) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioObjectPropertyOwnedObjects:
            case kAudioControlPropertyScope:
            case kAudioControlPropertyElement:
            case kAudioLevelControlPropertyScalarValue:
            case kAudioLevelControlPropertyDecibelValue:
            case kAudioLevelControlPropertyDecibelRange:
                return true;
            default:
                return false;
        }
    } else if (inObjectID == kObjectID_Mute_Output) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioObjectPropertyOwnedObjects:
            case kAudioControlPropertyScope:
            case kAudioControlPropertyElement:
            case kAudioBooleanControlPropertyValue:
                return true;
            default:
                return false;
        }
    }
    return false;
}

static OSStatus OpenSoundDriver_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable) {
    if (inObjectID == kObjectID_Box) {
        if (inAddress->mSelector == kAudioBoxPropertyAcquired) {
            *outIsSettable = true;
            return kAudioHardwareNoError;
        }
    } else if (inObjectID == kObjectID_Device) {
        if (inAddress->mSelector == kAudioDevicePropertyNominalSampleRate ||
            inAddress->mSelector == kAudioDevicePropertyBufferFrameSize) {
            *outIsSettable = true;
            return kAudioHardwareNoError;
        }
    } else if (inObjectID == kObjectID_Volume_Output) {
        if (inAddress->mSelector == kAudioLevelControlPropertyScalarValue) {
            *outIsSettable = true;
            return kAudioHardwareNoError;
        }
    } else if (inObjectID == kObjectID_Mute_Output) {
        if (inAddress->mSelector == kAudioBooleanControlPropertyValue) {
            *outIsSettable = true;
            return kAudioHardwareNoError;
        }
    }
    *outIsSettable = false;
    return kAudioHardwareNoError;
}

static OSStatus OpenSoundDriver_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize) {
    if (inObjectID == kObjectID_PlugIn) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyManufacturer:
            case kAudioPlugInPropertyBundleID:
            case kAudioPlugInPropertyResourceBundle:
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioPlugInPropertyBoxList:
            case kAudioPlugInPropertyTranslateUIDToBox:
            case kAudioPlugInPropertyDeviceList:
            case kAudioObjectPropertyOwnedObjects:
            case kAudioPlugInPropertyTranslateUIDToDevice:
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    } else if (inObjectID == kObjectID_Box) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyModelName:
            case kAudioObjectPropertyManufacturer:
            case kAudioObjectPropertySerialNumber:
            case kAudioObjectPropertyFirmwareVersion:
            case kAudioBoxPropertyBoxUID:
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects:
            case kAudioBoxPropertyDeviceList:
                *outDataSize = gBoxAcquired ? sizeof(AudioObjectID) : 0;
                return kAudioHardwareNoError;
            case kAudioBoxPropertyClockDeviceList:
                *outDataSize = 0;
                return kAudioHardwareNoError;
            case kAudioObjectPropertyIdentify:
            case kAudioBoxPropertyTransportType:
            case kAudioBoxPropertyHasAudio:
            case kAudioBoxPropertyHasVideo:
            case kAudioBoxPropertyHasMIDI:
            case kAudioBoxPropertyIsProtected:
            case kAudioBoxPropertyAcquired:
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioBoxPropertyAcquisitionFailed:
                *outDataSize = sizeof(OSStatus);
                return kAudioHardwareNoError;
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    } else if (inObjectID == kObjectID_Device) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyManufacturer:
            case kAudioDevicePropertyDeviceUID:
            case kAudioDevicePropertyModelUID:
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyTransportType:
            case kAudioDevicePropertyClockDomain:
            case kAudioDevicePropertyDeviceIsAlive:
            case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceIsRunningSomewhere:
            case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            case kAudioDevicePropertySafetyOffset:
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertyBufferFrameSize:
            case kAudioDevicePropertyZeroTimeStampPeriod:
            case kAudioDevicePropertyClockAlgorithm:
            case kAudioDevicePropertyClockIsStable:
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyRelatedDevices:
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyStreams:
                if (inAddress->mScope == kAudioObjectPropertyScopeInput) {
                    *outDataSize = 0;
                } else {
                    *outDataSize = sizeof(AudioObjectID);
                }
                return kAudioHardwareNoError;
            case kAudioObjectPropertyControlList:
                if (inAddress->mScope == kAudioObjectPropertyScopeInput) {
                    *outDataSize = 0;
                } else {
                    *outDataSize = sizeof(AudioObjectID) * 2;
                }
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects:
                *outDataSize = sizeof(AudioObjectID) * 3;
                return kAudioHardwareNoError;
            case kAudioDevicePropertyPreferredChannelsForStereo:
                *outDataSize = sizeof(UInt32) * 2;
                return kAudioHardwareNoError;
            case kAudioDevicePropertyNominalSampleRate:
                *outDataSize = sizeof(Float64);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyAvailableNominalSampleRates:
            case kAudioDevicePropertyBufferFrameSizeRange:
                *outDataSize = sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    } else if (inObjectID == kObjectID_Stream_Output) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects:
                *outDataSize = 0;
                return kAudioHardwareNoError;
            case kAudioStreamPropertyIsActive:
            case kAudioStreamPropertyDirection:
            case kAudioStreamPropertyTerminalType:
            case kAudioStreamPropertyStartingChannel:
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
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    } else if (inObjectID == kObjectID_Volume_Output) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects:
                *outDataSize = 0;
                return kAudioHardwareNoError;
            case kAudioControlPropertyScope:
                *outDataSize = sizeof(AudioObjectPropertyScope);
                return kAudioHardwareNoError;
            case kAudioControlPropertyElement:
                *outDataSize = sizeof(AudioObjectPropertyElement);
                return kAudioHardwareNoError;
            case kAudioLevelControlPropertyScalarValue:
            case kAudioLevelControlPropertyDecibelValue:
                *outDataSize = sizeof(Float32);
                return kAudioHardwareNoError;
            case kAudioLevelControlPropertyDecibelRange:
                *outDataSize = sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    } else if (inObjectID == kObjectID_Mute_Output) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects:
                *outDataSize = 0;
                return kAudioHardwareNoError;
            case kAudioControlPropertyScope:
                *outDataSize = sizeof(AudioObjectPropertyScope);
                return kAudioHardwareNoError;
            case kAudioControlPropertyElement:
                *outDataSize = sizeof(AudioObjectPropertyElement);
                return kAudioHardwareNoError;
            case kAudioBooleanControlPropertyValue:
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus OpenSoundDriver_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    if (inObjectID == kObjectID_PlugIn) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
                *(AudioClassID*)outData = kAudioObjectClassID;
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyClass:
                *(AudioClassID*)outData = kAudioPlugInClassID;
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner:
                *(AudioObjectID*)outData = kAudioObjectUnknown;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyName:
                *(CFStringRef*)outData = CFStringCreateWithCString(NULL, "OpenSound Plug-in", kCFStringEncodingUTF8);
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyManufacturer:
                *(CFStringRef*)outData = CFStringCreateWithCString(NULL, kManufacturerName, kCFStringEncodingUTF8);
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioPlugInPropertyBundleID:
                *(CFStringRef*)outData = CFStringCreateWithCString(NULL, kDriverBundleID, kCFStringEncodingUTF8);
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioPlugInPropertyResourceBundle:
                *(CFStringRef*)outData = CFStringCreateWithCString(NULL, "", kCFStringEncodingUTF8);
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioPlugInPropertyBoxList:
            case kAudioObjectPropertyOwnedObjects:
                *(AudioObjectID*)outData = kObjectID_Box;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioPlugInPropertyTranslateUIDToBox:
                if (inQualifierData && inQualifierDataSize >= sizeof(CFStringRef)) {
                    CFStringRef uid = *(CFStringRef*)inQualifierData;
                    if (CFStringCompare(uid, CFSTR(kBoxUID), 0) == kCFCompareEqualTo) {
                        *(AudioObjectID*)outData = kObjectID_Box;
                        *outDataSize = sizeof(AudioObjectID);
                        return kAudioHardwareNoError;
                    }
                }
                *(AudioObjectID*)outData = kAudioObjectUnknown;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioPlugInPropertyDeviceList:
                *(AudioObjectID*)outData = kObjectID_Device;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioPlugInPropertyTranslateUIDToDevice:
                if (inQualifierData && inQualifierDataSize >= sizeof(CFStringRef)) {
                    CFStringRef uid = *(CFStringRef*)inQualifierData;
                    if (CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo) {
                        *(AudioObjectID*)outData = kObjectID_Device;
                        *outDataSize = sizeof(AudioObjectID);
                        return kAudioHardwareNoError;
                    }
                }
                *(AudioObjectID*)outData = kAudioObjectUnknown;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    } else if (inObjectID == kObjectID_Box) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
                *(AudioClassID*)outData = kAudioObjectClassID;
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyClass:
                *(AudioClassID*)outData = kAudioBoxClassID;
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner:
                *(AudioObjectID*)outData = kObjectID_PlugIn;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyModelName:
                *(CFStringRef*)outData = CFStringCreateWithCString(NULL, kBoxName, kCFStringEncodingUTF8);
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyManufacturer:
                *(CFStringRef*)outData = CFStringCreateWithCString(NULL, kManufacturerName, kCFStringEncodingUTF8);
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioObjectPropertySerialNumber:
                *(CFStringRef*)outData = CFStringCreateWithCString(NULL, "OS-VAR-001", kCFStringEncodingUTF8);
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyFirmwareVersion:
                *(CFStringRef*)outData = CFStringCreateWithCString(NULL, "1.0.0", kCFStringEncodingUTF8);
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioBoxPropertyBoxUID:
                *(CFStringRef*)outData = CFStringCreateWithCString(NULL, kBoxUID, kCFStringEncodingUTF8);
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioBoxPropertyTransportType:
                *(UInt32*)outData = kAudioDeviceTransportTypeVirtual;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioBoxPropertyHasAudio:
                *(UInt32*)outData = 1;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioBoxPropertyHasVideo:
            case kAudioBoxPropertyHasMIDI:
            case kAudioBoxPropertyIsProtected:
            case kAudioObjectPropertyIdentify:
                *(UInt32*)outData = 0;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioBoxPropertyAcquired:
                *(UInt32*)outData = gBoxAcquired ? 1 : 0;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioBoxPropertyAcquisitionFailed:
                *(OSStatus*)outData = kAudioHardwareNoError;
                *outDataSize = sizeof(OSStatus);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects:
            case kAudioBoxPropertyDeviceList:
                if (gBoxAcquired) {
                    *(AudioObjectID*)outData = kObjectID_Device;
                    *outDataSize = sizeof(AudioObjectID);
                } else {
                    *outDataSize = 0;
                }
                return kAudioHardwareNoError;
            case kAudioBoxPropertyClockDeviceList:
                *outDataSize = 0;
                return kAudioHardwareNoError;
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    } else if (inObjectID == kObjectID_Device) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
                *(AudioClassID*)outData = kAudioObjectClassID;
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyClass:
                *(AudioClassID*)outData = kAudioDeviceClassID;
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner:
                *(AudioObjectID*)outData = kObjectID_Box;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyName:
                *(CFStringRef*)outData = CFStringCreateWithCString(NULL, kDeviceName, kCFStringEncodingUTF8);
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyManufacturer:
                *(CFStringRef*)outData = CFStringCreateWithCString(NULL, kManufacturerName, kCFStringEncodingUTF8);
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyDeviceUID:
                *(CFStringRef*)outData = CFStringCreateWithCString(NULL, kDeviceUID, kCFStringEncodingUTF8);
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyModelUID:
                *(CFStringRef*)outData = CFStringCreateWithCString(NULL, kDeviceModelUID, kCFStringEncodingUTF8);
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyTransportType:
                *(UInt32*)outData = kAudioDeviceTransportTypeVirtual;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyRelatedDevices:
                *(AudioObjectID*)outData = kObjectID_Device;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyClockDomain:
                *(UInt32*)outData = 0;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyStreams:
                if (inAddress->mScope == kAudioObjectPropertyScopeInput) {
                    *outDataSize = 0;
                } else {
                    *(AudioObjectID*)outData = kObjectID_Stream_Output;
                    *outDataSize = sizeof(AudioObjectID);
                }
                return kAudioHardwareNoError;
            case kAudioObjectPropertyControlList: {
                if (inAddress->mScope == kAudioObjectPropertyScopeInput) {
                    *outDataSize = 0;
                } else {
                    UInt32 maxControls = inDataSize / (UInt32)sizeof(AudioObjectID);
                    AudioObjectID* controls = (AudioObjectID*)outData;
                    if (maxControls > 0) {
                        controls[0] = kObjectID_Volume_Output;
                    }
                    if (maxControls > 1) {
                        controls[1] = kObjectID_Mute_Output;
                    }
                    *outDataSize = (maxControls >= 2 ? 2 : maxControls) * (UInt32)sizeof(AudioObjectID);
                }
                return kAudioHardwareNoError;
            }
            case kAudioObjectPropertyOwnedObjects: {
                UInt32 maxObjects = inDataSize / (UInt32)sizeof(AudioObjectID);
                AudioObjectID* objects = (AudioObjectID*)outData;
                AudioObjectID allOwned[3] = {
                    kObjectID_Stream_Output,
                    kObjectID_Volume_Output,
                    kObjectID_Mute_Output
                };
                UInt32 count = (maxObjects >= 3) ? 3 : maxObjects;
                for (UInt32 i = 0; i < count; ++i) {
                    objects[i] = allOwned[i];
                }
                *outDataSize = count * (UInt32)sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyPreferredChannelsForStereo: {
                UInt32* channels = (UInt32*)outData;
                channels[0] = 1;
                channels[1] = 2;
                *outDataSize = sizeof(UInt32) * 2;
                return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyNominalSampleRate:
                *(Float64*)outData = gSampleRate;
                *outDataSize = sizeof(Float64);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyAvailableNominalSampleRates: {
                AudioValueRange* range = (AudioValueRange*)outData;
                range->mMinimum = 48000.0;
                range->mMaximum = 48000.0;
                *outDataSize = sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyDeviceIsAlive:
                *(UInt32*)outData = 1;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceIsRunningSomewhere:
                *(UInt32*)outData = (gStartIOCount > 0) ? 1 : 0;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                *(UInt32*)outData = 1;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertySafetyOffset:
            case kAudioDevicePropertyLatency:
                *(UInt32*)outData = 0;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyBufferFrameSize:
                *(UInt32*)outData = gBufferFrameSize;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyBufferFrameSizeRange: {
                AudioValueRange* range = (AudioValueRange*)outData;
                range->mMinimum = 64;
                range->mMaximum = 4096;
                *outDataSize = sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyZeroTimeStampPeriod:
                *(UInt32*)outData = kZeroTimeStampPeriod;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyClockAlgorithm:
                *(UInt32*)outData = kAudioDeviceClockAlgorithmSimpleIIR;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyClockIsStable:
                *(UInt32*)outData = 1;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    } else if (inObjectID == kObjectID_Stream_Output) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
                *(AudioClassID*)outData = kAudioObjectClassID;
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyClass:
                *(AudioClassID*)outData = kAudioStreamClassID;
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner:
                *(AudioObjectID*)outData = kObjectID_Device;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects:
                *outDataSize = 0;
                return kAudioHardwareNoError;
            case kAudioStreamPropertyIsActive:
                *(UInt32*)outData = 1;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioStreamPropertyDirection:
                *(UInt32*)outData = 0;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioStreamPropertyTerminalType:
                *(UInt32*)outData = kAudioStreamTerminalTypeSpeaker;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioStreamPropertyStartingChannel:
                *(UInt32*)outData = 1;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioStreamPropertyLatency:
                *(UInt32*)outData = 0;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: {
                AudioStreamBasicDescription* format = (AudioStreamBasicDescription*)outData;
                format->mSampleRate = gSampleRate;
                format->mFormatID = kAudioFormatLinearPCM;
                format->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
                format->mBytesPerPacket = 8;
                format->mFramesPerPacket = 1;
                format->mBytesPerFrame = 8;
                format->mChannelsPerFrame = 2;
                format->mBitsPerChannel = 32;
                *outDataSize = sizeof(AudioStreamBasicDescription);
                return kAudioHardwareNoError;
            }
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: {
                AudioStreamRangedDescription* ranged = (AudioStreamRangedDescription*)outData;
                ranged->mFormat.mSampleRate = gSampleRate;
                ranged->mFormat.mFormatID = kAudioFormatLinearPCM;
                ranged->mFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
                ranged->mFormat.mBytesPerPacket = 8;
                ranged->mFormat.mFramesPerPacket = 1;
                ranged->mFormat.mBytesPerFrame = 8;
                ranged->mFormat.mChannelsPerFrame = 2;
                ranged->mFormat.mBitsPerChannel = 32;
                ranged->mSampleRateRange.mMinimum = 48000.0;
                ranged->mSampleRateRange.mMaximum = 48000.0;
                *outDataSize = sizeof(AudioStreamRangedDescription);
                return kAudioHardwareNoError;
            }
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    } else if (inObjectID == kObjectID_Volume_Output) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
                *(AudioClassID*)outData = kAudioControlClassID;
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyClass:
                *(AudioClassID*)outData = kAudioVolumeControlClassID;
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner:
                *(AudioObjectID*)outData = kObjectID_Device;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects:
                *outDataSize = 0;
                return kAudioHardwareNoError;
            case kAudioControlPropertyScope:
                *(AudioObjectPropertyScope*)outData = kAudioObjectPropertyScopeOutput;
                *outDataSize = sizeof(AudioObjectPropertyScope);
                return kAudioHardwareNoError;
            case kAudioControlPropertyElement:
                *(AudioObjectPropertyElement*)outData = kAudioObjectPropertyElementMain;
                *outDataSize = sizeof(AudioObjectPropertyElement);
                return kAudioHardwareNoError;
            case kAudioLevelControlPropertyScalarValue:
                *(Float32*)outData = gVolume;
                *outDataSize = sizeof(Float32);
                return kAudioHardwareNoError;
            case kAudioLevelControlPropertyDecibelValue:
                *(Float32*)outData = (gVolume > 0.0f) ? (20.0f * log10f(gVolume)) : -96.0f;
                *outDataSize = sizeof(Float32);
                return kAudioHardwareNoError;
            case kAudioLevelControlPropertyDecibelRange: {
                AudioValueRange* range = (AudioValueRange*)outData;
                range->mMinimum = -96.0;
                range->mMaximum = 0.0;
                *outDataSize = sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            }
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    } else if (inObjectID == kObjectID_Mute_Output) {
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
                *(AudioClassID*)outData = kAudioControlClassID;
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyClass:
                *(AudioClassID*)outData = kAudioMuteControlClassID;
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner:
                *(AudioObjectID*)outData = kObjectID_Device;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects:
                *outDataSize = 0;
                return kAudioHardwareNoError;
            case kAudioControlPropertyScope:
                *(AudioObjectPropertyScope*)outData = kAudioObjectPropertyScopeOutput;
                *outDataSize = sizeof(AudioObjectPropertyScope);
                return kAudioHardwareNoError;
            case kAudioControlPropertyElement:
                *(AudioObjectPropertyElement*)outData = kAudioObjectPropertyElementMain;
                *outDataSize = sizeof(AudioObjectPropertyElement);
                return kAudioHardwareNoError;
            case kAudioBooleanControlPropertyValue:
                *(UInt32*)outData = gMute ? 1 : 0;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus OpenSoundDriver_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData) {
    if (inObjectID == kObjectID_Box && inAddress->mSelector == kAudioBoxPropertyAcquired) {
        if (inDataSize >= sizeof(UInt32)) {
            Boolean newAcquired = (*(const UInt32*)inData != 0);
            if (gBoxAcquired != newAcquired) {
                gBoxAcquired = newAcquired;
                if (gHostRef && gHostRef->PropertiesChanged) {
                    AudioObjectPropertyAddress addresses[2] = {
                        { kAudioBoxPropertyAcquired, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                        { kAudioBoxPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain }
                    };
                    gHostRef->PropertiesChanged(gHostRef, kObjectID_Box, 2, addresses);
                }
            }
            return kAudioHardwareNoError;
        }
    } else if (inObjectID == kObjectID_Device && inAddress->mSelector == kAudioDevicePropertyBufferFrameSize) {
        if (inDataSize >= sizeof(UInt32)) {
            gBufferFrameSize = *(const UInt32*)inData;
            return kAudioHardwareNoError;
        }
    } else if (inObjectID == kObjectID_Volume_Output && inAddress->mSelector == kAudioLevelControlPropertyScalarValue) {
        if (inDataSize >= sizeof(Float32)) {
            gVolume = *(const Float32*)inData;
            return kAudioHardwareNoError;
        }
    } else if (inObjectID == kObjectID_Mute_Output && inAddress->mSelector == kAudioBooleanControlPropertyValue) {
        if (inDataSize >= sizeof(UInt32)) {
            gMute = (*(const UInt32*)inData != 0);
            return kAudioHardwareNoError;
        }
    }
    return kAudioHardwareNoError;
}

// MARK: - Real-Time Audio IO Loopback

static OSStatus OpenSoundDriver_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) {
    gStartIOCount++;
    return kAudioHardwareNoError;
}

static OSStatus OpenSoundDriver_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) {
    if (gStartIOCount > 0) {
        gStartIOCount--;
    }
    return kAudioHardwareNoError;
}

static OSStatus OpenSoundDriver_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
    UInt64 currentHostTime = mach_absolute_time();
    UInt64 elapsedHostTicks = currentHostTime - gAnchorHostTime;
    Float64 elapsedHostNanos = (Float64)(elapsedHostTicks * gTimebaseInfo.numer) / (Float64)gTimebaseInfo.denom;
    Float64 elapsedSamples = elapsedHostNanos * gSampleRate / 1000000000.0;

    UInt64 period = kZeroTimeStampPeriod;
    UInt64 iteration = (UInt64)(elapsedSamples / (Float64)period);

    *outSampleTime = (Float64)(iteration * period);
    Float64 nanosAtZeroTimeStamp = (*outSampleTime / gSampleRate) * 1000000000.0;
    UInt64 hostTicksAtZeroTimeStamp = (UInt64)((nanosAtZeroTimeStamp * (Float64)gTimebaseInfo.denom) / (Float64)gTimebaseInfo.numer);

    *outHostTime = gAnchorHostTime + hostTicksAtZeroTimeStamp;
    *outSeed = 1;
    return kAudioHardwareNoError;
}

static OSStatus OpenSoundDriver_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace) {
    if (inOperationID == kAudioServerPlugInIOOperationWriteMix) {
        *outWillDo = true;
        *outWillDoInPlace = true;
        return kAudioHardwareNoError;
    }
    *outWillDo = false;
    *outWillDoInPlace = true;
    return kAudioHardwareNoError;
}

static OSStatus OpenSoundDriver_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo) {
    return kAudioHardwareNoError;
}

static OSStatus OpenSoundDriver_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer) {
    if (!ioMainBuffer) {
        return kAudioHardwareNoError;
    }

    if (inOperationID == kAudioServerPlugInIOOperationWriteMix) {
        if (!gSharedMemory) {
            OpenSoundDriver_SetupSharedMemory();
        }
        if (gSharedMemory) {
            UInt32 sampleCount = inIOBufferFrameSize * 2;
            const float* floatBuffer = (const float*)ioMainBuffer;
            uint32_t writeHead = atomic_load(&gSharedMemory->writeHead);
            for (UInt32 i = 0; i < sampleCount; ++i) {
                gSharedMemory->buffer[(writeHead + i) % kRingBufferSize] = floatBuffer[i];
            }
            atomic_store(&gSharedMemory->writeHead, (writeHead + sampleCount) % kRingBufferSize);
        }
    }

    return kAudioHardwareNoError;
}

static OSStatus OpenSoundDriver_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo) {
    return kAudioHardwareNoError;
}
