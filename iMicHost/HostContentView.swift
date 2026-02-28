import SwiftUI
import Observation

struct HostContentView: View {
    @State private var receiver = AudioReceiver()
    @State private var driverInstalled = false
    @State private var installStatus: String?
    @State private var isInstalling = false

    var body: some View {
        if driverInstalled {
            mainView
        } else {
            onboardingView
        }
    }

    // MARK: - Onboarding

    private var onboardingView: some View {
        VStack(spacing: 24) {
            Spacer()

            Image(systemName: "mic.badge.plus")
                .font(.system(size: 64))
                .foregroundStyle(.blue)

            Text("Welcome to iMic")
                .font(.largeTitle)
                .fontWeight(.bold)

            Text("iMic needs to install a virtual audio driver so your Mac can use your iPhone as a microphone input.\n\nThis requires administrator privileges.")
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
                .frame(maxWidth: 360)

            Button {
                installDriver()
            } label: {
                if isInstalling {
                    ProgressView()
                        .controlSize(.small)
                        .frame(width: 200)
                } else {
                    Text("Install Audio Driver")
                        .frame(width: 200)
                }
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
            .disabled(isInstalling)

            if let status = installStatus {
                Text(status)
                    .font(.caption)
                    .foregroundStyle(status.contains("Error") ? .red : .green)
            }

            Spacer()
        }
        .padding(40)
        .frame(minWidth: 400, minHeight: 350)
        .onAppear {
            checkDriverInstalled()
        }
    }

    // MARK: - Main View

    private var mainView: some View {
        @Bindable var receiver = receiver

        return VStack(spacing: 20) {
            Text("iMic Host")
                .font(.largeTitle)
                .fontWeight(.bold)

            VStack(spacing: 10) {
                Text("Connection Mode")
                    .font(.headline)

                Picker("Connection Mode", selection: $receiver.transportMode) {
                    ForEach(AudioReceiver.TransportMode.allCases) { mode in
                        Text(mode.rawValue).tag(mode)
                    }
                }
                .pickerStyle(.segmented)
                .disabled(receiver.connectionState != "Idle")

                Text("Direct mode enables Apple's peer-to-peer transport, which can use Wi-Fi and Bluetooth during discovery and connection.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }

            GroupBox("Connection") {
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Circle()
                            .fill(statusColor)
                            .frame(width: 10, height: 10)
                        Text(receiver.connectionState)
                    }

                    if let name = receiver.peerName {
                        HStack {
                            Text("Peer:")
                                .foregroundStyle(.secondary)
                            Text(name)
                                .fontWeight(.medium)
                        }
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(4)
            }

            HStack {
                Button("Start") {
                    receiver.start()
                }
                .disabled(receiver.connectionState != "Idle")

                Button("Stop") {
                    receiver.stop()
                }
                .disabled(receiver.connectionState == "Idle")
            }

            Divider()

            HStack {
                Image(systemName: "checkmark.circle.fill")
                    .foregroundStyle(.green)
                Text("Audio driver installed")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(30)
        .frame(minWidth: 350, minHeight: 300)
        .onAppear {
            receiver.start()
        }
    }

    // MARK: - Helpers

    private var statusColor: Color {
        switch receiver.connectionState {
        case "Connected":
            return .green
        case "Idle":
            return .gray
        case let s where s.starts(with: "Disconnected"):
            return .red
        default:
            return .orange
        }
    }

    private func checkDriverInstalled() {
        let driverPath = "/Library/Audio/Plug-Ins/HAL/iMicDriver.driver"
        driverInstalled = FileManager.default.fileExists(atPath: driverPath)
    }

    private func installDriver() {
        let driverName = "iMicDriver.driver"
        guard let bundledDriverPath = Bundle.main.path(forResource: "iMicDriver", ofType: "driver") else {
            installStatus = "Error: Driver not found in app bundle."
            return
        }

        isInstalling = true
        installStatus = nil

        let script = """
        do shell script "rm -rf /Library/Audio/Plug-Ins/HAL/\(driverName); cp -R '\(bundledDriverPath)' /Library/Audio/Plug-Ins/HAL/ && kill -9 $(pgrep coreaudiod)" with administrator privileges
        """

        Task.detached {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
            process.arguments = ["-e", script]

            let pipe = Pipe()
            process.standardError = pipe

            do {
                try process.run()
                process.waitUntilExit()

                let success = process.terminationStatus == 0
                let errorData = pipe.fileHandleForReading.readDataToEndOfFile()
                let errorMsg = String(data: errorData, encoding: .utf8) ?? ""

                await MainActor.run {
                    isInstalling = false
                    if success {
                        installStatus = "Driver installed successfully!"
                        driverInstalled = true
                    } else {
                        installStatus = "Error: \(errorMsg)"
                    }
                }
            } catch {
                await MainActor.run {
                    isInstalling = false
                    installStatus = "Error: \(error.localizedDescription)"
                }
            }
        }
    }
}
