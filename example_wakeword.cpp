#include <iostream>
#include <thread>
#include <chrono>
#include <mutex>
#include "wakeWord_whisper.hpp"
#include "term_util.h"

std::mutex g_consoleMutex;

// Asynchronous execution handle activated ONLY upon signature word detection matches
void onWakeWordActivated(const std::string &matchedWord, const std::string &fullSentence)
{
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    std::cout << "\n🔥 [WAKE EVENT DETECTED]: System Activated!" << std::endl;
    std::cout << "🏷️  Trigger Key  : " << matchedWord << std::endl;
    std::cout << "💬 Full Context : \"" << fullSentence << "\"" << std::endl;
    std::cout << "=========================================================\n"
              << std::endl;
}

int main()
{
    TerminalInput term;
    std::string model_path = "/Users/phelelanicwele/models/ggml-base.en.bin";

    // Define the keyword array triggers
    std::vector<std::string> wakeWords = {"computer", "jarvis", "alexa"};

    std::cout << "Initializing Wake Word Listening Daemon..." << std::endl;
    std::unique_ptr<WakeWordEngine> wakeEngine;

    try
    {
        wakeEngine = std::make_unique<WakeWordEngine>(model_path, wakeWords, onWakeWordActivated);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Initialization Error: " << e.what() << std::endl;
        return 1;
    }

    if (!wakeEngine->startThread(cBaseWorker_V2::duration_type{15000}))
    {
        std::cerr << "Fatal Error: Failed to start the background engine layout thread." << std::endl;
        return 1;
    }

    term.enableRawMode();

    {
        std::lock_guard<std::mutex> lock(g_consoleMutex);
        std::cout << "\n==========================================================" << std::endl;
        std::cout << "🛡️  Wake Detection System Active. Listening in background..." << std::endl;
        std::cout << "Keywords monitored: [ COMPUTER, JARVIS, ALEXA ]" << std::endl;
        std::cout << "Press [ Q ] to safely exit the application." << std::endl;
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
            std::cerr << "\n⚠️ Listener thread crashed or stopped unexpectedly!" << std::endl;
            app_running = false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::lock_guard<std::mutex> lock(g_consoleMutex);
    std::cout << "\nTearing down audio streams..." << std::endl;
    term.disableRawMode();

    static_cast<void>(wakeEngine->stopThread());
    std::cout << "Shutdown complete." << std::endl;
    return 0;
}