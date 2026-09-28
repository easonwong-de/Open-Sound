import CoreAudio
import Foundation

/// Represents a physical or virtual audio endpoint in macOS Core Audio.
struct AudioDevice: Identifiable, Hashable, Sendable {
    let id: AudioObjectID
    let name: String
    let uid: String
    let hasOutput: Bool
    let hasInput: Bool
    let sampleRate: Double
    let isVirtualRouter: Bool

    var isDefaultOutput: Bool = false
}

/// Stores routing configuration and playback parameters for an audio endpoint.
struct DeviceRoutingConfig: Identifiable, Sendable {
    let id: AudioObjectID
    var isEnabled: Bool
    var volume: Float
    var isMuted: Bool

    /// Initialises a routing configuration for a specific device.
    /// - Parameters:
    ///   - id: The Core Audio object identifier.
    ///   - isEnabled: Whether audio output is actively routed to the device.
    ///   - volume: Linear volume attenuation factor in range `0.0...1.0`.
    ///   - isMuted: Whether audio playback is muted.
    init(id: AudioObjectID, isEnabled: Bool = false, volume: Float = 1.0, isMuted: Bool = false) {
        self.id = id
        self.isEnabled = isEnabled
        self.volume = volume
        self.isMuted = isMuted
    }
}
