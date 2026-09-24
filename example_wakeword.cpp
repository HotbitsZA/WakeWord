#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "wakeWord_whisper.hpp"
#include "audio_utils.hpp"
#include "term_util.h"

namespace
{
    std::mutex g_consoleMutex;

    void onWakeWordActivated(const std::string &matchedWord, const std::string &fullSentence)
    {
        std::lock_guard<std::mutex> lock(g_consoleMutex);
        std::cout << "\n[WAKE EVENT DETECTED] System Activated!" << std::endl;
        std::cout << "  Trigger Key  : " << matchedWord << std::endl;
        std::cout << "  Full Context : \"" << fullSentence << "\"" << std::endl;
        std::cout << "=========================================================\n"
                  << std::endl;
    }

    // argv -> env -> ~/models/ggml-base.en.bin
    std::string resolve_model_path(int argc, char **argv)
    {
        if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0')
        {
            return expand_home(argv[1]);
        }
        if (const char *env = std::getenv("WAKE_MODEL"))
        {
            if (env[0] != '\0')
            {
                return expand_home(env);
            }
        }
        return expand_home("~/models/ggml-base.en.bin");
    }

    // Comma-separated WAKE_WORDS env var -> default list.
    std::vector<std::string> resolve_wake_words()
    {
        std::vector<std::string> words = {"computer", "jarvis", "alexa", "teacher", "tutor"};
        if (const char *env = std::getenv("WAKE_WORDS"))
        {
            std::string list = env;
            size_t start = 0;
            std::vector<std::string> parsed;
            while (start <= list.size())
            {
                size_t comma = list.find(',', start);
                std::string word = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                // Trim whitespace
                while (!word.empty() && isspace(static_cast<unsigned char>(word.front())))
                {
                    word.erase(word.begin());
                }
                while (!word.empty() && isspace(static_cast<unsigned char>(word.back())))
                {
                    word.pop_back();
                }
                if (!word.empty())
                {
                    parsed.push_back(word);
                }
                if (comma == std::string::npos)
                {
                    break;
                }
                start = comma + 1;
            }
            if (!parsed.empty())
            {
                words = parsed;
            }
        }
        return words;
    }
} // namespace

int main(int argc, char **argv)
{
    TerminalInput term;

    WakeWordConfig cfg;
    cfg.modelPath = resolve_model_path(argc, argv);
    cfg.wakeWords = resolve_wake_words();

    std::cout << "Initializing Wake Word Listening Daemon..." << std::endl;
    std::cout << "  Model   : " << cfg.modelPath << std::endl;

    std::unique_ptr<WakeWordEngine> wakeEngine;
    try
    {
        wakeEngine = std::make_unique<WakeWordEngine>(cfg, onWakeWordActivated);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Initialization Error: " << e.what() << std::endl;
        return 1;
    }

    if (!wakeEngine->startThread(cBaseWorker_V2::duration_type{15000}))
    {
        std::cerr << "Fatal Error: Failed to start the background engine thread." << std::endl;
        return 1;
    }

    term.enableRawMode();

    {
        std::lock_guard<std::mutex> lock(g_consoleMutex);
        std::cout << "\n==========================================================" << std::endl;
        std::cout << "  Wake Detection System Active. Listening in background..." << std::endl;
        std::cout << "  Keywords monitored: [";
        for (size_t i = 0; i < cfg.wakeWords.size(); ++i)
        {
            std::cout << (i ? ", " : " ") << cfg.wakeWords[i];
        }
        std::cout << " ]" << std::endl;
        std::cout << "  Press [ Q ] to safely exit the application." << std::endl;
        std::cout << "==========================================================\n"
                  << std::endl;
    }

    bool app_running = true;

    while (app_running)
    {
        int key = term.checkKey();
        if (key == 'q' || key == 'Q')
        {
            app_running = false;
        }

        if (wakeEngine->getState() == cBaseWorker_V2::enm_State::Stopped)
        {
            std::lock_guard<std::mutex> lock(g_consoleMutex);
            std::cerr << "\nListener thread crashed or stopped unexpectedly!" << std::endl;
            app_running = false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::lock_guard<std::mutex> lock(g_consoleMutex);
    std::cout << "\nTearing down audio streams..." << std::endl;
    term.disableRawMode();

    wakeEngine->stopThreadAndJoin();
    std::cout << "Shutdown complete." << std::endl;
    return 0;
}