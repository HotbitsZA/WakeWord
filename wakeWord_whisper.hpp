#pragma once

#include "cBaseWorker_V2.h"

#include "audio_capture.hpp"
#include "wakeword_config.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

class Vad;
class WhisperEngine;

// Continuous, offline wake-word listener (VAD based). Audio capture + VAD run
// on the RtAudio callback thread; resampling, whisper inference, and wake-word
// matching run on this class's background worker thread.
class WakeWordEngine : public cBaseWorker_V2
{
public:
    // Fires ONLY when a registered wake word is cleanly matched in speech.
    using WakeWordCallback =
        std::function<void(const std::string &matchedWord, const std::string &fullSentence)>;

    explicit WakeWordEngine(const WakeWordConfig &cfg, WakeWordCallback callback = nullptr);
    ~WakeWordEngine() noexcept override;

    WakeWordEngine(const WakeWordEngine &) = delete;
    WakeWordEngine &operator=(const WakeWordEngine &) = delete;

    void set_callback(WakeWordCallback callback);
    void update_wake_words(const std::vector<std::string> &new_words);

protected:
    bool preRun() override;
    void run() override;
    void stopTriggered() override;

private:
    void onAudioSamples(const int16_t *samples, unsigned frames);
    void enqueuePhrase(std::vector<int16_t> &&samples);

    WakeWordConfig m_cfg;

    mutable std::mutex m_cbMutex;
    WakeWordCallback m_callback;
    std::vector<std::string> m_wakeWords;

    std::unique_ptr<WhisperEngine> m_engine;
    std::unique_ptr<AudioCapture> m_capture;
    std::unique_ptr<Vad> m_vad;

    std::queue<std::vector<int16_t>> m_phraseQueue;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCV;
    size_t m_dropped = 0;
};