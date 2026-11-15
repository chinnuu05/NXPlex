// Simulated real-time audio device and frame-counting video sink for host tests.
#pragma once
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "platform.hpp"
#include "player.hpp"

namespace nxp {

// Behaves like audout: a fixed-size output queue drained in real time.
class SimAudio : public AudioSink {
public:
    std::vector<int16_t> captured;
    bool open(uint32_t rate) override {
        std::lock_guard<std::mutex> lk(mu_);
        rate_ = rate;
        start_us_ = 0;
        written_ = 0;
        return true;
    }
    uint32_t rate() const override { return rate_; }
    bool write(const int16_t* pcm, size_t frames) override {
        std::unique_lock<std::mutex> lk(mu_);
        if (!start_us_) start_us_ = now_us();
        written_ += frames;
        captured.insert(captured.end(), pcm, pcm + frames * 2);
        lk.unlock();
        // Block while more than kQueue frames are waiting to play.
        while (written_ > elapsed_frames() + kQueue) sleep_ms(2);
        return true;
    }
    void flush() override {
        std::lock_guard<std::mutex> lk(mu_);
        start_us_ = 0;
        written_ = 0;
    }
    uint64_t played_frames() override {
        std::lock_guard<std::mutex> lk(mu_);
        uint64_t e = elapsed_frames();
        return e < written_ ? e : written_;
    }
    void close() override {}

private:
    static const uint64_t kQueue = 4 * 1536;
    uint64_t elapsed_frames() const { return start_us_ ? (now_us() - start_us_) * rate_ / 1000000 : 0; }
    std::mutex mu_;
    uint32_t rate_ = 48000;
    uint64_t start_us_ = 0, written_ = 0;
};

class SimVideo : public VideoSink {
public:
    Player* player = nullptr;  // optional: measures A/V offset
    std::string outdir;
    int shown = 0;
    double sum_abs_err = 0, max_err = 0;
    int w = 0, h = 0;
    void present(const uint8_t* rgba, int fw, int fh, int64_t pts) override {
        w = fw;
        h = fh;
        double err = player ? (double)(player->position_ms() - pts) : 0.0;  // >0 = shown late
        sum_abs_err += std::fabs(err);
        if (std::fabs(err) > std::fabs(max_err)) max_err = err;
        if (!outdir.empty() && (shown == 0 || shown % 120 == 0) && shown <= 480) {
            char path[512];
            snprintf(path, sizeof(path), "%s/frame_%04d.ppm", outdir.c_str(), shown);
            FILE* f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", fw, fh);
                for (int i = 0; i < fw * fh; i++) fwrite(rgba + i * 4, 1, 3, f);
                fclose(f);
            }
        }
        shown++;
    }
};

}  // namespace nxp
