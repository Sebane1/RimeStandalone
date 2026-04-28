#include "RingBuffer.h"
#include <algorithm>
#include <cstring>

RingBuffer::RingBuffer(int capacityFrames, int channels) 
    : capacity(capacityFrames), readPos(0), writePos(0), channels(channels) {
    buffer.resize(capacityFrames * channels, 0.0f);
}

RingBuffer::~RingBuffer() {}

int RingBuffer::availableRead() const {
    int w = writePos.load(std::memory_order_acquire);
    int r = readPos.load(std::memory_order_acquire);
    if (w >= r) {
        return w - r;
    } else {
        return capacity - r + w;
    }
}

int RingBuffer::availableWrite() const {
    return capacity - availableRead() - 1; // leave 1 space to distinguish full vs empty
}

int RingBuffer::write(const float* data, int numFrames) {
    int available = availableWrite();
    int framesToWrite = std::min(numFrames, available);
    if (framesToWrite == 0) return 0;

    int w = writePos.load(std::memory_order_relaxed);
    
    int spaceAtEnd = capacity - w;
    int samplesToWrite = framesToWrite * channels;
    int samplesSpaceAtEnd = spaceAtEnd * channels;

    if (framesToWrite <= spaceAtEnd) {
        std::memcpy(&buffer[w * channels], data, samplesToWrite * sizeof(float));
    } else {
        std::memcpy(&buffer[w * channels], data, samplesSpaceAtEnd * sizeof(float));
        int remainingSamples = samplesToWrite - samplesSpaceAtEnd;
        std::memcpy(&buffer[0], data + samplesSpaceAtEnd, remainingSamples * sizeof(float));
    }

    int nextW = (w + framesToWrite) % capacity;
    writePos.store(nextW, std::memory_order_release);

    return framesToWrite;
}

int RingBuffer::read(float* outData, int numFrames) {
    int available = availableRead();
    int framesToRead = std::min(numFrames, available);
    if (framesToRead == 0) return 0;

    int r = readPos.load(std::memory_order_relaxed);
    
    int spaceAtEnd = capacity - r;
    int samplesToRead = framesToRead * channels;
    int samplesSpaceAtEnd = spaceAtEnd * channels;

    if (framesToRead <= spaceAtEnd) {
        std::memcpy(outData, &buffer[r * channels], samplesToRead * sizeof(float));
    } else {
        std::memcpy(outData, &buffer[r * channels], samplesSpaceAtEnd * sizeof(float));
        int remainingSamples = samplesToRead - samplesSpaceAtEnd;
        std::memcpy(outData + samplesSpaceAtEnd, &buffer[0], remainingSamples * sizeof(float));
    }

    int nextR = (r + framesToRead) % capacity;
    readPos.store(nextR, std::memory_order_release);

    return framesToRead;
}

int RingBuffer::skip(int numFrames) {
    int available = availableRead();
    int framesToSkip = std::min(numFrames, available);
    if (framesToSkip == 0) return 0;

    int r = readPos.load(std::memory_order_relaxed);
    int nextR = (r + framesToSkip) % capacity;
    readPos.store(nextR, std::memory_order_release);

    return framesToSkip;
}
