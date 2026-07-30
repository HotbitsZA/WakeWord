#include "wakeWord_whisper.hpp"
#include <whisper.h>
#include <ggml-backend.h>
#include <RtAudio.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <cmath>
#include <numeric>
#include <algorithm>

namespace
{
    constexpr unsigned int TARGET_WHISPER_FREQ = 16000;
    constexpr unsigned int BUFFER_FRAMES = 512;
    constexpr float PAUSE_DURATION_SECONDS = 1.0f; // Aggressive parsing response window for wake triggers
    constexpr float PREROLL_DURATION_SECONDS = 0.5f;
}

class WakeWordEngine::Impl
{
public:
    unsigned int nativeSampleRate = TARGET_WHISPER_FREQ;
    std::vector<int16_t> prerollBuffer;
    size_t maxPrerollSamples = 0;

    bool isCalibrating = true;
    size_t calibrationSamplesAccumulated = 0;
    double calibrationEnergySum = 0.0;
    size_t targetCalibrationSamples = 0;
};

WakeWordEngine::WakeWordEngine(const std::string &model_path,
                               const std::vector<std::string> &wake_words,
                               WakeWordCallback callback)
    : cBaseWorker_V2("WakeWordEngineWorker"),
      m_modelPath(model_path),
      m_onWakeWordDetected(std::move(callback)),
      ctx(nullptr),
      m_pImpl(std::make_unique<Impl>())
{
    if (!std::ifstream(m_modelPath).good())
    {
        throw std::runtime_error("WakeWord Error: Model file missing at " + model_path);
    }
    update_wake_words(wake_words);
}

WakeWordEngine::~WakeWordEngine() noexcept
{
    static_cast<void>(stopThread());
    try
    {
        if (adc && adc->isStreamOpen())
        {
            adc->closeStream();
        }
    }
    catch (...)
    {
    }

    if (ctx)
    {
        whisper_free(ctx);
        ctx = nullptr;
    }
}

void WakeWordEngine::update_wake_words(const std::vector<std::string> &new_words)
{
    std::lock_guard<std::mutex> lock(m_wordsMutex);
    m_wakeWords.clear();
    for (auto word : new_words)
    {
        // Enforce lowercase transformations locally to streamline lookups
        std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c)
                       { return std::tolower(c); });
        m_wakeWords.push_back(word);
    }
}

int WakeWordEngine::audio_callback(void *outputBuffer, void *inputBuffer, unsigned int nBufferFrames,
                                   double streamTime, unsigned int status, void *userData)
{
    (void)outputBuffer;
    (void)streamTime;
    (void)status;
    auto *self = static_cast<WakeWordEngine *>(userData);
    if (!self || !inputBuffer)
        return 0;

    const int16_t *in = static_cast<const int16_t *>(inputBuffer);
    self->process_incoming_samples(in, nBufferFrames);
    return 0;
}

void WakeWordEngine::process_incoming_samples(const int16_t *samples, unsigned int frames)
{
    std::lock_guard<std::mutex> lock(audio_mutex);

    double blockEnergySum = 0.0;
    for (unsigned int i = 0; i < frames; ++i)
    {
        blockEnergySum += std::abs(static_cast<double>(samples[i]) / 32768.0);
    }
    float currentEnergy = static_cast<float>(blockEnergySum / frames);

    if (m_pImpl->isCalibrating)
    {
        m_pImpl->calibrationEnergySum += blockEnergySum;
        m_pImpl->calibrationSamplesAccumulated += frames;

        if (m_pImpl->calibrationSamplesAccumulated >= m_pImpl->targetCalibrationSamples)
        {
            float averageAmbientNoise = static_cast<float>((m_pImpl->calibrationEnergySum / m_pImpl->calibrationSamplesAccumulated) / frames);
            m_vadThreshold = std::max(0.003f, averageAmbientNoise * 2.5f);
            m_pImpl->isCalibrating = false;
            std::cout << "\n[Wake VAD Calibrated] Floor: " << averageAmbientNoise << " -> Threshold: " << m_vadThreshold << "\n"
                      << std::endl;
        }
        return;
    }

    if (currentEnergy > m_vadThreshold)
    {
        if (!m_isSpeaking)
        {
            m_isSpeaking = true;
            audio_buffer.insert(audio_buffer.end(), m_pImpl->prerollBuffer.begin(), m_pImpl->prerollBuffer.end());
            m_pImpl->prerollBuffer.clear();
        }
        audio_buffer.insert(audio_buffer.end(), samples, samples + frames);
        m_consecutiveSilenceSamples = 0;
    }
    else
    {
        if (m_isSpeaking)
        {
            audio_buffer.insert(audio_buffer.end(), samples, samples + frames);
            m_consecutiveSilenceSamples += frames;
            if (m_consecutiveSilenceSamples >= m_silenceTimeoutSamples)
            {
                slice_and_queue_active_phrase();
            }
        }
        else
        {
            m_pImpl->prerollBuffer.insert(m_pImpl->prerollBuffer.end(), samples, samples + frames);
            if (m_pImpl->prerollBuffer.size() > m_pImpl->maxPrerollSamples)
            {
                m_pImpl->prerollBuffer.erase(m_pImpl->prerollBuffer.begin(),
                                             m_pImpl->prerollBuffer.begin() + (m_pImpl->prerollBuffer.size() - m_pImpl->maxPrerollSamples));
            }
        }
    }
}

