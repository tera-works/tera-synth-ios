#include "audio_out.h"

#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <thread>

#include "hsynth/fileio.h"
#include "miniaudio.h"

struct AudioOut::Impl {
    ma_context context{};
    bool contextOk = false;
    ma_device device{};
    bool deviceOk = false;
    std::vector<ma_device_info> playback;
    std::vector<ma_device_info> capture;
};

namespace {
int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void dataCallback(ma_device* device, void* output, const void* input, ma_uint32 frames) {
    auto* self = static_cast<AudioOut*>(device->pUserData);
    const bool duplex = device->type == ma_device_type_duplex;
    self->audioCallback(static_cast<float*>(output), duplex ? static_cast<const float*>(input) : nullptr, frames,
                        device->playback.channels, duplex ? device->capture.channels : 0);
}
}  // namespace

AudioOut::AudioOut(hsynth::Engine& engine) : impl_(std::make_unique<Impl>()), engine_(engine) {
    impl_->contextOk = ma_context_init(nullptr, 0, nullptr, &impl_->context) == MA_SUCCESS;
}

AudioOut::~AudioOut() {
    stopFileRecording();  // joins the writer thread, completes the file
    stop();
    if (impl_->contextOk) ma_context_uninit(&impl_->context);
}

std::vector<AudioDeviceInfo> AudioOut::listDevices() {
    std::vector<AudioDeviceInfo> out;
    if (!impl_->contextOk) return out;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&impl_->context, &infos, &count, nullptr, nullptr) != MA_SUCCESS) return out;
    std::lock_guard<std::mutex> lock(lock_);
    impl_->playback.assign(infos, infos + count);
    for (ma_uint32 i = 0; i < count; ++i) out.push_back({infos[i].name, infos[i].isDefault != 0});
    return out;
}

std::vector<AudioDeviceInfo> AudioOut::listInputDevices() {
    std::vector<AudioDeviceInfo> out;
    if (!impl_->contextOk) return out;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&impl_->context, nullptr, nullptr, &infos, &count) != MA_SUCCESS) return out;
    std::lock_guard<std::mutex> lock(lock_);
    impl_->capture.assign(infos, infos + count);
    for (ma_uint32 i = 0; i < count; ++i) out.push_back({infos[i].name, infos[i].isDefault != 0});
    return out;
}

bool AudioOut::start(int deviceIndex, int sampleRate, int periodFrames, bool exclusive, int inputDevice, std::string& error) {
    stop();
    std::lock_guard<std::mutex> lock(lock_);
    if (!impl_->contextOk) { error = "audio backend unavailable"; return false; }

    const bool withInput = inputDevice >= -1;
    ma_device_config cfg = ma_device_config_init(withInput ? ma_device_type_duplex : ma_device_type_playback);
    if (withInput) {
        cfg.capture.format = ma_format_f32;
        cfg.capture.channels = 2;
        cfg.capture.shareMode = ma_share_mode_shared;
        if (inputDevice >= 0 && inputDevice < (int)impl_->capture.size()) cfg.capture.pDeviceID = &impl_->capture[(size_t)inputDevice].id;
    }
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.playback.shareMode = exclusive ? ma_share_mode_exclusive : ma_share_mode_shared;
    if (deviceIndex >= 0 && deviceIndex < (int)impl_->playback.size()) cfg.playback.pDeviceID = &impl_->playback[(size_t)deviceIndex].id;
    cfg.sampleRate = (ma_uint32)sampleRate;
    cfg.periodSizeInFrames = (ma_uint32)periodFrames;
    cfg.periods = 2;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.noPreSilencedOutputBuffer = MA_TRUE;  // the engine overwrites everything
    cfg.noClip = MA_TRUE;
    cfg.wasapi.noAutoConvertSRC = MA_FALSE;
    cfg.dataCallback = dataCallback;
    cfg.pUserData = this;

    ma_result r = ma_device_init(&impl_->context, &cfg, &impl_->device);
    if (r != MA_SUCCESS && exclusive) {
        cfg.playback.shareMode = ma_share_mode_shared;
        r = ma_device_init(&impl_->context, &cfg, &impl_->device);
    }
    if (r != MA_SUCCESS && withInput) {
        // keep the synth playing even if the microphone cannot be opened
        error = std::string("audio input unavailable (") + ma_result_description(r) + "), output only";
        cfg.deviceType = ma_device_type_playback;
        r = ma_device_init(&impl_->context, &cfg, &impl_->device);
    }
    if (r != MA_SUCCESS) {
        error = std::string("could not open audio device: ") + ma_result_description(r);
        return false;
    }
    impl_->deviceOk = true;
    sampleRate_ = (int)impl_->device.sampleRate;
    // The engine must run at the real device rate; prepare while no callback runs.
    engine_.prepare((float)sampleRate_, hsynth::kMaxBlockSize);
    lastCallbackNs_ = 0;
    late_ = 0;
    r = ma_device_start(&impl_->device);
    if (r != MA_SUCCESS) {
        error = std::string("could not start audio device: ") + ma_result_description(r);
        ma_device_uninit(&impl_->device);
        impl_->deviceOk = false;
        return false;
    }
    running_ = true;
    return true;
}

