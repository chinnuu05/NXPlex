# Changelog

## 0.5.1

- **New shortcuts: hold L3** (left stick pressed) instead of L + R3, with every
  action on the right hand: **ZR** hide/show the window, **R** pause/resume,
  **Y / A** back 10 s / forward 30 s, **X / B** volume up/down, **+** stop.

## 0.5.0

Everything since 0.3.0.

**Watching**
- **Autoplay**: when an episode ends, the next one starts (including the first episode of the next season). Can be turned off in Settings.
- **Next episode** button in Now playing.
- Episodes and movies watched to the end are marked watched in Plex.

**Controls**
- **Position slider** with a **live preview** of the video in Now playing, so you can see where you're seeking without closing Tesla.
- **Hide / show the video window** (a shortcut, or *Video window* in Now playing) when it covers part of a game. Audio keeps playing.
- The shortcut no longer moves the window to the next corner; use *Window position* in Tesla.

**Picture**
- **360p and 480p** quality. Frame memory went from ~17.5 to ~6 bytes per pixel, and JPEG decoding uses NEON.
- **Auto quality** (new default): follows the window size, one step higher when docked.
- **Five window sizes**: Small, Medium (default), Large, Extra large, Huge (about two-thirds of the screen).

Update both files together (the overlay and the background service): the overlay will tell you if they don't match.

## 0.3.0

First public release: Plex sign-in with a code, Continue Watching and library browsing, picture-in-picture playback over games, Tesla remote, resume points synced with Plex.
