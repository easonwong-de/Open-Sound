import AppKit
import Foundation
import Observation
import os.log

/// State enumeration representing the Core Audio HAL plugin lifecycle.
enum PluginState: Equatable, Sendable {
    case notInstalled
    case installing
    case active
    case uninstalling
    case error(String)

    /// User-facing descriptive representation of the driver plugin state.
    var description: String {
        switch self {
        case .notInstalled:
            return "Driver Not Installed"
        case .installing:
            return "Installing Driver..."
        case .active:
            return "Active & Ready"
        case .uninstalling:
            return "Uninstalling..."
        case let .error(message):
            return "Error: \(message)"
        }
    }
}

/// Manages the installation, verification, and uninstallation of the Core Audio HAL driver plugin.
@MainActor
@Observable
final class PluginManager {
    static let shared = PluginManager()
    private let logger = Logger(subsystem: "de.easonwong.OpenSound", category: "PluginManager")

    static let driverBundleName = "OpenSoundDriver.driver"
    static let halDirectoryPath = "/Library/Audio/Plug-Ins/HAL"
    static let installedDriverPath = "\(halDirectoryPath)/\(driverBundleName)"

    /// Current operational state of the Core Audio HAL driver.
    var state: PluginState = .notInstalled

    private init() {
        checkInstallationStatus()
    }

    /// Verifies whether the driver plugin bundle is installed in the system HAL directory.
    func checkInstallationStatus() {
        let isInstalled = FileManager.default.fileExists(atPath: Self.installedDriverPath)
        let hasVirtualDevice = CoreAudioHAL.shared.getAudioDevices().contains(where: { $0.isVirtualRouter })

        if isInstalled && hasVirtualDevice {
            state = .active
        } else if isInstalled {
            state = .error("Driver installed, but virtual device is not responding.")
        } else {
            state = .notInstalled
        }
    }

    /// Verifies driver registration repeatedly while Core Audio completes reinitialisation.
    private func verifyInstallation(attemptsRemaining: Int) {
        checkInstallationStatus()
        if state == .active || attemptsRemaining <= 0 {
            return
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { [weak self] in
            self?.verifyInstallation(attemptsRemaining: attemptsRemaining - 1)
        }
    }

    /// Installs the bundled Core Audio HAL driver to `/Library/Audio/Plug-Ins/HAL/` and restarts Core Audio.
    func installPlugin() {
        guard let bundledDriverURL = Bundle.main.url(forResource: "OpenSoundDriver", withExtension: "driver") ??
            Bundle.main.builtInPlugInsURL?.appendingPathComponent(Self.driverBundleName)
        else {
            state = .error("Bundled driver plugin not found.")
            logger.error("Could not locate OpenSoundDriver.driver in application bundle.")
            return
        }

        state = .installing
        let sourcePath = bundledDriverURL.path

        let script = """
        do shell script "mkdir -p '\(Self.halDirectoryPath)' && rm -rf '\(Self.installedDriverPath)' && cp -R '\(sourcePath)' '\(Self.installedDriverPath)' && chown -R root:wheel '\(Self.installedDriverPath)' && chmod -R 755 '\(Self.installedDriverPath)' && (launchctl kickstart -kp system/com.apple.audio.coreaudiod || killall coreaudiod)" with administrator privileges
        """

        executePrivilegedScript(script: script) { [weak self] success, error in
            Task { @MainActor [weak self] in
                guard let self else { return }
                if success {
                    self.logger.info("Driver installed successfully. Core Audio restarted.")
                    self.verifyInstallation(attemptsRemaining: 5)
                } else {
                    let errMessage = error ?? "Installation failed."
                    self.state = .error(errMessage)
                    self.logger.error("Failed to install driver: \(errMessage)")
                }
            }
        }
    }

    /// Removes the driver plugin from `/Library/Audio/Plug-Ins/HAL/` and restarts Core Audio.
    func uninstallPlugin() {
        state = .uninstalling

        let script = """
        do shell script "rm -rf '\(Self.installedDriverPath)' && (launchctl kickstart -kp system/com.apple.audio.coreaudiod || killall coreaudiod)" with administrator privileges
        """

        executePrivilegedScript(script: script) { [weak self] success, error in
            Task { @MainActor [weak self] in
                guard let self else { return }
                if success {
                    self.logger.info("Driver uninstalled successfully.")
                    DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
                        self.checkInstallationStatus()
                    }
                } else {
                    let errMessage = error ?? "Uninstallation failed."
                    self.state = .error(errMessage)
                    self.logger.error("Failed to uninstall driver: \(errMessage)")
                }
            }
        }
    }

    /// Executes an AppleScript command with administrator privileges.
    /// - Parameters:
    ///   - script: The AppleScript command string.
    ///   - completion: Callback with success flag and optional error message.
    private func executePrivilegedScript(script: String, completion: @escaping @Sendable (Bool, String?) -> Void) {
        DispatchQueue.global(qos: .userInitiated).async {
            var errorInfo: NSDictionary?
            if let appleScript = NSAppleScript(source: script) {
                appleScript.executeAndReturnError(&errorInfo)
                if let errorInfo {
                    let message = errorInfo[NSAppleScript.errorMessage] as? String ?? "Authorization failed."
                    completion(false, message)
                } else {
                    completion(true, nil)
                }
            } else {
                completion(false, "Failed to initialize AppleScript.")
            }
        }
    }
}
