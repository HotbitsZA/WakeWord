#include "wakeWord_whisper.hpp"

#include "audio_utils.hpp"
#include "vad.hpp"
#include "wakeword_matcher.hpp"
#include "whisper_engine.hpp"

#include <cctype>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace wakeword;

namespace
{
    constexpr unsigned int TARGET_WHISPER_FREQ = 16000;
}

WakeWordEngine::WakeWordEngine(const WakeWordConfig &cfg, WakeWordCallback callback)
    : cBaseWorker_V2("WakeWordEngine"),
      m_cfg(cfg),
      m_callback(std::move(callback)),
      m_engine(std::make_unique<WhisperEngine>(cfg))
{
    update_wake_words(cfg.wakeWords);
}

WakeWordEngine::~WakeWordEngine() noexcept
{
    // Guarantee the worker thread (which uses m_engine/m_capture/m_vad) has
    // fully exited before those members are destroyed.
    stopThreadAndJoin();
}

void WakeWordEngine::set_callback(WakeWordCallback callback)
{
    std::lock_guard<std::mutex> lock(m_cbMutex);
    m_callback = std::move(callback);
}

void WakeWordEngine::update_wake_words(const std::vector<std::string> &new_words)
{
    std::lock_guard<std::mutex> lock(m_cbMutex);
    m_wakeWords.clear();
    for (const auto &word : new_words)
    {
        // Normalize to lowercase at the boundary so lookup stays simple.
        m_wakeWords.push_back(normalize_wake_text(word));
    }
}

bool WakeWordEngine::preRun()
{
    if (!m_engine || !m_engine->valid())
    {
        return false;
    }

    try
    {
        m_capture = std::make_unique<AudioCapture>(m_cfg);

        unsigned sampleRate = 0;
        std::string error;
        if (!m_capture->openDefault(
                [this](const int16_t *samples, unsigned frames) { onAudioSamples(samples, frames); },
                sampleRate, error))
        {
            std::cerr << "[" << name() << "] Capture open error: " << error << std::endl;
            return false;
        }

        m_vad = std::make_unique<Vad>(sampleRate, m_cfg);

        if (!m_capture->start(error))
        {
            std::cerr << "[" << name() << "] Capture start error: " << error << std::endl;
            return false;
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "[" << name() << "] Capture error: " << e.what() << std::endl;
        return false;
    }

    std::cout << "[" << name() << "] System online at " << m_capture->sampleRate()
              << " Hz. Calibrating noise levels... (please stay quiet briefly)" << std::endl;
    return true;
}

void WakeWordEngine::run()
{
    while (continueRunning())
    {
        std::vector<int16_t> phrase;

        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCV.wait(lock, [this]()
                           { return !m_phraseQueue.empty() || stopRequested(); });

            if (stopRequested() && m_phraseQueue.empty())
            {
                break;
            }
            if (!m_phraseQueue.empty())
            {
                phrase = std::move(m_phraseQueue.front());
                m_phraseQueue.pop();
            }
        }

        if (phrase.empty())
        {
            continue;
        }

        updateHeartbeat();

        // Conversion + resampling live on the worker thread, never on the audio thread.
        const unsigned nativeRate = m_capture ? m_capture->sampleRate() : TARGET_WHISPER_FREQ;
        std::vector<float> pcm = pcm16_to_float(phrase);
        pcm = resample_linear(pcm, nativeRate, TARGET_WHISPER_FREQ);

        std::string text;
        try
        {
            text = m_engine->transcribe(pcm);
        }
        catch (const std::exception &e)
        {
            std::cerr << "[" << name() << "] Whisper failure: " << e.what() << std::endl;
            continue;
        }

        if (text.empty())
        {
            continue;
        }

        std::vector<std::string> wakeWords;
        WakeWordCallback cb;
        {
            std::lock_guard<std::mutex> lock(m_cbMutex);
            wakeWords = m_wakeWords;
            cb = m_callback;
        }

        if (!cb)
        {
            continue;
        }

        const std::string matched = match_wake_word(text, wakeWords);
        if (!matched.empty())
        {
            cb(matched, text);
        }
    }
}

void WakeWordEngine::stopTriggered()
{
    m_queueCV.notify_all();
}

void WakeWordEngine::onAudioSamples(const int16_t *samples, unsigned frames)
{
    if (m_vad)
    {
        m_vad->feed(samples, frames);
    }

    std::optional<Vad::Phrase> phrase;
    while ((phrase = m_vad ? m_vad->takeCompleted() : std::nullopt))
    {
        enqueuePhrase(std::move(phrase->samples));
    }
}

void WakeWordEngine::enqueuePhrase(std::vector<int16_t> &&samples)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    if (m_phraseQueue.size() >= m_cfg.maxQueueDepth)
    {
        // Back-pressure: keep the newest phrase, drop the oldest, warn throttled.
        m_phraseQueue.pop();
        ++m_dropped;
        if (m_dropped == 1 || (m_dropped % 32) == 0)
        {
            std::cerr << "[" << name() << "] WARNING: transcription backlog, dropped "
                      << m_dropped << " phrase(s)" << std::endl;
        }
    }

    m_phraseQueue.push(std::move(samples));
    m_queueCV.notify_one();
}