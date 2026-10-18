# Changelog

## 1.0.0

- Replay buffer per mix: save from the dock, its own hotkey, obs-websocket, or together with the OBS replay buffer.
  Shares the recording's encoder.
- Stream per mix to a second destination (RTMP, RTMPS, SRT, RIST) with automatic reconnect and live bitrate.
- Every output starts with the OBS recording, stream or replay buffer, or manually.
- Source mixes can record only the source's own audio (volume, mute and sync offset applied).
- Filters on scenes and groups that contain hidden items now apply in the mix.
- Show/hide transitions of scene items play in mixes that re-render their scene.
- obs-websocket vendor `video-mixes`: list mixes, start/stop/toggle outputs, save replays, get/set item visibility,
  output and replay events.
- New dock: a card per mix with live status per output, preview, settings and a menu (start/stop, open folder,
  duplicate, delete); eye toggles per program mix in the visibility tree.
- New settings dialog with General, Recording, Audio, Replay Buffer and Stream pages. New mixes are only created when
  saved.
- Duplicate a mix, including its hidden items.
- Russian translation.
- Windows installer and CI builds.

## 0.2.0

- Source mixes: one source at its on-screen size.
- New mixes copy the OBS recording settings; GPU encoders are chosen by capability.
- Per-mix frame rate.

## 0.1.0

- Program mixes with per-item visibility, recording with OBS, live preview.
