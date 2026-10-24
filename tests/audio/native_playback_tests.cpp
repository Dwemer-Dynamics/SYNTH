// Compile with fake_windows before the SDK include path. No device, DLL, game or provider is used.
#include "audio/native_playback.hpp"
#include <iostream>
#include <stdexcept>

using namespace synth;
#define CHECK(value) do { if (!(value)) throw std::runtime_error{#value}; } while (false)

namespace {
std::shared_ptr<const std::vector<std::byte>> wave(unsigned channels = 1) {
    auto bytes = std::make_shared<std::vector<std::byte>>(76);
    const auto tag = [&](std::size_t at, const char* text) {
        for (std::size_t i = 0; i != 4; ++i) (*bytes)[at+i] = static_cast<std::byte>(text[i]);
    };
    const auto number = [&](std::size_t at, std::uint32_t value, std::size_t size) {
        for (std::size_t i = 0; i != size; ++i) (*bytes)[at+i] = static_cast<std::byte>((value >> (i*8)) & 0xFF);
    };
    tag(0,"RIFF"); number(4,68,4); tag(8,"WAVE"); tag(12,"fmt "); number(16,16,4);
    number(20,1,2); number(22,channels,2); number(24,16000,4); number(28,32000*channels,4);
    number(32,2*channels,2); number(34,16,2); tag(36,"data"); number(40,32,4);
    return bytes;
}
audio::NativeClip clip(const core::CancellationToken& token, std::string utterance = "utterance") {
    audio::NativeClip result;
    result.generation = token.generation().value(); result.request_id="request"; result.turn_id="turn";
    result.utterance_id=std::move(utterance); result.speaker_id="0x00000101";
    result.bytes=wave(); result.cancellation=token;
    result.speech_owner=runtime::RuntimeSpeechOwner{result.generation,result.request_id,result.turn_id,
        result.utterance_id,{0x101,"Fallout4.esm","save:a"},token};
    return result;
}
bool pump_until(audio::NativeAudioPlayback& playback, std::uint64_t generation,
                const audio::SpeechOwnerPump& callback, bool want_started) {
    for (unsigned i=0;i<100000;++i) {
        if (!playback.pump(generation,callback)) return false;
        if (want_started && playback.active_frame()) return true;
        if (!want_started && playback.idle()) return true;
        std::this_thread::yield();
    }
    throw std::runtime_error{"fake engine did not become ready"};
}
bool pump_until_spatial_pending(audio::NativeAudioPlayback& playback, std::uint64_t generation,
                                const audio::SpeechOwnerPump& callback) {
    for(unsigned i=0;i<100000;++i) {
        if(!playback.pump(generation,callback)) return false;
        if(playback.active_spatial_request(generation)) return true;
        std::this_thread::yield();
    }
    throw std::runtime_error{"fake spatial voice did not become pending"};
}
std::size_t position(std::string_view event) {
    const auto it=std::find(fake_xaudio::events.begin(),fake_xaudio::events.end(),event);
    CHECK(it!=fake_xaudio::events.end());
    return static_cast<std::size_t>(it-fake_xaudio::events.begin());
}
bool close(float left, float right) { return std::abs(left-right)<0.0001F; }
audio::SpatialPlaybackSettings spatial_settings() { return {0.8F,1.0,100.0,200.0,1.0,1.0,false}; }
audio::NativeClip spatial_clip(const core::CancellationToken& token, unsigned channels = 1) {
    auto value = clip(token); value.bytes = wave(channels); value.spatial = spatial_settings(); return value;
}
}

