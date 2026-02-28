//
//  ContentView.swift
//  iMic
//
//  Created by Yumi Izumi on 2026/02/28.
//

import SwiftUI

struct ContentView: View {
    @State var streamer = AudioStreamer()

    var body: some View {
        VStack(spacing: 32) {
            Spacer()

            Text("iMic")
                .font(.largeTitle)
                .fontWeight(.bold)

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