void AudioOut::stop() {
    std::lock_guard<std::mutex> lock(lock_);
    if (impl_->deviceOk) {
        ma_device_uninit(&impl_->device);  // stops and joins the audio thread
        impl_->deviceOk = false;
    }
    running_ = false;
}

void AudioOut::audioCallback(float* out, const float* in, uint32_t frames, uint32_t channels, uint32_t inChannels) {
    const int64_t t0 = nowNs();
    // late-callback detection: interval much longer than the period
    if (lastCallbackNs_ != 0) {
        const double expectedNs = 1e9 * (double)frames / (double)sampleRate_;
        if ((double)(t0 - lastCallbackNs_) > expectedNs * 2.5 + 2e6) late_.fetch_add(1, std::memory_order_relaxed);
    }
    lastCallbackNs_ = t0;

    uint32_t done = 0;
    int sp = scopePos_.load(std::memory_order_relaxed);
    const bool rec = recording_.load(std::memory_order_acquire);
    size_t rp = recPos_.load(std::memory_order_relaxed);
    while (done < frames) {
        const int n = (int)std::min<uint32_t>(frames - done, hsynth::kMaxBlockSize);
        if (in && inChannels > 0) {
            const float* src = in + (size_t)done * inChannels;
            float pk = 0.0f;
            for (int i = 0; i < n; ++i) {
                inL_[i] = src[(size_t)i * inChannels];
                inR_[i] = inChannels > 1 ? src[(size_t)i * inChannels + 1] : inL_[i];
                pk = std::max(pk, std::max(std::fabs(inL_[i]), std::fabs(inR_[i])));
            }
            if (pk > inPeak_.load(std::memory_order_relaxed)) inPeak_.store(pk, std::memory_order_relaxed);
            engine_.setInputBuffer(inL_, inR_);
        }
        engine_.process(left_, right_, n);
        float* o = out + (size_t)done * channels;
        const bool mute = muted_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) {
            float* f = o + (size_t)i * channels;
            f[0] = mute ? 0.0f : left_[i];
            if (channels > 1) f[1] = mute ? 0.0f : right_[i];
            for (uint32_t c = 2; c < channels; ++c) f[c] = 0.0f;
            scopeL_[sp] = left_[i];
            scopeR_[sp] = right_[i];
            sp = (sp + 1) & (kScopeSize - 1);
        }
        if (rec) {
            const bool fromIn = recInput_.load(std::memory_order_relaxed);
            const bool haveIn = in && inChannels > 0;
            for (int i = 0; i < n && rp + 2 <= recBuf_.size(); ++i) {
                recBuf_[rp++] = fromIn ? (haveIn ? inL_[i] : 0.0f) : left_[i];
                recBuf_[rp++] = fromIn ? (haveIn ? inR_[i] : 0.0f) : right_[i];
            }
        }
        if (fileRec_.load(std::memory_order_acquire)) {
            // ring full (the writer fell behind): the frames are dropped, never waited for
            const size_t cap = fileRing_.size();
            size_t w = ringWrite_.load(std::memory_order_relaxed);
            const size_t r = ringRead_.load(std::memory_order_acquire);
            for (int i = 0; i < n && w - r + 2 <= cap; ++i) {
                fileRing_[w & (cap - 1)] = left_[i];
                fileRing_[(w + 1) & (cap - 1)] = right_[i];
                w += 2;
            }
            ringWrite_.store(w, std::memory_order_release);
        }
        done += (uint32_t)n;
    }
    scopePos_.store(sp, std::memory_order_release);
    if (rec) recPos_.store(rp, std::memory_order_release);

    const float us = (float)(nowNs() - t0) / 1000.0f;
    const float load = us / (1e6f * (float)frames / (float)sampleRate_);
    cbUsAvg_.store(cbUsAvg_.load(std::memory_order_relaxed) * 0.98f + us * 0.02f, std::memory_order_relaxed);
    if (us > cbUsMax_.load(std::memory_order_relaxed)) cbUsMax_.store(us, std::memory_order_relaxed);
    if (load > cbLoadMax_.load(std::memory_order_relaxed)) cbLoadMax_.store(load, std::memory_order_relaxed);
    lastFrames_.store((int)frames, std::memory_order_relaxed);
    callbacks_.fetch_add(1, std::memory_order_relaxed);
}

AudioOutStats AudioOut::stats() {
    AudioOutStats s;
    std::lock_guard<std::mutex> lock(lock_);
    s.running = running_;
    if (impl_->contextOk) s.backend = ma_get_backend_name(impl_->context.backend);
    if (impl_->deviceOk) {
        s.deviceName = impl_->device.playback.name;
        s.sampleRate = (int)impl_->device.sampleRate;
        s.periodFrames = (int)impl_->device.playback.internalPeriodSizeInFrames;
        s.periods = (int)impl_->device.playback.internalPeriods;
        s.exclusive = impl_->device.playback.shareMode == ma_share_mode_exclusive;
        s.input = impl_->device.type == ma_device_type_duplex;
        if (s.input) s.inputName = impl_->device.capture.name;
    }
    s.inputPeak = inPeak_.load();
    s.lastCallbackFrames = lastFrames_.load();
    s.callbackUsAvg = cbUsAvg_.load();
    s.callbackUsMax = cbUsMax_.load();
    s.callbackLoadMax = cbLoadMax_.load();
    s.lateCallbacks = late_.load();
    s.callbacks = callbacks_.load();
    return s;
}

