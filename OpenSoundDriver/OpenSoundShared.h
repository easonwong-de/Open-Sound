#ifndef OpenSoundShared_h
#define OpenSoundShared_h

#include <stdint.h>

#define kOpenSoundShmName "/opensound_audio_ring"
#define kRingBufferSize 131072

typedef struct {
    _Atomic uint32_t writeHead;
    uint32_t sampleRate;
    uint32_t channels;
    uint32_t reserved;
    float buffer[kRingBufferSize];
} OpenSoundSharedMemory;

#endif /* OpenSoundShared_h */
