import CoreAudio
import SwiftUI

/// Main user interface providing audio endpoint routing toggles, volume sliders, and driver lifecycle controls.
struct ContentView: View {
    @State private var viewModel = RouterViewModel()

    var body: some View {
        VStack(spacing: 0) {
            headerView
            Divider()

            if !viewModel.isDefaultOutputVirtualDevice && viewModel.virtualDevice != nil {
                defaultDeviceNoticeView
                Divider()
            }

            deviceListView
            Divider()

            footerView
        }
        .frame(minWidth: 520, minHeight: 440)
        .background(Color(NSColor.windowBackgroundColor))
    }

    // MARK: - Header

    /// Status header displaying application title and Core Audio HAL driver state.
    private var headerView: some View {
        HStack(spacing: 12) {
            Image(systemName: "waveform.circle.fill")
                .resizable()
                .frame(width: 32, height: 32)
                .foregroundStyle(.blue)

            VStack(alignment: .leading, spacing: 2) {
                Text("OpenSound Router")
                    .font(.headline)
                    .fontWeight(.semibold)

                HStack(spacing: 6) {
                    Circle()
                        .fill(driverStatusColor)
                        .frame(width: 8, height: 8)

                    Text(viewModel.pluginManager.state.description)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            Spacer()

            if viewModel.pluginManager.state == .active {
                Button("Uninstall Driver") {
                    viewModel.uninstallDriver()
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
            } else if viewModel.pluginManager.state != .installing && viewModel.pluginManager.state != .uninstalling {
                Button("Install Driver") {
                    viewModel.installDriver()
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.small)
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
    }

    /// Colour indicator corresponding to the current HAL driver state.
    private var driverStatusColor: Color {
        switch viewModel.pluginManager.state {
        case .active:
            return .green
        case .installing, .uninstalling:
            return .orange
        case .error:
            return .red
        case .notInstalled:
            return .secondary
        }
    }

    // MARK: - Default Device Notice

    /// Warning banner shown when macOS default audio output is not directed to the virtual router.
    private var defaultDeviceNoticeView: some View {
        HStack(spacing: 12) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundStyle(.orange)

            Text("System audio is not routed to Virtual Audio Router.")
                .font(.callout)

            Spacer()

            Button("Set Default Output") {
                viewModel.setVirtualDeviceAsDefaultOutput()
            }
            .buttonStyle(.bordered)
            .controlSize(.small)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
        .background(Color.orange.opacity(0.1))
    }

    // MARK: - Device List

    /// List displaying all detected physical output audio endpoints with individual controls.
    private var deviceListView: some View {
        Group {
            if viewModel.outputDevices.isEmpty {
                VStack(spacing: 12) {
                    Spacer()
                    Image(systemName: "speaker.slash")
                        .font(.system(size: 36))
                        .foregroundStyle(.secondary)
                    Text("No physical audio output devices detected.")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                    Spacer()
                }
            } else {
                List {
                    Section(header: Text("Physical Audio Outputs").font(.subheadline).fontWeight(.medium)) {
                        ForEach(viewModel.outputDevices) { device in
                            DeviceRowView(
                                device: device,
                                config: viewModel.routingConfigs[device.id] ?? DeviceRoutingConfig(id: device.id),
                                onToggleRoute: { viewModel.toggleRouting(for: device.id) },
                                onVolumeChange: { volume in viewModel.updateVolume(for: device.id, volume: volume) },
                                onToggleMute: { viewModel.toggleMute(for: device.id) }
                            )
                            .padding(.vertical, 4)
                        }
                    }
                }
                .listStyle(.inset(alternatesRowBackgrounds: true))
            }
        }
    }

    // MARK: - Footer

    /// Footer bar providing endpoint count summary and clean application termination action.
    private var footerView: some View {
        HStack {
            Text("\(viewModel.outputDevices.count) physical endpoints detected")
                .font(.caption)
                .foregroundStyle(.secondary)

            Spacer()

            Button("Quit") {
                viewModel.quit()
            }
            .buttonStyle(.bordered)
            .controlSize(.small)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
    }
}

// MARK: - Device Row View

/// Table row component for controlling routing, volume attenuation, and mute for a single device.
struct DeviceRowView: View {
    let device: AudioDevice
    let config: DeviceRoutingConfig
    let onToggleRoute: () -> Void
    let onVolumeChange: (Float) -> Void
    let onToggleMute: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            Toggle("", isOn: Binding(
                get: { config.isEnabled },
                set: { _ in onToggleRoute() }
            ))
            .toggleStyle(.checkbox)
            .labelsHidden()

            VStack(alignment: .leading, spacing: 2) {
                Text(device.name)
                    .font(.body)
                    .fontWeight(.medium)

                Text(String(format: "%.0f Hz", device.sampleRate))
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }
            .frame(width: 170, alignment: .leading)

            Button(action: onToggleMute) {
                Image(systemName: config.isMuted ? "speaker.slash.fill" : "speaker.wave.2.fill")
                    .foregroundStyle(config.isMuted ? .red : .secondary)
                    .frame(width: 18)
            }
            .buttonStyle(.plain)
            .frame(width: 20)
            .disabled(!config.isEnabled)

            Slider(
                value: Binding(
                    get: { config.volume },
                    set: { onVolumeChange($0) }
                ),
                in: 0.0 ... 1.0
            )
            .disabled(!config.isEnabled || config.isMuted)

            Text("\(Int(config.volume * 100))%")
                .font(.caption)
                .monospacedDigit()
                .foregroundStyle(.secondary)
                .frame(width: 36, alignment: .trailing)
        }
        .opacity(config.isEnabled ? 1.0 : 0.6)
    }
}

#Preview {
    ContentView()
}
