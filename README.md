# Local AI Wake Word Engine

An offline wake-word listener built in C++17 for macOS and Apple Silicon. It captures microphone audio with RtAudio, segments speech with a lightweight voice activity detector, transcribes candidate phrases with `whisper.cpp`, and fires a callback when a configured wake word appears in the transcription.

The example monitors the words `computer`, `jarvis`, and `alexa`, then prints a wake event with the matched word and the full detected phrase.

## Highlights

- **Offline wake-word detection** with no cloud telemetry.
- **Continuous microphone monitoring** through RtAudio.
- **Ambient noise calibration** during startup to tune the VAD threshold (percentile-based and robust to transient spikes).
- **Pre-roll buffering** so phrase starts are less likely to be clipped.
- **Max-phrase duration guard** so long monologue is sliced instead of growing unboundedly.
- **Hysteresis hangover** so VAD does not chatter at the threshold boundary mid-phrase.
- **Whole-word (word-boundary) matching** to avoid false triggers like "computer" inside "microcomputer".
- **Whisper beam search** for more stable candidate transcriptions.
- **Callback-based event dispatch** for connecting wake events to local assistants, macros, or automation flows.
- **Bounded worker queue** with throttled back-pressure warnings.
- **Worker-thread lifecycle** through the sibling `ThreadComponent` project.

## How It Works

1. The app opens the default microphone and samples ambient room noise for calibration.
2. Incoming audio is scanned for active speech and split when a pause (or the max-phrase duration) is reached.
3. Captured speech is resampled to Whisper's 16 kHz mono PCM format when needed.
4. `whisper.cpp` transcribes the speech segment with beam search.
5. The transcript is lowercased, stripped of punctuation, and checked word-by-word against the configured wake-word list.
6. A user callback fires when a wake word is matched.

## Project Layout

```text
WakeWord/
|-- CMakeLists.txt           # ww_core static lib + WhisperWakeWordEngine + ww_tests
|-- README.md
|-- .gitignore
|-- example_wakeword.cpp     # demo app, terminal loop, model resolution
|-- wakeWord_whisper.hpp     # WakeWordEngine public interface (thin composition)
|-- wakeWord_whisper.cpp     # worker composition: capture + VAD + whisper + matching
|-- wakeword_config.hpp      # WakeWordConfig struct
|-- audio_utils.hpp          # shared inline helpers (resample, convert, trim, expand_home)
|-- vad.hpp / vad.cpp        # pure-logic voice activity detector + calibration
|-- whisper_engine.hpp/.cpp  # whisper.cpp lifecycle + transcription wrapper
|-- audio_capture.hpp/.cpp   # RAII RtAudio input wrapper
|-- wakeword_matcher.hpp/.cpp # pure whole-word wake matcher
|-- term_util.h              # RAII raw terminal polling helper
`-- tests/
    `-- test_wakeword_utils.cpp  # unit tests (pure logic, no hardware)
```

The engine also expects the sibling `ThreadComponent` project to be available at:

```text
../ThreadComponent/cBaseWorker_V2.h
```

## Requirements

- macOS 14 or newer
- AppleClang with C++17 support
- Homebrew
- Apple Silicon is recommended for best performance

Install the native dependencies:

```bash
brew install cmake pkg-config rtaudio whisper-cpp
```

## Model Setup

Download a Whisper GGML model before running the example. The default location is:

```text
~/models/ggml-base.en.bin
```

Create the folder and download the base English model:

```bash
mkdir -p ~/models
curl -L \
  -o ~/models/ggml-base.en.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin
```

To use a different model path, pass it as the first argument or set `WAKE_MODEL`:

```bash
./build/WhisperWakeWordEngine /path/to/ggml-base.en.bin
WAKE_MODEL=/path/to/ggml-base.en.bin ./build/WhisperWakeWordEngine
```

## Build

From this directory:

```bash
cmake -S . -B build
cmake --build build
```

The executable is created at:

```text
build/WhisperWakeWordEngine
```

Run the unit tests:

```bash
ctest --test-dir build
```

## Run

```bash
./build/WhisperWakeWordEngine
```

When the app starts, stay quiet during calibration. After the listener is active, say one of the configured wake words in a natural phrase.

Press `Q` to stop the listener, close audio streams, join the worker thread, and exit cleanly.

Example output:

```text
[WakeWordEngine] System online at 48000 Hz. Calibrating noise levels... (please stay quiet briefly)

[VAD Calibration Complete] Ambient noise: 0.00042 -> Dynamic Threshold auto-set to: 0.003

[WAKE EVENT DETECTED] System Activated!
  Trigger Key  : jarvis
  Full Context : "jarvis open the workspace"
=========================================================
```

