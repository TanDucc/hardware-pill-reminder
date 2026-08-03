#include <MAX98357_i2s.h>

static File wavFile;

static uint8_t *bufferA = nullptr;
static uint8_t *bufferB = nullptr;
static size_t bytesReadA = 0;
static size_t bytesReadB = 0;

static volatile bool playing = false;
static volatile bool useBufferA = true;
static TaskHandle_t i2sTaskHandle = nullptr;
static uint32_t wavSampleRate = SAMPLE_RATE;
static uint16_t wavChannels = 1;

namespace MAX98357 {

    // I2S pin configuration
    bool i2sInit(int I2S_LRC, int I2S_BCK, int I2S_DOUT, uint32_t sample_rate) {
        i2s_config_t i2s_config = {
            .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
            .sample_rate = sample_rate,
            .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
            .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_MSB),
            .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
            .dma_buf_count = 12,
            .dma_buf_len = 1024,
            .use_apll = true,
            .tx_desc_auto_clear = true,
            .fixed_mclk = 0
        };
        i2s_pin_config_t pin_config = {
            .bck_io_num = I2S_BCK,      // Bit Clock
            .ws_io_num = I2S_LRC,        // Word Select (LRCK)
            .data_out_num = I2S_DOUT,   // Data Out
            .data_in_num = I2S_PIN_NO_CHANGE
        };
        // i2s_driver_uninstall(i2s_num);
        if (i2s_driver_install(i2s_num, &i2s_config, 0, NULL) != ESP_OK) {
            Serial.println("MAX98357_i2s log: ❌ I2S driver install failed!");
            return false;
        }
        i2s_set_pin(i2s_num, &pin_config);
        i2s_set_clk(i2s_num, sample_rate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
        i2s_zero_dma_buffer(i2s_num);
        return true;
    }

    // Play audio data
    void play(const uint8_t* data, size_t size) {
        size_t bytes_written;
        i2s_write(i2s_num, data, size, &bytes_written, portMAX_DELAY);
    }

    // Stop playback
    void stop() {
        i2s_zero_dma_buffer(i2s_num);
    }

    // Generate waveform data
    void generateWave(WaveType type, float freq, int duration_ms, int sampleRate) {
        int samples = duration_ms * sampleRate / 1000;
        int16_t buffer[256];
        float A = 32767.0f;

        for (int i = 0; i < samples; i += 256) {
            for (int j = 0; j < 256; j++) {
                int n = i + j;
                float t = (float)n / sampleRate;
                float value = 0;

                switch (type) {
                    case SINE:
                        value = sin(2 * M_PI * freq * t);
                        break;
                    case SQUARE:
                        value = (sin(2 * M_PI * freq * t) > 0) ? 1.0f : -1.0f;
                        break;
                    case TRIANGLE:
                        value = asin(sin(2 * M_PI * freq * t)) * (2.0f / M_PI);
                        break;
                    case SAWTOOTH:
                        value = 2.0f * (freq * t - floor(0.5f + freq * t));
                        break;
                }

                buffer[j] = (int16_t)(value * A);
            }
            play((const uint8_t*)buffer, sizeof(buffer));
        }
    }

    // Play a simple beep sound
    void playBeep() {
        generateWave(SINE, 1000.0f, 1000, 16000);
    }

    // ====================== I2S TASK ===========================
    static uint32_t _readWavSampleRateFromPath(const char *path) {
        File f = SD_MMC.open(path, "r");
        if (!f) return SAMPLE_RATE;
        f.seek(24);
        uint32_t rate = 0;
        f.read((uint8_t*)&rate, 4);
        f.close();
        return rate;
    }

    static uint16_t _readWavChannelsFromPath(const char *path) {
        File f = SD_MMC.open(path, "r");
        if (!f) return 1;
        f.seek(22);
        uint16_t ch = 0;
        f.read((uint8_t*)&ch, 2);
        f.close();
        return ch;
    }

