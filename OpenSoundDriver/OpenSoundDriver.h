#ifndef OpenSoundDriver_h
#define OpenSoundDriver_h

#include <CoreAudio/AudioServerPlugIn.h>

#if defined(__cplusplus)
extern "C" {
#endif

/// Factory function called by macOS Core Audio to instantiate the plug-in driver interface.
/// - Parameter inAllocator: CFAllocatorRef used for object allocation.
/// - Parameter inRequestedTypeUUID: Requested COM interface UUID.
/// - Returns: Pointer to the initialized AudioServerPlugInDriverInterface structure.
__attribute__((visibility("default")))
void* OpenSoundDriver_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);

#if defined(__cplusplus)
}
#endif

#endif /* OpenSoundDriver_h */
