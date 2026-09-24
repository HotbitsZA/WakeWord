#include "wakeword_matcher.hpp"

#include <cctype>
#include <string>
#include <vector>

// Lowercase + strip punctuation. Whitespace and digits are preserved so whole
// words remain tokenizable.
std::string normalize_wake_text(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::ispunct(uc))
        {
            continue;
        }
        out += static_cast<char>(std::tolower(uc));
    }
    return out;
}

std::string match_wake_word(const std::string &transcription,
                            const std::vector<std::string> &wakeWords)
{
    const std::string normalized = normalize_wake_text(transcription);
    if (normalized.empty() || wakeWords.empty())
    {
        return std::string();
    }

    std::vector<std::string> tokens;
    size_t start = 0;
    while (start < normalized.size())
    {
        while (start < normalized.size() && normalized[start] == ' ')
        {
            ++start;
        }
        if (start >= normalized.size())
        {
            break;
        }
        size_t end = start;
        while (end < normalized.size() && normalized[end] != ' ')
        {
            ++end;
        }
        tokens.push_back(normalized.substr(start, end - start));
        start = end;
    }

    for (const auto &token : tokens)
    {
        if (token.empty())
        {
            continue;
        }
        for (const auto &wake : wakeWords)
        {
            if (normalize_wake_text(wake) == token)
            {
                return wake;
            }
        }
    }
    return std::string();
}