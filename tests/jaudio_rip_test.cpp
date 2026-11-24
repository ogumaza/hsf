// SPDX-License-Identifier: MIT

// Tests the descriptors of the JAudio games' variants (JobDescriptor, rip/jaudio_rip.h), the tracks of Pikmin 2's
// layers (LayerTracks, rip/jaudio1_rip.h) and how the length analysis measures Pikmin 2's sounds (Jaudio1MeasureOf).

#include "rip/jaudio_rip.h"

#include "xsflib/testing/check.h"

#include "hsf/format.h"
#include "rip/jaudio1_rip.h"

using namespace xsflib::testing;

namespace
{

// The descriptor of a variant is the library descriptor, with the track states of the variant for its sound, or with
// its muted tracks for its piece. Nothing else changes.
void TestJobDescriptor()
{
    hsf::ArchiveDescriptor library;
    library.jaudio.track_states.push_back({0x10005, {1, 1, 0}});
    library.jaudio.library2.pieces.resize(2);
    library.jaudio.library2.pieces[1].muted_tracks = 0x0ff0;

    hsf::rip::JaudioJob job;
    job.id = 0x10005;
    Check(hsf::rip::JobDescriptor(library, job).jaudio == library.jaudio,
          "a sound's job keeps the library's descriptor");

    job.track_states = hsf::JaudioTrackStates{0x10005, {1, 1, 1}};
    hsf::ArchiveDescriptor variant = hsf::rip::JobDescriptor(library, job);
    Check(variant.jaudio.track_states.size() == 1 && variant.jaudio.track_states[0] == *job.track_states,
          "a variant's track states replace those of its sound");

    job.track_states = hsf::JaudioTrackStates{0x10006, {2}};
    variant = hsf::rip::JobDescriptor(library, job);
    Check(variant.jaudio.track_states.size() == 2 && variant.jaudio.track_states[1] == *job.track_states,
          "a variant of a sound without track states adds them");

    job = {};
    job.id = hsf::JaudioLibrary2::kPieceSection | 1;
    job.muted_tracks = 0x0fc0;
    variant = hsf::rip::JobDescriptor(library, job);
    Check(variant.jaudio.library2.pieces[1].muted_tracks == 0x0fc0 &&
              variant.jaudio.library2.pieces[0] == library.jaudio.library2.pieces[0],
          "a variant's muted tracks replace those of its piece");

    job.id = hsf::JaudioLibrary2::kPieceSection | 2;
    Check(hsf::rip::JobDescriptor(library, job).jaudio == library.jaudio, "a piece past the list changes nothing");
}

// The tracks of a track map's layers follow its basic tracks, in the map's order. A track past the 16th has no bit.
void TestLayerTracks()
{
    const hsf::rip::TrackCounts counts = {4, 2, 0, 3, 1, 2};
    Check(hsf::rip::MutedTracks(counts) == 0x0ff0, "the directors mute every layer");
    Check(hsf::rip::LayerTracks(counts, 1) == 0x0030, "the event layer follows the basic tracks");
    Check(hsf::rip::LayerTracks(counts, 2) == 0, "a layer without tracks has none");
    Check(hsf::rip::LayerTracks(counts, 3) == 0x01c0, "the layer of enemies nearby follows the treasure layer");
    Check(hsf::rip::LayerTracks(counts, 4) == 0x0200, "the battle layer follows");
    Check(hsf::rip::LayerTracks(counts, 5) == 0x0c00, "the ground layer comes last");

    const hsf::rip::TrackCounts wide = {14, 4, 0, 0, 0, 255};
    Check(hsf::rip::LayerTracks(wide, 1) == 0xc000, "a layer keeps its tracks up to the 16th");
    Check(hsf::rip::LayerTracks(wide, 5) == 0, "a layer past the 16th track has none");
}

// A piece of music runs to the long limit. A piece that a conductor plays never repeats: it keeps the default limit,
// its loops don't count, and it reaches the limit with no warning. An effect keeps the default limit.
void TestMeasure()
{
    hsf::JaudioLibrary2 library;
    library.pieces.resize(2);
    library.pieces[1].kind = hsf::JaudioPiece::kConducted;
    library.pieces[1].conductor = "caveconc_00_0.cnd";

    const hsf::rip::Jaudio1Measure piece = hsf::rip::Jaudio1MeasureOf(library, hsf::JaudioLibrary2::kPieceSection);
    const hsf::rip::Jaudio1Measure conducted =
        hsf::rip::Jaudio1MeasureOf(library, hsf::JaudioLibrary2::kPieceSection | 1);
    const hsf::rip::Jaudio1Measure effect = hsf::rip::Jaudio1MeasureOf(library, 0x0800 | 0x12);

    Check(piece.loops && piece.limit == xsflib::rip::Limit::kLong && piece.warns_at_limit,
          "a piece runs to the long limit");
    Check(!conducted.loops && conducted.limit == xsflib::rip::Limit::kDefault && !conducted.warns_at_limit,
          "a conducted piece keeps the default limit, with no warning");
    Check(effect.loops && effect.limit == xsflib::rip::Limit::kDefault && effect.warns_at_limit,
          "an effect keeps the default limit");
}

} // namespace

int main()
{
    TestJobDescriptor();
    TestLayerTracks();
    TestMeasure();

    return Summary("jaudio_rip");
}
