// NXPlex application logic, shared by the Switch sysmodule and the PC test
// harness. Owns the Plex account, server choice, browsing and playback.
//
// Threads:
//   - IPC/caller threads call cmd_*() and get_*(): these never block on the
//     network; slow work is handed to the worker thread.
//   - worker thread: sign-in polling, server discovery, browsing, starting
//     streams, seeking (stream restarts), Plex timeline reports.
//   - display thread: calls video_tick() in a loop.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "config.hpp"
#include "nxplex_ipc.h"
#include "player.hpp"
#include "plex.hpp"
#include "sources.hpp"
#include "thread.hpp"

namespace nxp {

class App {
public:
    struct Hooks {
        std::function<void(int volume)> set_volume;            // 0..100
        std::function<void(int corner, int size)> set_layout;  // PiP placement
    };

    App(Config& cfg, const PlexClient& client, AudioSink& audio, VideoSink& video, Hooks hooks);
    ~App();

    bool start();
    void stop();

    // ---- queries (fast)
    void get_status(NxpStatus& out);
    void get_servers(NxpServers& out);
    void get_browse(NxpBrowse& out);
    bool quit_requested();

    // ---- commands (fast; work is queued)
    void cmd_start_login();
    void cmd_logout();
    void cmd_refresh_servers();
    void cmd_select_server(uint32_t index);
    uint32_t cmd_browse(const std::string& path, uint32_t start);
    void cmd_play(const std::string& key, bool from_start);
    void cmd_play_file(const std::string& path);
    void cmd_set_paused(int mode);  // 0 resume, 1 pause, 2 toggle
    void cmd_seek_relative(int32_t delta_ms);
    void cmd_seek_absolute(uint32_t ms);
    void cmd_stop();
    void cmd_next_episode();
    void cmd_set_autoplay(bool on);
    void cmd_set_volume(int v);
    void cmd_set_layout(int corner, int size);
    void cmd_set_quality(int q);  // 0..4, or NXP_QUALITY_AUTO
    void cmd_set_docked(bool docked);  // platform reports TV/handheld (Auto quality)
    void cmd_quit();

    // ---- display thread
    // Presents the due frame. Returns ms to sleep; `active` = a stream is open.
    int video_tick(bool& active);
    bool playing_or_loading();

    // Test hook: blocks until the worker queue is idle (host tests only).
    void wait_idle(uint32_t timeout_ms);

private:
    enum class Job : uint8_t { None, Servers, SelectServer, Browse, Play, PlayFile, Seek, Stop, Restart, Next };

    static void worker_entry(void* p) { ((App*)p)->worker_loop(); }
    void worker_loop();
    void run_job(Job job);
    void periodic();

    void job_servers();
    void job_select_server();
    void job_browse();
    void job_play();
    void job_play_file();
    void job_seek();
    void job_stop(bool report, const char* reason);
    void job_restart();
    // Stops the current episode and plays the one after it. `finished`: the
    // current one played to the end (autoplay) rather than being skipped.
    void job_next(bool finished);

    bool open_stream(uint32_t offset_ms, std::string& err);
    void teardown_player();
    void report_timeline(const char* state, int64_t pos_ms = -1);  // pos_ms < 0: current position
    // Ends the stream, then reports "stopped" at the position it had. In that
    // order: when the transcode ends Plex may record the transcoder's own
    // (far-ahead) position, and our report has to land after it.
    void stop_and_report();
    void mark_watched();  // the current item played to the end
    uint32_t position_locked();  // needs play_mu_
    void set_error(const std::string& e);
    void queue(Job j);
    PlexServerSession session();
    void save_config_locked();

    Config& cfg_;
    PlexClient client_;     // plex.tv calls
    PlexClient server_cl_;  // server calls (platform "Generic" so the transcoder accepts our profile)
    AudioSink& audio_;
    VideoSink& video_;
    Hooks hooks_;

    // state_mu_ guards everything below except the player objects.
    std::mutex state_mu_;
    std::condition_variable cv_;
    std::vector<Job> jobs_;
    bool running_ = false, quit_ = false, busy_ = false;
    Thread worker_;

    // account
    uint8_t auth_ = NxpAuth_SignedOut;
    PlexPin pin_;
    uint64_t pin_deadline_ = 0, pin_next_poll_ = 0;
    std::string account_;

    // servers
    uint8_t servers_state_ = NxpServers_Idle;
    std::vector<PlexServer> servers_;
    std::string servers_error_;
    uint32_t select_index_ = 0;

    // browse
    uint32_t browse_seq_ = 0;
    std::string browse_path_;
    uint32_t browse_start_ = 0;
    NxpBrowse* browse_ = nullptr;  // large: heap

    // playback request / state
    std::string play_key_, play_file_;
    bool play_from_start_ = false;
    int64_t pending_seek_ms_ = -1;
    uint8_t play_state_ = NxpPlay_Stopped;
    PlexItem item_;
    uint32_t duration_ms_ = 0;
    std::string title_, subtitle_, error_;
    int volume_ = 100, corner_ = 0, size_ = 1, quality_ = 1;
    bool autoplay_ = true;
    bool quality_auto_ = true, docked_ = false;
    uint64_t auto_due_ms_ = 0;  // when to re-pick the Auto quality (debounced)
    int auto_quality_locked() const;
    bool cfg_dirty_ = false;
    bool end_reported_ = false;
    uint64_t next_timeline_ms_ = 0;
    uint32_t restarts_ = 0;
    uint64_t restart_window_ms_ = 0;

    // play_mu_ guards the player objects (lock order: play_mu_, then state_mu_).
    std::mutex play_mu_;
    std::unique_ptr<HttpSource> http_;
    std::unique_ptr<FileSource> file_;
    std::unique_ptr<Player> player_;
    uint32_t stream_offset_ms_ = 0;  // stream restarts at an offset; pts start at 0
    bool is_file_ = false;
    std::string transcode_session_;
    bool finished_flag_ = false;
};

}  // namespace nxp
