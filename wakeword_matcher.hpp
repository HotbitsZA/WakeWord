#pragma once

#include <string>
#include <vector>

// Pure logic: normalized (lowercase, punctuation-stripped) text and wake-word
// list, returning the first wake word that appears as a whole token. Returns an
// empty string when no wake word is present. No I/O, no state: testable.
std::string match_wake_word(const std::string &transcription,
                            const std::vector<std::string> &wakeWords);

// Lowercase + strip non-alphanumeric characters. Used to normalize both the
// transcription and the configured wake words.
std::string normalize_wake_text(const std::string &text);