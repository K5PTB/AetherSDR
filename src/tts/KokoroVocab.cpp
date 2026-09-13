#include "tts/KokoroVocab.h"

#include <QHash>

namespace AetherSDR {
namespace KokoroVocab {

namespace {

struct Entry {
    char32_t codepoint;
    int id;
};

// Kokoro-82M v1.0 symbol vocabulary: one IPA/punctuation codepoint per token
// id, exactly as published in the model's config.json ("vocab", 114 entries;
// Apache-2.0, hexgrad/Kokoro-82M). Generated from that file — do not edit by
// hand; a symbol missing here is silently dropped from the model input.
constexpr Entry kEntries[] = {
    {0x003B,   1},  // ;
    {0x003A,   2},  // :
    {0x002C,   3},  // ,
    {0x002E,   4},  // .
    {0x0021,   5},  // !
    {0x003F,   6},  // ?
    {0x2014,   9},  // —
    {0x2026,  10},  // …
    {0x0022,  11},  // "
    {0x0028,  12},  // (
    {0x0029,  13},  // )
    {0x201C,  14},  // “
    {0x201D,  15},  // ”
    {0x0020,  16},  //
    {0x0303,  17},  // ̃
    {0x02A3,  18},  // ʣ
    {0x02A5,  19},  // ʥ
    {0x02A6,  20},  // ʦ
    {0x02A8,  21},  // ʨ
    {0x1D5D,  22},  // ᵝ
    {0xAB67,  23},  // ꭧ
    {0x0041,  24},  // A
    {0x0049,  25},  // I
    {0x004F,  31},  // O
    {0x0051,  33},  // Q
    {0x0053,  35},  // S
    {0x0054,  36},  // T
    {0x0057,  39},  // W
    {0x0059,  41},  // Y
    {0x1D4A,  42},  // ᵊ
    {0x0061,  43},  // a
    {0x0062,  44},  // b
    {0x0063,  45},  // c
    {0x0064,  46},  // d
    {0x0065,  47},  // e
    {0x0066,  48},  // f
    {0x0068,  50},  // h
    {0x0069,  51},  // i
    {0x006A,  52},  // j
    {0x006B,  53},  // k
    {0x006C,  54},  // l
    {0x006D,  55},  // m
    {0x006E,  56},  // n
    {0x006F,  57},  // o
    {0x0070,  58},  // p
    {0x0071,  59},  // q
    {0x0072,  60},  // r
    {0x0073,  61},  // s
    {0x0074,  62},  // t
    {0x0075,  63},  // u
    {0x0076,  64},  // v
    {0x0077,  65},  // w
    {0x0078,  66},  // x
    {0x0079,  67},  // y
    {0x007A,  68},  // z
    {0x0251,  69},  // ɑ
    {0x0250,  70},  // ɐ
    {0x0252,  71},  // ɒ
    {0x00E6,  72},  // æ
    {0x03B2,  75},  // β
    {0x0254,  76},  // ɔ
    {0x0255,  77},  // ɕ
    {0x00E7,  78},  // ç
    {0x0256,  80},  // ɖ
    {0x00F0,  81},  // ð
    {0x02A4,  82},  // ʤ
    {0x0259,  83},  // ə
    {0x025A,  85},  // ɚ
    {0x025B,  86},  // ɛ
    {0x025C,  87},  // ɜ
    {0x025F,  90},  // ɟ
    {0x0261,  92},  // ɡ
    {0x0265,  99},  // ɥ
    {0x0268, 101},  // ɨ
    {0x026A, 102},  // ɪ
    {0x029D, 103},  // ʝ
    {0x026F, 110},  // ɯ
    {0x0270, 111},  // ɰ
    {0x014B, 112},  // ŋ
    {0x0273, 113},  // ɳ
    {0x0272, 114},  // ɲ
    {0x0274, 115},  // ɴ
    {0x00F8, 116},  // ø
    {0x0278, 118},  // ɸ
    {0x03B8, 119},  // θ
    {0x0153, 120},  // œ
    {0x0279, 123},  // ɹ
    {0x027E, 125},  // ɾ
    {0x027B, 126},  // ɻ
    {0x0281, 128},  // ʁ
    {0x027D, 129},  // ɽ
    {0x0282, 130},  // ʂ
    {0x0283, 131},  // ʃ
    {0x0288, 132},  // ʈ
    {0x02A7, 133},  // ʧ
    {0x028A, 135},  // ʊ
    {0x028B, 136},  // ʋ
    {0x028C, 138},  // ʌ
    {0x0263, 139},  // ɣ
    {0x0264, 140},  // ɤ
    {0x03C7, 142},  // χ
    {0x028E, 143},  // ʎ
    {0x0292, 147},  // ʒ
    {0x0294, 148},  // ʔ
    {0x02C8, 156},  // ˈ
    {0x02CC, 157},  // ˌ
    {0x02D0, 158},  // ː
    {0x02B0, 162},  // ʰ
    {0x02B2, 164},  // ʲ
    {0x2193, 169},  // ↓
    {0x2192, 171},  // →
    {0x2197, 172},  // ↗
    {0x2198, 173},  // ↘
    {0x1D7B, 177},  // ᵻ
};

} // namespace

int tokenFor(char32_t codepoint)
{
    static const QHash<char32_t, int> table = [] {
        QHash<char32_t, int> t;
        for (const Entry& e : kEntries)
            t.insert(e.codepoint, e.id);
        return t;
    }();
    return table.value(codepoint, -1);
}

int size()
{
    return int(sizeof(kEntries) / sizeof(kEntries[0]));
}

} // namespace KokoroVocab
} // namespace AetherSDR
