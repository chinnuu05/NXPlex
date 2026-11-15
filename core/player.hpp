// A/V player for Plex's "tiny" transcode format: Matroska + MJPEG + MP3.
//
//   reader thread : ByteSource -> MkvDemuxer -> audio/video packet queues
//   audio thread  : MP3 decode -> stereo resample -> AudioSink (blocking)
//   caller thread : video_tick() decodes and presents the frame that is due
//
// The audio sink's playback position is the master clock; video frames are
// shown when due and dropped when late. Without an audio track a wall clock
// is used instead.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "decoders.hpp"
#include "mkv.hpp"
#include "thread.hpp"

namespace nxp {

struct AudioSink {
    virtual bool open(uint32_t rate) = 0;  // stereo s16
    virtual uint32_t rate() const = 0;
    // Queues interleaved stereo frames; blocks while the output queue is full.
    virtual bool write(const int16_t* pcm, size_t frames) = 0;
    // Frames actually played so far (monotonic, may be interpolated).
    virtual uint64_t played_frames() = 0;
    // Drops queued audio and resets played_frames() to 0 (used when seeking).
    virtual void flush() = 0;
    virtual void close() = 0;
    virtual ~AudioSink() = default;
};

// Frames are decoded straight into the sink's memory (RGB565, row stride = w)
// so there is only one copy of each frame: begin_frame() hands out the buffer
// (or nullptr to skip the frame), end_frame() is always called afterwards.
struct VideoSink {
    virtual uint16_t* begin_frame(int w, int h) = 0;
    virtual void end_frame(bool ok, int64_t pts_ms) = 0;
    virtual ~VideoSink() = default;
};

struct PlayerStats {
    uint32_t frames_shown = 0, frames_dropped = 0, decode_errors = 0;
    uint32_t video_queue = 0, audio_queue = 0;  // packets queued
    uint32_t queue_bytes = 0;
    uint32_t max_decode_us = 0, avg_decode_us = 0;
    uint32_t underruns = 0;
};

class Player {
public:
    Player(ByteSource& src, AudioSink& audio, VideoSink& video) : src_(src), audio_(audio), video_(video) {}
    ~Player() { stop(); }

    // Reads the stream header in the calling thread, then starts the reader
    // and audio threads.
    bool start(std::string& err);
    void stop();

    void set_paused(bool p);
    bool paused() const { return paused_; }

    // Jumps to target_ms (local files only for now). Keeps the paused state.
    bool seek(int64_t target_ms);
    bool can_seek() { return src_.size() > 0; }
    int64_t duration_ms() const { return demux_ ? demux_->duration_ms() : -1; }

    // Call from the display loop. Presents the due frame, if any, and returns
    // how many ms the caller may sleep before calling again.
    int video_tick();

    int64_t position_ms();
    int64_t start_pts_ms() const { return base_pts_; }
    bool finished();
    std::string error();
    PlayerStats stats();

    // Thread priorities (Switch: larger = lower). Everything but audio sits
    // below Tesla's UI threads (0x2C poller, 0x31 renderer) on the shared core;
    // audio is just above the renderer so the menu can't cause dropouts.
    // Limits for the packet queues.
    size_t max_queue_bytes = 1536 * 1024;

private:
    static void reader_entry(void* p) { ((Player*)p)->reader_loop(); }
    static void audio_entry(void* p) { ((Player*)p)->audio_loop(); }
    void reader_loop();
    void audio_loop();
    int64_t clock_ms_locked();
    bool start_threads();
    void stop_threads();

    ByteSource& src_;
    AudioSink& audio_;
    VideoSink& video_;
    MkvDemuxer* demux_ = nullptr;
    uint64_t vtrack_ = 0, atrack_ = 0;

    Thread reader_thread_, audio_thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<MkvPacket> vq_, aq_;
    size_t queue_bytes_ = 0;
    bool eof_ = false, stopping_ = false, audio_done_ = false;
    std::atomic<bool> paused_{false};
    std::string error_;
    int64_t skip_until_ms_ = -1;       // after a seek: drop packets before this
    int64_t last_pos_ms_ = 0;          // reported while the clock isn't running yet
    std::deque<MkvPacket> carry_;      // packets read while locating the seek point

    // Clock.
    int64_t base_pts_ = -1;           // pts of the first audio sample (or first video frame)
    uint64_t wall_start_ms_ = 0;      // no-audio mode
    uint64_t paused_at_ms_ = 0, paused_total_ms_ = 0;
    bool first_frame_shown_ = false;

    PlayerStats stats_;
    uint64_t decode_us_total_ = 0;
};

}  // namespace nxp
