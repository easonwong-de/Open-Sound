import CoreAudio
import Darwin
import Foundation
import os.log

private let kOpenSoundShmName = "/opensound_audio_ring"
private let kRingBufferSize: Int = 131072

private struct OpenSoundSharedHeader {
    var writeHead: UInt32
    var sampleRate: UInt32
    var channels: UInt32
    var reserved: UInt32
}

/// Encapsulates runtime routing state for an individual physical audio output target.
private final class DestinationDeviceContext {
    let deviceID: AudioObjectID
    var procID: AudioDeviceIOProcID?
    var isRunning: Bool = false
    var volume: Float = 1.0
    var isMuted: Bool = false
    var readHead: Int = 0

    /// Initialises a destination context for an audio endpoint.
    /// - Parameters:
    ///   - deviceID: The destination audio device object ID.
    ///   - procID: The associated Core Audio IO procedure identifier.
    ///   - volume: Initial volume factor.
    ///   - isMuted: Initial mute status.
    init(deviceID: AudioObjectID, procID: AudioDeviceIOProcID? = nil, volume: Float = 1.0, isMuted: Bool = false) {
        self.deviceID = deviceID
        self.procID = procID
        self.volume = volume
        self.isMuted = isMuted
    }
}

/// Real-time audio routing engine broadcasting from driver shared memory to physical destinations.
final class AudioRouterEngine: @unchecked Sendable {
    static let shared = AudioRouterEngine()
    private let logger = Logger(subsystem: "de.easonwong.OpenSound", category: "AudioRouterEngine")

    private let destinationLock = NSLock()
    private var destinations: [AudioObjectID: DestinationDeviceContext] = [:]

    nonisolated(unsafe) private var shmPointer: UnsafeMutableRawPointer?
    private let totalShmSize = MemoryLayout<OpenSoundSharedHeader>.size + (kRingBufferSize * MemoryLayout<Float>.size)

    private init() {
        setupSharedMemory()
    }

    deinit {
        if let ptr = shmPointer {
            munmap(ptr, totalShmSize)
        }
    }

    private typealias ShmOpenFunction = @convention(c) (UnsafePointer<CChar>, CInt, mode_t) -> CInt

