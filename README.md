# Video Mixes for OBS Studio

Hide sources per output, like audio tracks but for video. Stream with your camera, alerts and chat. At the same
time, record a **clean** version of the exact same scenes without them.

- Add a mix (e.g. *Clean*). In the **Video Mixes** dock, untick the sources it should not show.
- The mix records automatically together with your OBS recording or stream, or manually / by hotkey.
- Same scenes, same transitions, no duplicated scenes, no second OBS, no virtual-camera loopback.
- Hidden sources keep running. They are only left out of that mix.

Requires **OBS Studio 32.x** (canvas API, 31.1+). Windows x64 is the primary target.

## Using it

**Docks → Video Mixes**, then **+**. There are two kinds of mix:

| | Program mix | Source mix |
|---|---|---|
| Records | your live program: same scenes, switches and transitions, minus unticked items | one source or scene, by itself |
| Resolution | OBS output resolution (configurable) | the size the source has on screen (crop, scale, bounds, rotation, nested scenes applied), e.g. a webcam cropped and scaled to 480×270 gives a 480×270 file. A source not placed in any scene uses its own size |
| Frame rate | OBS fps (or a fixed lower rate) | Auto: the source's rate where it reports one, otherwise OBS fps; or a fixed rate (30, 20, 15…) |
| Typical use | clean gameplay without camera, alerts and chat | isolated webcam track for editing |

1. The settings dialog opens: choose when the mix starts, the format, the encoder and the audio tracks. For a
   Source mix, also choose the source.
2. The tree lists the sources of the current scene (the preview scene in Studio Mode), with one column per program
   mix. Untick a source to hide it in that mix. Groups and nested scenes can be expanded, so you can hide things
   inside them too.
3. **Preview** shows the mix live. It only renders while the preview is open.
4. Recordings are saved next to the main recording, with the same name plus ` (<mix name>)`, so the pair lines up
   in an editor. They can also go to a folder you choose.

**Encoding:** a new mix copies your OBS recording settings (*Settings → Output → Recording*): encoder with all its
settings (rate control, bitrate, preset…), audio tracks, audio bitrate and format. *Use OBS recording settings* in the
mix dialog re-syncs an existing mix. Without usable OBS settings, mixes use the GPU encoder (NVENC / AMF / QSV,
detected by capability) in a constant-quality mode. The dialog warns about constant bitrate only when the mix deviates
from your OBS recording.

**Audio:** alerts usually make sound too. That is handled by OBS' existing audio tracks: put alert/browser sources on
a track the mix does not record (mix settings → *Audio tracks*).

Hotkeys: *Settings → Hotkeys → "Start/Stop recording video mix '…'"*.

## How it works

- Each mix is a private libobs **canvas** that exists only while it records or previews. libobs renders every
  canvas every frame, so idle mixes cost nothing.
- The canvas renders a private **root source**. It walks the program's scene graph and leaves out items whose
  per-item mask (stored in the scene item's private settings, saved with the scene collection) contains the mix.
- libobs caches item, nested-scene and transition renders once per frame. Re-rendering the program graph would
  reuse textures that still contain the hidden items. The renderer therefore draws every subtree that contains a
  hidden item with its own render targets, mirroring libobs' `render_item` (crop, bounds crop, scale filters,
  blend modes, color spaces). Subtrees without hidden items go through libobs and share its caches. When nothing
  in the current program differs, the mix just copies the finished program texture.
- Transitions: if neither side contains hidden items, the original transition is drawn (stingers, luma wipes).
  Otherwise the mix uses a fade or a cut (per-mix setting).

## Development

C++20 / Qt 6 / CMake only. Requirements: Visual Studio (MSVC), CMake 3.28+ and Ninja. Run the commands from an
**x64 Native Tools Command Prompt** (or Developer PowerShell):

```bat
cmake --preset windows                          :: first run downloads + verifies the SDK into .deps/
cmake --build --preset windows                  :: build/obs-video-mixes.dll
ctest --preset windows                          :: headless libobs + D3D11 render test against your installed OBS
cmake --install build                           :: → %ProgramData%/obs-studio/plugins (close OBS first)
cmake --build --preset windows --target package :: build/obs-video-mixes-<version>-windows-x64.zip
```

OBS is found through the registry or `Program Files`. If it is installed elsewhere, pass `-DOBS_INSTALL_DIR=...`,
or put it in a gitignored `CMakeUserPresets.json` preset that inherits `windows`.

On first configure, `cmake/obs-sdk.cmake` downloads the OBS 32.2.1 sources and OBS' own Qt 6.11.1 build (both
SHA-256 pinned) and keeps only the headers and Qt import libs it needs (~25 MB). It then generates import libraries
from your installed `obs.dll` / `obs-frontend-api.dll`, so the plugin binary always matches the OBS you test with.
With `-DVMIX_USE_INSTALLED_OBS=OFF`, regular `find_package(libobs / obs-frontend-api / Qt6)` is used instead, for
builds against a full OBS build tree (CI, other platforms).

### Layout

```
src/plugin.cpp               module entry
src/core/                    config (mixes.json), manager (runtime, frontend sync, hotkeys),
                             mix-canvas (private canvas), recording (encoders + muxer), visibility (item masks)
src/render/                  root-source (canvas channel source), mix-renderer (scene walk), geometry
src/ui/                      dock, settings dialog, encoder properties form, live preview
tests/smoke.cpp              end-to-end render check on real libobs (CTest)
cmake/obs-sdk.cmake          SDK setup against the installed OBS
```

## Known limitations (v0.1)

- Filters on a **scene or group** that contains hidden items are not applied in that mix. Filters on regular
  sources are fine.
- Item show/hide transitions are not animated in a mix that re-renders that item's scene.
- A program mix's canvas size always equals the OBS base canvas. The output resolution is configurable.
- A Source mix takes its on-screen size when recording starts. If the placement changes size during the recording,
  the picture is letterboxed into the original size. Crops on a *parent* nested scene are not applied.
- Auto frame rate can only use rates a source reports (DirectShow custom fps, browser custom fps). Phone cameras,
  device-default webcams and media files fall back to OBS fps; pick a fixed rate for them.
- Source mixes record the selected mixer tracks (all audio), not only the source's own audio.
- Preview requires Windows or macOS.
- Mix definitions are global (not per profile). Visibility is per scene item and lives in the scene collection.
