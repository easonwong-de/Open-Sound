#include "OpenSoundAudioDriver.h"
#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/OSLog.h>

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "OpenSoundAudioDriver: " fmt, ##__VA_ARGS__)

bool OpenSoundAudioDriver::init()
{
    if (!super::init()) {
        return false;
    }
    return true;
}

void OpenSoundAudioDriver::free()
{
    super::free();
}

kern_return_t IMPL(OpenSoundAudioDriver, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    Log("OpenSound Virtual Audio Device started successfully.");
    return kIOReturnSuccess;
}

kern_return_t IMPL(OpenSoundAudioDriver, Stop)
{
    Log("OpenSound Virtual Audio Device stopping.");
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t IMPL(OpenSoundAudioDriver, StartIOProc)
{
    Log("StartIOProc called.");
    return StartIOProc(proc, SUPERDISPATCH);
}

kern_return_t IMPL(OpenSoundAudioDriver, StopIOProc)
{
    Log("StopIOProc called.");
    return StopIOProc(proc, SUPERDISPATCH);
}
