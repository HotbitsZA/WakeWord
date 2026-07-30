# Local AI Wake Word Engine

An offline wake-word listener built in C++17 for macOS and Apple Silicon. It captures microphone audio with RtAudio, segments speech with a lightweight voice activity detector, transcribes candidate phrases with `whisper.cpp`, and fires a callback when a configured wake word appears in the transcription.

The example monitors the words `computer`, `jarvis`, and `alexa`, then prints a wake event with the matched word and the full detected phrase.

## Highlights

- **Offline wake-word detection** with no cloud telemetry.
- **Continuous microphone monitoring** through RtAudio.
- **Ambient noise calibration** during startup to tune the VAD threshold.
- **Pre-roll buffering** so phrase starts are less likely to be clipped.
- **Whisper beam search** for more stable candidate transcriptions.
- **Callback-based event dispatch** for connecting wake events to local assistants, macros, or automation flows.
- **Worker-thread lifecycle** through the sibling `ThreadComponent` project.

## How It Works

1. The app opens the default microphone and samples ambient room noise for calibration.
2. Incoming audio is scanned for active speech and split when a pause is detected.
3. Captured speech is resampled to Whisper's 16 kHz mono PCM format when needed.
4. `whisper.cpp` transcribes the speech segment with beam search.
5. The transcript is lowercased, stripped of punctuation, and checked against the configured wake-word list.
6. A user callback fires when a wake word is matched.

## Project Layout

```text
WakeWord/
|-- CMakeLists.txt          # executable build: WhisperWakeWordEngine
|-- README.md
|-- .gitignore
|-- example_wakeword.cpp    # demo app and terminal loop
|-- wakeWord_whisper.hpp    # WakeWordEngine public interface
|-- wakeWord_whisper.cpp    # capture, VAD, resampling, transcription, matching
`-- term_util.h             # raw terminal polling helper
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

Download a Whisper GGML model before running the example. The current demo expects:

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

To use a different model path, update `model_path` in `example_wakeword.cpp`.

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

## Run

```bash
./build/WhisperWakeWordEngine
```

When the app starts, stay quiet during calibration. After the listener is active, say one of the configured wake words in a natural phrase.

Press `Q` to stop the listener, close audio streams, join the worker thread, and exit cleanly.

Example output:

```text
[Wake VAD Calibrated] Floor: 0.00042 -> Threshold: 0.003

[WAKE EVENT DETECTED]: System Activated!
Trigger Key  : jarvis
Full Context : "jarvis open the workspace"
=========================================================
```

## Configure Wake Words

The example defines its wake words in `example_wakeword.cpp`:

```cpp
std::vector<std::string> wakeWords = {"computer", "jarvis", "alexa"};
```

At runtime, downstream code can replace the list:

```cpp
wakeEngine->update_wake_words({"assistant", "computer"});
```

Wake words are normalized to lowercase internally before matching.

## Public Interface

Use `WakeWordEngine` with a callback that receives both the matched trigger word and the full transcribed phrase:

```cpp
class WakeWordEngine : public cBaseWorker_V2
{
public:
    using WakeWordCallback = std::function<void(
        const std::string &matchedWord,
        const std::string &fullSentence)>;

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
};
```

Minimal usage:

```cpp
auto on_wake = [](const std::string &word, const std::string &sentence) {
    std::cout << "Wake word: " << word << "\n";
    std::cout << "Phrase: " << sentence << "\n";
};

WakeWordEngine engine(
    "~/models/ggml-base.en.bin",
    {"computer", "jarvis", "alexa"},
    on_wake);

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

### Missing GGML symbols while linking

The CMake file links both Whisper and GGML. If your Homebrew installation changes library names or paths, confirm the libraries are visible:

```bash
ls /opt/homebrew/lib/libwhisper*
ls /opt/homebrew/lib/libggml*
```

### Model file not found

Confirm the model exists at the path used by `example_wakeword.cpp`:

```bash
ls ~/models/ggml-base.en.bin
```

### Microphone permission

macOS may require Terminal, iTerm, or your IDE to have microphone access:

```text
System Settings -> Privacy & Security -> Microphone
```

## Roadmap

- Add a small configuration object for VAD thresholds, pause timing, and wake-word sensitivity.
- Support phrase-boundary matching to reduce accidental substring matches.
- Emit structured wake events for local LLM and automation integrations.
- Add optional logging hooks for debugging calibration and matching behavior.