    // ---- Mono -> Stereo in-place (buffer capacity must be >= 2 * bytes_in) ----
    static size_t convertMono16ToStereoInplace(uint8_t *buf, size_t bytesIn) {
        if (!buf || bytesIn == 0) return 0;
        size_t samples = bytesIn / 2; // số sample 16-bit
        // copy từ cuối -> đầu
        int16_t *p = (int16_t*)buf;
        for (int i = (int)samples - 1; i >= 0; --i) {
            int16_t v = p[i];
            p[2*i + 0] = v;
            p[2*i + 1] = v;
        }
        return bytesIn * 2;
    }

    // ---- i2s play task (dùng bufferA/bufferB do playWavDoubleBuffer cấp phát) ----
    static void IRAM_ATTR i2sPlayTask(void *arg) {
        Serial.println("MAX98357_i2s log: i2sPlayTask started");
        // We assume bufferA/bufferB already allocated and initial bytesReadA/bytesReadB filled
        while (playing) {
            if (useBufferA) {
                if (bytesReadA == 0) break;
                size_t bytesWritten = 0;
                esp_err_t r = i2s_write(i2s_num, bufferA, bytesReadA, &bytesWritten, portMAX_DELAY);
                if (r != ESP_OK) {
                    Serial.printf("MAX98357_i2s log: i2s_write err %d\n", r);
                }
                // read next into A
                size_t rlen = wavFile.read(bufferA, BUFFER_SIZE);
                if (rlen > 0) {
                    if (wavChannels == 1) {
                        // convert in-place: ensure capacity available
                        rlen -= (rlen % 2);
                        size_t outBytes = convertMono16ToStereoInplace(bufferA, rlen);
                        // align to 4 bytes
                        outBytes -= (outBytes % 4);
                        bytesReadA = outBytes;
                    } else {
                        rlen -= (rlen % 4);
                        bytesReadA = rlen;
                    }
                } else {
                    bytesReadA = 0;
                }
            } else {
                if (bytesReadB == 0) break;
                size_t bytesWritten = 0;
                esp_err_t r = i2s_write(i2s_num, bufferB, bytesReadB, &bytesWritten, portMAX_DELAY);
                if (r != ESP_OK) {
                    Serial.printf("MAX98357_i2s log: i2s_write err %d\n", r);
                }
                // read next into B
                size_t rlen = wavFile.read(bufferB, BUFFER_SIZE);
                if (rlen > 0) {
                    if (wavChannels == 1) {
                        rlen -= (rlen % 2);
                        size_t outBytes = convertMono16ToStereoInplace(bufferB, rlen);
                        outBytes -= (outBytes % 4);
                        bytesReadB = outBytes;
                    } else {
                        rlen -= (rlen % 4);
                        bytesReadB = rlen;
                    }
                } else {
                    bytesReadB = 0;
                }
            }
            useBufferA = !useBufferA;
        }

        // finish
        i2s_zero_dma_buffer(i2s_num);
        i2s_stop(i2s_num);
        if (wavFile) wavFile.close();
        // free buffers (they were allocated in play function, free here to be safe)
        if (bufferA) { free(bufferA); bufferA = nullptr; }
        if (bufferB) { free(bufferB); bufferB = nullptr; }
        i2sTaskHandle = nullptr;
        playing = false;
        Serial.println("MAX98357_i2s log: i2sPlayTask exiting");
        vTaskDelete(NULL);
    }

