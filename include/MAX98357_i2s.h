#pragma once
#include <driver/i2s.h>
#include <math.h>
#include "FileStorage.h"
#include <AudioFileSourceFS.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>
#include <Arduino.h>
#include <AudioFileSourceBuffer.h>

#define i2s_num                         I2S_NUM_0
#define BUFFER_SIZE                     (16 * 1024)  // mỗi block 16KB
#define BUFFER_CAPACITY_BYTES           (BUFFER_SIZE * 2)
#define SAMPLE_RATE                     44100       // mặc định 44.1kHz
#define TASK_WAIT_EXIT_MS               500

enum WaveType { SINE, SQUARE, TRIANGLE, SAWTOOTH };

namespace MAX98357 {
    bool i2sInit(int I2S_LRC = 21, int I2S_BCLK = 20, int I2S_DOUT = 19, uint32_t  sample_rate = SAMPLE_RATE);
    void play(const uint8_t* data, size_t size);
    void stop();
    void generateWave(WaveType type = SINE, float freq = 1000.0f, int duration_ms = 1000, int sampleRate = SAMPLE_RATE);
    void playBeep();
    uint32_t  getWavSampleRate(const char *path);
    uint16_t  getWavChannels(const char *path);
    bool playWavDoubleBuffer(const char *filename);
    void stopWavDoubleBuffer();
    bool isPlayingDoubleBuffer();
}
