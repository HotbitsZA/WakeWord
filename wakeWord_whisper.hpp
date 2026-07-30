#pragma once

#include "cBaseWorker_V2.h"
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <functional>

struct whisper_context;
class RtAudio;

class WakeWordEngine : public cBaseWorker_V2
{
public:
    // This callback fires ONLY when a registered wake word is cleanly matched in speech
    using WakeWordCallback = std::function<void(const std::string &matchedWord, const std::string &fullSentence)>;

    // Pass model path, a list of lowercase wake words, and the target callback handler
    WakeWordEngine(const std::string &model_path,
                   const std::vector<std::string> &wake_words,
                   WakeWordCallback callback = nullptr);
    ~WakeWordEngine() noexcept override;

    void set_callback(WakeWordCallback callback);
    void update_wake_words(const std::vector<std::string> &new_words);

protected:
    bool preRun() override;
    void run() override;
    void stopTriggered() override;

private:
    static int audio_callback(void *outputBuffer, void *inputBuffer, unsigned int nBufferFrames,
                              double streamTime, unsigned int status, void *userData);

    void process_incoming_samples(const int16_t *samples, unsigned int frames);
    void slice_and_queue_active_phrase();
    void evaluate_transcription_for_wake_words(const std::string &text);

    std::string m_modelPath;
    WakeWordCallback m_onWakeWordDetected;
    struct whisper_context *ctx = nullptr;
    std::unique_ptr<RtAudio> adc;

    // VAD & Energy State Parameters
    std::vector<int16_t> audio_buffer;
    std::mutex audio_mutex;

    float m_vadThreshold = 0.003f;
    size_t m_silenceTimeoutSamples = 0;
    size_t m_consecutiveSilenceSamples = 0;
    bool m_isSpeaking = false;

    // Wake Word matching tracking lists
    std::vector<std::string> m_wakeWords; // Sorted, lower-case strings for fast lookups
    std::mutex m_wordsMutex;

    // Deep Asynchronous Execution Pipelines
    std::queue<std::vector<float>> m_taskQueue;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCV;

    class Impl;
    std::unique_ptr<Impl> m_pImpl;
};
