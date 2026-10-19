// Radio Music (unofficial) - folder naming rules and settings.txt parsing (milestone 4).
// Clean-room implementation from the documented behaviour of Music Thing
// Modular's Radio Music mk2 (Tom Whitwell with Chris Johnson).
//
// Pure functions on strings, shared by the library scanner, the plug-in and the tests.
#pragma once

#include <juce_core/juce_core.h>
#include <map>

namespace rm
{
constexpr int kNumBanks         = 16;    // documented: banks 0 to 15
constexpr int kMaxFilesPerBank  = 250;   // documented: 250 audio files in one bank
constexpr int kMaxFilesTotal    = 800;   // documented: 800 audio files per card

// settings.txt as a map from lower-case key (no spaces) to integer value.
using Settings = std::map<juce::String, int>;

// Documented: bank folders are named with a leading number 0 to 15 ("0", "05 shortwave", "15 - Koto plucks").
// Returns the bank number, or -1 if the name does not start with a number from 0 to 15.
inline int parseBankNumber (const juce::String& name)
{
    int digits = 0;
    while (digits < name.length() && name[digits] >= '0' && name[digits] <= '9')
        ++digits;

    if (digits == 0 || digits > 3)
        return -1;

    const int value = name.substring (0, digits).getIntValue();
    return (value >= 0 && value < kNumBanks) ? value : -1;
}

// Documented: if the text before the extension ends in "skipN" (N from 1 to 255), the next N resets are ignored
// while that file plays. Returns N, or 0 if the name does not follow the rule. (Letter case is not specified; we ignore it.)
inline int parseSkipCount (const juce::String& fileStem)
{
    int end = fileStem.length();
    int begin = end;
    while (begin > 0 && fileStem[begin - 1] >= '0' && fileStem[begin - 1] <= '9')
        --begin;

    const int numDigits = end - begin;
    if (numDigits < 1 || numDigits > 3 || begin < 4)
        return 0;

    if (! fileStem.substring (begin - 4, begin).equalsIgnoreCase ("skip"))
        return 0;

    const int n = fileStem.substring (begin).getIntValue();
    return (n >= 1 && n <= 255) ? n : 0;
}

// Documented: a subfolder whose name ends in "next" steps through its files in order; other subfolders pick at random.
inline bool endsWithNext (const juce::String& folderName)
{
    return folderName.trim().endsWithIgnoreCase ("next");
}

// Documented format: "<setting> = <whole number>", text after '#' is a comment, whitespace is ignored (also inside
// names), names are not case-sensitive. Not documented: what happens to a malformed line. We skip it.
inline Settings parseSettings (const juce::String& text)
{
    Settings out;

    for (auto line : juce::StringArray::fromLines (text))
    {
        const int hash = line.indexOfChar ('#');
        if (hash >= 0)
            line = line.substring (0, hash);

        juce::String compact;
        for (auto c : line)
            if (! juce::CharacterFunctions::isWhitespace (c))
                compact += c;

        const int eq = compact.indexOfChar ('=');
        if (eq <= 0)
            continue;

        const juce::String key = compact.substring (0, eq).toLowerCase();
        juce::String value = compact.substring (eq + 1);

        bool negative = false;
        if (value.startsWithChar ('-'))      { negative = true; value = value.substring (1); }
        else if (value.startsWithChar ('+')) { value = value.substring (1); }

        if (value.isEmpty() || value.length() > 9 || ! value.containsOnly ("0123456789"))
            continue;

        const int v = value.getIntValue();
        out[key] = negative ? -v : v;
    }

    return out;
}

// Per-bank settings inherit everything they do not set from the top-level file.
inline Settings mergeSettings (Settings base, const Settings& overrides)
{
    for (const auto& kv : overrides)
        base[kv.first] = kv.second;
    return base;
}

inline int settingOr (const Settings& s, const char* key, int fallback)
{
    const auto it = s.find (juce::String (key).toLowerCase());
    return it == s.end() ? fallback : it->second;
}

inline bool hasSetting (const Settings& s, const char* key)
{
    return s.find (juce::String (key).toLowerCase()) != s.end();
}
} // namespace rm
