#include "audio.h"

#include <atomic>
#include <cmath>
#include <cstring>

#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "rpc.h"
#include "streams.h"

namespace audio {
namespace {

const char* TAG = "audio";

constexpr uint32_t kSampleRate = 24000;
constexpr int kChunkFrames = kSampleRate / 50;  // 20 ms
constexpr gpio_num_t kMclk = GPIO_NUM_0;
constexpr gpio_num_t kWs = GPIO_NUM_33;
constexpr gpio_num_t kBclk = GPIO_NUM_34;
constexpr gpio_num_t kDout = GPIO_NUM_13;  // to AW88298
constexpr gpio_num_t kDin = GPIO_NUM_14;   // from ES7210
constexpr int kDefaultVolume = 45;
constexpr float kMicGainDb = 30;

enum class Wave { Sine, Square, Triangle, Ramp };
constexpr const char* kWaveNames[] = {"sine", "square", "triangle", "ramp"};
constexpr int kDefaultBeepMs = 250;

struct Beep {
    int freq_hz;
    int duration_ms;
    Wave wave;
};

// One period sample at phase p (0 .. 1), roughly equal loudness across waves
float waveSample(Wave w, float p) {
    switch (w) {
        case Wave::Square: return (p < 0.5f ? 1.0f : -1.0f) * 0.55f;
        case Wave::Triangle: return (p < 0.5f ? 4 * p - 1 : 3 - 4 * p);
        case Wave::Ramp: return (2 * p - 1) * 0.75f;
        default: return sinf(2 * 3.14159265f * p);
    }
}

i2s_chan_handle_t tx = nullptr;
i2s_chan_handle_t rx = nullptr;
esp_codec_dev_handle_t speaker = nullptr;
esp_codec_dev_handle_t mic = nullptr;
QueueHandle_t beeps = nullptr;
std::atomic<int> volume{kDefaultVolume};
std::atomic<int> level_rms{0};

// I2S setup after the official firmware (CoreS3AudioCodec::CreateDuplexChannels)
bool createChannels() {
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 6;
    chan.dma_frame_num = 240;
    chan.auto_clear_after_cb = true;
    if (i2s_new_channel(&chan, &tx, &rx) != ESP_OK) {
        return false;
    }

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_cfg.slot_cfg.left_align = true;
    std_cfg.gpio_cfg = {kMclk, kBclk, kWs, kDout, I2S_GPIO_UNUSED, {false, false, false}};

    i2s_tdm_config_t tdm_cfg = {};
    tdm_cfg.clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(kSampleRate);
    tdm_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    tdm_cfg.clk_cfg.bclk_div = 8;
    tdm_cfg.slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
        static_cast<i2s_tdm_slot_mask_t>(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3));
    tdm_cfg.gpio_cfg = {kMclk, kBclk, kWs, I2S_GPIO_UNUSED, kDin, {false, false, false}};

    return i2s_channel_init_std_mode(tx, &std_cfg) == ESP_OK && i2s_channel_init_tdm_mode(rx, &tdm_cfg) == ESP_OK &&
           i2s_channel_enable(tx) == ESP_OK && i2s_channel_enable(rx) == ESP_OK;
}

bool createCodecs() {
    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = I2S_NUM_0;
    i2s_cfg.rx_handle = rx;
    i2s_cfg.tx_handle = tx;
    const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port = I2C_NUM_1;
    i2c_cfg.addr = AW88298_CODEC_DEFAULT_ADDR;
    i2c_cfg.bus_handle = i2c::bus();
    const audio_codec_ctrl_if_t* out_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();

    aw88298_codec_cfg_t aw_cfg = {};
    aw_cfg.ctrl_if = out_ctrl;
    aw_cfg.gpio_if = gpio_if;
    aw_cfg.reset_pin = GPIO_NUM_NC;
    aw_cfg.hw_gain.pa_voltage = 5.0;
    aw_cfg.hw_gain.codec_dac_voltage = 3.3;
    aw_cfg.hw_gain.pa_gain = 1;
    const audio_codec_if_t* out_codec = aw88298_codec_new(&aw_cfg);

    i2c_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
    const audio_codec_ctrl_if_t* in_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    es7210_codec_cfg_t es_cfg = {};
    es_cfg.ctrl_if = in_ctrl;
    es_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3;
    const audio_codec_if_t* in_codec = es7210_codec_new(&es_cfg);

    if (data_if == nullptr || out_codec == nullptr || in_codec == nullptr) {
        return false;
    }
    esp_codec_dev_cfg_t dev_cfg = {ESP_CODEC_DEV_TYPE_OUT, out_codec, data_if};
    speaker = esp_codec_dev_new(&dev_cfg);
    dev_cfg = {ESP_CODEC_DEV_TYPE_IN, in_codec, data_if};
    mic = esp_codec_dev_new(&dev_cfg);
    if (speaker == nullptr || mic == nullptr) {
        return false;
    }

    esp_codec_dev_sample_info_t out_fs = {};
    out_fs.bits_per_sample = 16;
    out_fs.channel = 1;
    out_fs.sample_rate = kSampleRate;
    esp_codec_dev_sample_info_t in_fs = {};
    in_fs.bits_per_sample = 16;
    in_fs.channel = 2;
    in_fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
    in_fs.sample_rate = kSampleRate;
    if (esp_codec_dev_open(speaker, &out_fs) != ESP_CODEC_DEV_OK || esp_codec_dev_open(mic, &in_fs) != ESP_CODEC_DEV_OK) {
        return false;
    }
    esp_codec_dev_set_out_vol(speaker, volume);
    esp_codec_dev_set_in_channel_gain(mic, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), kMicGainDb);
    return true;
}

