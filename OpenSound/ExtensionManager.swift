import Foundation
import os.log
import Observation
import SystemExtensions

/// State enumeration representing the DriverKit system extension lifecycle.
enum ExtensionState: Equatable, Sendable {
    case notInstalled
    case requestingActivation
    case requiresApproval
    case active
    case requestingDeactivation
    case error(String)

    /// User-facing descriptive representation of the extension state.
    var description: String {
        switch self {
        case .notInstalled:
            return "Not Installed"
        case .requestingActivation:
            return "Requesting Activation..."
        case .requiresApproval:
            return "Requires System Approval"
        case .active:
            return "Active & Ready"
        case .requestingDeactivation:
            return "Deactivating..."
        case let .error(message):
            return "Error: \(message)"
        }
    }
}

/// Manages the activation, approval, and deactivation lifecycle of the AudioDriverKit extension.
@MainActor
@Observable
final class ExtensionManager: NSObject, OSSystemExtensionRequestDelegate {
    static let shared = ExtensionManager()
    private let logger = Logger(subsystem: "de.easonwong.OpenSound", category: "ExtensionManager")

    static let driverExtensionIdentifier = "de.easonwong.OpenSound.AudioDriver"

    /// Current operational state of the DriverKit extension.
    var state: ExtensionState = .notInstalled

    private override init() {
        super.init()
    }

    /// Submits an activation request for the AudioDriverKit extension via `OSSystemExtensionManager`.
    func activateExtension() {
        state = .requestingActivation
        let request = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: Self.driverExtensionIdentifier,
            queue: .main
        )
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
        logger.info("Submitted activation request for extension: \(Self.driverExtensionIdentifier)")
    }

    /// Submits a deactivation request to remove the AudioDriverKit extension from the host system.
    func deactivateExtension() {
        state = .requestingDeactivation
        let request = OSSystemExtensionRequest.deactivationRequest(
            forExtensionWithIdentifier: Self.driverExtensionIdentifier,
            queue: .main
        )
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
        logger.info("Submitted deactivation request for extension: \(Self.driverExtensionIdentifier)")
    }

    // MARK: - OSSystemExtensionRequestDelegate

    /// Determines replacement strategy when upgrading an existing system extension.
    nonisolated func request(
        _ request: OSSystemExtensionRequest,
        actionForReplacingExtension existing: OSSystemExtensionProperties,
        withExtension ext: OSSystemExtensionProperties
    ) -> OSSystemExtensionRequest.ReplacementAction {
        return .replace
    }

    /// Handles approval prompt requirements in macOS System Settings.
    nonisolated func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        Task { @MainActor in
            self.state = .requiresApproval
            self.logger.notice("Extension requires user approval in System Settings.")
        }
    }

    /// Handles successful completion of an extension lifecycle request.
    nonisolated func request(
        _ request: OSSystemExtensionRequest,
        didFinishWithResult result: OSSystemExtensionRequest.Result
    ) {
        Task { @MainActor in
            switch result {
            case .completed:
                self.state = .active
                self.logger.info("Extension request completed successfully.")
            case .willCompleteAfterReboot:
                self.state = .active
                self.logger.info("Extension request will complete after system restart.")
            @unknown default:
                self.state = .active
            }
        }
    }

    /// Handles failure during extension activation or deactivation.
    nonisolated func request(_ request: OSSystemExtensionRequest, didFailWithError error: any Error) {
        Task { @MainActor in
            self.state = .error(error.localizedDescription)
            self.logger.error("Extension request failed with error: \(error.localizedDescription)")
        }
    }
}
