#include "core/runtime/runtime_manifest.h"
#include "core/compose/compose_type.h"

#include <cstdio>
#include <unordered_set>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

irs3::RuntimeManifest make_valid_manifest() {
    irs3::RuntimeManifest manifest;
    manifest.sources.push_back(irs3::RuntimeSourceSpec{"live/test"});
    manifest.sinks.push_back(irs3::RuntimeSinkSpec{
        "record-a",
        "live/test",
        "./record-a",
        IRS3_HLS_SINK_OUTPUT_RECORD,
    });
    manifest.decoders.push_back(irs3::RuntimeDecoderSpec{
        "decoder-a",
        "live/test",
    });
    manifest.composes.push_back(irs3::RuntimeComposeSpec{
        "compose-a",
        "decoder-a",
        irs3::kComposeTypeJpgSnapshot,
        "./frames",
        "",
        "",
        50,
    });
    return manifest;
}

} // namespace

int main() {
    const irs3::RuntimeManifest manifest = make_valid_manifest();
    std::unordered_set<std::string> active_sources{"live/test"};

    irs3::RuntimeManifestValidationResult bootstrap =
        irs3::ValidateRuntimeManifest(manifest, active_sources, false);
    expect(bootstrap.ok, "bootstrap validation should accept manifest with decoder and compose");

    irs3::RuntimeManifestValidationResult strict =
        irs3::ValidateRuntimeManifest(manifest, active_sources, true);
    expect(strict.ok, "strict validation should accept active source decoder");

    irs3::RuntimeManifest unknown_compose_decoder = manifest;
    unknown_compose_decoder.composes[0].decoder_id = "missing-decoder";
    irs3::RuntimeManifestValidationResult unknown_decoder =
        irs3::ValidateRuntimeManifest(unknown_compose_decoder, active_sources, false);
    expect(!unknown_decoder.ok, "compose with unknown decoder_id should be rejected");

    irs3::RuntimeManifest unknown_compose_type = manifest;
    unknown_compose_type.composes[0].compose_type = "unknown";
    irs3::RuntimeManifestValidationResult unknown_type =
        irs3::ValidateRuntimeManifest(unknown_compose_type, active_sources, false);
    expect(!unknown_type.ok, "compose with unknown compose_type should be rejected");

    irs3::RuntimeManifest clip_prompt_manifest = manifest;
    clip_prompt_manifest.composes[0].compose_type = irs3::kComposeTypeClipPrompt;
    clip_prompt_manifest.composes[0].prompt = "a minion character";
    irs3::RuntimeManifestValidationResult clip_prompt =
        irs3::ValidateRuntimeManifest(clip_prompt_manifest, active_sources, false);
    expect(clip_prompt.ok, "clip_prompt compose with prompt should be accepted");

    irs3::RuntimeManifest clip_prompt_missing = clip_prompt_manifest;
    clip_prompt_missing.composes[0].prompt.clear();
    irs3::RuntimeManifestValidationResult clip_prompt_invalid =
        irs3::ValidateRuntimeManifest(clip_prompt_missing, active_sources, false);
    expect(!clip_prompt_invalid.ok, "clip_prompt compose without prompt should be rejected");

    irs3::RuntimeManifest side_by_side_manifest = manifest;
    side_by_side_manifest.composes[0].compose_type = irs3::kComposeTypeSideBySide;
    irs3::RuntimeManifestValidationResult side_by_side =
        irs3::ValidateRuntimeManifest(side_by_side_manifest, active_sources, false);
    expect(side_by_side.ok, "side_by_side compose type should be accepted");

    irs3::RuntimeDesiredState desired = irs3::BuildRuntimeDesiredState(manifest);
    expect(desired.decoders_by_id.size() == 1, "desired state should contain decoder");
    expect(desired.composes_by_id.size() == 1, "desired state should contain compose");
    expect(desired.compose_ids_by_decoder_id["decoder-a"].size() == 1, "compose route should be indexed by decoder");
    expect(desired.routes_by_compose_id["compose-a"] == "decoder-a", "compose route should map to decoder");

    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }

    std::fprintf(stderr, "runtime_manifest_test passed\n");
    return 0;
}
