#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Central configuration for the wake-word listener and its shared core.
struct WakeWordConfig
{
    // Whisper GGML model file, e.g. ~/models/ggml-base.en.bin.
    std::string modelPath;

    // Wake words to monitor. Normalized to lowercase before matching.
    std::vector<std::string> wakeWords = {"computer", "jarvis", "alexa", "teacher", "tutor"};

    std::string language = "en";
    int cpuThreads = 4;

    // 1 = greedy decoding, >1 = beam search.
    int beamSize = 5;
    float entropyThreshold = 2.4f;

    // VAD timing windows (seconds).
    float calibrationSeconds = 1.5f;
    float pauseSeconds = 1.0f;
    float prerollSeconds = 0.5f;
    float maxPhraseSeconds = 60.0f;

    // VAD energy tuning.
    float thresholdMultiplier = 2.5f;
    float hysteresisRatio = 0.7f;
    float thresholdFloor = 0.003f;

    // Worker queue back-pressure.
    size_t maxQueueDepth = 32;
    unsigned bufferFrames = 512;
};