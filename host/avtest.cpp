// avtest: runs the real Player against a simulated real-time audio device.
// Reports A/V sync, dropped frames and decode cost; writes the decoded audio
// as WAV and a few frames as PPM for inspection.
//
//   avtest <file.mkv> [seconds] [outdir]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "platform.hpp"
#include "player.hpp"
#include "sources.hpp"
#include "sim.hpp"

using namespace nxp;

static void write_wav(const std::string& path, const std::vector<int16_t>& pcm, uint32_t rate) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    uint32_t data = (uint32_t)(pcm.size() * 2), riff = 36 + data, fmt = 16, br = rate * 4;
    uint16_t pcm_fmt = 1, ch = 2, ba = 4, bits = 16;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f);
    fwrite(&pcm_fmt, 2, 1, f);
    fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data, 4, 1, f);
    fwrite(pcm.data(), 2, pcm.size(), f);
    fclose(f);
}

static void log_stderr(const char* line) { fprintf(stderr, "[log] %s\n", line); }

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: avtest <file.mkv> [seconds] [outdir]\n");
        return 2;
    }
    log_set_sink(log_stderr);
    int seconds = argc >= 3 ? atoi(argv[2]) : 10;
    FileSource src;
    if (!src.open(argv[1])) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    SimAudio audio;
    SimVideo video;
    if (argc >= 4) video.outdir = argv[3];
    Player player(src, audio, video);
    video.player = &player;
    std::string err;
    if (!player.start(err)) {
        fprintf(stderr, "start failed: %s\n", err.c_str());
        return 1;
    }
    // Optional seek schedule: AVTEST_SEEKS="3:600000,6:30000" = at 3 s jump to
    // 600 s, at 6 s jump to 30 s. Each seek is checked a moment later.
    std::vector<std::pair<int, int64_t>> seeks;
    if (const char* e = getenv("AVTEST_SEEKS")) {
        std::string spec = e;
        size_t p = 0;
        while (p < spec.size()) {
            size_t c = spec.find(',', p);
            std::string item = spec.substr(p, c == std::string::npos ? std::string::npos : c - p);
            size_t colon = item.find(':');
            if (colon != std::string::npos)
                seeks.emplace_back(atoi(item.c_str()), atoll(item.c_str() + colon + 1));
            if (c == std::string::npos) break;
            p = c + 1;
        }
    }
    size_t next_seek = 0;
    int64_t check_target = -1;
    uint64_t check_at = 0, seek_issued_at = 0;
    int seek_fail = 0;
    uint64_t t0 = now_ms();
    while (now_ms() - t0 < (uint64_t)seconds * 1000 && !player.finished()) {
        uint64_t el = now_ms() - t0;
        if (next_seek < seeks.size() && el >= (uint64_t)seeks[next_seek].first * 1000) {
            int64_t tgt = seeks[next_seek++].second;
            seek_issued_at = now_ms();
            bool ok = player.seek(tgt);
            printf("seek -> %lld ms: %s (took %llu ms)\n", (long long)tgt, ok ? "ok" : "FAILED",
                   (unsigned long long)(now_ms() - seek_issued_at));
            if (!ok) seek_fail++;
            check_target = tgt;
            check_at = now_ms() + 1000;
        }
        if (check_target >= 0 && now_ms() >= check_at) {
            int64_t pos = player.position_ms();
            int64_t dur = player.duration_ms();
            int64_t want = check_target;
            if (dur > 0 && want > dur - 2000) want = dur - 2000;
            bool good = pos >= want && pos <= want + 2500;
            printf("  1 s after seek: position %lld ms (expected ~%lld) %s\n", (long long)pos, (long long)(want + 1000),
                   good ? "OK" : "WRONG");
            if (!good) seek_fail++;
            check_target = -1;
        }
        int wait = player.video_tick();
        if (wait > 0) sleep_ms((uint32_t)wait);
    }
    if (!seeks.empty()) printf("seek checks failed: %d\n", seek_fail);
    double wall = (double)(now_ms() - t0) / 1000.0;
    PlayerStats st = player.stats();
    int64_t pos = player.position_ms() - player.start_pts_ms();
    std::string perr = player.error();
    player.stop();

    printf("wall %.2fs, media advanced %.2fs, video %dx%d\n", wall, pos / 1000.0, video.w, video.h);
    printf("frames shown %u, dropped %u, decode errors %u, decode avg %u us max %u us\n", st.frames_shown,
           st.frames_dropped, st.decode_errors, st.avg_decode_us, st.max_decode_us);
    printf("A/V offset at present: mean |%.1f| ms, worst %.1f ms\n", video.shown ? video.sum_abs_err / video.shown : 0,
           video.max_err);
    printf("audio captured %.2fs\n", audio.captured.size() / 2.0 / 48000.0);
    if (!perr.empty()) printf("player error: %s\n", perr.c_str());
    if (!video.outdir.empty()) write_wav(video.outdir + "/audio.wav", audio.captured, 48000);
    return 0;
}