int main() try {
    const auto generation=core::RuntimeGeneration::initial();
    {
        fake_xaudio::reset(); core::CancellationSource cancellation{generation};
        audio::NativeAudioPlayback playback;
        const audio::SpeechOwnerPump owner=[&](const auto& value,bool discard) {
            (void)playback.idle(); // Reentrant queue inspection proves actor callback is outside playback lock.
            fake_xaudio::events.push_back(discard?"discard":value?"owner":"release"); return true;
        };
        CHECK(playback.enqueue(clip(cancellation.token())));
        CHECK(pump_until(playback,generation.value(),owner,true));
        CHECK(position("owner")<position("start"));
        playback.set_paused(true); fake_xaudio::events.clear();
        CHECK(playback.pump(generation.value(),owner));
        CHECK(!playback.active_frame() && fake_xaudio::events==std::vector<std::string>{"owner"});
        playback.set_paused(false); CHECK(fake_xaudio::events==std::vector<std::string>{"owner"});
        CHECK(playback.pump(generation.value(),owner)); CHECK(position("owner")<position("start"));
        fake_xaudio::events.clear(); fake_xaudio::voice->queued=0;
        CHECK(playback.pump(generation.value(),owner) && playback.idle());
        CHECK(position("destroy")<position("release"));
        CHECK(playback.enqueue(clip(cancellation.token(),"cancel")));
        CHECK(pump_until(playback,generation.value(),owner,true));
        fake_xaudio::events.clear(); cancellation.cancel();
        CHECK(playback.pump(generation.value(),owner) && playback.idle());
        CHECK(position("destroy")<position("release"));
    }
    for (int failure=0;failure<5;++failure) {
        fake_xaudio::reset(); core::CancellationSource cancellation{generation};
        audio::NativeAudioPlayback playback;
        bool admission_called{};
        const audio::SpeechOwnerPump owner=[&](const auto& value,bool) {
            fake_xaudio::events.push_back(value?"owner":"release");
            if(value) {
                admission_called=true;
                if(failure==0) return false;
                if(failure==3) cancellation.cancel(); // Candidate changed after admission, before audio commit.
                if(failure==4) throw std::runtime_error{"admission exception"};
            }
            return true;
        };
        fake_xaudio::fail_create=failure==1; fake_xaudio::fail_start=failure==2;
        CHECK(playback.enqueue(clip(cancellation.token())));
        const auto result=pump_until(playback,generation.value(),owner,false);
        CHECK(admission_called && !playback.active_frame());
        if(failure!=3) CHECK(!result);
        CHECK(position("owner")<position("release"));
        if(failure==2) CHECK(position("destroy")<position("release"));
        if(failure==0 || failure==3 || failure==4)
            CHECK(std::find(fake_xaudio::events.begin(),fake_xaudio::events.end(),"start")==fake_xaudio::events.end());
    }
    {
        fake_xaudio::reset(); core::CancellationSource cancellation{generation};
        audio::NativeAudioPlayback playback;
        const audio::SpeechOwnerPump owner=[](const auto& value,bool discard) {
            fake_xaudio::events.push_back(discard?"discard":value?"owner":"release"); return true;
        };
        CHECK(playback.enqueue(clip(cancellation.token())));
        CHECK(pump_until(playback,generation.value(),owner,true));
        fake_xaudio::events.clear(); CHECK(playback.stop_on_game_thread(owner));
        CHECK(playback.idle() && position("destroy")<position("release"));
        CHECK(!playback.enqueue(clip(cancellation.token(),"late")));
        fake_xaudio::events.clear();
        CHECK(playback.pump(generation.value(),owner));
        CHECK(playback.stop_on_game_thread(owner,true));
        CHECK(fake_xaudio::events.empty()); // A retired session cannot clear a newly adopted owner's state.
    }
    {
        fake_xaudio::reset(); core::CancellationSource cancellation{generation};
        audio::NativeAudioPlayback playback;
        const audio::SpeechOwnerPump owner=[](const auto& value,bool discard) {
            fake_xaudio::events.push_back(discard?"discard":value?"owner":"release"); return true;
        };
        CHECK(playback.enqueue(clip(cancellation.token(),"load")));
        CHECK(pump_until(playback,generation.value(),owner,true));
        fake_xaudio::events.clear(); CHECK(playback.stop_on_game_thread(owner,true));
        CHECK(playback.idle() && position("destroy")<position("discard"));
    }
    {
        fake_xaudio::reset(); core::CancellationSource cancellation{generation};
        audio::NativeAudioPlayback playback;
        const audio::SpeechOwnerPump reject_npc=[](const auto& value,bool) {
            fake_xaudio::events.push_back(value?"owner":"release"); return !value;
        };
        auto player=clip(cancellation.token(),"player");
        player.speaker_id="0x00000014"; player.speech_owner->speaker.form_id=0x14;
        CHECK(playback.enqueue(std::move(player)));
        CHECK(pump_until(playback,generation.value(),reject_npc,true));
        CHECK(std::find(fake_xaudio::events.begin(),fake_xaudio::events.end(),"owner")==fake_xaudio::events.end());
        fake_xaudio::voice->queued=0; CHECK(playback.pump(generation.value(),reject_npc));
        CHECK(playback.idle());
        auto unresolved=clip(cancellation.token(),"unresolved"); unresolved.speech_owner.reset();
        fake_xaudio::events.clear(); CHECK(playback.enqueue(std::move(unresolved)));
        CHECK(!playback.pump(generation.value(),reject_npc));
        CHECK(playback.idle() && fake_xaudio::events.empty());
    }
    {
        fake_xaudio::reset(); core::CancellationSource cancellation{generation};
        audio::NativeAudioPlayback playback;
        bool admit=true;
        const audio::SpeechOwnerPump owner=[&](const auto& value,bool) {
            fake_xaudio::events.push_back(value?"owner":"release"); return !value || admit;
        };
        CHECK(playback.enqueue(clip(cancellation.token())));
        CHECK(pump_until(playback,generation.value(),owner,true));
        fake_xaudio::events.clear(); admit=false;
        CHECK(!playback.pump(generation.value(),owner) && playback.idle());
        CHECK(position("owner")<position("destroy") && position("destroy")<position("release"));
    }
    {
        const auto settings = spatial_settings();
        audio::Pose listener{{0,0,0},{0,1,0},{0,0,1},1};
        const auto right = audio::spatial_mix(settings,{100,0,0},listener,false);
        CHECK(right && close(right->pan,1.0F) && close(right->volume,0.4F));
        listener.forward={0,-1,0};
        const auto turned = audio::spatial_mix(settings,{100,0,0},listener,false);
        CHECK(turned && close(turned->pan,-1.0F) && close(turned->volume,0.4F));
        listener.position={100,0,0};
        const auto near = audio::spatial_mix(settings,{100,0,0},listener,false);
        CHECK(near && close(near->pan,0.0F) && close(near->volume,0.8F));
        const auto far = audio::spatial_mix(settings,{300,0,0},listener,false);
        CHECK(far && close(far->volume,0.0F));
        const auto interior = audio::spatial_mix(settings,{200,0,0},listener,true);
        CHECK(interior && close(interior->volume,0.0F));
        auto inverted = settings; inverted.invert_heading=true;
        CHECK(close(audio::spatial_mix(inverted,{200,0,0},listener,false)->pan,1.0F));
        listener.forward={0,0,0}; CHECK(!audio::spatial_mix(settings,{200,0,0},listener,false));
        listener.forward={0,1,0};
        CHECK(!audio::spatial_mix(settings,{std::numeric_limits<double>::quiet_NaN(),0,0},listener,false));
        auto invalid=settings; invalid.base_volume=std::numeric_limits<float>::quiet_NaN();
        CHECK(!audio::spatial_mix(invalid,{0,0,0},listener,false));
        CHECK(!audio::point_source_matrix(1,2,4,0.0F)); // Mask/count mismatch.
        CHECK(!audio::point_source_matrix(1,2,0x30,0.0F)); // Rear-only pair is not a front stereo layout.
        CHECK(!audio::point_source_matrix(3,2,3,0.0F)); // Unsupported multichannel TTS fails closed.
        CHECK(!audio::point_source_matrix(1,1,0x8,0.0F)); // LFE is not full-range mono dialogue.
        CHECK(!audio::point_source_matrix(1,1,0x80000000,0.0F));
        const auto mono_output = audio::point_source_matrix(2,1,4,1.0F);
        CHECK(mono_output && *mono_output==std::vector<float>({0.5F,0.5F}));
    }
    for (unsigned channels : {1U,2U}) {
        fake_xaudio::reset(); core::CancellationSource cancellation{generation};
        audio::NativeAudioPlayback playback;
        const audio::SpeechOwnerPump owner=[](const auto&,bool) { return true; };
        CHECK(playback.enqueue(spatial_clip(cancellation.token(),channels)));
        CHECK(pump_until_spatial_pending(playback,generation.value(),owner));
        CHECK(close(fake_xaudio::volume,0.0F) && fake_xaudio::start_samples.empty() && !playback.active_frame());
        fake_xaudio::voice->advance(4);
        CHECK(fake_xaudio::voice->samples_played==0); // Submitted waiting audio cannot consume the first words.
        const auto request=playback.active_spatial_request(generation.value()); CHECK(request);
        CHECK(playback.set_spatial_mix(*request,audio::SpatialMix{0.4F,-1.0F},generation.value()));
        CHECK(close(fake_xaudio::volume,0.4F) && fake_xaudio::matrix_source_channels==channels);
        CHECK(fake_xaudio::start_samples==std::vector<std::uint64_t>{0} && playback.active_frame());
        const float source_weight=1.0F/static_cast<float>(channels);
        CHECK(close(fake_xaudio::matrix[0],source_weight) && close(fake_xaudio::matrix[channels],0.0F));
        const auto input=channels==1 ? std::vector<float>{2.0F} : std::vector<float>{2.0F,6.0F};
        auto rendered=fake_xaudio::render(input);
        CHECK(close(rendered[0],channels==1 ? 0.8F : 1.6F) && close(rendered[1],0.0F));
        CHECK(playback.set_spatial_mix(*request,audio::SpatialMix{0.8F,1.0F},generation.value()));
        CHECK(close(fake_xaudio::volume,0.8F) && close(fake_xaudio::matrix[0],0.0F) &&
              close(fake_xaudio::matrix[channels],source_weight));
        if(channels==2) CHECK(close(fake_xaudio::matrix[1],0.0F) && close(fake_xaudio::matrix[3],0.5F));
        rendered=fake_xaudio::render(input);
        CHECK(close(rendered[0],0.0F) && close(rendered[1],channels==1 ? 1.6F : 3.2F));
        CHECK(fake_xaudio::start_samples.size()==1); // A live mix update does not restart the source.
        fake_xaudio::voice->advance(4);
        CHECK(playback.set_spatial_mix(*request,std::nullopt,generation.value()));
        CHECK(!fake_xaudio::voice->running && !playback.active_frame());
        fake_xaudio::voice->advance(4); CHECK(fake_xaudio::voice->samples_played==4);
        CHECK(playback.set_spatial_mix(*request,audio::SpatialMix{0.8F,0.0F},generation.value()));
        CHECK(close(fake_xaudio::volume,0.8F)); // Missing-pose recovery does not compound attenuation.
        CHECK(fake_xaudio::start_samples==std::vector<std::uint64_t>({0,4}));
        auto wrong=*request; ++wrong.playback_token;
        CHECK(!playback.set_spatial_mix(wrong,audio::SpatialMix{0.2F,0.0F},generation.value()));
        wrong=*request; wrong.owner.speaker.playthrough_id="another-save";
        CHECK(!playback.set_spatial_mix(wrong,audio::SpatialMix{0.2F,0.0F},generation.value()));
        wrong=*request; wrong.owner.turn_id="different-turn";
        CHECK(!playback.set_spatial_mix(wrong,audio::SpatialMix{0.2F,0.0F},generation.value()));
        CHECK(!playback.set_spatial_mix(*request,audio::SpatialMix{0.2F,0.0F},generation.value()+1));
        CHECK(!playback.active_spatial_request(generation.value()+1));
        CHECK(!playback.set_spatial_mix(*request,audio::SpatialMix{0.2F,std::numeric_limits<float>::quiet_NaN()},generation.value()));
        CHECK(close(fake_xaudio::volume,0.8F));
        playback.set_paused(true);
        CHECK(!playback.active_spatial_request(generation.value()));
        CHECK(!playback.set_spatial_mix(*request,audio::SpatialMix{0.2F,0.0F},generation.value()));
        playback.set_paused(false); CHECK(playback.pump(generation.value(),owner));
        CHECK(playback.active_spatial_request(generation.value()));
        CHECK(!fake_xaudio::voice->running); // Resume waits for fresh current pose rather than the old matrix.
        CHECK(playback.set_spatial_mix(*request,audio::SpatialMix{0.8F,0.0F},generation.value()));
        fake_xaudio::voice->queued=0;
        CHECK(!playback.set_spatial_mix(*request,audio::SpatialMix{0.2F,0.0F},generation.value()));
        CHECK(playback.pump(generation.value(),owner) && playback.idle());
        CHECK(!playback.set_spatial_mix(*request,audio::SpatialMix{0.2F,0.0F},generation.value()));
        CHECK(playback.enqueue(spatial_clip(cancellation.token(),channels)));
        CHECK(pump_until_spatial_pending(playback,generation.value(),owner));
        const auto next=playback.active_spatial_request(generation.value()); CHECK(next);
        CHECK(next->playback_token!=request->playback_token && !playback.set_spatial_mix(*request,
            audio::SpatialMix{0.2F,0.0F},generation.value()));
        CHECK(playback.set_spatial_mix(*next,audio::SpatialMix{0.8F,0.0F},generation.value()));
        fake_xaudio::fail_matrix=true;
        CHECK(!playback.set_spatial_mix(*next,audio::SpatialMix{0.2F,0.0F},generation.value()));
        CHECK(!fake_xaudio::voice->running && !playback.active_frame() && !playback.active_spatial_request(generation.value()));
        fake_xaudio::fail_matrix=false;
        CHECK(!playback.set_spatial_mix(*next,audio::SpatialMix{0.8F,0.0F},generation.value()));
        CHECK(!playback.pump(generation.value(),owner) && playback.idle());
        CHECK(playback.enqueue(spatial_clip(cancellation.token(),channels)) &&
              pump_until_spatial_pending(playback,generation.value(),owner));
        const auto cancelled=playback.active_spatial_request(generation.value()); CHECK(cancelled);
        cancellation.cancel();
        CHECK(!playback.active_spatial_request(generation.value()));
        CHECK(!playback.set_spatial_mix(*cancelled,audio::SpatialMix{0.2F,0.0F},generation.value()));
        CHECK(playback.pump(generation.value(),owner) && playback.idle());
    }
    {
        fake_xaudio::reset(); core::CancellationSource cancellation{generation};
        audio::NativeAudioPlayback first,second;
        const audio::SpeechOwnerPump owner=[](const auto&,bool) { return true; };
        CHECK(first.enqueue(spatial_clip(cancellation.token())) && second.enqueue(spatial_clip(cancellation.token())));
        CHECK(pump_until_spatial_pending(first,generation.value(),owner) && pump_until_spatial_pending(second,generation.value(),owner));
        const auto request=first.active_spatial_request(generation.value()); CHECK(request);
        CHECK(!second.set_spatial_mix(*request,audio::SpatialMix{0.8F,0.0F},generation.value()));
        CHECK(first.stop_on_game_thread(owner,true));
        CHECK(!first.set_spatial_mix(*request,audio::SpatialMix{0.8F,0.0F},generation.value()));
    }
    for (unsigned channels : {1U,2U}) {
        fake_xaudio::reset(); fake_xaudio::destination_channels=6; fake_xaudio::destination_mask=0x3F;
        core::CancellationSource cancellation{generation}; audio::NativeAudioPlayback playback;
        const audio::SpeechOwnerPump owner=[](const auto&,bool) { return true; };
        CHECK(playback.enqueue(spatial_clip(cancellation.token(),channels)));
        CHECK(pump_until_spatial_pending(playback,generation.value(),owner));
        const auto request=playback.active_spatial_request(generation.value()); CHECK(request);
        CHECK(playback.set_spatial_mix(*request,audio::SpatialMix{0.8F,1.0F},generation.value()));
        CHECK(fake_xaudio::matrix.size()==channels*6 && close(fake_xaudio::matrix[0],0.0F));
        const auto rendered=fake_xaudio::render(channels==1 ? std::vector<float>{2.0F} : std::vector<float>{2.0F,6.0F});
        CHECK(close(rendered[0],0.0F) && close(rendered[1],channels==1 ? 1.6F : 3.2F));
        for(unsigned destination=2;destination<6;++destination) CHECK(close(rendered[destination],0.0F));
    }
    {
        fake_xaudio::reset(); core::CancellationSource cancellation{generation};
        audio::NativeAudioPlayback playback;
        const audio::SpeechOwnerPump owner=[](const auto&,bool) { return true; };
        auto disabled=clip(cancellation.token()); disabled.bytes=wave(2); disabled.volume=0.65F;
        CHECK(playback.enqueue(std::move(disabled)) && pump_until(playback,generation.value(),owner,true));
        CHECK(!playback.active_spatial_request(generation.value()));
        CHECK(close(fake_xaudio::volume,0.65F) && fake_xaudio::matrix.empty()); // Disabled3D stereo keeps native routing.
    }
    {
        fake_xaudio::reset(); fake_xaudio::destination_mask=0;
        core::CancellationSource cancellation{generation}; audio::NativeAudioPlayback playback;
        const audio::SpeechOwnerPump owner=[](const auto&,bool) { return true; };
        CHECK(playback.enqueue(spatial_clip(cancellation.token())));
        CHECK(!pump_until(playback,generation.value(),owner,false) && !playback.active_frame());
    }
    {
        fake_xaudio::reset(); core::CancellationSource cancellation{generation};
        audio::NativeAudioPlayback playback;
        const audio::SpeechOwnerPump owner=[](const auto&,bool) { return true; };
        CHECK(playback.enqueue(spatial_clip(cancellation.token())) && pump_until_spatial_pending(playback,generation.value(),owner));
        const auto request=playback.active_spatial_request(generation.value()); CHECK(request);
        auto stale=*request; ++stale.playback_token;
        CHECK(!playback.set_spatial_mix(stale,audio::SpatialMix{0.8F,0.0F},generation.value()));
        stale=*request; stale.owner.turn_id="foreign-turn";
        CHECK(!playback.set_spatial_mix(stale,audio::SpatialMix{0.8F,0.0F},generation.value()));
        CHECK(!playback.set_spatial_mix(*request,audio::SpatialMix{0.8F,0.0F},generation.value()+1));
        CHECK(playback.set_spatial_mix(*request,std::nullopt,generation.value()));
        fake_xaudio::voice->advance(4); CHECK(fake_xaudio::voice->samples_played==0 && fake_xaudio::start_samples.empty());
        const auto now=core::SnapshotClock::now();
        playback.set_paused(true,now);
        CHECK(!playback.set_spatial_mix(*request,audio::SpatialMix{0.8F,0.0F},generation.value()));
        CHECK(playback.pump(generation.value(),owner,nullptr,now+std::chrono::seconds{10}));
        CHECK(fake_xaudio::start_samples.empty() && fake_xaudio::voice->samples_played==0);
        playback.set_paused(false,now+std::chrono::seconds{10});
        CHECK(playback.pump(generation.value(),owner,nullptr,now+std::chrono::seconds{10}));
        CHECK(fake_xaudio::start_samples.empty());
        CHECK(playback.set_spatial_mix(*request,audio::SpatialMix{0.8F,0.0F},generation.value(),now+std::chrono::seconds{10}));
        CHECK(fake_xaudio::start_samples==std::vector<std::uint64_t>{0} && playback.active_frame());
    }
    for(bool initially_spoken : {false,true}) {
        fake_xaudio::reset(); core::CancellationSource cancellation{generation};
        audio::NativeAudioPlayback playback;
        const audio::SpeechOwnerPump owner=[](const auto&,bool) { return true; };
        auto value=spatial_clip(cancellation.token());
        auto caption=std::make_shared<presentation::DialogueCaption>();
        caption->generation=generation.value(); caption->speaker="Test NPC"; caption->text="The complete reply stays readable.";
        caption->cancellation=cancellation.token(); value.caption=caption;
        CHECK(playback.enqueue(std::move(value)) && pump_until_spatial_pending(playback,generation.value(),owner));
        const auto request=playback.active_spatial_request(generation.value()); CHECK(request);
        if(initially_spoken) {
            CHECK(playback.set_spatial_mix(*request,audio::SpatialMix{0.8F,0.0F},generation.value()));
            fake_xaudio::voice->advance(4);
            CHECK(playback.set_spatial_mix(*request,std::nullopt,generation.value()));
        }
        fake_xaudio::voice->advance(4);
        CHECK(fake_xaudio::voice->samples_played==(initially_spoken ? 4U : 0U));
        presentation::DialogueCaptions::Caption failed;
        const auto now=core::SnapshotClock::now()+std::chrono::seconds{3};
        CHECK(!playback.pump(generation.value(),owner,&failed,now) && playback.idle());
        CHECK(failed==caption && caption->delivery->load()==presentation::DialogueDelivery::discarded && !caption->delivered());
        CHECK(fake_xaudio::start_samples.size()==(initially_spoken ? 1U : 0U));
        presentation::DialogueCaptions fallback;
        fallback.enqueue_failed(failed);
        CHECK(!failed && fallback.frame(generation.value(),{},false,now)==caption);
        CHECK(caption->delivery->load()==presentation::DialogueDelivery::reading && !caption->delivered());
        CHECK(!fallback.frame(generation.value(),{},false,now+std::chrono::seconds{31}) && caption->delivered());
        CHECK(caption->delivery->load()==presentation::DialogueDelivery::text_complete);
        CHECK(playback.enqueue(spatial_clip(cancellation.token())) && pump_until_spatial_pending(playback,generation.value(),owner));
        const auto next=playback.active_spatial_request(generation.value()); CHECK(next);
        CHECK(!playback.set_spatial_mix(*request,audio::SpatialMix{0.8F,0.0F},generation.value()));
        CHECK(playback.set_spatial_mix(*next,audio::SpatialMix{0.8F,0.0F},generation.value()));
    }
    {
        fake_xaudio::reset(); core::CancellationSource cancellation{generation};
        audio::NativeAudioPlayback playback;
        const audio::SpeechOwnerPump owner=[](const auto&,bool) { return true; };
        CHECK(playback.enqueue(spatial_clip(cancellation.token())) && pump_until_spatial_pending(playback,generation.value(),owner));
        const auto request=playback.active_spatial_request(generation.value()); CHECK(request);
        CHECK(playback.stop_on_game_thread(owner,true));
        CHECK(fake_xaudio::start_samples.empty() && playback.idle());
        CHECK(!playback.set_spatial_mix(*request,audio::SpatialMix{0.8F,0.0F},generation.value()));
    }
    for(int failure=0;failure<3;++failure) {
        fake_xaudio::reset(); core::CancellationSource cancellation{generation};
        audio::NativeAudioPlayback playback;
        const audio::SpeechOwnerPump owner=[](const auto&,bool) { return true; };
        CHECK(playback.enqueue(spatial_clip(cancellation.token())) && pump_until_spatial_pending(playback,generation.value(),owner));
        const auto request=playback.active_spatial_request(generation.value()); CHECK(request);
        if(failure!=1) {
            CHECK(playback.set_spatial_mix(*request,audio::SpatialMix{0.8F,0.0F},generation.value()));
            fake_xaudio::voice->advance(4);
            fake_xaudio::fail_volume=failure==0; fake_xaudio::throw_matrix=failure==2;
        } else fake_xaudio::fail_start=true;
        CHECK(!playback.set_spatial_mix(*request,audio::SpatialMix{0.2F,1.0F},generation.value()));
        CHECK(!fake_xaudio::voice->running && !playback.active_frame());
        const auto consumed=fake_xaudio::voice->samples_played;
        fake_xaudio::voice->advance(4); CHECK(fake_xaudio::voice->samples_played==consumed);
        CHECK(!playback.active_spatial_request(generation.value()));
        CHECK(!playback.pump(generation.value(),owner) && playback.idle());
    }
    std::cout<<"native playback tests passed: deferred spatial start/resume, sample preservation, text timeout/failure fallback, destination-major channel mixing and lifecycle fences\n";
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
