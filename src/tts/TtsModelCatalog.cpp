#include "tts/TtsModelCatalog.h"

namespace AetherSDR {
namespace TtsModelCatalog {

// Kokoro-82M v1.0 weights (Apache-2.0, hexgrad) as exported to ONNX by the
// kokoro-onnx project (MIT). Sizes and hashes pinned from the release files.
const AsrModelTier& kokoroModel()
{
    static const AsrModelTier tier = [] {
        AsrModelTier t;
        t.id = QStringLiteral("kokoro-v1.0-int8");
        t.displayName = QStringLiteral("Speech model");
        t.fileName = QStringLiteral("kokoro-v1.0.int8.onnx");
        t.sizeBytes = 92361271;
        t.sha256 = QStringLiteral("6e742170d309016e5891a994e1ce1559c702a2ccd0075e67ef7157974f6406cb");
        t.sources = {QStringLiteral(
            "https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/kokoro-v1.0.int8.onnx")};
        return t;
    }();
    return tier;
}

const AsrModelTier& kokoroVoices()
{
    static const AsrModelTier tier = [] {
        AsrModelTier t;
        t.id = QStringLiteral("kokoro-voices-v1.0");
        t.displayName = QStringLiteral("Voices");
        t.fileName = QStringLiteral("voices-v1.0.bin");
        t.sizeBytes = 28214398;
        t.sha256 = QStringLiteral("bca610b8308e8d99f32e6fe4197e7ec01679264efed0cac9140fe9c29f1fbf7d");
        t.sources = {QStringLiteral(
            "https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/voices-v1.0.bin")};
        return t;
    }();
    return tier;
}

} // namespace TtsModelCatalog
} // namespace AetherSDR
