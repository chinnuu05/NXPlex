// Streaming Matroska demuxer. Reads strictly forward from a ByteSource (a
// network stream or a file), so it never needs to seek and keeps only the
// current block in memory. Handles unknown-size Segments/Clusters (live
// output from Plex's transcoder) and all four lacing modes.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace nxp {

struct ByteSource {
    // >0 bytes read, 0 end of stream, -1 error.
    virtual long read(void* buf, size_t n) = 0;
    // Optional random access (local files). Network streams return false/-1.
    virtual bool seek(uint64_t pos) { (void)pos; return false; }
    virtual int64_t size() { return -1; }
    virtual ~ByteSource() = default;
};

enum class TrackKind : uint8_t { Unknown = 0, Video = 1, Audio = 2, Subtitle = 17 };

struct MkvTrack {
    uint64_t number = 0;
    TrackKind kind = TrackKind::Unknown;
    std::string codec_id;  // e.g. V_MJPEG, A_MPEG/L3
    std::vector<uint8_t> codec_private;
    uint64_t default_duration_ns = 0;
    uint32_t width = 0, height = 0;
    uint32_t channels = 0;
    double sample_rate = 0;
};

struct MkvPacket {
    uint64_t track = 0;
    int64_t pts_ms = 0;
    bool keyframe = false;
    std::vector<uint8_t> data;
};

class MkvDemuxer {
public:
    explicit MkvDemuxer(ByteSource& src, size_t max_block = 4u << 20) : src_(src), max_block_(max_block) {}

    // Reads the header up to the track list. False on errors (see error()).
    bool open();
    const std::vector<MkvTrack>& tracks() const { return tracks_; }
    const MkvTrack* find_track(TrackKind kind) const;

    // 1 = packet returned, 0 = end of stream, -1 = error.
    int next(MkvPacket& pkt);

    // Jumps to a byte offset (source must support seek) and resumes at the
    // next Cluster. Tracks from open() are kept.
    bool seek_bytes(uint64_t pos);

    const std::string& error() const { return err_; }
    uint64_t bytes_read() const { return pos_; }
    int64_t duration_ms() const { return duration_ > 0 ? (int64_t)(duration_ * (double)timecode_scale_ / 1e6) : -1; }

private:
    struct Open {
        uint32_t id;
        uint64_t end;  // UINT64_MAX = unknown size
    };

    bool read_exact(void* buf, size_t n);
    bool skip(uint64_t n);
    // Element header; returns 0 on EOF before any byte, -1 on error, 1 ok.
    int read_header(uint32_t& id, uint64_t& size, bool& unknown);
    bool read_uint(uint64_t size, uint64_t& out);
    bool read_float(uint64_t size, double& out);
    bool read_string(uint64_t size, std::string& out);
    bool read_bytes(uint64_t size, std::vector<uint8_t>& out);
    bool parse_block(uint64_t size, bool simple, bool keyframe_hint);
    int step();  // processes one element: 1 progress, 0 EOF, -1 error

    ByteSource& src_;
    size_t max_block_;
    uint64_t pos_ = 0;
    std::string err_;
    std::vector<Open> stack_;
    std::vector<MkvTrack> tracks_;
    MkvTrack* cur_track_ = nullptr;
    bool tracks_done_ = false;
    uint64_t timecode_scale_ = 1000000;  // ns per timecode unit
    double duration_ = 0;               // in timecode units
    int64_t cluster_tc_ = 0;
    std::deque<MkvPacket> pending_;
    std::vector<uint8_t> block_;
};

}  // namespace nxp
