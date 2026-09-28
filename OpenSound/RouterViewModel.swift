import AppKit
import CoreAudio
import Foundation
import os.log
import SwiftUI

/// Main presentation model managing audio endpoint discovery, routing matrix, and system events.
@MainActor
@Observable
final class RouterViewModel {
    private let hal = CoreAudioHAL.shared
    private let engine = AudioRouterEngine.shared
    let extensionManager = ExtensionManager.shared

    private let logger = Logger(subsystem: "de.easonwong.OpenSound", category: "RouterViewModel")

    /// Physical audio output endpoints available for multi-route playback.
    var outputDevices: [AudioDevice] = []

    /// Discovered virtual audio device endpoint, if registered.
    var virtualDevice: AudioDevice?

    /// Whether the system default output is currently directed to the virtual router.
    var isDefaultOutputVirtualDevice: Bool = false

    /// Mapping of audio device identifiers to their active routing and volume parameters.
    var routingConfigs: [AudioObjectID: DeviceRoutingConfig] = [:]

    /// Initialises the router view model and registers hardware change observers.
    init() {
        refreshDevices()
        setupListeners()
    }

    /// Queries Core Audio HAL to update connected devices and routing configurations.
    func refreshDevices() {
        let allDevices = hal.getAudioDevices()
        let defaultOutputID = hal.getDefaultOutputDeviceID()

        virtualDevice = allDevices.first(where: { $0.isVirtualRouter })
        isDefaultOutputVirtualDevice = virtualDevice?.id == defaultOutputID

        outputDevices = allDevices.filter { $0.hasOutput && !$0.isVirtualRouter }

        for device in outputDevices {
            if routingConfigs[device.id] == nil {
                routingConfigs[device.id] = DeviceRoutingConfig(id: device.id, isEnabled: false, volume: 1.0)
            }
        }

        engine.setVirtualDevice(deviceID: virtualDevice?.id)
    }

    /// Subscribes to system-level device addition, removal, and default route changes.
    private func setupListeners() {
        hal.addDevicesListener { [weak self] _, _ in
            Task { @MainActor [weak self] in
                self?.refreshDevices()
            }
        }

        hal.addDefaultOutputDeviceListener { [weak self] _, _ in
            Task { @MainActor [weak self] in
                self?.refreshDevices()
            }
        }
    }

    /// Toggles active audio transmission for a specific physical endpoint.
    /// - Parameter deviceID: The destination audio device object ID.
    func toggleRouting(for deviceID: AudioObjectID) {
        guard var config = routingConfigs[deviceID] else { return }
        config.isEnabled.toggle()
        routingConfigs[deviceID] = config
        engine.setRouteEnabled(for: deviceID, enabled: config.isEnabled)
    }

    /// Adjusts playback volume level for a specific physical endpoint.
    /// - Parameters:
    ///   - deviceID: The destination audio device object ID.
    ///   - volume: Linear gain factor in range `0.0...1.0`.
    func updateVolume(for deviceID: AudioObjectID, volume: Float) {
        guard var config = routingConfigs[deviceID] else { return }
        config.volume = volume
        routingConfigs[deviceID] = config
        engine.setVolume(for: deviceID, volume: volume)
    }

    /// Toggles mute state for a specific physical endpoint.
    /// - Parameter deviceID: The destination audio device object ID.
    func toggleMute(for deviceID: AudioObjectID) {
        guard var config = routingConfigs[deviceID] else { return }
        config.isMuted.toggle()
        routingConfigs[deviceID] = config
        engine.setMuted(for: deviceID, isMuted: config.isMuted)
    }

    /// Designates the virtual audio device as macOS system default output.
    func setVirtualDeviceAsDefaultOutput() {
        guard let vDevice = virtualDevice else { return }
        _ = hal.setDefaultOutputDevice(deviceID: vDevice.id)
        refreshDevices()
    }

    /// Requests system extension installation and authorization.
    func activateExtension() {
        extensionManager.activateExtension()
    }

    /// Tears down all active audio pipelines, requests extension uninstallation, and terminates the application.
    func deactivateAndQuit() {
        engine.stopAllRoutes()
        extensionManager.deactivateExtension()

        DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) {
            NSApplication.shared.terminate(nil)
        }
    }
}
