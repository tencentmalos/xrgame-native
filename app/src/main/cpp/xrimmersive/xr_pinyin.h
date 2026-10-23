#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace xrimmersive {

// The system dictionary Kotlin unpacked from the APK and a writable user dictionary that learns
// chosen words. Any thread; the engine opens on first use.
void SetPinyinDictionary(const std::string &systemDictionary, const std::string &userDictionary);

/**
 * Pinyin to Chinese with AOSP's PinyinIME engine (libgooglepinyin): letters build the spelling,
 * candidates come from the system and user dictionaries, and choosing one fixes it; once the whole
 * spelling is fixed the sentence is committed. One composer per process (the engine is global).
 * XR thread only.
 */
class PinyinComposer {
public:
    // Opens the engine when a dictionary is set; false without one (the keyboard stays Latin).
    bool available();
    bool composing() const { return !spelling_.empty(); }
    void addLetter(char letter);
    // Removes the last fixed choice, else the last letter. False when not composing.
    bool backspace();
    // Chooses candidate `index`; returns the text to commit once the sentence is complete.
    std::string choose(size_t index);
    // The typed letters as they are, ending the composition.
    std::string commitLetters();
    void reset();
    // Writes what the user dictionary learnt; when the keyboard closes.
    void flush();

    // What the composition shows: the fixed Chinese followed by the spelling still to convert.
    const std::string &preedit() const { return preedit_; }
    size_t candidateCount() const { return count_; }
    const std::string &candidate(size_t index);

private:
    void refresh(size_t count);

    bool tried_ = false;
    bool open_ = false;
    std::string spelling_;
    size_t count_ = 0;
    std::string preedit_;
    std::vector<std::string> candidates_;
};

}  // namespace xrimmersive
