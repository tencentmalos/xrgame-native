#include "xr_pinyin.h"

#include <android/log.h>

#include <algorithm>
#include <mutex>

#if XRGAME_PINYIN
#include "pinyinime.h"
#endif

#define LOG_TAG "xrimmersive_pinyin"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace xrimmersive {
namespace {

std::mutex gDictionaryMutex;
std::string gSystemDictionary;
std::string gUserDictionary;

#if XRGAME_PINYIN
constexpr size_t kMaxCandidate = 64;
// Longer spellings are dropped by the engine; ten characters cover a phrase.
constexpr size_t kMaxSpelling = 40;
constexpr size_t kMaxChinese = 20;

std::string Utf8(const ime_pinyin::char16 *text, size_t length) {
    std::string out;
    for (size_t i = 0; i < length && text[i] != 0; ++i) {
        const unsigned c = text[i];
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::u16string Candidate(size_t index) {
    ime_pinyin::char16 buffer[kMaxCandidate + 1] = {};
    if (ime_pinyin::im_get_candidate(index, buffer, kMaxCandidate) == nullptr) return {};
    std::u16string text;
    for (size_t i = 0; i < kMaxCandidate && buffer[i] != 0; ++i) text += static_cast<char16_t>(buffer[i]);
    return text;
}

std::string Utf8(const std::u16string &text) {
    return Utf8(reinterpret_cast<const ime_pinyin::char16 *>(text.c_str()), text.size());
}
#endif

}  // namespace

void SetPinyinDictionary(const std::string &systemDictionary, const std::string &userDictionary) {
    std::lock_guard<std::mutex> lock(gDictionaryMutex);
    gSystemDictionary = systemDictionary;
    gUserDictionary = userDictionary;
}

bool PinyinComposer::available() {
#if XRGAME_PINYIN
    if (open_ || tried_) return open_;
    std::string system, user;
    {
        std::lock_guard<std::mutex> lock(gDictionaryMutex);
        system = gSystemDictionary;
        user = gUserDictionary;
    }
    if (system.empty()) return false;
    tried_ = true;
    open_ = ime_pinyin::im_open_decoder(system.c_str(), user.empty() ? nullptr : user.c_str());
    if (open_) {
        ime_pinyin::im_set_max_lens(kMaxSpelling, kMaxChinese);
        LOGI("pinyin engine open");
    } else {
        LOGE("pinyin engine could not open its dictionary; the XR keyboard stays Latin");
    }
    return open_;
#else
    return false;
#endif
}

void PinyinComposer::refresh(size_t count) {
    count_ = count;
    candidates_.clear();
#if XRGAME_PINYIN
    if (spelling_.empty() || count == 0) {
        preedit_ = spelling_;
        return;
    }
    // The fixed part of the sentence candidate, then the letters from the first unfixed spelling.
    const std::u16string sentence = Candidate(0);
    const size_t fixed = ime_pinyin::im_get_fixed_len();
    const ime_pinyin::uint16 *starts = nullptr;
    const size_t spellings = ime_pinyin::im_get_spl_start_pos(starts);
    size_t decoded = 0;
    const char *letters = ime_pinyin::im_get_sps_str(&decoded);
    std::string rest = letters != nullptr ? letters : spelling_;
    if (starts != nullptr && fixed <= spellings && starts[fixed] <= rest.size()) rest = rest.substr(starts[fixed]);
    preedit_ = Utf8(sentence.substr(0, std::min(fixed, sentence.size()))) + rest;
#else
    preedit_ = spelling_;
#endif
}

void PinyinComposer::addLetter(char letter) {
#if XRGAME_PINYIN
    if (spelling_.size() >= kMaxSpelling) return;
    spelling_ += letter;
    // im_add_letter is a stub in this engine (it returns 0); a search of the whole spelling
    // continues from the previous one's common prefix and keeps the choices fixed in it.
    refresh(ime_pinyin::im_search(spelling_.c_str(), spelling_.size()));
#else
    (void)letter;
#endif
}

bool PinyinComposer::backspace() {
    if (spelling_.empty()) return false;
#if XRGAME_PINYIN
    if (ime_pinyin::im_get_fixed_len() > 0) {
        refresh(ime_pinyin::im_cancel_last_choice());
        return true;
    }
    spelling_.pop_back();
    if (spelling_.empty()) {
        reset();
        return true;
    }
    ime_pinyin::im_reset_search();
    refresh(ime_pinyin::im_search(spelling_.c_str(), spelling_.size()));
#endif
    return true;
}

std::string PinyinComposer::choose(size_t index) {
#if XRGAME_PINYIN
    if (spelling_.empty() || index >= count_) return {};
    const size_t count = ime_pinyin::im_choose(index);
    const ime_pinyin::uint16 *starts = nullptr;
    const size_t spellings = ime_pinyin::im_get_spl_start_pos(starts);
    if (ime_pinyin::im_get_fixed_len() >= spellings) {
        // The whole spelling is fixed: the only candidate left is the sentence.
        const std::string sentence = Utf8(Candidate(0));
        reset();
        return sentence;
    }
    refresh(count);
#else
    (void)index;
#endif
    return {};
}

std::string PinyinComposer::commitLetters() {
    std::string letters;
    letters.swap(spelling_);
    reset();
    return letters;
}

void PinyinComposer::reset() {
    spelling_.clear();
    preedit_.clear();
    candidates_.clear();
    count_ = 0;
#if XRGAME_PINYIN
    if (open_) ime_pinyin::im_reset_search();
#endif
}

void PinyinComposer::flush() {
#if XRGAME_PINYIN
    if (open_) ime_pinyin::im_flush_cache();
#endif
}

const std::string &PinyinComposer::candidate(size_t index) {
    static const std::string kEmpty;
    if (index >= count_) return kEmpty;
#if XRGAME_PINYIN
    while (candidates_.size() <= index) candidates_.push_back(Utf8(Candidate(candidates_.size())));
    return candidates_[index];
#else
    return kEmpty;
#endif
}

}  // namespace xrimmersive
