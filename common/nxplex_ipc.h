/*
 * NXPlex IPC protocol between the background sysmodule (server, named port
 * "nxplex") and the Tesla overlay (client). Plain C, fixed-size PODs only:
 * these structs cross the process boundary byte for byte. Large replies use
 * HIPC mapped out-buffers.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXP_API_VERSION 5
#define NXP_PORT_NAME "nxplex"
#define NXP_SYSMODULE_TID 0x4200000000504C58ULL /* "PLX" */

#define NXP_MAX_ITEMS 40 /* items per browse page */
#define NXP_QUALITY_AUTO 5 /* SetQuality: pick from the window size */
#define NXP_MAX_SERVERS 8

enum NxpAuthState { NxpAuth_SignedOut = 0, NxpAuth_PinPending, NxpAuth_SignedIn };
enum NxpPlayState { NxpPlay_Stopped = 0, NxpPlay_Loading, NxpPlay_Playing, NxpPlay_Paused };
enum NxpBrowseState { NxpBrowse_Idle = 0, NxpBrowse_Loading, NxpBrowse_Ready, NxpBrowse_Error };
/* The overlay tells the service when Tesla shows/hides it, so the video
 * window can step aside (only one of the two layers can be visible). */
enum NxpUiState { NxpUi_Hidden = 0, NxpUi_Shown = 1, NxpUi_Exiting = 2 };
enum NxpServersState { NxpServers_Idle = 0, NxpServers_Loading, NxpServers_Ready, NxpServers_Error };

typedef struct {
    uint32_t api_version;
    uint8_t auth_state;    /* NxpAuthState */
    uint8_t play_state;    /* NxpPlayState */
    uint8_t browse_state;  /* NxpBrowseState */
    uint8_t servers_state; /* NxpServersState */
    uint8_t volume;        /* 0..100 */
    uint8_t corner;        /* 0=top right 1=top left 2=bottom left 3=bottom right */
    uint8_t size;          /* 0..4: small, medium (default), large, extra large, huge */
    uint8_t quality;       /* 0..4: 144p, 216p, 288p, 360p, 480p (the one in use, also in Auto) */
    uint8_t has_server;
    uint8_t is_episode;  /* the current item is a TV episode (Next episode applies) */
    uint8_t autoplay;    /* start the next episode when one finishes */
    uint8_t quality_auto; /* quality follows the window size (and docked mode) */
    uint32_t browse_seq;
    uint32_t position_ms;
    uint32_t duration_ms;
    char pin_code[8];
    char account[48];
    char server_name[48];
    char title[96];
    char subtitle[96];
    char error[128];
} NxpStatus;

typedef struct {
    uint32_t duration_ms;
    uint32_t view_offset_ms;
    uint32_t leaf_count;
    uint32_t viewed_leaf_count;
    int16_t index;
    int16_t parent_index;
    uint16_t year;
    uint8_t is_leaf;    /* playable */
    uint8_t is_section; /* a library */
    char type[12];
    char title[96];
    char parent[64];
    char grandparent[64];
    char key[112];
} NxpItem;

typedef struct {
    uint32_t seq;
    uint8_t state; /* NxpBrowseState */
    uint8_t pad[3];
    uint32_t offset;
    uint32_t count;
    uint32_t total;
    char path[112];
    char title[64];
    char error[96];
    NxpItem items[NXP_MAX_ITEMS];
} NxpBrowse;

typedef struct {
    char name[48];
    uint8_t owned;
    uint8_t selected;
    uint8_t online;
    uint8_t pad;
} NxpServerInfo;

typedef struct {
    uint32_t count;
    char error[96];
    NxpServerInfo servers[NXP_MAX_SERVERS];
} NxpServers;

/* Latest video frame, for the preview inside the Tesla overlay (the PiP
 * window itself can't be shown while Tesla is open). Pixels follow the
 * header only when seq differs from the one the client passed in. */
#define NXP_FRAME_MAX_W 512
#define NXP_FRAME_MAX_H 288
typedef struct {
    uint32_t seq;  /* 0 = no frame */
    uint16_t width, height;
    uint8_t has_pixels;
    uint8_t pad[7];
} NxpFrameHeader;

typedef struct {
    char key[112]; /* item path, e.g. /library/metadata/123 */
    uint8_t from_start;
    uint8_t pad[7];
} NxpPlayArgs;

typedef struct {
    char path[112];
    uint32_t start;
    uint32_t pad;
} NxpBrowseArgs;

enum NxpCmd {
    NxpCmd_GetStatus = 0,      /* out buf: NxpStatus */
    NxpCmd_StartLogin = 1,
    NxpCmd_Logout = 2,
    NxpCmd_RefreshServers = 3,
    NxpCmd_GetServers = 4,     /* out buf: NxpServers */
    NxpCmd_SelectServer = 5,   /* in: u32 index */

    NxpCmd_Browse = 10,        /* in buf: NxpBrowseArgs; out: u32 seq */
    NxpCmd_GetBrowse = 11,     /* out buf: NxpBrowse */

    NxpCmd_Play = 20,          /* in buf: NxpPlayArgs */
    NxpCmd_SetPaused = 21,     /* in: u32 (0/1, 2 = toggle) */
    NxpCmd_Stop = 22,
    NxpCmd_SeekRelative = 23,  /* in: s32 ms */
    NxpCmd_SeekAbsolute = 24,  /* in: u32 ms */
    NxpCmd_SetVolume = 25,     /* in: u32 0..100 */
    NxpCmd_SetLayout = 26,     /* in: u32 corner | size << 8 */
    NxpCmd_SetQuality = 27,    /* in: u32 0..4, or NXP_QUALITY_AUTO */
    NxpCmd_PlayFile = 28,      /* in buf: path (debug: local MKV) */
    NxpCmd_UiState = 30,       /* in: u32 NxpUi* (overlay shown/hidden/closing) */
    NxpCmd_NextEpisode = 31,
    NxpCmd_SetAutoplay = 32,   /* in: u32 0/1 */
    NxpCmd_GetFrame = 33,      /* in: u32 last seq; out buf: NxpFrameHeader + RGB565 pixels */

    NxpCmd_Quit = 50,
};

#ifdef __cplusplus
}
#endif
