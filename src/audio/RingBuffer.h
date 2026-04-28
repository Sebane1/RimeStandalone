#pragma once
#include <vector>
#include <atomic>

class RingBuffer {
public:
    // capacity in number of frames
    RingBuffer(int capacityFrames, int channels = 2);
    ~RingBuffer();

    // write frames. Returns the amount actually written.
    int write(const float* data, int numFrames);
    
    // read frames. Returns the amount actually read.
    int read(float* outData, int numFrames);

    // skip frames. Returns the amount actually skipped.
    int skip(int numFrames);

    int availableRead() const;
    int availableWrite() const;

private:
    std::vector<float> buffer;
    int capacity;
    std::atomic<int> readPos;
    std::atomic<int> writePos;
    int channels;
};