    private func openSharedMemory(name: String, flags: CInt, mode: mode_t) -> CInt {
        guard let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "shm_open") else {
            return -1
        }
        let fn = unsafeBitCast(sym, to: ShmOpenFunction.self)
        return name.withCString { fn($0, flags, mode) }
    }

    /// Attaches to the POSIX shared memory ring buffer managed by the HAL driver.
    private func setupSharedMemory() {
        let fd = openSharedMemory(name: kOpenSoundShmName, flags: O_CREAT | O_RDWR, mode: 0o666)
        guard fd >= 0 else {
            logger.error("Failed to open shared memory: \(errno)")
            return
        }
        fchmod(fd, 0o666)
        ftruncate(fd, off_t(totalShmSize))
        let addr = mmap(nil, totalShmSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
        close(fd)
        if addr != MAP_FAILED, let validAddr = addr {
            shmPointer = validAddr
            logger.info("Successfully mapped OpenSound shared memory ring buffer")
        } else {
            logger.error("Failed to map shared memory ring buffer")
        }
    }

    /// Informs the engine of virtual device discovery.
    /// - Parameter deviceID: The audio device object ID, or `nil` to clear.
    func setVirtualDevice(deviceID: AudioObjectID?) {
        if shmPointer == nil {
            setupSharedMemory()
        }
    }

    /// Enables or disables routing to a specific physical output endpoint.
    /// - Parameters:
    ///   - deviceID: The physical audio device object ID.
    ///   - enabled: Whether routing should be active.
    func setRouteEnabled(for deviceID: AudioObjectID, enabled: Bool) {
        destinationLock.lock()
        defer { destinationLock.unlock() }

        if enabled {
            guard destinations[deviceID] == nil else { return }
            let context = DestinationDeviceContext(deviceID: deviceID)
            if let ptr = shmPointer {
                let writeHead = ptr.load(as: UInt32.self)
                context.readHead = Int(writeHead)
            }
            destinations[deviceID] = context
            startDestinationOutput(context: context)
        } else {
            guard let context = destinations.removeValue(forKey: deviceID) else { return }
            stopDestinationOutput(context: context)
        }
    }

    /// Updates the volume gain factor applied to an output destination.
    /// - Parameters:
    ///   - deviceID: The physical audio device object ID.
    ///   - volume: Linear gain factor in range `0.0...1.0`.
    func setVolume(for deviceID: AudioObjectID, volume: Float) {
        destinationLock.lock()
        defer { destinationLock.unlock() }
        destinations[deviceID]?.volume = max(0.0, min(1.0, volume))
    }

    /// Toggles the mute state for an output destination.
    /// - Parameters:
    ///   - deviceID: The physical audio device object ID.
    ///   - isMuted: `true` to mute output; `false` to unmute.
    func setMuted(for deviceID: AudioObjectID, isMuted: Bool) {
        destinationLock.lock()
        defer { destinationLock.unlock() }
        destinations[deviceID]?.isMuted = isMuted
    }

    /// Begins output playback on a specific destination device.
    /// - Parameter context: The destination runtime context.
    private func startDestinationOutput(context: DestinationDeviceContext) {
        let outputCallback: AudioDeviceIOProc = { _, _, _, _, outOutputData, _, inClientData in
            guard let inClientData else { return noErr }
            let context = Unmanaged<DestinationDeviceContext>.fromOpaque(inClientData).takeUnretainedValue()
            AudioRouterEngine.shared.handleOutputAudio(context: context, bufferList: outOutputData)
            return noErr
        }

        let refCon = UnsafeMutableRawPointer(Unmanaged.passUnretained(context).toOpaque())
        let status = AudioDeviceCreateIOProcID(context.deviceID, outputCallback, refCon, &context.procID)

        if status == noErr, let procID = context.procID {
            let startStatus = AudioDeviceStart(context.deviceID, procID)
            if startStatus == noErr {
                context.isRunning = true
                logger.info("Started audio output routing to physical device \(context.deviceID)")
            } else {
                logger.error("Failed to start physical output device \(context.deviceID): \(startStatus)")
            }
        } else {
            logger.error("Failed to create IO proc for physical device \(context.deviceID): \(status)")
        }
    }

    /// Stops output playback on a specific destination device.
    /// - Parameter context: The destination runtime context.
    private func stopDestinationOutput(context: DestinationDeviceContext) {
        guard let procID = context.procID, context.isRunning else { return }

        AudioDeviceStop(context.deviceID, procID)
        AudioDeviceDestroyIOProcID(context.deviceID, procID)
        context.procID = nil
        context.isRunning = false
        logger.info("Stopped audio output routing to physical device \(context.deviceID)")
    }

    /// Halts all active output routes.
    func stopAllRoutes() {
        destinationLock.lock()
        let contexts = Array(destinations.values)
        destinations.removeAll()
        destinationLock.unlock()

        for context in contexts {
            stopDestinationOutput(context: context)
        }
    }

    /// Reads buffered PCM frames from driver shared memory and populates destination buffers.
    /// - Parameters:
    ///   - context: The destination runtime context.
    ///   - bufferList: Target audio buffer list to populate.
    private func handleOutputAudio(context: DestinationDeviceContext, bufferList: UnsafeMutablePointer<AudioBufferList>?) {
        guard let bufferList, let ptr = shmPointer else { return }
        let buffers = UnsafeMutableAudioBufferListPointer(bufferList)
        let volume = context.isMuted ? 0.0 : context.volume
        let bufferBase = ptr.advanced(by: MemoryLayout<OpenSoundSharedHeader>.size).assumingMemoryBound(to: Float.self)

        let writeHead = Int(ptr.load(as: UInt32.self))
        let available = (writeHead - context.readHead + kRingBufferSize) % kRingBufferSize

        for buffer in buffers {
            guard let mData = buffer.mData else { continue }
            let floatBuffer = mData.assumingMemoryBound(to: Float.self)
            let sampleCount = Int(buffer.mDataByteSize) / MemoryLayout<Float>.size

            if available > kRingBufferSize - sampleCount {
                context.readHead = (writeHead - sampleCount + kRingBufferSize) % kRingBufferSize
            }

            let samplesToRead = min(sampleCount, available)

            for i in 0 ..< samplesToRead {
                let sample = bufferBase[(context.readHead + i) % kRingBufferSize]
                floatBuffer[i] = sample * volume
            }

            for i in samplesToRead ..< sampleCount {
                floatBuffer[i] = 0.0
            }

            context.readHead = (context.readHead + samplesToRead) % kRingBufferSize
        }
    }
}
