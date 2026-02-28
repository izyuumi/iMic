//
//  ContentView.swift
//  iMic
//
//  Created by Yumi Izumi on 2026/02/28.
//

import SwiftUI
import Observation

struct ContentView: View {
    @State var streamer = AudioStreamer()

    var body: some View {
        @Bindable var streamer = streamer

        VStack(spacing: 32) {
            Spacer()

            Text("iMic")
                .font(.largeTitle)
                .fontWeight(.bold)

            VStack(spacing: 10) {
                Text("Connection")
                    .font(.headline)

                Picker("Connection", selection: $streamer.transportMode) {
                    ForEach(AudioStreamer.TransportMode.allCases) { mode in
                        Text(mode.rawValue).tag(mode)
                    }
                }
                .pickerStyle(.segmented)
                .disabled(streamer.isStreaming)

                Text("Direct mode enables Apple's peer-to-peer transport, which can use Wi-Fi and Bluetooth for discovery.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
            .padding(.horizontal)

            Button {
                if streamer.isStreaming {
                    streamer.stop()
                } else {
                    streamer.start()
                }
            } label: {
                Circle()
                    .fill(streamer.isStreaming ? Color.red : Color.green)
                    .frame(width: 140, height: 140)
                    .overlay {
                        Image(systemName: streamer.isStreaming ? "stop.fill" : "mic.fill")
                            .font(.system(size: 48))
                            .foregroundStyle(.white)
                    }
                    .shadow(color: streamer.isStreaming ? .red.opacity(0.4) : .green.opacity(0.4),
                            radius: 16)
            }
            .buttonStyle(.plain)

            Text(streamer.isStreaming ? "Tap to Stop" : "Tap to Start")
                .font(.headline)
                .foregroundStyle(.secondary)

            VStack(spacing: 8) {
                Text(streamer.statusText)
                    .font(.subheadline)
                    .foregroundStyle(.primary)
                    .multilineTextAlignment(.center)

                if let peer = streamer.connectedPeer {
                    Label(peer, systemImage: "laptopcomputer")
                        .font(.subheadline)
                        .foregroundStyle(.blue)
                }

                if let serviceName = streamer.serviceName {
                    Text("Advertising as: \(serviceName)")
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                }
            }
            .padding(.horizontal)

            Spacer()
            Spacer()
        }
        .padding()
    }
}

#Preview {
    ContentView()
}
