import Foundation
import Network
import Observation

@Observable
@MainActor
final class AudioReceiver {
    var connectionState: String = "Idle"
    var peerName: String?

    private var browser: NWBrowser?
    private var connection: NWConnection?
    private let ringBuffer = SharedRingBuffer()
    private let queue = DispatchQueue(label: "com.imic.receiver", qos: .userInteractive)

    func start() {
        connectionState = "Browsing..."
        peerName = nil

        let params = NWParameters()
        params.includePeerToPeer = true

        let browser = NWBrowser(for: .bonjour(type: "_imic._tcp", domain: nil), using: params)
        self.browser = browser

        browser.stateUpdateHandler = { [weak self] state in
            Task { @MainActor [weak self] in
                guard let self else { return }
                switch state {
                case .ready:
                    self.connectionState = "Browsing..."
                case .failed(let error):
                    self.connectionState = "Browse failed: \(error)"
                    self.scheduleRestart()
                default:
                    break
                }
            }
        }

        browser.browseResultsChangedHandler = { [weak self] results, _ in
            Task { @MainActor [weak self] in
                guard let self else { return }
                guard self.connection == nil, let result = results.first else { return }
                self.connectTo(result: result)
            }
        }

        browser.start(queue: queue)
    }

    func stop() {
        browser?.cancel()
        browser = nil
        connection?.cancel()
        connection = nil
        connectionState = "Idle"
        peerName = nil
    }

    private func connectTo(result: NWBrowser.Result) {
        browser?.cancel()
        browser = nil

        if case .service(let name, _, _, _) = result.endpoint {
            peerName = name
        }

        let params = NWParameters.tcp
        let conn = NWConnection(to: result.endpoint, using: params)
        self.connection = conn

        conn.stateUpdateHandler = { [weak self] state in
            Task { @MainActor [weak self] in
                guard let self else { return }
                switch state {
                case .preparing:
                    self.connectionState = "Connecting..."
                case .ready:
                    self.connectionState = "Connected"
                    self.receiveFrameHeader(on: conn)
                case .failed(let error):
                    self.connectionState = "Disconnected: \(error)"
                    self.connection = nil
                    self.peerName = nil
                    self.scheduleRestart()
                case .cancelled:
                    self.connectionState = "Disconnected"
                    self.connection = nil
                    self.peerName = nil
                default:
                    break
                }
            }
        }

        connectionState = "Connecting..."
        conn.start(queue: queue)
    }

    private nonisolated func receiveFrameHeader(on conn: NWConnection) {
        conn.receive(minimumIncompleteLength: 4, maximumLength: 4) { [weak self] data, _, isComplete, error in
            guard let self else { return }

            if let error {
                Task { @MainActor in self.handleDisconnect(error: error) }
                return
            }

            if isComplete {
                Task { @MainActor in self.handleDisconnect(error: nil) }
                return
            }

            guard let data, data.count == 4 else {
                self.receiveFrameHeader(on: conn)
                return
            }

            let frameCount = data.withUnsafeBytes { $0.load(as: UInt32.self).littleEndian }
            let byteCount = Int(frameCount) * MemoryLayout<Float32>.size

            self.receiveFrameData(on: conn, byteCount: byteCount)
        }
    }

    private nonisolated func receiveFrameData(on conn: NWConnection, byteCount: Int) {
        conn.receive(minimumIncompleteLength: byteCount, maximumLength: byteCount) { [weak self] data, _, isComplete, error in
            guard let self else { return }

            if let error {
                Task { @MainActor in self.handleDisconnect(error: error) }
                return
            }

            if let data, !data.isEmpty {
                data.withUnsafeBytes { rawBuf in
                    let floatPtr = rawBuf.bindMemory(to: Float32.self)
                    self.ringBuffer.write(frames: floatPtr.baseAddress!, count: floatPtr.count)
                }
            }

            if isComplete {
                Task { @MainActor in self.handleDisconnect(error: nil) }
                return
            }

            self.receiveFrameHeader(on: conn)
        }
    }

    private func handleDisconnect(error: NWError?) {
        connection?.cancel()
        connection = nil
        peerName = nil
        connectionState = error != nil ? "Disconnected: \(error!)" : "Disconnected"
        scheduleRestart()
    }

    private func scheduleRestart() {
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(2))
            guard let self, self.connection == nil else { return }
            self.start()
        }
    }
}
