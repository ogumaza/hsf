// SPDX-License-Identifier: MIT

// Checks the model of nn::atk's voices, channels and sequence player on the model of the renderer, without game data: a
// low-level voice on the renderer, its queue of wave buffers, and small archives that play notes through the engine.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

#include "atk/engine.h"
#include "atk/low_level_voice.h"
#include "audio/renderer.h"
#include "check.h"
#include "fsar/formats.h"
#include "fsar_files.h"

namespace
{

namespace atk = hsf::atk;
namespace audio = hsf::audio;

// `count` PCM16 samples of `value`, little-endian.
std::vector<uint8_t> ConstantWave(int count, int16_t value)
{
    std::vector<uint8_t> bytes;
    for (int i = 0; i < count; ++i)
    {
        bytes.push_back(static_cast<uint8_t>(static_cast<uint16_t>(value)));
        bytes.push_back(static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8));
    }

    return bytes;
}

// A renderer at 48 kHz with a final mix of six channels, whose front left and right the device sink takes, and the
// low-level voices on it.
struct Stage
{
    Stage() : renderer(Parameter()), voices(renderer, hardware, 4)
    {
        renderer.AcquireFinalMix(6);
        constexpr std::array<int, 2> kInputs = {0, 1};
        renderer.AddDeviceSink(kInputs);
        receiver.type = atk::OutputReceiver::Type::kFinalMix;
        receiver.mix = audio::Renderer::kFinalMix;
        receiver.channel_count = 6;
        receiver.bus_count = 1;
        receiver.clamp_send = {true, true, true, true};
    }

    static audio::RendererParameter Parameter()
    {
        audio::RendererParameter parameter;
        parameter.mix_buffer_count = 6;
        parameter.voice_count = 4;

        return parameter;
    }

    // A frame: the voices' statuses, their parameters and states, then the renderer's frame.
    void Frame()
    {
        voices.UpdateAllVoiceStatus();
        voices.UpdateAllVoice(atk::OutputMode::kStereo);
        renderer.RenderFrame();
    }

    atk::HardwareSettings hardware;
    audio::Renderer renderer;
    atk::LowLevelVoiceAllocator voices;
    atk::OutputReceiver receiver;
};

void TestLowLevelVoice()
{
    // A voice that starts to play acquires a voice of the renderer. The voice starts the renderer's voice with its
    // first buffer and gives the renderer's voice the volume and the gains. The first frame ramps the volume up from
    // silence, and the second has it. A constant comes out of the medium resampler at ratio 1 as 9959 in 10000.
    Stage stage;
    atk::LowLevelVoice* voice = stage.voices.AllocVoice();
    if (!Check(voice != nullptr, "a low-level voice is allocated"))
    {
        return;
    }
    const std::vector<uint8_t> wave = ConstantWave(4800, 10000);
    atk::WaveBuffer buffer;
    buffer.data = wave;
    buffer.sample_length = 4800;
    buffer.loop = true;
    voice->SetWave(atk::SampleFormat::kPcm16, 48000, {}, &stage.receiver);
    voice->Param().volume = 0.5f;
    voice->Param().mix[0] = 1.0f;
    voice->Param().mix[1] = 0.25f;
    voice->AppendWaveBuffer(buffer);
    voice->SetState(atk::LowLevelVoice::State::kPlay);

    stage.Frame();
    const bool started = voice->Slot() >= 0;
    stage.Frame();

    HSF_CHECK(started && buffer.status == atk::WaveBuffer::Status::kPlay);
    HSF_CHECK(stage.renderer.GetVoicePlayState(voice->Slot()) == audio::PlayState::kStarted);
    const std::span<const int16_t> output = stage.renderer.DeviceOutput();
    HSF_CHECK(output.size() == 480 && output[0] == 4980 && output[1] == 1245 && output[478] == 4980);

    // Stopping releases the renderer's voice at once, and the buffer is done.
    voice->FreeAllWaveBuffer();
    voice->SetState(atk::LowLevelVoice::State::kStop);
    HSF_CHECK(voice->Slot() < 0 && buffer.status == atk::WaveBuffer::Status::kDone);
}

