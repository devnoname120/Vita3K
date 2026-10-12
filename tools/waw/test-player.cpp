#include <codec/state.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static void timestamps(const char *path) {
    PlayerState player;
    player.queue(path);
    assert(player.get_framerate_microseconds() == 41667);
    for (int i=0; i<12; ++i) {
        auto data=player.receive_video();
        assert(!data.empty());
        const uint64_t expected=av_rescale_q(i, AVRational{1,24}, AVRational{1,1000});
        if (player.last_timestamp != expected) {
            std::fprintf(stderr,"Frame %d: timestamp=%llu ms, expected=%llu ms\n",i,
                (unsigned long long)player.last_timestamp,(unsigned long long)expected);
            std::abort();
        }
    }
}

template<class T> static void set_looping(T &player, bool loop) {
    if constexpr (requires { player.looping=loop; }) player.looping=loop;
    else { std::fprintf(stderr,"Decoder has no looping implementation\n"); std::abort(); }
}

static void drain(const char *path, bool loop, unsigned cycles, unsigned expected_frames=24) {
    PlayerState player;
    player.queue(path);
    if (!expected_frames) {
        const auto count=player.format->streams[player.video_stream_id]->nb_frames;
        assert(count>0); expected_frames=static_cast<unsigned>(count);
    }
    if (loop) set_looping(player,true);
    unsigned frames=0, resets=0, audio_frames=0;
    uint64_t previous=0;
    for (unsigned iteration=0; iteration<10000 && !player.video_playing.empty(); ++iteration) {
        const auto video=player.receive_video();
        if (!video.empty()) {
            if (frames && player.last_timestamp < previous) ++resets;
            previous=player.last_timestamp;
            ++frames;
        }
        for (unsigned n=0; n<3; ++n) {
            const auto audio=player.receive_audio();
            if (!audio.empty()) ++audio_frames;
        }
        if (loop && frames>=expected_frames*cycles) break;
    }
    std::printf("frames=%u audio_frames=%u resets=%u active=%d\n",frames,audio_frames,resets,!player.video_playing.empty());
    assert(frames==expected_frames*cycles);
    assert(resets==(loop?cycles-1:0));
    assert(loop || player.video_playing.empty());
    if (loop) {
        set_looping(player,false);
        for (unsigned i=0;i<1000 && !player.video_playing.empty();++i) {
            player.receive_video(); player.receive_audio(); player.receive_audio(); player.receive_audio();
        }
        assert(player.video_playing.empty());
    }
    player.free_video();
    assert(player.receive_video().empty());
    assert(player.receive_audio().empty());
}

int main(int argc,char **argv) {
    assert(argc==3);
    const std::string mode=argv[1];
    if(mode=="timestamps") timestamps(argv[2]);
    else if(mode=="drain") drain(argv[2],false,1);
    else if(mode=="loop") drain(argv[2],true,3);
    else if(mode=="source") {timestamps(argv[2]);drain(argv[2],false,1,0);drain(argv[2],true,2,0);}
    else std::abort();
    std::puts("PASS");
}
