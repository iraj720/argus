#include "core/runtime/runtime_manifest.h"
#include "core/sources/packet_typing.h"

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
    manifest.manifest_version = irs3::kRuntimeManifestVersion;
    manifest.sources.push_back(irs3::RuntimeSourceSpec{"live/test"});

    irs3::RuntimeSinkSpec sink;
    sink.sink_id = "rec-a";
    sink.output_root = "./record-a";
    sink.output_mode = IRS3_HLS_SINK_OUTPUT_RECORD;
    sink.inputs.push_back(irs3::RuntimeSinkInputSpec{
        "source",
        "live/test",
        "video",
        "video/h264",
        "video/main",
    });
    sink.inputs.push_back(irs3::RuntimeSinkInputSpec{
        "source",
        "live/test",
        "voice",
        "voice/aac",
        "voice/main",
    });
    manifest.sinks.push_back(std::move(sink));
    return manifest;
}

} // namespace

int main() {
    const irs3::RuntimeManifest manifest = make_valid_manifest();
    std::unordered_set<std::string> active_sources{"live/test"};

    irs3::RuntimeManifestValidationResult bootstrap =
        irs3::ValidateRuntimeManifest(manifest, active_sources, false);
    expect(bootstrap.ok, "bootstrap validation should accept v2 manifest");

    irs3::RuntimeManifestValidationResult strict =
        irs3::ValidateRuntimeManifest(manifest, active_sources, true);
    expect(strict.ok, "strict validation should accept active source");

    irs3::RuntimeManifest wrong_version = manifest;
    wrong_version.manifest_version = 1;
    irs3::RuntimeManifestValidationResult version_check =
        irs3::ValidateRuntimeManifest(wrong_version, active_sources, false);
    expect(!version_check.ok, "manifest_version 1 should be rejected");

    irs3::RuntimeManifest voice_only = manifest;
    voice_only.sinks[0].inputs.erase(voice_only.sinks[0].inputs.begin());
    irs3::RuntimeManifestValidationResult no_video =
        irs3::ValidateRuntimeManifest(voice_only, active_sources, false);
    expect(!no_video.ok, "sink without video input should be rejected");

    irs3::RuntimeManifest bad_packet_type = manifest;
    bad_packet_type.sinks[0].inputs[0].packet_type = "voice/aac";
    irs3::RuntimeManifestValidationResult packet_type_mismatch =
        irs3::ValidateRuntimeManifest(bad_packet_type, active_sources, false);
    expect(!packet_type_mismatch.ok, "packet_type/stream_type mismatch should be rejected");

    irs3::RuntimeDesiredState desired = irs3::BuildRuntimeDesiredState(manifest);
    expect(desired.sinks_by_id.size() == 1, "desired state should contain sink");
    expect(desired.source_ids_by_sink_id["rec-a"].size() == 1, "sink should reference one source id");
    expect(desired.sink_ids_by_source_id["live/test"].size() == 1, "source should map to sink");

    expect(irs3::IsRemuxAllowedStreamType("video"), "video stream type allowed");
    expect(!irs3::IsRemuxAllowedStreamType("text"), "text stream type not allowed for remux");
    expect(irs3::PacketTypeMatchesStreamType("video/h264", "video"), "video/h264 matches video");

    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }

    std::fprintf(stderr, "runtime_manifest_test passed\n");
    return 0;
}
