# NXPlex

**Watch Plex while you play.** NXPlex is a homebrew Plex client for the
Nintendo Switch (Atmosphère) that plays your Plex library in a small
picture-in-picture window **on top of any game**. The game keeps running and
keeps the controller; you control playback from the Tesla overlay menu.

- Streams movies and TV from your own Plex Media Server, with sound mixed over the game
- Sign in with a code at [plex.tv/link](https://plex.tv/link) (no typing passwords on the Switch)
- *Continue Watching* and full library browsing (movies, shows, seasons, episodes)
- Resume points and watched state sync with your other Plex apps
- **Autoplay**: when an episode ends, the next one starts (next season too); can be turned off
- **Next episode** button while watching a show
- Pause, skip ±10/30 s, volume, window corner and size, video quality (144p/216p/288p)
- Keeps playing while you open Tesla or other overlays
- Optional controller shortcuts so you don't even need to open the menu

> NXPlex is an independent fan project. It is not affiliated with, endorsed by,
> or sponsored by Plex, Inc. or Nintendo. "Plex" is a trademark of Plex, Inc.

## Requirements

- A Switch running **Atmosphère** (tested on 1.9.x with firmware 20.0.1)
- **Tesla**: [nx-ovlloader](https://github.com/WerWolv/nx-ovlloader) + [Tesla-Menu](https://github.com/WerWolv/Tesla-Menu)
- A **Plex Media Server** your Switch can reach (same network is best) and a Plex account.
  The server does the heavy lifting (transcoding), so it needs a little CPU to spare.

## Install

1. Download `NXPlex-<version>.zip` from the [releases](../../releases).
2. Extract it to the **root of your SD card**. It contains:
   - `atmosphere/contents/4200000000504C58/`: the background service
   - `switch/.overlays/nxplex.ovl`: the Tesla overlay
3. Reboot.

The background service is **not** started at boot: the overlay starts it when
you open NXPlex, and it exits after 10 idle minutes (or via *Stop NXPlex
service*), so it doesn't use memory when you're not watching.

## Usage

1. In a game, open Tesla (default **L + D-pad Down + R3**) and choose **NXPlex**.
2. **Sign in to Plex**: on your phone or computer go to plex.tv/link and enter the code shown.
3. Pick your server if you have more than one.
4. Open **Continue Watching** or **Libraries**. **A** plays (resuming where you
   left off), **Y** plays from the beginning.
5. Close Tesla. The video keeps playing in the corner.

Open Tesla again any time for the **Now playing** controls: pause, skip,
next episode, volume, window position and size, stop. *Settings* has video
quality and *Autoplay next episode* (on by default). While Tesla is open the video
window steps aside (audio keeps playing) and comes back when Tesla closes;
see [Limitations](#limitations).

### Shortcuts (optional)

Without opening Tesla: hold **L + R3** (right stick pressed), then press:

| Button | Action |
|---|---|
| Minus | Stop the video |
| D-pad Up | Pause / resume |
| D-pad Left / Right | Back 10 s / forward 30 s |
| ZR / R | Video volume up / down |
| ZL | Move the window to the next corner |

The game also sees these buttons, so use them at a calm moment.

## How it works

The Switch side has to be tiny: a background module gets a few MB of memory
and the one CPU core games leave free. So the Plex server does all the work and
sends a stream that is trivial to play:

- **Matroska** container, **MJPEG** video (every frame is a small standalone
  JPEG) and **MP3** stereo audio, about 0.5–1.5 Mbit/s.
- Requested from the universal transcoder (`/video/:/transcode/universal/decision`
  then `/start`, same query) with `X-Plex-Platform: Generic` and
  `X-Plex-Client-Profile-Extra: add-transcode-target(type=videoProfile&context=streaming&protocol=http&container=mkv&videoCodec=mjpeg&audioCodec=mp3)`.
- Seeking restarts the transcode at `offset=` (timestamps restart at 0).
- Progress is reported with `/:/timeline` so resume points sync; items played
  to the end are marked watched with `/:/scrobble`.
- The next episode is the next one in the season, else episode 1 of the next
  season (specials only continue within specials).

Plex quirks found along the way: unknown `X-Plex-Platform` values are
rejected; audio-only and baseline-H.264 requests are ignored (you get the full
video anyway); `/start` answers HTTP 400 unless `/decision` was called first
for the same session.

On the Switch, the **sysmodule** (`sysmodule/`) streams over HTTPS (mbedTLS),
demuxes MKV, decodes JPEG (stb_image) and MP3 (minimp3), draws frames on its
own vi layer and plays audio through `audout`, mixed with the game. Audio is
the master clock. The **overlay** (`overlay/`, libtesla) is a thin remote that
talks to the sysmodule over a named port (`common/nxplex_ipc.h`).

Everything except the Switch glue lives in `core/` and also builds on Linux,
so the streaming code is tested end to end on a PC against a real server
(`host/apptest`).

## Limitations

- **The video hides while Tesla is open.** The display shows only one of the
  two overlay layers; with both present, Tesla opens invisibly. NXPlex infers
  Tesla's state from its combo (read from `config/tesla/config.ini`), B/A on the
  Tesla menu, and the NXPlex overlay itself. If it ever gets out of sync, press
  the Tesla combo twice. Transitions are logged as `tesla: ... -> ...`.
- Low resolution by design (up to 288p): it's a corner window.
- No subtitles yet, no music libraries yet, first audio track only.
- Titles in scripts the Switch's system font lacks may not render in Tesla.

## Troubleshooting

- Logs: `sd:/switch/nxplex/nxplex.log`. Account names, tokens and public IPs are
  not written to it, so it's safe to attach to an issue.
- Settings and the sign-in token: `sd:/config/nxplex/nxplex.ini`
  (**keep this file private**; delete it to sign out completely).
- Crash reports: `sd:/atmosphere/crash_reports/`. Please attach the newest one to an issue.

## Building

You need [devkitPro](https://devkitpro.org/wiki/Getting_Started) with
`switch-dev` (libnx, devkitA64) and `switch-mbedtls`.

```sh
make dist        # -> dist/NXPlex-<version>.zip
```

On Ubuntu 22.04 the current devkitA64 needs a newer glibc than the system
has; `tools/setup-toolchain.sh` makes a patched copy (via Nix) without touching
`/opt/devkitpro`:

```sh
tools/setup-toolchain.sh
DEVKITA64=~/.local/share/nxplex-toolchain/devkitA64 make dist
```

### PC tools and tests (Linux/WSL)

```sh
make host                   # downloads mbedTLS 2.28.10 on first run
host/json_test              # unit tests
host/nxplex-cli login       # sign in (stored in ~/.config/nxplex/nxplex.ini)
host/nxplex-cli servers && host/nxplex-cli use 1
host/apptest                # end-to-end: browse, stream, seek, pause, quality, resume point
host/episodetest            # Next episode + autoplay (restores watched state afterwards)
host/avtest file.mkv 10     # play a file through the player with a simulated audio device
```

`apptest` plays the first *Continue Watching* item and restores its resume
point afterwards.

## Project layout

| Path | |
|---|---|
| `core/` | Portable C++: HTTP/TLS, streaming JSON, Plex API, MKV demuxer, decoders, A/V player, app logic |
| `common/` | IPC protocol shared by the sysmodule and the overlay |
| `sysmodule/` | Background service (title `4200000000504C58`) |
| `overlay/` | Tesla overlay (remote control and browser) |
| `host/` | PC tools and tests |
| `tools/` | Toolchain helper, mbedTLS fetcher, CA bundle generator |

## Credits and licenses

NXPlex is licensed under the **GNU GPL v2** (see `LICENSE`).

It builds on:
- [libnx](https://github.com/switchbrew/libnx) and [devkitPro](https://devkitpro.org)
- [libtesla](https://github.com/WerWolv/libtesla) by WerWolv (GPL-2.0), vendored in `overlay/lib/libtesla`
- [mbedTLS](https://github.com/Mbed-TLS/mbedtls) (Apache-2.0 or GPL-2.0-or-later)
- [stb_image](https://github.com/nothings/stb) (public domain / MIT)
- [minimp3](https://github.com/lieff/minimp3) (CC0)
- Ideas from [sys-tune](https://github.com/HookedBehemoth/sys-tune) (background audio from a sysmodule),
  [Switchfin](https://github.com/dragonflylee/switchfin) and [Plezy](https://github.com/edde746/plezy)
