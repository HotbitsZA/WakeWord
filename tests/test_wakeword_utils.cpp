#include "audio_utils.hpp"
#include "vad.hpp"
#include "wakeword_config.hpp"
#include "wakeword_matcher.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace wakeword;

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do                                                                    \
    {                                                                     \
        if (!(cond))                                                      \
        {                                                                 \
            std::cerr << "FAIL: " << #cond << " (line " << __LINE__ << ")" \
                      << std::endl;                                       \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

namespace
{
    std::vector<int16_t> make_sine(int16_t amplitude, double freq, unsigned rate, double seconds)
    {
        std::vector<int16_t> out(static_cast<size_t>(rate * seconds));
        for (size_t i = 0; i < out.size(); ++i)
        {
            out[i] = static_cast<int16_t>(
                amplitude * std::sin(2.0 * 3.14159265358979 * freq * i / rate));
        }
        return out;
    }
}

static void test_pcm16_to_float()
{
    std::vector<int16_t> in = {0, 32767, -32768, 16384, -16384};
    std::vector<float> out = pcm16_to_float(in);

    CHECK(out.size() == in.size());
    CHECK(std::fabs(out[0]) < 1e-6f);
    CHECK(std::fabs(out[1] - 1.0f) < 1e-4f);
    CHECK(std::fabs(out[2] + 1.0f) < 1e-4f);
    CHECK(std::fabs(out[3] - 0.5f) < 1e-4f);
    CHECK(std::fabs(out[4] + 0.5f) < 1e-4f);
}

static void test_resample_identity_and_shape()
{
    // Identity when rates match
    std::vector<float> in = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    CHECK(resample_linear(in, 16000, 16000) == in);

    // Downsample 48k -> 16k (2s of audio) => ~32000 samples
    std::vector<int16_t> raw = make_sine(12000, 440.0, 48000, 2.0);
    std::vector<float> pcm = pcm16_to_float(raw);
    std::vector<float> out = resample_linear(pcm, 48000, 16000);
    CHECK(!out.empty());
    CHECK(std::labs(static_cast<long>(out.size()) - 32000L) <= 2L);

    // Upsample 16k -> 48k
    std::vector<float> up = resample_linear(in, 16000, 48000);
    CHECK(up.size() > in.size());

    // Empty input
    CHECK(resample_linear({}, 48000, 16000).empty());
}

static void test_matcher()
{
    const std::vector<std::string> words = {"computer", "jarvis", "alexa"};

    // Case + punctuation insensitive match
    CHECK(match_wake_word("Hey, Computer!", words) == "computer");
    CHECK(match_wake_word("jarvis: run a task", words) == "jarvis");
    CHECK(match_wake_word("aLEXA play music", words) == "alexa");

    // Word boundary: no accidental substring matches
    CHECK(match_wake_word("microcomputer", words).empty());
    CHECK(match_wake_word("computerization cost money", words).empty());

    // Whole-word presence, order preserved
    CHECK(match_wake_word("please alexa", words) == "alexa");

    // No match
    CHECK(match_wake_word("hello world", words).empty());
    CHECK(match_wake_word("", words).empty());
    CHECK(match_wake_word("???", words).empty());
}

static void test_normalize_wake_text()
{
    CHECK(normalize_wake_text("  Jarvis!  ") == "  jarvis  ");
    CHECK(normalize_wake_text("Alexa's Song") == "alexas song");
    CHECK(normalize_wake_text("comp3") == "comp3");
}

static void test_vad_slices_on_silence()
{
    WakeWordConfig cfg;
    cfg.pauseSeconds = 0.5f;
    cfg.prerollSeconds = 0.0f;
    cfg.calibrationSeconds = 0.1f;
    cfg.thresholdFloor = 0.01f;
    cfg.thresholdMultiplier = 1.5f;

    const unsigned rate = 16000;
    Vad vad(rate, cfg);

    const auto t0 = std::chrono::steady_clock::time_point{std::chrono::seconds(1000)};

    // Calibration: feed quiet noise
    std::vector<int16_t> quiet = make_sine(200, 100.0, rate, cfg.calibrationSeconds + 0.05);
    size_t idx = 0;
    const size_t block = 512;
    while (idx < quiet.size())
    {
        const size_t n = std::min(block, quiet.size() - idx);
        vad.feed(quiet.data() + idx, n, t0 + std::chrono::microseconds(static_cast<int64_t>(idx * 1e6 / rate)));
        idx += n;
    }
    CHECK(!vad.calibrating());
    CHECK(vad.threshold() > 0.0f);

    // Speech: 0.4s of loud audio
    std::vector<int16_t> speech = make_sine(14000, 300.0, rate, 0.4);
    idx = 0;
    while (idx < speech.size())
    {
        const size_t n = std::min(block, speech.size() - idx);
        vad.feed(speech.data() + idx, n, t0 + std::chrono::microseconds(static_cast<int64_t>(idx * 1e6 / rate)));
        idx += n;
    }
    CHECK(!vad.takeCompleted().has_value());

    // Silence > pauseSeconds
    std::vector<int16_t> silence(static_cast<size_t>(rate * 0.7), 0);
    idx = 0;
    while (idx < silence.size())
    {
        const size_t n = std::min(block, silence.size() - idx);
        vad.feed(silence.data() + idx, n, t0 + std::chrono::microseconds(static_cast<int64_t>(idx * 1e6 / rate)));
        idx += n;
    }

    auto phrase = vad.takeCompleted();
    CHECK(phrase.has_value());
    if (phrase)
    {
        CHECK(phrase->samples.size() >= speech.size());
        CHECK(!phrase->samples.empty());
    }
}

static void test_vad_max_phrase_duration()
{
    WakeWordConfig cfg;
    cfg.pauseSeconds = 5.0f;     // long pause window so silence does NOT trigger
    cfg.maxPhraseSeconds = 1.0f; // short max duration
    cfg.prerollSeconds = 0.0f;
    cfg.calibrationSeconds = 0.1f;
    cfg.thresholdFloor = 0.01f;
    cfg.thresholdMultiplier = 1.5f;

    const unsigned rate = 16000;
    Vad vad(rate, cfg);

    const auto t0 = std::chrono::steady_clock::time_point{std::chrono::seconds(1000)};

    std::vector<int16_t> quiet(static_cast<size_t>(rate * 0.15), 0);
    size_t idx = 0;
    const size_t block = 512;
    while (idx < quiet.size())
    {
        const size_t n = std::min(block, quiet.size() - idx);
        vad.feed(quiet.data() + idx, n, t0);
        idx += n;
    }
    CHECK(!vad.calibrating());

    // Continuous loud audio spanning > maxPhraseSeconds => forced slice
    std::vector<int16_t> speech = make_sine(14000, 300.0, rate, 1.2);
    idx = 0;
    while (idx < speech.size())
    {
        const size_t n = std::min(block, speech.size() - idx);
        vad.feed(speech.data() + idx, n, t0 + std::chrono::microseconds(static_cast<int64_t>(idx * 1e6 / rate)));
        idx += n;
    }

    auto phrase = vad.takeCompleted();
    CHECK(phrase.has_value());
    if (phrase)
    {
        CHECK(phrase->samples.size() <= static_cast<size_t>(rate * cfg.maxPhraseSeconds) + block + 8);
    }
}

int main()
{
    test_pcm16_to_float();
    test_resample_identity_and_shape();
    test_matcher();
    test_normalize_wake_text();
    test_vad_slices_on_silence();
    test_vad_max_phrase_duration();

    if (g_failures == 0)
    {
        std::cout << "All tests passed." << std::endl;
        return 0;
    }
    std::cerr << g_failures << " test(s) FAILED" << std::endl;
    return 1;
}