void AudioOut::resetPeaks() {
    inPeak_ = 0;
    cbUsMax_ = 0;
    cbLoadMax_ = 0;
    late_ = 0;
}

void AudioOut::copyScope(float* left, float* right, int n) const {
    const int end = scopePos_.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        const int idx = (end - n + i) & (kScopeSize - 1);
        left[i] = scopeL_[idx];
        right[i] = scopeR_[idx];
    }
}

void AudioOut::startRecording(double seconds, bool fromInput) {
    if (recording_) return;
    recInput_.store(fromInput);
    recBuf_.assign((size_t)(seconds * sampleRate_) * 2, 0.0f);  // UI thread allocates
    recPos_.store(0);
    recording_.store(true, std::memory_order_release);
}

size_t AudioOut::stopRecording(std::vector<float>& out) {
    recording_.store(false, std::memory_order_release);
    // let a running callback finish writing
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const size_t n = recPos_.load();
    out.assign(recBuf_.begin(), recBuf_.begin() + (std::ptrdiff_t)n);
    recBuf_.clear();
    recBuf_.shrink_to_fit();
    return n / 2;
}

double AudioOut::recordedSeconds() const { return (double)recPos_.load() / 2.0 / (double)sampleRate_; }

namespace {
void put32(FILE* f, uint32_t v) {
    const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    std::fwrite(b, 1, 4, f);
}
void put16(FILE* f, uint16_t v) {
    const uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    std::fwrite(b, 1, 2, f);
}
// 32-bit float stereo WAV header; sizes are filled in when the recording stops
void wavHeader(FILE* f, int rate, uint64_t frames) {
    const uint64_t data = frames * 8;
    const uint32_t d32 = data > 0xFFFFFFF0ull ? 0xFFFFFFF0u : (uint32_t)data;
    std::fwrite("RIFF", 1, 4, f);
    put32(f, 36 + d32);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 3);  // IEEE float
    put16(f, 2);
    put32(f, (uint32_t)rate);
    put32(f, (uint32_t)rate * 8);
    put16(f, 8);
    put16(f, 32);
    std::fwrite("data", 1, 4, f);
    put32(f, d32);
}
}  // namespace

bool AudioOut::startFileRecording(const std::string& path, std::string& error) {
    if (fileRec_.load()) return true;
    FILE* f = hsynth::fopenUtf8(path, "wb");
    if (!f) {
        error = "cannot write " + path;
        return false;
    }
    wavHeader(f, sampleRate_, 0);
    size_t cap = 1;
    while (cap < (size_t)sampleRate_ * 2 * 4) cap <<= 1;  // about 4 s of stereo
    fileRing_.assign(cap, 0.0f);
    ringWrite_.store(0);
    ringRead_.store(0);
    fileFrames_.store(0);
    fileHandle_ = f;
    fileQuit_.store(false);
    fileThread_ = std::thread([this] { fileWriter(); });
    fileRec_.store(true, std::memory_order_release);
    return true;
}

void AudioOut::fileWriter() {
    FILE* f = static_cast<FILE*>(fileHandle_);
    const size_t cap = fileRing_.size();
    std::vector<float> chunk;
    for (;;) {
        const bool quit = fileQuit_.load(std::memory_order_acquire);
        const size_t w = ringWrite_.load(std::memory_order_acquire);
        size_t r = ringRead_.load(std::memory_order_relaxed);
        if (w > r) {
            chunk.resize(w - r);
            for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = fileRing_[(r + i) & (cap - 1)];
            std::fwrite(chunk.data(), sizeof(float), chunk.size(), f);
            fileFrames_.fetch_add((w - r) / 2, std::memory_order_relaxed);
            ringRead_.store(w, std::memory_order_release);
        }
        if (quit) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

double AudioOut::stopFileRecording() {
    if (!fileRec_.load()) return 0.0;
    fileRec_.store(false, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // a running callback finishes its block
    fileQuit_.store(true, std::memory_order_release);
    if (fileThread_.joinable()) fileThread_.join();
    FILE* f = static_cast<FILE*>(fileHandle_);
    fileHandle_ = nullptr;
    const uint64_t frames = fileFrames_.load();
    if (f) {
        std::fseek(f, 0, SEEK_SET);
        wavHeader(f, sampleRate_, frames);
        std::fclose(f);
    }
    fileRing_.clear();
    fileRing_.shrink_to_fit();
    return (double)frames / (double)sampleRate_;
}

double AudioOut::fileRecordSeconds() const { return (double)fileFrames_.load() / (double)sampleRate_; }
