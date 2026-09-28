import CoreAudio
import Foundation
import os.log

/// Wrapper around the macOS Core Audio Hardware Abstraction Layer (HAL).
final class CoreAudioHAL: @unchecked Sendable {
    static let shared = CoreAudioHAL()
    private let logger = Logger(subsystem: "de.easonwong.OpenSound", category: "CoreAudioHAL")

    static let virtualDeviceUID = "de.easonwong.OpenSound.VirtualAudioRouter"
    static let virtualDeviceName = "Virtual Audio Router"

    private init() {}

    /// Retrieves all registered audio object IDs from the system object.
    /// - Returns: An array of `AudioObjectID` values.
    func getAllDeviceIDs() -> [AudioObjectID] {
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        var dataSize: UInt32 = 0
        let sizeStatus = AudioObjectGetPropertyDataSize(
            AudioObjectID(kAudioObjectSystemObject),
            &propertyAddress,
            0,
            nil,
            &dataSize
        )

        guard sizeStatus == noErr, dataSize > 0 else {
            return []
        }

        let deviceCount = Int(dataSize) / MemoryLayout<AudioObjectID>.size
        var deviceIDs = [AudioObjectID](repeating: 0, count: deviceCount)

        let dataStatus = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &propertyAddress,
            0,
            nil,
            &dataSize,
            &deviceIDs
        )

        guard dataStatus == noErr else {
            return []
        }

        return deviceIDs
    }

    /// Fetches the user-visible name of a device.
    /// - Parameter deviceID: The audio device object ID.
    /// - Returns: The device name, or a fallback string if unavailable.
    func getDeviceName(for deviceID: AudioObjectID) -> String {
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioObjectPropertyName,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        var unmanagedName: Unmanaged<CFString>?
        var dataSize = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)

        let status = withUnsafeMutablePointer(to: &unmanagedName) { ptr in
            AudioObjectGetPropertyData(
                deviceID,
                &propertyAddress,
                0,
                nil,
                &dataSize,
                ptr
            )
        }

        if status == noErr, let name = unmanagedName?.takeRetainedValue() as String? {
            return name
        }
        return "Unknown Device (\(deviceID))"
    }

    /// Retrieves the unique persistent identifier (UID) for a device.
    /// - Parameter deviceID: The audio device object ID.
    /// - Returns: The device UID string.
    func getDeviceUID(for deviceID: AudioObjectID) -> String {
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyDeviceUID,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        var unmanagedUID: Unmanaged<CFString>?
        var dataSize = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)

        let status = withUnsafeMutablePointer(to: &unmanagedUID) { ptr in
            AudioObjectGetPropertyData(
                deviceID,
                &propertyAddress,
                0,
                nil,
                &dataSize,
                ptr
            )
        }

        if status == noErr, let uid = unmanagedUID?.takeRetainedValue() as String? {
            return uid
        }
        return ""
    }

    /// Checks whether an audio device has output streams available.
    /// - Parameter deviceID: The audio device object ID.
    /// - Returns: `true` if the device supports audio output; otherwise `false`.
    func hasOutputStreams(for deviceID: AudioObjectID) -> Bool {
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyStreams,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: kAudioObjectPropertyElementMain
        )

        var dataSize: UInt32 = 0
        let status = AudioObjectGetPropertyDataSize(
            deviceID,
            &propertyAddress,
            0,
            nil,
            &dataSize
        )

        return status == noErr && dataSize > 0
    }

    /// Checks whether an audio device has input streams available.
    /// - Parameter deviceID: The audio device object ID.
    /// - Returns: `true` if the device supports audio input; otherwise `false`.
    func hasInputStreams(for deviceID: AudioObjectID) -> Bool {
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyStreams,
            mScope: kAudioDevicePropertyScopeInput,
            mElement: kAudioObjectPropertyElementMain
        )

        var dataSize: UInt32 = 0
        let status = AudioObjectGetPropertyDataSize(
            deviceID,
            &propertyAddress,
            0,
            nil,
            &dataSize
        )

        return status == noErr && dataSize > 0
    }

    /// Queries the nominal sample rate of an audio device.
    /// - Parameter deviceID: The audio device object ID.
    /// - Returns: The sample rate in Hertz, defaulting to 48000.0 on failure.
    func getNominalSampleRate(for deviceID: AudioObjectID) -> Double {
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyNominalSampleRate,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        var sampleRate: Float64 = 0.0
        var dataSize = UInt32(MemoryLayout<Float64>.size)

        let status = AudioObjectGetPropertyData(
            deviceID,
            &propertyAddress,
            0,
            nil,
            &dataSize,
            &sampleRate
        )

        if status == noErr {
            return Double(sampleRate)
        }
        return 48000.0
    }

    /// Retrieves the device ID of the current system default audio output endpoint.
    /// - Returns: The `AudioObjectID` of the default output device, or `nil` if none.
    func getDefaultOutputDeviceID() -> AudioObjectID? {
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        var deviceID: AudioObjectID = 0
        var dataSize = UInt32(MemoryLayout<AudioObjectID>.size)

        let status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &propertyAddress,
            0,
            nil,
            &dataSize,
            &deviceID
        )

        guard status == noErr, deviceID != kAudioObjectUnknown else {
            return nil
        }
        return deviceID
    }

    /// Sets the specified device as the system default audio output endpoint.
    /// - Parameter deviceID: The target audio device object ID.
    /// - Returns: `true` if the property was set successfully; otherwise `false`.
    func setDefaultOutputDevice(deviceID: AudioObjectID) -> Bool {
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        var targetID = deviceID
        let dataSize = UInt32(MemoryLayout<AudioObjectID>.size)

        let status = AudioObjectSetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &propertyAddress,
            0,
            nil,
            dataSize,
            &targetID
        )

        return status == noErr
    }

    /// Enumerates and models all active audio devices currently known to macOS Core Audio.
    /// - Returns: An array of `AudioDevice` instances.
    func getAudioDevices() -> [AudioDevice] {
        let defaultOutputID = getDefaultOutputDeviceID()
        let deviceIDs = getAllDeviceIDs()

        return deviceIDs.compactMap { id in
            let name = getDeviceName(for: id)
            let uid = getDeviceUID(for: id)
            let hasOutput = hasOutputStreams(for: id)
            let hasInput = hasInputStreams(for: id)
            let sampleRate = getNominalSampleRate(for: id)
            let isVirtual = uid == Self.virtualDeviceUID || name == Self.virtualDeviceName

            guard hasOutput || hasInput else { return nil }

            return AudioDevice(
                id: id,
                name: name,
                uid: uid,
                hasOutput: hasOutput,
                hasInput: hasInput,
                sampleRate: sampleRate,
                isVirtualRouter: isVirtual,
                isDefaultOutput: id == defaultOutputID
            )
        }
    }

    /// Registers a listener block for changes to the system audio device list.
    /// - Parameter block: The listener callback block to invoke on device changes.
    func addDevicesListener(block: @escaping AudioObjectPropertyListenerBlock) {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        AudioObjectAddPropertyListenerBlock(
            AudioObjectID(kAudioObjectSystemObject),
            &address,
            DispatchQueue.main,
            block
        )
    }

    /// Registers a listener block for changes to the system default output device.
    /// - Parameter block: The listener callback block to invoke when the default output changes.
    func addDefaultOutputDeviceListener(block: @escaping AudioObjectPropertyListenerBlock) {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        AudioObjectAddPropertyListenerBlock(
            AudioObjectID(kAudioObjectSystemObject),
            &address,
            DispatchQueue.main,
            block
        )
    }
}
