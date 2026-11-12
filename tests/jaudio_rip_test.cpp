// SPDX-License-Identifier: MIT

// Tests the descriptors of the JAudio games' variants (JobDescriptor, rip/jaudio_rip.h) and the tracks of Pikmin 2's
// layers (LayerTracks, rip/jaudio1_rip.h).

#include "rip/jaudio_rip.h"

#include "xsflib/testing/check.h"

#include "hsf/format.h"
#include "rip/jaudio1_rip.h"

using namespace xsflib::testing;

namespace
{

// A variant's descriptor is the library's, with the variant's track states for its sound, or with its muted tracks for
// its piece. Nothing else changes.
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

} // namespace

int main()
{
    TestJobDescriptor();
    TestLayerTracks();

    return Summary("jaudio_rip");
}
