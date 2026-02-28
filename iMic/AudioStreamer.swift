//
//  AudioStreamer.swift
//  iMic
//
//  Created by Yumi Izumi on 2026/02/28.
//

import AVFoundation
import Network
import Observation
#if canImport(UIKit)
import UIKit
#endif

@Observable
@MainActor
final class AudioStreamer {

    enum TransportMode: String, CaseIterable, Identifiable {
        case localNetwork = "Wi-Fi / LAN"
        case peerToPeer = "Direct (Wi-Fi/Bluetooth)"

        var id: Self { self }

        var includesPeerToPeer: Bool {
            self == .peerToPeer
        }
    }

    // MARK: - Published state

    var isStreaming = false
    var statusText = "Ready"
    var connectedPeer: String? = nil
    var serviceName: String? = nil
    var transportMode: TransportMode = .localNetwork

    // MARK: - Private

    private var audioEngine: AVAudioEngine?
    private var listener: NWListener?
    private var connection: NWConnection?

    /// A reference to the active connection that the audio tap can safely
    /// read from the real-time audio thread. Writes happen only on MainActor;
    /// reads happen on the audio thread. NWConnection.send is thread-safe.
    private nonisolated(unsafe) var activeConnection: NWConnection?

    // MARK: - Start

    func start() {
        statusText = "Requesting microphone access…"

        AVAudioApplication.requestRecordPermission { [weak self] granted in
            Task { @MainActor in
                guard let self else { return }
                if granted {
                    self.beginStreaming()
                } else {
                    self.statusText = "Microphone access denied"
                }
            }
        }
    }

    // MARK: - Stop

    func stop() {
        audioEngine?.inputNode.removeTap(onBus: 0)
        audioEngine?.stop()
        audioEngine = nil

        connection?.cancel()
        connection = nil
        activeConnection = nil

        listener?.cancel()
        listener = nil

        isStreaming = false
        connectedPeer = nil
        serviceName = nil
        statusText = "Ready"
    }

    // MARK: - Internal

    private func beginStreaming() {
        do {
            try startAudioEngine()
            try startListener()
            isStreaming = true
            statusText = "Waiting for Mac…"
        } catch {
            statusText = "Error: \(error.localizedDescription)"
            stop()
        }
    }

    // MARK: - Audio Engine

    private func startAudioEngine() throws {
        let engine = AVAudioEngine()

        let inputNode = engine.inputNode
        let hardwareFormat = inputNode.outputFormat(forBus: 0)

        // Desired format: Float32, 48 kHz, mono
        guard let desiredFormat = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate: 48_000,
            channels: 1,
            interleaved: false
        ) else {
            throw StreamerError.formatCreationFailed
        }

        // If hardware doesn't match, install a tap with the hardware format
        // and let the audio engine do conversion, or install directly with desired format.
        let tapFormat: AVAudioFormat
        if hardwareFormat.sampleRate == desiredFormat.sampleRate &&
            hardwareFormat.channelCount == desiredFormat.channelCount &&
            hardwareFormat.commonFormat == desiredFormat.commonFormat {
            tapFormat = desiredFormat
        } else {
            // Use the hardware format and convert manually if needed,
            // or just use desired format (AVAudioEngine handles conversion).
            tapFormat = desiredFormat
        }

        inputNode.installTap(onBus: 0, bufferSize: 4096, format: tapFormat) { [weak self] buffer, _ in
            self?.sendAudioBuffer(buffer)
        }

        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.record, mode: .measurement, options: [])
        try session.setPreferredSampleRate(48_000)
        try session.setActive(true)

        try engine.start()
        self.audioEngine = engine
    }

    // MARK: - Network Listener

    private func startListener() throws {
        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        params.includePeerToPeer = transportMode.includesPeerToPeer

        let listener = try NWListener(using: params)
        listener.service = NWListener.Service(type: "_imic._tcp")

        listener.stateUpdateHandler = { [weak self] state in
            Task { @MainActor [weak self] in
                guard let self else { return }
                switch state {
                case .waiting(let error):
                    self.statusText = "Waiting for network path… \(error.localizedDescription)"
                case .ready:
                    if let port = self.listener?.port {
                        self.serviceName = "\(self.deviceName) (port \(port))"
                    }
                    if self.connection == nil {
                        self.statusText = "Waiting for Mac…"
                    }
                case .failed(let error):
                    self.statusText = "Listener failed: \(error.localizedDescription)"
                    self.stop()
                case .cancelled:
                    break
                default:
                    break
                }
            }
        }

        listener.newConnectionHandler = { [weak self] newConnection in
            Task { @MainActor [weak self] in
                guard let self else { return }
                // Accept only one connection at a time
                if let existing = self.connection {
                    existing.cancel()
                }
                self.acceptConnection(newConnection)
            }
        }

        listener.start(queue: .main)
        self.listener = listener
    }

    // MARK: - Connection Handling

    private func acceptConnection(_ newConnection: NWConnection) {
        connection = newConnection
        activeConnection = newConnection

        newConnection.stateUpdateHandler = { [weak self] state in
            Task { @MainActor [weak self] in
                guard let self else { return }
                switch state {
                case .waiting(let error):
                    self.statusText = "Connection interrupted… \(error.localizedDescription)"
                case .ready:
                    let endpoint = newConnection.endpoint
                    if case .hostPort(let host, _) = endpoint {
                        self.connectedPeer = "\(host)"
                    } else {
                        self.connectedPeer = "Mac"
                    }
                    self.statusText = "Streaming…"
                case .failed(let error):
                    self.statusText = "Connection lost: \(error.localizedDescription). Waiting for Mac…"
                    self.connectedPeer = nil
                    self.connection = nil
                    self.activeConnection = nil
                case .cancelled:
                    self.connectedPeer = nil
                    self.connection = nil
                    self.activeConnection = nil
                    if self.isStreaming {
                        self.statusText = "Waiting for Mac…"
                    }
                default:
                    break
                }
            }
        }

        newConnection.start(queue: .main)
    }

    // MARK: - Send Audio

    /// Called on the audio engine's real-time thread.
    /// NWConnection.send is thread-safe so we can call it directly.
    private nonisolated func sendAudioBuffer(_ buffer: AVAudioPCMBuffer) {
        guard let conn = activeConnection else { return }
        guard let floatData = buffer.floatChannelData else { return }

        let frameCount = buffer.frameLength
        let samples = floatData[0]

        // Build packet: UInt32 frame count (little-endian) + Float32 samples
        var header = frameCount.littleEndian
        let headerData = Data(bytes: &header, count: MemoryLayout<UInt32>.size)
        let sampleData = Data(bytes: samples, count: Int(frameCount) * MemoryLayout<Float32>.size)

        let packet = headerData + sampleData

        conn.send(content: packet, completion: .idempotent)
    }

    // MARK: - Helpers

    private var deviceName: String {
        #if os(iOS)
        return UIDevice.current.name
        #else
        return Host.current().localizedName ?? "iMic"
        #endif
    }

    // MARK: - Errors

    private enum StreamerError: LocalizedError {
        case formatCreationFailed

        var errorDescription: String? {
            switch self {
            case .formatCreationFailed:
                return "Failed to create audio format"
            }
        }
    }
}
