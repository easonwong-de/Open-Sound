import AVFoundation
import CoreAudio
import Foundation
import os.log

/// Encapsulates runtime routing state for an individual physical audio output target.
private final class DestinationDeviceContext {
    let deviceID: AudioObjectID
    var procID: AudioDeviceIOProcID?
    var isRunning: Bool = false
    var volume: Float = 1.0
    var isMuted: Bool = false

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

/// Real-time audio routing engine managing virtual input capture and multi-destination broadcasting.
final class AudioRouterEngine: @unchecked Sendable {
    static let shared = AudioRouterEngine()
    private let logger = Logger(subsystem: "de.easonwong.OpenSound", category: "AudioRouterEngine")

    private var virtualInputDeviceID: AudioObjectID?
    private var virtualInputProcID: AudioDeviceIOProcID?
    private var isCapturing: Bool = false

    private let destinationLock = NSLock()
    private var destinations: [AudioObjectID: DestinationDeviceContext] = [:]

    private let ringBufferSize: Int = 131072
    private var ringBuffer: [Float]
    private var writeHead: Int = 0
    private var readHead: Int = 0
    private let ringBufferLock = NSLock()

    private init() {
        ringBuffer = [Float](repeating: 0.0, count: 131072)
    }

    /// Sets the source virtual audio device for loopback capture.
    /// - Parameter deviceID: The audio device object ID, or `nil` to clear.
    func setVirtualDevice(deviceID: AudioObjectID?) {
        guard virtualInputDeviceID != deviceID else { return }

        stopCapture()
        virtualInputDeviceID = deviceID
        if deviceID != nil {
            startCapture()
        }
    }

    /// Starts capturing PCM audio frames from the virtual input stream.
    func startCapture() {
        guard let deviceID = virtualInputDeviceID, !isCapturing else { return }

        let captureCallback: AudioDeviceIOProc = { _, _, inInputData, _, _, _, inClientData in
            guard let inClientData else { return noErr }
            let engine = Unmanaged<AudioRouterEngine>.fromOpaque(inClientData).takeUnretainedValue()
            engine.handleInputAudio(bufferList: inInputData, frameCount: 512)
            return noErr
        }

        let refCon = UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque())
        let status = AudioDeviceCreateIOProcID(deviceID, captureCallback, refCon, &virtualInputProcID)

        if status == noErr, let procID = virtualInputProcID {
            let startStatus = AudioDeviceStart(deviceID, procID)
            if startStatus == noErr {
                isCapturing = true
                logger.info("Started virtual audio capture on device \(deviceID)")
            } else {
                logger.error("Failed to start audio device IO proc: \(startStatus)")
            }
        } else {
            logger.error("Failed to create IO Proc ID for virtual input device: \(status)")
        }
    }

    /// Stops capturing audio from the virtual input stream.
    func stopCapture() {
        guard let deviceID = virtualInputDeviceID, let procID = virtualInputProcID, isCapturing else { return }

        AudioDeviceStop(deviceID, procID)
        AudioDeviceDestroyIOProcID(deviceID, procID)
        virtualInputProcID = nil
        isCapturing = false
        logger.info("Stopped virtual audio capture")
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

    /// Halts all active output routes and tears down capture procedures.
    func stopAllRoutes() {
        destinationLock.lock()
        let contexts = Array(destinations.values)
        destinations.removeAll()
        destinationLock.unlock()

        for context in contexts {
            stopDestinationOutput(context: context)
        }
        stopCapture()
    }

    /// Ingests captured raw PCM frames into the internal ring buffer.
    /// - Parameters:
    ///   - bufferList: Pointer to incoming audio buffer list.
    ///   - frameCount: Number of audio frames.
    private func handleInputAudio(bufferList: UnsafePointer<AudioBufferList>?, frameCount: UInt32) {
        guard let bufferList else { return }
        let buffers = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: bufferList))

        for buffer in buffers {
            guard let mData = buffer.mData else { continue }
            let floatBuffer = mData.assumingMemoryBound(to: Float.self)
            let sampleCount = Int(buffer.mDataByteSize) / MemoryLayout<Float>.size

            ringBufferLock.lock()
            for index in 0 ..< sampleCount {
                ringBuffer[(writeHead + index) % ringBufferSize] = floatBuffer[index]
            }
            writeHead = (writeHead + sampleCount) % ringBufferSize
            ringBufferLock.unlock()
        }
    }

    /// Reads buffered PCM frames, applies attenuation, and populates destination buffers.
    /// - Parameters:
    ///   - context: The destination runtime context.
    ///   - bufferList: Target audio buffer list to populate.
    private func handleOutputAudio(context: DestinationDeviceContext, bufferList: UnsafeMutablePointer<AudioBufferList>) {
        let buffers = UnsafeMutableAudioBufferListPointer(bufferList)
        let volume = context.isMuted ? 0.0 : context.volume

        for buffer in buffers {
            guard let mData = buffer.mData else { continue }
            let floatBuffer = mData.assumingMemoryBound(to: Float.self)
            let sampleCount = Int(buffer.mDataByteSize) / MemoryLayout<Float>.size

            ringBufferLock.lock()
            let available = (writeHead - readHead + ringBufferSize) % ringBufferSize
            let samplesToRead = min(sampleCount, available)

            for i in 0 ..< samplesToRead {
                let sample = ringBuffer[(readHead + i) % ringBufferSize]
                floatBuffer[i] = sample * volume
            }

            for i in samplesToRead ..< sampleCount {
                floatBuffer[i] = 0.0
            }
            ringBufferLock.unlock()
        }
    }
}