void playBeep(const Beep& b, int16_t* buf) {
    const int total = kSampleRate * b.duration_ms / 1000;
    const int ramp = kSampleRate * 5 / 1000;  // 5 ms fade in / out
    const float step = static_cast<float>(b.freq_hz) / kSampleRate;  // phase in periods
    float phase = 0;
    for (int done = 0; done < total;) {
        int n = total - done < kChunkFrames ? total - done : kChunkFrames;
        for (int i = 0; i < n; i++) {
            int k = done + i;
            float env = 1;
            if (k < ramp) env = static_cast<float>(k) / ramp;
            if (total - k < ramp) env = static_cast<float>(total - k) / ramp;
            buf[i] = static_cast<int16_t>(waveSample(b.wave, phase) * env * 16000);
            phase += step;
            if (phase >= 1) phase -= 1;
        }
        esp_codec_dev_write(speaker, buf, n * sizeof(int16_t));
        done += n;
    }
    // Trailing silence so the DMA does not repeat the last chunk
    for (int i = 0; i < kChunkFrames; i++) buf[i] = 0;
    esp_codec_dev_write(speaker, buf, kChunkFrames * sizeof(int16_t));
}

void run(void*) {
    int16_t* out = static_cast<int16_t*>(heap_caps_malloc(kChunkFrames * sizeof(int16_t), MALLOC_CAP_INTERNAL));
    int16_t* in = static_cast<int16_t*>(heap_caps_malloc(kChunkFrames * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL));
    float smoothed = 0;
    while (true) {
        Beep b;
        if (xQueueReceive(beeps, &b, 0) == pdTRUE) {
            playBeep(b, out);
            continue;
        }
        if (esp_codec_dev_read(mic, in, kChunkFrames * 2 * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        // RMS of channel 0 (microphone), with the DC offset removed
        int64_t sum = 0;
        for (int i = 0; i < kChunkFrames; i++) sum += in[i * 2];
        float mean = static_cast<float>(sum) / kChunkFrames;
        double sq = 0;
        for (int i = 0; i < kChunkFrames; i++) {
            float v = in[i * 2] - mean;
            sq += v * v;
        }
        float rms = sqrtf(static_cast<float>(sq / kChunkFrames));
        smoothed = smoothed * 0.8f + rms * 0.2f;
        level_rms = static_cast<int>(smoothed);
    }
}

bool audioSource(cJSON* obj) {
    if (speaker == nullptr) {
        return false;
    }
    int rms = level_rms;
    cJSON_AddNumberToObject(obj, "volume", volume.load());
    cJSON_AddNumberToObject(obj, "level_rms", rms);
    cJSON_AddNumberToObject(obj, "level_dbfs", rms > 0 ? roundf(200 * log10f(rms / 32768.0f)) / 10.0 : -100.0);
    return true;
}

cJSON* get(const cJSON*, rpc::Error* err) {
    cJSON* obj = cJSON_CreateObject();
    if (!audioSource(obj)) {
        cJSON_Delete(obj);
        return rpc::fail(err, rpc::kNotAvailable, "audio not available");
    }
    return obj;
}

cJSON* setVolume(const cJSON* params, rpc::Error* err) {
    int v = 0;
    if (speaker == nullptr) return rpc::fail(err, rpc::kNotAvailable, "audio not available");
    if (!rpc::getInt(params, "value", 0, 100, &v, err)) return nullptr;
    volume = v;
    esp_codec_dev_set_out_vol(speaker, v);
    return cJSON_CreateObject();
}

cJSON* beep(const cJSON* params, rpc::Error* err) {
    int freq = 0, duration = kDefaultBeepMs;
    if (speaker == nullptr) return rpc::fail(err, rpc::kNotAvailable, "audio not available");
    if (!rpc::getInt(params, "freq_hz", 20, 12000, &freq, err) ||
        !rpc::getOptInt(params, "duration_ms", 1, 1000, &duration, err)) {
        return nullptr;
    }
    bool ok = true;
    const char* wave_name = rpc::getOptString(params, "wave", err, &ok);
    if (!ok) return nullptr;
    Wave wave = Wave::Ramp;
    if (wave_name != nullptr) {
        int i = 0;
        while (i < 4 && strcmp(kWaveNames[i], wave_name) != 0) i++;
        if (i == 4) return rpc::fail(err, rpc::kInvalidParams, "wave: sine / square / triangle / ramp");
        wave = static_cast<Wave>(i);
    }
    Beep b = {freq, duration, wave};
    if (xQueueSend(beeps, &b, 0) != pdTRUE) {
        return rpc::fail(err, rpc::kBusy, "beep queue full");
    }
    return cJSON_CreateObject();
}

const rpc::Method kMethods[] = {
    {"audio.get", get, nullptr},
    {"audio.set_volume", setVolume, nullptr},
    {"audio.beep", beep, nullptr},
};

}  // namespace

void init() {
    if (!createChannels() || !createCodecs()) {
        ESP_LOGE(TAG, "audio init failed");
        events::error("sensor_init", "audio");
        speaker = nullptr;
        return;
    }
    beeps = xQueueCreate(4, sizeof(Beep));
}

void registerMethods() {
    rpc::registerMethods(kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}

void start() {
    if (speaker == nullptr) {
        return;
    }
    streams::add("audio", audioSource);
    xTaskCreatePinnedToCore(run, "audio", 4096, nullptr, 4, nullptr, 0);
}

}  // namespace audio