## Configure Wake Words

By default the example listens for `computer`, `jarvis`, and `alexa`. Override with the comma-separated `WAKE_WORDS` environment variable:

```bash
WAKE_WORDS="assistant,computer" ./build/WhisperWakeWordEngine
```

At runtime, downstream code can replace the list:

```cpp
wakeEngine->update_wake_words({"assistant", "computer"});
```

Wake words are normalized to lowercase internally before matching, and matching is performed on whole words only.

## Namespaced Internals

The low-level engine classes (`WhisperEngine`, `AudioCapture`, `Vad`, plus the
`audio_utils` helpers and `match_wake_word`) live in a `wakeword::` namespace.
The public `WakeWordEngine` / `WakeWordConfig` / `WakeWordCallback` interface
stays in the global namespace. This lets the sibling `VoiceAssistantCore`
project compile SpeechToText's equally-named globals and these copies into a
single binary without symbol collisions. Standalone consumers are unaffected.

## Public Interface

Use `WakeWordEngine` with a `WakeWordConfig` and a callback that receives both the matched trigger word and the full transcribed phrase:

```cpp
class WakeWordEngine : public cBaseWorker_V2
{
public:
    using WakeWordCallback = std::function<void(
        const std::string &matchedWord,
        const std::string &fullSentence)>;

    explicit WakeWordEngine(const WakeWordConfig &cfg, WakeWordCallback callback = nullptr);
    ~WakeWordEngine() noexcept override;

    void set_callback(WakeWordCallback callback);
    void update_wake_words(const std::vector<std::string> &new_words);

protected:
    bool preRun() override;
    void run() override;
    void stopTriggered() override;
};
```

`WakeWordConfig` centralizes model path, wake words, VAD timing (calibration, pause, pre-roll, max phrase duration, threshold tuning, hysteresis), whisper parameters (`cpuThreads`, `beamSize`, `entropyThreshold`, `language`), and worker queue depth:

```cpp
struct WakeWordConfig
{
    std::string modelPath;
    std::vector<std::string> wakeWords = {"computer", "jarvis", "alexa"};
    std::string language = "en";
    int cpuThreads = 4;
    int beamSize = 5;
    float entropyThreshold = 2.4f;
    float calibrationSeconds = 1.5f;
    float pauseSeconds = 1.0f;
    float prerollSeconds = 0.5f;
    float maxPhraseSeconds = 60.0f;
    float thresholdMultiplier = 2.5f;
    float hysteresisRatio = 0.7f;
    float thresholdFloor = 0.003f;
    size_t maxQueueDepth = 32;
    unsigned bufferFrames = 512;
};
```

Minimal usage:

```cpp
auto on_wake = [](const std::string &word, const std::string &sentence) {
    std::cout << "Wake word: " << word << "\n";
    std::cout << "Phrase: " << sentence << "\n";
};

WakeWordConfig cfg;
cfg.modelPath = "~/models/ggml-base.en.bin";
cfg.wakeWords = {"computer", "jarvis", "alexa"};

WakeWordEngine engine(cfg, on_wake);
engine.startThread(cBaseWorker_V2::duration_type{15000});
```

## Troubleshooting

### CMake cannot find RtAudio or Whisper

Make sure Homebrew packages are installed and visible to `pkg-config`:

```bash
brew install pkg-config rtaudio whisper-cpp
pkg-config --modversion rtaudio
pkg-config --modversion whisper
```

The build prefers `pkg-config` for whisper and ggml but validates that the reported include directory actually contains `whisper.h` / `ggml-backend.h` (guarding against stale Homebrew Cellar paths), then falls back to `find_library` and `brew --prefix`.

### Missing GGML symbols while linking

The CMake file links both Whisper and GGML. If your Homebrew installation changes library names or paths, confirm the libraries are visible:

```bash
ls /opt/homebrew/lib/libwhisper*
ls /opt/homebrew/lib/libggml*
```

### Model file not found

Confirm the model exists at the resolved path:

```bash
ls ~/models/ggml-base.en.bin
```

### Microphone permission

macOS may require Terminal, iTerm, or your IDE to have microphone access:

```text
System Settings -> Privacy & Security -> Microphone
```

## Roadmap

- Emit structured wake events for local LLM and automation integrations.
- Add optional logging hooks for debugging calibration and matching behavior.