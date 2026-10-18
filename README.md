# Video Mixes for OBS Studio

Hide sources per output, like audio tracks but for video. Stream with your camera, alerts and chat, and at the same
time record, buffer or stream a **clean** version of the exact same scenes without them.

- **Program mixes**: your live program, same scenes and transitions, minus the sources you hide.
- **Source mixes**: one source or scene on its own, at the size it has on screen. An isolated webcam track, for
  example, with only the webcam's own audio.
- Every mix can **record** to a file, keep a **replay buffer** and **stream** to a second destination. Each output
  starts together with OBS' recording, stream or replay buffer, or on demand.
- Same scenes, no duplicated scenes, no second OBS, no virtual-camera loopback. Hidden sources keep running; they are
  only left out of that mix.

Requires **OBS Studio 32** (canvas API) on **Windows x64**.

## Install

Download the latest release from the [Releases](https://github.com/AWGshka/obs-video-mixes/releases) page:

- **Installer** (`obs-video-mixes-<version>-windows-x64-installer.exe`): close OBS and run it.
- **Zip** (`obs-video-mixes-<version>-windows-x64.zip`): extract it into `C:\ProgramData\obs-studio\plugins`.

Then start OBS and open **Docks → Video Mixes**.

## Quick start

1. In the **Video Mixes** dock, click **+** → **Program mix**. The settings open; the defaults copy your OBS recording
   settings, so you can just click **Add mix**.
2. Below the mixes, every source of the current scene has an eye per program mix. Click the eye of the camera, alerts
   and chat in your new mix's column to hide them there.
3. Start recording in OBS. Next to your normal recording, a second file `… (Clean).mkv` appears without those sources.

**Preview** on a mix shows it live in the dock. It only renders while the preview is open.

## Mixes

| | Program mix | Source mix |
|---|---|---|
| Shows | the live program: same scenes, switches and transitions, minus hidden items | one source or scene, by itself |
| Resolution | OBS output resolution, or any size | the size the source has on screen (crop, scale, bounds, rotation, nested scenes applied), e.g. a webcam cropped and scaled to 480×270 gives a 480×270 video. A source not placed in any scene uses its own size |
| Frame rate | OBS fps, or a fixed lower rate | Auto: the source's own rate where it reports one, otherwise OBS fps; or a fixed rate |
| Audio | OBS audio tracks of your choice | the source's own audio (volume and mute applied), or OBS audio tracks |
| Typical use | clean gameplay without camera, alerts and chat | isolated webcam track for editing |

Visibility is stored per scene item in the scene collection, so the same webcam can be hidden in one scene and visible
in another, and duplicated scenes keep their settings. Groups and nested scenes can be expanded in the dock to hide
things inside them.

## Outputs

Each mix has three outputs, enabled in its settings:

- **Recording**: a file per mix. When it starts with the OBS recording, it is saved next to it with the same name plus
  ` (<mix name>)`, so the pair lines up in an editor. It can also go to a folder of its own. Formats: MKV, MP4, MOV and
  the crash-safe Hybrid MP4/MOV. The mix pauses and resumes with OBS.
- **Replay buffer**: keeps the last seconds of the mix in memory. Save it with the dock's save button, its own hotkey,
  obs-websocket, or automatically whenever the OBS replay buffer is saved. It shares the recording's encoder, so
  recording plus replay buffer encode only once.
- **Stream**: sends the mix to a second destination (RTMP, RTMPS, SRT or RIST), for example the clean feed to YouTube
  while OBS streams the full program to Twitch. It reconnects automatically and shows its bitrate in the dock.

Each output starts **together with the OBS recording, the OBS stream, the OBS replay buffer, or manually** (dock,
hotkey or obs-websocket).

**Encoding.** A new mix copies your OBS recording settings (*Settings → Output → Recording*): the encoder with all its
settings, audio tracks, audio bitrate and format. The stream copies your OBS streaming encoder and bitrate. *Use OBS
recording settings* and *Use OBS stream encoder settings* re-sync an existing mix. GPU encoders (NVENC, AMF, QSV) encode
mixes straight from the GPU. The settings warn when a recording uses constant bitrate.

**Audio.** Alerts usually make sound too. Put alert and browser sources on an audio track the mix does not record
(*Edit → Advanced Audio Properties*) and untick that track in the mix's audio settings.

**Hotkeys.** *Settings → Hotkeys* lists, per mix, start/stop recording, start/stop/save the replay buffer and start/stop
streaming.

## obs-websocket

The plugin registers the vendor `video-mixes` with obs-websocket 5. Send requests with `CallVendorRequest`:

```json
{ "requestType": "CallVendorRequest",
  "requestData": { "vendorName": "video-mixes", "requestType": "StartOutput",
                   "requestData": { "mixName": "Clean", "output": "record" } } }
```

| Request | Data | Response |
|---|---|---|
| `GetMixList` | | `mixes: [Mix]` |
| `GetMix` | `mixName` or `mixId` | `Mix` |
| `StartOutput`, `StopOutput` | `mixName` or `mixId`, `output`: `record` \| `replay` \| `stream` | |
| `ToggleOutput` | same | `active` |
| `SaveReplay` | `mixName` or `mixId` | |
| `GetItemVisibility` | `sceneName`, `sceneItemId` or `sourceName` | `mixes: [{ mixId, mixName, visible }]` |
| `SetItemVisibility` | `sceneName`, `sceneItemId` or `sourceName`, `mixName` or `mixId`, `visible` | |

`Mix` is `{ mixId, mixName, kind, sourceName, previewing, outputs: { record, replay, stream } }`, and each output is
`{ enabled, active, paused, reconnecting, trigger, path, error }`. Every response has `ok`; failures add `error`.

Vendor events (subscribe to `Vendors`): `OutputStateChanged { mixId, mixName, output, active, paused, path }` and
`ReplaySaved { mixId, mixName, path }`.

Tools that can send raw obs-websocket requests, such as Streamer.bot's *OBS Raw* action, can call these directly.

## How it works

- Each mix is a private libobs **canvas** that exists only while one of its outputs runs or its preview is open.
  libobs renders every canvas every frame, so idle mixes cost nothing.
- The canvas renders a private **root source**. It walks the program's scene graph and leaves out items whose
  per-item mask (stored in the scene item's private settings, saved with the scene collection) contains the mix.
- libobs caches item, nested-scene, filter and transition renders once per frame, so re-rendering the program graph
  would reuse textures that still contain the hidden items. The renderer therefore draws every subtree that contains a
  hidden item with its own render targets, mirroring libobs' `render_item` (crop, bounds crop, scale filters, blend
  modes, color spaces, show/hide transitions). Filters on such scenes and groups run as private copies that follow the
  originals' settings. Subtrees without hidden items go through libobs and share its caches. When nothing in the
  current program differs, the mix copies the finished program texture.
- Scene transitions: if neither side contains hidden items, the original transition is drawn (stingers, luma wipes).
  Otherwise the mix uses a fade or a cut (per-mix setting).
- A source mix's own audio is captured after the source's filters, with its volume, mute and sync offset applied, and
  fed to the encoder as its own track on the OBS clock.

## Known limitations

- A source mix takes its on-screen size when an output starts. If the placement changes size while it runs, the
  picture is letterboxed into the original size. Crops on a *parent* nested scene are not applied.
- Auto frame rate can only use rates a source reports (DirectShow custom fps, browser custom fps). Phone cameras,
  device-default webcams and media files fall back to OBS fps; pick a fixed rate for them.
- A mix stream carries one audio track. It does not support OBS' multitrack video (Enhanced Broadcasting) or service
  integrations (login-based Twitch/YouTube setup): enter the server URL and stream key.
- Mix definitions are global (not per profile). Visibility lives in the scene collection.
- Preview requires Windows.

## Building

C++20, Qt 6, CMake. Requirements: Visual Studio 2022 or newer (MSVC), CMake 3.28+ and Ninja. Run from an
**x64 Native Tools Command Prompt** (or Developer PowerShell):

```bat
cmake --preset windows                          :: first run downloads + verifies the SDK into .deps/
cmake --build --preset windows                  :: build/obs-video-mixes.dll
ctest --preset windows                          :: headless libobs + D3D11 render and output tests against your OBS
cmake --install build                           :: → %ProgramData%/obs-studio/plugins (close OBS first)
cmake --build --preset windows --target package :: build/obs-video-mixes-<version>-windows-x64.zip
```

OBS is found through the registry or `Program Files`. If it is installed elsewhere, pass `-DOBS_INSTALL_DIR=...`, or
put it in a gitignored `CMakeUserPresets.json` preset that inherits `windows`.

On first configure, `cmake/obs-sdk.cmake` downloads the OBS 32.2.1 sources and OBS' own Qt 6.11.1 build (both SHA-256
pinned) and keeps only the headers and Qt import libraries it needs (~25 MB). It then generates import libraries from
your installed `obs.dll` / `obs-frontend-api.dll`, so the plugin binary always matches the OBS you test with. With
`-DVMIX_USE_INSTALLED_OBS=OFF`, regular `find_package(libobs / obs-frontend-api / Qt6)` is used instead, for builds
against a full OBS build tree.

The installer is built with [Inno Setup](https://jrsoftware.org/isinfo.php) from `installer/obs-video-mixes.iss`; CI
(`.github/workflows/build.yml`) builds, tests and packages every push and drafts a release for `v*` tags.

### Layout

```
src/plugin.cpp               module entry
src/core/                    config (mixes.json), manager (runtime, frontend sync, hotkeys), mix-canvas (private
                             canvas), outputs (encoders, recording / replay buffer / stream), source-audio,
                             visibility (item masks), websocket (vendor API), encoders, frame-rate, item-path
src/render/                  root-source (canvas channel source), mix-renderer (scene walk), scene-filters, geometry
src/ui/                      dock, visibility tree, settings dialog, encoder properties form, live preview
tests/smoke.cpp              end-to-end checks on real libobs (CTest)
cmake/obs-sdk.cmake          SDK setup against the installed OBS
installer/                   Inno Setup script
```

## License

GPL-2.0-or-later, like OBS Studio. See [LICENSE](LICENSE).
