#pragma once

#include "asr/AsrModelCatalog.h"

namespace AetherSDR {

// The files text-to-speech downloads on first use. Same contract as the ASR
// catalog (pinned size + SHA-256, verified before use) so the same model
// manager fetches them; they share its models folder.
namespace TtsModelCatalog {

const AsrModelTier& kokoroModel();    // kokoro-v1.0.int8.onnx, ~88 MB
const AsrModelTier& kokoroVoices();   // voices-v1.0.bin, ~27 MB

} // namespace TtsModelCatalog
} // namespace AetherSDR
