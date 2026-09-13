#pragma once

namespace AetherSDR {
namespace KokoroVocab {

// The Kokoro-82M token id for one phoneme/punctuation codepoint, or -1 when the
// model has no such symbol (the caller drops it, as the reference tokenizer does).
int tokenFor(char32_t codepoint);

// Number of symbols in the vocabulary (114 for Kokoro v1.0).
int size();

} // namespace KokoroVocab
} // namespace AetherSDR
