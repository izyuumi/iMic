import Darwin
import Foundation

final class SharedRingBuffer {
    static let filePath = "/tmp/imic_audio_buffer"
    static let sampleCount = 65536
    static let headerSize = 16 // writeIndex (UInt64) + readIndex (UInt64)
    static let totalSize = headerSize + sampleCount * MemoryLayout<Float32>.size // 262160

    private let fd: Int32
    private let ptr: UnsafeMutableRawPointer
    private let size: Int

    init() {
        size = SharedRingBuffer.totalSize

        fd = open(SharedRingBuffer.filePath, O_RDWR | O_CREAT, 0o666)
        precondition(fd >= 0, "Failed to open shared ring buffer file: \(String(cString: strerror(errno)))")

        ftruncate(fd, off_t(size))

        let mapped = mmap(nil, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
        precondition(mapped != MAP_FAILED, "mmap failed: \(String(cString: strerror(errno)))")

        ptr = mapped!

        // Zero out header on creation
        ptr.storeBytes(of: UInt64(0), toByteOffset: 0, as: UInt64.self)
        ptr.storeBytes(of: UInt64(0), toByteOffset: 8, as: UInt64.self)
    }

    deinit {
        munmap(ptr, size)
        close(fd)
    }

    /// Writes Float32 frames into the ring buffer and updates writeIndex atomically.
    /// Safe to call from any thread (single-writer assumption).
    func write(frames: UnsafePointer<Float32>, count: Int) {
        let sampleCount = SharedRingBuffer.sampleCount
        let headerSize = SharedRingBuffer.headerSize
        let dataBase = ptr.advanced(by: headerSize)
            .assumingMemoryBound(to: Float32.self)

        let currentWriteIndex = ptr.load(fromByteOffset: 0, as: UInt64.self)
        var writePos = Int(currentWriteIndex) % sampleCount

        let firstChunk = min(count, sampleCount - writePos)
        dataBase.advanced(by: writePos).update(from: frames, count: firstChunk)

        if firstChunk < count {
            let remaining = count - firstChunk
            dataBase.update(from: frames.advanced(by: firstChunk), count: remaining)
        }

        // Memory barrier before updating write index so the consumer sees the data
        OSMemoryBarrier()

        let newWriteIndex = currentWriteIndex &+ UInt64(count)
        ptr.storeBytes(of: newWriteIndex, toByteOffset: 0, as: UInt64.self)
    }
}
