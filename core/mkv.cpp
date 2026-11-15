#include "mkv.hpp"

#include <cstring>

namespace nxp {

namespace {
enum : uint32_t {
    ID_EBML = 0x1A45DFA3,
    ID_Segment = 0x18538067,
    ID_Info = 0x1549A966,
    ID_TimecodeScale = 0x2AD7B1,
    ID_Duration = 0x4489,
    ID_Tracks = 0x1654AE6B,
    ID_TrackEntry = 0xAE,
    ID_TrackNumber = 0xD7,
    ID_TrackType = 0x83,
    ID_CodecID = 0x86,
    ID_CodecPrivate = 0x63A2,
    ID_DefaultDuration = 0x23E383,
    ID_Video = 0xE0,
    ID_PixelWidth = 0xB0,
    ID_PixelHeight = 0xBA,
    ID_Audio = 0xE1,
    ID_SamplingFrequency = 0xB5,
    ID_Channels = 0x9F,
    ID_Cluster = 0x1F43B675,
    ID_Timecode = 0xE7,
    ID_SimpleBlock = 0xA3,
    ID_BlockGroup = 0xA0,
    ID_Block = 0xA1,
    ID_ReferenceBlock = 0xFB,
    // Top-level children of Segment (used to close unknown-size Clusters).
    ID_SeekHead = 0x114D9B74,
    ID_Cues = 0x1C53BB6B,
    ID_Tags = 0x1254C367,
    ID_Chapters = 0x1043A770,
    ID_Attachments = 0x1941A469,
};

bool is_master(uint32_t id) {
    switch (id) {
    case ID_Segment: case ID_Info: case ID_Tracks: case ID_TrackEntry: case ID_Video: case ID_Audio:
    case ID_Cluster: case ID_BlockGroup:
        return true;
    }
    return false;
}

bool is_segment_child(uint32_t id) {
    switch (id) {
    case ID_SeekHead: case ID_Info: case ID_Tracks: case ID_Cluster: case ID_Cues: case ID_Tags:
    case ID_Chapters: case ID_Attachments:
        return true;
    }
    return false;
}

// Valid parents, so a corrupt/unknown-size stream can't nest things oddly.
uint32_t parent_of(uint32_t id) {
    switch (id) {
    case ID_Info: case ID_Tracks: case ID_Cluster: return ID_Segment;
    case ID_TrackEntry: return ID_Tracks;
    case ID_Video: case ID_Audio: return ID_TrackEntry;
    case ID_BlockGroup: return ID_Cluster;
    }
    return 0;
}

}  // namespace

const MkvTrack* MkvDemuxer::find_track(TrackKind kind) const {
    for (auto& t : tracks_)
        if (t.kind == kind) return &t;
    return nullptr;
}

bool MkvDemuxer::read_exact(void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    while (n) {
        long r = src_.read(p, n);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
        pos_ += (uint64_t)r;
    }
    return true;
}

bool MkvDemuxer::skip(uint64_t n) {
    uint8_t tmp[1024];
    while (n) {
        size_t k = n < sizeof(tmp) ? (size_t)n : sizeof(tmp);
        if (!read_exact(tmp, k)) return false;
        n -= k;
    }
    return true;
}

int MkvDemuxer::read_header(uint32_t& id, uint64_t& size, bool& unknown) {
    uint8_t b;
    long r = src_.read(&b, 1);
    if (r == 0) return 0;
    if (r < 0) return -1;
    pos_++;
    int len = 1;
    while (len <= 4 && !(b & (0x80 >> (len - 1)))) len++;
    if (len > 4) return -1;
    id = b;
    for (int i = 1; i < len; i++) {
        if (!read_exact(&b, 1)) return -1;
        id = (id << 8) | b;
    }
    if (!read_exact(&b, 1)) return -1;
    len = 1;
    while (len <= 8 && !(b & (0x80 >> (len - 1)))) len++;
    if (len > 8) return -1;
    uint64_t v = b & (0xFF >> len);
    bool all_ones = v == (uint64_t)(0xFF >> len);
    for (int i = 1; i < len; i++) {
        if (!read_exact(&b, 1)) return -1;
        v = (v << 8) | b;
        all_ones = all_ones && b == 0xFF;
    }
    unknown = all_ones;
    size = v;
    return 1;
}

bool MkvDemuxer::read_uint(uint64_t size, uint64_t& out) {
    if (size > 8) return false;
    uint8_t b[8];
    if (!read_exact(b, (size_t)size)) return false;
    out = 0;
    for (uint64_t i = 0; i < size; i++) out = (out << 8) | b[i];
    return true;
}

bool MkvDemuxer::read_float(uint64_t size, double& out) {
    uint64_t u = 0;
    if ((size != 4 && size != 8) || !read_uint(size, u)) return false;
    if (size == 4) {
        uint32_t u32 = (uint32_t)u;
        float f;
        memcpy(&f, &u32, 4);
        out = f;
    } else {
        memcpy(&out, &u, 8);
    }
    return true;
}

bool MkvDemuxer::read_string(uint64_t size, std::string& out) {
    if (size > 4096) return false;
    out.resize((size_t)size);
    if (size && !read_exact(&out[0], (size_t)size)) return false;
    while (!out.empty() && out.back() == 0) out.pop_back();
    return true;
}

bool MkvDemuxer::read_bytes(uint64_t size, std::vector<uint8_t>& out) {
    if (size > max_block_) return false;
    out.resize((size_t)size);
    return size == 0 || read_exact(out.data(), (size_t)size);
}

bool MkvDemuxer::parse_block(uint64_t size, bool simple, bool keyframe_hint) {
    if (size < 4 || !read_bytes(size, block_)) {
        err_ = "bad block";
        return false;
    }
    const uint8_t* p = block_.data();
    const uint8_t* end = p + block_.size();
    // Track number (EBML vint).
    int len = 1;
    while (len <= 8 && !(p[0] & (0x80 >> (len - 1)))) len++;
    if (len > 8 || p + len + 3 > end) {
        err_ = "bad block header";
        return false;
    }
    uint64_t track = p[0] & (0xFF >> len);
    for (int i = 1; i < len; i++) track = (track << 8) | p[i];
    p += len;
    int16_t rel = (int16_t)((p[0] << 8) | p[1]);
    uint8_t flags = p[2];
    p += 3;

    const MkvTrack* t = nullptr;
    for (auto& tr : tracks_)
        if (tr.number == track) t = &tr;
    if (!t) return true;  // a track we don't know: ignore

    int64_t tc = cluster_tc_ + rel;
    int64_t pts_ms = (int64_t)((double)tc * (double)timecode_scale_ / 1e6);
    bool key = simple ? (flags & 0x80) != 0 : keyframe_hint;

    std::vector<size_t> sizes;
    int lacing = (flags >> 1) & 3;
    if (lacing == 0) {
        sizes.push_back((size_t)(end - p));
    } else {
        if (p >= end) return false;
        size_t count = (size_t)*p++ + 1;
        size_t total = 0;
        if (lacing == 1) {  // Xiph
            for (size_t i = 0; i + 1 < count; i++) {
                size_t s = 0;
                for (;;) {
                    if (p >= end) return false;
                    uint8_t b = *p++;
                    s += b;
                    if (b != 255) break;
                }
                sizes.push_back(s);
                total += s;
            }
        } else if (lacing == 3) {  // EBML
            int64_t prev = 0;
            for (size_t i = 0; i + 1 < count; i++) {
                if (p >= end) return false;
                int l = 1;
                while (l <= 8 && !(p[0] & (0x80 >> (l - 1)))) l++;
                if (l > 8 || p + l > end) return false;
                uint64_t v = p[0] & (0xFF >> l);
                for (int k = 1; k < l; k++) v = (v << 8) | p[k];
                p += l;
                int64_t s;
                if (i == 0) s = (int64_t)v;
                else s = prev + ((int64_t)v - (((int64_t)1 << (7 * l - 1)) - 1));  // signed delta
                if (s < 0) return false;
                sizes.push_back((size_t)s);
                total += (size_t)s;
                prev = s;
            }
        } else {  // fixed
            size_t each = (size_t)(end - p) / count;
            for (size_t i = 0; i + 1 < count; i++) sizes.push_back(each);
            total = each * (count - 1);
        }
        if (lacing != 2) {
            if (total > (size_t)(end - p)) return false;
            sizes.push_back((size_t)(end - p) - total);
        } else {
            sizes.push_back((size_t)(end - p) - (size_t)(end - p) / count * (count - 1));
        }
    }

    for (size_t i = 0; i < sizes.size(); i++) {
        if (p + sizes[i] > end) return false;
        MkvPacket pkt;
        pkt.track = track;
        pkt.pts_ms = pts_ms + (int64_t)(i * t->default_duration_ns / 1000000);
        pkt.keyframe = key;
        pkt.data.assign(p, p + sizes[i]);
        p += sizes[i];
        pending_.push_back(std::move(pkt));
    }
    return true;
}

int MkvDemuxer::step() {
    // Close finished masters.
    while (!stack_.empty() && stack_.back().end != UINT64_MAX && pos_ >= stack_.back().end) {
        if (stack_.back().id == ID_TrackEntry) cur_track_ = nullptr;
        if (stack_.back().id == ID_Tracks) tracks_done_ = true;
        stack_.pop_back();
    }

    uint32_t id;
    uint64_t size;
    bool unknown;
    int h = read_header(id, size, unknown);
    if (h == 0) return 0;
    if (h < 0) {
        err_ = "truncated element header";
        return -1;
    }

    // An unknown-size element ends when a sibling/ancestor-level ID appears.
    if (is_segment_child(id)) {
        while (!stack_.empty() && stack_.back().id != ID_Segment) {
            if (stack_.back().id == ID_Tracks) tracks_done_ = true;
            stack_.pop_back();
        }
    } else if (id == ID_Segment || id == ID_EBML) {
        stack_.clear();
    }

    if (is_master(id)) {
        uint32_t need = parent_of(id);
        if (need && (stack_.empty() || stack_.back().id != need)) {
            // Out-of-place master: skip it.
            if (unknown) {
                err_ = "unknown-size element in unexpected place";
                return -1;
            }
            return skip(size) ? 1 : -1;
        }
        if (unknown && id != ID_Segment && id != ID_Cluster) {
            err_ = "unsupported unknown-size element";
            return -1;
        }
        stack_.push_back({id, unknown ? UINT64_MAX : pos_ + size});
        if (id == ID_TrackEntry) {
            tracks_.emplace_back();
            cur_track_ = &tracks_.back();
        } else if (id == ID_Cluster) {
            tracks_done_ = true;
            cluster_tc_ = 0;
        }
        return 1;
    }
    if (unknown) {
        err_ = "unknown-size leaf element";
        return -1;
    }

    uint32_t parent = stack_.empty() ? 0 : stack_.back().id;
    uint64_t u = 0;
    bool ok = true;
    if (parent == ID_Info && id == ID_TimecodeScale) {
        ok = read_uint(size, u);
        if (ok && u) timecode_scale_ = u;
    } else if (parent == ID_Info && id == ID_Duration) {
        ok = read_float(size, duration_);
    } else if (parent == ID_TrackEntry && cur_track_) {
        switch (id) {
        case ID_TrackNumber: ok = read_uint(size, cur_track_->number); break;
        case ID_TrackType:
            ok = read_uint(size, u);
            cur_track_->kind = (u == 1 || u == 2 || u == 17) ? (TrackKind)u : TrackKind::Unknown;
            break;
        case ID_CodecID: ok = read_string(size, cur_track_->codec_id); break;
        case ID_CodecPrivate: ok = read_bytes(size, cur_track_->codec_private); break;
        case ID_DefaultDuration: ok = read_uint(size, cur_track_->default_duration_ns); break;
        default: ok = skip(size);
        }
    } else if (parent == ID_Video && cur_track_ && (id == ID_PixelWidth || id == ID_PixelHeight)) {
        ok = read_uint(size, u);
        (id == ID_PixelWidth ? cur_track_->width : cur_track_->height) = (uint32_t)u;
    } else if (parent == ID_Audio && cur_track_ && id == ID_SamplingFrequency) {
        ok = read_float(size, cur_track_->sample_rate);
    } else if (parent == ID_Audio && cur_track_ && id == ID_Channels) {
        ok = read_uint(size, u);
        cur_track_->channels = (uint32_t)u;
    } else if (parent == ID_Cluster && id == ID_Timecode) {
        ok = read_uint(size, u);
        cluster_tc_ = (int64_t)u;
    } else if (parent == ID_Cluster && id == ID_SimpleBlock) {
        ok = parse_block(size, true, false);
    } else if (parent == ID_BlockGroup && id == ID_Block) {
        // Keyframe info for BlockGroups lives in ReferenceBlock (absent = key);
        // it may come after the Block, so treat Block-in-group as non-key only
        // when it's unknown. MJPEG/MP3 frames are all independent anyway.
        ok = parse_block(size, false, true);
    } else {
        ok = skip(size);
    }
    if (!ok) {
        if (err_.empty()) err_ = "truncated or malformed element";
        return -1;
    }
    return 1;
}

bool MkvDemuxer::open() {
    uint32_t id;
    uint64_t size;
    bool unknown;
    if (read_header(id, size, unknown) != 1 || id != ID_EBML || unknown) {
        err_ = "not a Matroska stream";
        return false;
    }
    std::string doctype;
    uint64_t end = pos_ + size;
    while (pos_ < end) {
        uint32_t cid;
        uint64_t csize;
        if (read_header(cid, csize, unknown) != 1 || unknown) {
            err_ = "bad EBML header";
            return false;
        }
        bool ok = cid == 0x4282 ? read_string(csize, doctype) : skip(csize);
        if (!ok) {
            err_ = "bad EBML header";
            return false;
        }
    }
    if (doctype != "matroska" && doctype != "webm") {
        err_ = "unsupported doctype '" + doctype + "'";
        return false;
    }
    while (!tracks_done_) {
        int r = step();
        if (r <= 0) {
            if (err_.empty()) err_ = "stream ended before the track list";
            return false;
        }
    }
    if (tracks_.empty()) {
        err_ = "no tracks";
        return false;
    }
    return true;
}

bool MkvDemuxer::seek_bytes(uint64_t target) {
    pending_.clear();
    err_.clear();
    cur_track_ = nullptr;
    if (!src_.seek(target)) {
        err_ = "source is not seekable";
        return false;
    }
    pos_ = target;
    // Scan for a Cluster ID whose first child is a CRC-32 or Timecode element;
    // that rules out the 4-byte pattern occurring inside frame data.
    const uint64_t limit = target + (8u << 20);
    uint32_t window = 0;
    uint8_t b;
    while (pos_ < limit) {
        if (!read_exact(&b, 1)) {
            err_ = "end of stream while seeking";
            return false;
        }
        window = (window << 8) | b;
        if (window != ID_Cluster) continue;
        window = 0;
        // Cluster size (EBML vint).
        if (!read_exact(&b, 1)) return false;
        int len = 1;
        while (len <= 8 && !(b & (0x80 >> (len - 1)))) len++;
        if (len > 8) continue;
        uint64_t size = b & (0xFF >> len);
        bool all_ones = size == (uint64_t)(0xFF >> len);
        for (int i = 1; i < len; i++) {
            if (!read_exact(&b, 1)) return false;
            size = (size << 8) | b;
            all_ones = all_ones && b == 0xFF;
        }
        uint64_t data_start = pos_;
        uint32_t cid;
        uint64_t csize;
        bool cunknown;
        if (read_header(cid, csize, cunknown) != 1 || cunknown) continue;
        if (cid != 0xBF && cid != ID_Timecode) continue;
        stack_.clear();
        stack_.push_back({ID_Segment, UINT64_MAX});
        stack_.push_back({ID_Cluster, all_ones ? UINT64_MAX : data_start + size});
        tracks_done_ = true;
        cluster_tc_ = 0;
        uint64_t u = 0;
        bool ok = cid == ID_Timecode ? read_uint(csize, u) : skip(csize);
        if (!ok) {
            err_ = "end of stream while seeking";
            return false;
        }
        if (cid == ID_Timecode) cluster_tc_ = (int64_t)u;
        return true;
    }
    err_ = "no cluster found near seek target";
    return false;
}

int MkvDemuxer::next(MkvPacket& pkt) {
    while (pending_.empty()) {
        int r = step();
        if (r <= 0) return r;
    }
    pkt = std::move(pending_.front());
    pending_.pop_front();
    return 1;
}

}  // namespace nxp