void TestWaveBufferQueue()
{
    // A voice gives the renderer one buffer ahead of the one that plays, one a frame. So a first buffer shorter than a
    // frame leaves a gap before the second buffer. The second buffer arrives with the next frame. The renderer's report
    // that it has finished with a buffer reaches the voice two frames later, one buffer a frame. After the last buffer,
    // the voice stops the renderer's voice.
    Stage stage;
    atk::LowLevelVoice* voice = stage.voices.AllocVoice();
    if (!Check(voice != nullptr, "a low-level voice is allocated"))
    {
        return;
    }
    std::vector<uint8_t> wave = ConstantWave(100, 1000);
    const std::vector<uint8_t> rest = ConstantWave(300, 2000);
    wave.insert(wave.end(), rest.begin(), rest.end());
    atk::WaveBuffer first;
    first.data = wave;
    first.sample_length = 100;
    atk::WaveBuffer second;
    second.data = wave;
    second.sample_offset = 100;
    second.sample_length = 400;
    voice->SetWave(atk::SampleFormat::kPcm16, 48000, {}, &stage.receiver);
    voice->Param().mix[0] = 1.0f;
    voice->AppendWaveBuffer(first);
    voice->AppendWaveBuffer(second);
    voice->SetState(atk::LowLevelVoice::State::kPlay);

    stage.Frame();
    const std::vector<int16_t> frame1(stage.renderer.DeviceOutput().begin(), stage.renderer.DeviceOutput().end());
    stage.Frame();
    const std::vector<int16_t> frame2(stage.renderer.DeviceOutput().begin(), stage.renderer.DeviceOutput().end());
    stage.Frame();
    stage.Frame();
    const bool moved_on =
        first.status == atk::WaveBuffer::Status::kDone && second.status == atk::WaveBuffer::Status::kPlay;
    stage.Frame();
    stage.Frame();

    HSF_CHECK(frame1[2 * 50] == 995 && frame1[2 * 150] == 0 && frame1[2 * 239] == 0);
    HSF_CHECK(frame2[2 * 100] == 1991 && frame2[2 * 239] == 1991);
    HSF_CHECK(moved_on);
    HSF_CHECK(second.status == atk::WaveBuffer::Status::kDone);
    HSF_CHECK(stage.renderer.GetVoicePlayState(voice->Slot()) == audio::PlayState::kStopped);
}

// An archive with one sequence sound that plays `bytecode` on a bank of one instrument: a PCM16 wave of `samples`
// samples of 0x1000 at 48 kHz for every key, at original key 60.
std::vector<uint8_t> NoteArchive(const std::vector<uint8_t>& bytecode, int samples)
{
    fsar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.sample_rate = 48000;
    wave.loop_end = static_cast<uint32_t>(samples);
    wave.channels = {ConstantWave(samples, 0x1000)};

    fsar_files::Region region;
    region.original_key = 60;
    fsar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    fsar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    fsar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};
    bank.instruments = {keys};

    fsar_files::SoundSpec sound;
    sound.name = "SEQ_NOTE";
    sound.volume = 127;
    sound.banks = {0};
    fsar_files::ArchiveSpec spec;
    spec.sounds = {sound};
    spec.banks = {{"BANK_NOTE", 1}};
    spec.wave_archives = {{"WARC_NOTE", 2}};
    spec.players = {{"PLAYER_NOTE", 1}};
    spec.files = {{fsar_files::Sequence(false, bytecode, {{"Note", 0}}), "", false},
                  {fsar_files::Bank(false, bank), "", false},
                  {fsar_files::WaveArchive(false, {fsar_files::Wave(false, wave)}), "", false}};

    return fsar_files::Archive(false, spec);
}

// Whether a frame's output has any sound.
bool Sounds(std::span<const int16_t> output)
{
    for (const int16_t sample : output)
    {
        if (sample != 0)
        {
            return true;
        }
    }

    return false;
}

void TestEngine()
{
    // At a timebase of 48 and 120 beats a minute, a note of 24 ticks (250 ms) on a wave of 100 ms, then the end. The
    // first frame takes the start to the sound thread. The note starts in the second, and its voice sounds in the same
    // frame. The wave ends after 100 ms, and the sequence after its 24 ticks.
    const std::vector<uint8_t> kBytecode = {0xB0, 48, 0xE1, 120, 0x00, 60, 127, 24, 0xFF};
    const hsf::fsar::SoundArchive archive = hsf::fsar::SoundArchive::Load(NoteArchive(kBytecode, 4800));
    atk::ArchiveModel model(archive);
    atk::Engine& engine = *model.engine;
    HSF_CHECK(engine.StartSequence(0) != nullptr);
    HSF_CHECK(engine.StartSequence(1) == nullptr);

    engine.RunFrame();
    const bool silent_at_first = engine.Channels().ActiveCount() == 0 && !Sounds(model.renderer.DeviceOutput());
    engine.RunFrame();
    const bool sounds_next = engine.Channels().ActiveCount() == 1 && Sounds(model.renderer.DeviceOutput());
    int frames = 2;
    while (engine.IsBusy() && frames < 1000)
    {
        engine.RunFrame();
        ++frames;
    }

    HSF_CHECK(silent_at_first);
    HSF_CHECK(sounds_next);
    Check(!engine.IsBusy() && frames >= 50 && frames <= 54, "the sequence ends after " + std::to_string(frames));
    HSF_CHECK(engine.Channels().ActiveCount() == 0 && engine.Voices().ActiveCount() == 0);
    HSF_CHECK(engine.LowLevelVoices().AllocatedCount() == 0);
}