    // ====================== PLAY FUNCTION ===========================
    bool playWavDoubleBuffer(const char *filename) {
        // stop previous nicely (wait)
        if (playing) {
            stopWavDoubleBuffer();
            // small delay let resources free
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        if (!SD_MMC.exists(filename)) {
            Serial.printf("MAX98357_i2s log: ❌ Không tìm thấy file: %s\n", filename);
            return false;
        }

        // Read header info via helper functions (they open/close file internally)
        uint32_t rate = _readWavSampleRateFromPath(filename);
        uint16_t channels = _readWavChannelsFromPath(filename);
        if (rate < 8000 || rate > 96000) rate = SAMPLE_RATE;
        if (channels < 1) channels = 1;
        wavSampleRate = rate;
        wavChannels = channels;

        Serial.printf("MAX98357_i2s log: ℹ️ Sample Rate: %u Hz\n", wavSampleRate);
        Serial.printf("MAX98357_i2s log: ℹ️ Channels: %u\n", wavChannels);

        // open file for streaming (single File object used by task)
        wavFile = SD_MMC.open(filename, "r");
        if (!wavFile) {
            Serial.printf("MAX98357_i2s log: ❌ Không thể mở file %s\n", filename);
            return false;
        }

        // skip header
        wavFile.seek(44);

        // allocate buffers (capacity must be enough for mono->stereo expansion)
        if (!bufferA) bufferA = (uint8_t*)ps_malloc(BUFFER_CAPACITY_BYTES);
        if (!bufferB) bufferB = (uint8_t*)ps_malloc(BUFFER_CAPACITY_BYTES);
        if (!bufferA || !bufferB) {
            Serial.println("MAX98357_i2s log: ❌ Không đủ PSRAM cho buffers!");
            if (bufferA) { free(bufferA); bufferA = nullptr; }
            if (bufferB) { free(bufferB); bufferB = nullptr; }
            wavFile.close();
            return false;
        }

        // init I2S with correct sample rate and stereo output (we will output stereo frames)
        if (!i2sInit(21, 20, 19, wavSampleRate)) {
            Serial.println("MAX98357_i2s log: ❌ i2sInit failed");
            free(bufferA); bufferA = nullptr;
            free(bufferB); bufferB = nullptr;
            wavFile.close();
            return false;
        }

        // Fill initial two buffers
        size_t rA = wavFile.read(bufferA, BUFFER_SIZE);
        size_t rB = wavFile.read(bufferB, BUFFER_SIZE);

        if (rA == 0) {
            Serial.println("MAX98357_i2s log: ⚠️ File rỗng!");
            free(bufferA); bufferA = nullptr;
            free(bufferB); bufferB = nullptr;
            wavFile.close();
            return false;
        }

        // adjust & convert if mono
        if (wavChannels == 1) {
            rA -= (rA % 2);
            bytesReadA = convertMono16ToStereoInplace(bufferA, rA);
            bytesReadA -= (bytesReadA % 4);
            if (rB > 0) {
                rB -= (rB % 2);
                bytesReadB = convertMono16ToStereoInplace(bufferB, rB);
                bytesReadB -= (bytesReadB % 4);
            } else {
                bytesReadB = 0;
            }
        } else {
            rA -= (rA % 4);
            bytesReadA = rA;
            if (rB > 0) {
                rB -= (rB % 4);
                bytesReadB = rB;
            } else {
                bytesReadB = 0;
            }
        }

        useBufferA = true;
        playing = true;

        // create task on core 1 with higher priority so it has enough CPU/DMA time
        BaseType_t ok = xTaskCreatePinnedToCore(i2sPlayTask, "i2sPlayTask", 8192, NULL, 4, &i2sTaskHandle, 1);
        if (ok != pdPASS) {
            Serial.println("MAX98357_i2s log: ❌ tạo i2sPlayTask thất bại");
            free(bufferA); bufferA = nullptr;
            free(bufferB); bufferB = nullptr;
            wavFile.close();
            playing = false;
            return false;
        }

        Serial.printf("MAX98357_i2s log: 🎵 Đang phát file: %s\n", filename);
        return true;
    }

    // ====================== STOP FUNCTION ===========================
    void stopWavDoubleBuffer() {
        if (!playing) return;

        // ask task to stop
        playing = false;

        // wait for task to exit gracefully
        uint32_t t = 0;
        while (i2sTaskHandle != nullptr && t < TASK_WAIT_EXIT_MS) {
            vTaskDelay(pdMS_TO_TICKS(10));
            t += 10;
        }

        if (i2sTaskHandle != nullptr) {
            // force delete if still alive
            vTaskDelete(i2sTaskHandle);
            i2sTaskHandle = nullptr;
        }

        // free buffers if still allocated
        if (bufferA) { free(bufferA); bufferA = nullptr; }
        if (bufferB) { free(bufferB); bufferB = nullptr; }

        if (wavFile) {
            wavFile.close();
        }

        i2s_zero_dma_buffer(i2s_num);
        i2s_driver_uninstall(i2s_num);

        Serial.println("MAX98357_i2s log: 🛑 Đã dừng phát âm thanh (yêu cầu).");
    }

    bool isPlayingDoubleBuffer() {
        return playing;
    }
}