void WakeWordEngine::slice_and_queue_active_phrase()
{
    std::vector<int16_t> captured_audio = std::move(audio_buffer);
    audio_buffer.clear();
    m_isSpeaking = false;
    m_consecutiveSilenceSamples = 0;

    if (captured_audio.empty())
        return;

    std::vector<float> pcmf32_native(captured_audio.size());
    for (size_t i = 0; i < captured_audio.size(); ++i)
    {
        pcmf32_native[i] = static_cast<float>(captured_audio[i]) / 32768.0f;
    }

    std::vector<float> pcmf32_whisper;
    if (m_pImpl->nativeSampleRate == TARGET_WHISPER_FREQ)
    {
        pcmf32_whisper = std::move(pcmf32_native);
    }
    else
    {
        double resampleRatio = static_cast<double>(TARGET_WHISPER_FREQ) / m_pImpl->nativeSampleRate;
        size_t targetSize = static_cast<size_t>(pcmf32_native.size() * resampleRatio);
        pcmf32_whisper.resize(targetSize);

        for (size_t i = 0; i < targetSize; ++i)
        {
            double srcIdx = i / resampleRatio;
            size_t idxLower = static_cast<size_t>(std::floor(srcIdx));
            size_t idxUpper = std::min(idxLower + 1, pcmf32_native.size() - 1);
            double weight = srcIdx - idxLower;
            pcmf32_whisper[i] = (1.0 - weight) * pcmf32_native[idxLower] + weight * pcmf32_native[idxUpper];
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_taskQueue.push(std::move(pcmf32_whisper));
    }
    m_queueCV.notify_one();
}

void WakeWordEngine::evaluate_transcription_for_wake_words(const std::string &text)
{
    // 1. Uniformly clean text and map it to lowercase
    std::string cleanText = text;
    std::transform(cleanText.begin(), cleanText.end(), cleanText.begin(), [](unsigned char c)
                   { return std::tolower(c); });

    // Punctuation removal to secure exact edge phrase lookups
    cleanText.erase(std::remove_if(cleanText.begin(), cleanText.end(), [](unsigned char c)
                                   { return std::ispunct(c); }),
                    cleanText.end());

    std::vector<std::string> localWakeWords;
    WakeWordCallback localCallback;
    {
        std::lock_guard<std::mutex> lock(m_wordsMutex);
        localWakeWords = m_wakeWords;
    }
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        localCallback = m_onWakeWordDetected;
    }

    if (!localCallback)
        return;

    // 2. Perform localized substring parsing checks
    for (const auto &wakeWord : localWakeWords)
    {
        if (cleanText.find(wakeWord) != std::string::npos)
        {
            // Wake word found! Emit notification alert payload
            localCallback(wakeWord, text);
            break;
        }
    }
}

void WakeWordEngine::set_callback(WakeWordCallback callback)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_onWakeWordDetected = std::move(callback);
}

bool WakeWordEngine::preRun()
{
    ggml_backend_load_all();

    struct whisper_context_params cparams = whisper_context_default_params();
    ctx = whisper_init_from_file_with_params(m_modelPath.c_str(), cparams);
    if (!ctx)
        return false;

    adc = std::make_unique<RtAudio>();
    if (adc->getDeviceCount() < 1)
        return false;

    RtAudio::StreamParameters params;
    params.deviceId = adc->getDefaultInputDevice();
    params.nChannels = 1;
    unsigned int bufferFrames = BUFFER_FRAMES;

    RtAudio::DeviceInfo info = adc->getDeviceInfo(params.deviceId);
    m_pImpl->nativeSampleRate = info.preferredSampleRate;

    m_silenceTimeoutSamples = static_cast<size_t>(m_pImpl->nativeSampleRate * PAUSE_DURATION_SECONDS);
    m_pImpl->maxPrerollSamples = static_cast<size_t>(m_pImpl->nativeSampleRate * PREROLL_DURATION_SECONDS);
    m_pImpl->targetCalibrationSamples = static_cast<size_t>(m_pImpl->nativeSampleRate * 1.5f);

    try
    {
        adc->openStream(nullptr, &params, RTAUDIO_SINT16, m_pImpl->nativeSampleRate, &bufferFrames, &WakeWordEngine::audio_callback, this);
        adc->startStream();
        std::cout << "[" << name() << "] System online. Calibrating noise levels..." << std::endl;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[" << name() << "] Error initializing capture: " << e.what() << std::endl;
        return false;
    }

    return true;
}

void WakeWordEngine::run()
{
    while (continueRunning())
    {
        std::vector<float> chunkToProcess;

        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCV.wait(lock, [this]()
                           { return !m_taskQueue.empty() || stopRequested(); });

            if (stopRequested() && m_taskQueue.empty())
                break;

            if (!m_taskQueue.empty())
            {
                chunkToProcess = std::move(m_taskQueue.front());
                m_taskQueue.pop();
            }
        }

        if (!chunkToProcess.empty())
        {
            updateHeartbeat();

            whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
            wparams.print_progress = false;
            wparams.print_special = false;
            wparams.print_timestamps = false;
            wparams.language = "en";
            wparams.n_threads = 4;
            wparams.beam_search.beam_size = 5;
            if (whisper_full(ctx, wparams, chunkToProcess.data(), static_cast<int>(chunkToProcess.size())) != 0)
            {
                continue;
            }
            std::string text_output = "";
            int segments = whisper_full_n_segments(ctx);
            for (int i = 0; i < segments; ++i)
            {
                text_output += whisper_full_get_segment_text(ctx, i);
            }
            // Route raw text results through the wake validation filtering layer
            evaluate_transcription_for_wake_words(text_output);
        }
    }
}

void WakeWordEngine::stopTriggered()
{
    m_queueCV.notify_all();
}