void TestNoteOffBeforeUpdate()
{
    // At the fastest tempo, a note of one tick ends in the frame it starts in, before the channel's first update. The
    // release waits for that update's envelope step. The note therefore sounds for a frame. Released at once, as on the
    // Wii U, it would be silent.
    const std::vector<uint8_t> kBytecode = {0xB0, 48, 0xE1, 0xFF, 0x03, 60, 127, 1, 0x80, 48, 0xFF};
    const hsf::fsar::SoundArchive archive = hsf::fsar::SoundArchive::Load(NoteArchive(kBytecode, 4800));
    atk::ArchiveModel model(archive);
    atk::Engine& engine = *model.engine;
    engine.StartSequence(0);

    engine.RunFrame();
    engine.RunFrame();
    const bool sounds = Sounds(model.renderer.DeviceOutput());
    engine.RunFrame();
    engine.RunFrame();

    HSF_CHECK(sounds);
    HSF_CHECK(engine.Channels().ActiveCount() == 0);
}

// The output of `frames` frames of the archive's sound.
std::vector<int16_t> Render(const std::vector<uint8_t>& bytecode, int frames, const atk::EngineOptions& options = {})
{
    const hsf::fsar::SoundArchive archive = hsf::fsar::SoundArchive::Load(NoteArchive(bytecode, 48000));
    atk::ArchiveModel model(archive, options);
    model.engine->StartSequence(0);
    std::vector<int16_t> output;
    for (int i = 0; i < frames; ++i)
    {
        model.engine->RunFrame();
        const std::span<const int16_t> frame = model.renderer.DeviceOutput();
        output.insert(output.end(), frame.begin(), frame.end());
    }

    return output;
}

void TestAuxSend()
{
    // A note sent only to aux bus A, with no effect on it, reaches the output as a note sent only to the main bus does.
    const std::vector<uint8_t> kMain = {0xB0, 48, 0xE1, 120, 0x00, 60, 127, 24, 0xFF};
    const std::vector<uint8_t> kAux = {0xB0, 48, 0xE1, 120, 0x00, 0xDB, 0, 0xD9, 127, 60, 127, 24, 0xFF};

    const std::vector<int16_t> through_main = Render(kMain, 20);
    const std::vector<int16_t> through_aux = Render(kAux, 20);

    HSF_CHECK(Sounds(through_main) && through_aux == through_main);
}

void TestAuxReverb()
{
    // A note sent only to aux bus A. An I3DL2 reverb there whose reflections and late reverb are silent, at a dry gain
    // of 1, passes it through 4 frames late, the ring's round trip. The reverb the reference game's stages use goes on
    // after the note has ended.
    const std::vector<uint8_t> kAux = {0xB0, 48, 0xE1, 120, 0x00, 0xDB, 0, 0xD9, 127, 60, 127, 24, 0xFF};
    hsf::audio::I3dl2ReverbParameterSet silent;
    silent.room_gain = -10000.0f;
    silent.reflections_gain = -10000.0f;
    silent.reverb_gain = -10000.0f;
    silent.late_reverb_decay_time = 1.0f;
    silent.late_reverb_hf_decay_ratio = 0.5f;
    silent.hf_reference = 5000.0f;
    silent.dry_gain = 1.0f;
    atk::EngineOptions transparent;
    transparent.aux_reverbs[0] = silent;
    atk::EngineOptions stage;
    stage.aux_reverbs[0] = hsf::audio::I3dl2ReverbParameterSet{-1000.0f, -1000.0f, 2.0f,  0.8f, -700.0f, 0.005f,
                                                               -500.0f,  0.04f,    50.0f, 3.0f, 6000.0f, 0.25f};
    constexpr std::size_t kRoundTrip = 4 * 240 * 2;

    const std::vector<int16_t> dry = Render(kAux, 200);
    const std::vector<int16_t> delayed = Render(kAux, 200, transparent);
    const std::vector<int16_t> reverberant = Render(kAux, 200, stage);

    const auto last = std::find_if(dry.rbegin(), dry.rend(), [](int16_t s) { return s != 0; });
    const std::size_t end = static_cast<std::size_t>(dry.rend() - last);
    HSF_CHECK(Sounds(dry) && end + kRoundTrip < dry.size());
    HSF_CHECK(!Sounds(std::span(delayed).first(kRoundTrip)));
    HSF_CHECK(std::equal(dry.begin(), dry.end() - kRoundTrip, delayed.begin() + kRoundTrip));
    HSF_CHECK(Sounds(std::span(reverberant).subspan(end + kRoundTrip)));
}

} // namespace

int main()
{
    TestLowLevelVoice();
    TestWaveBufferQueue();
    TestEngine();
    TestNoteOffBeforeUpdate();
    TestAuxSend();
    TestAuxReverb();
    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);

        return 1;
    }

    return 0;
}
