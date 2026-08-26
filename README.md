# Video Cut + Compress

Simple Windows desktop app to cut, preview, and compress videos with FFmpeg through a clean Qt interface.

<p align="center">
  <img src="docs/images/app-preview.png" alt="Video Cut and Compress preview" width="1000">
</p>

## Install

1. Download [`VideoCutCompress-Setup.exe`](https://github.com/SuicidaI-Idol/videocutandcompress/raw/main/dist/VideoCutCompress-Setup.exe).
2. Run the installer.
3. Launch **Video Cut and Compress** from the Start menu or desktop shortcut.

## Features

- Video loading with the file picker or drag and drop, including directly on
  the video area
- YouTube URL input with an asynchronous temporary download and progress
  display
- Video playback with seek bar and relative skip controls
- Automatic low-latency audio path for normal single-track playback
- Multiple audio-track selection, per-track volume, software amplification,
  and track mixing
- Interactive rectangular crop selection
- Live preview that displays only the selected crop and scales it smoothly to
  the available preview area
- Crop dimensions and position displayed in source-video pixels
- `Reset crop` action to restore the complete frame
- `Start` and `End` trim markers displayed on the timeline
- Toggleable preview mode for virtual segment playback
- Output folder shortcut after a successful export
- Three export modes:

  - `Max size`: compress to a target file size in MB
  - `Preset (editable)`: use customizable bitrate and width settings
  - `Input (cut only)`: cut the selected segment without recompression; the
    video is re-encoded only when a crop or an exact non-zero start cut is
    required

### Cropping a video

1. Load a video.
2. Select `Select crop`.
3. Drag a rectangle over the part of the full frame to keep.
4. Release the mouse button. The preview then displays only that area.
5. Select `Select crop` again to replace the selection, or `Reset crop` to
   restore the full frame.

The crop uses the source-video coordinates shown beside the crop controls and
is applied to every export mode through FFmpeg.

### Loading a YouTube video

1. Paste a YouTube video URL into `YouTube URL (temporary download)`.
2. Select `Download` or press Enter.
3. Wait for the progress indicator to finish. The downloaded video is loaded
   automatically and can then be trimmed, cropped, and compressed normally.

Only one video is downloaded (`--no-playlist`). The source file is stored in a
temporary directory and removed when another source replaces it or when the
application closes. Exports made from a YouTube source are saved in the user's
Videos directory rather than in the temporary directory.

The downloader selects the highest-quality video stream and the highest-quality
audio stream exposed by YouTube, without restricting them to MP4 codecs. The
streams are merged into a temporary MKV file so VP9, AV1, high-frame-rate, and
HDR variants can be retained when available. Consequently, 4K or 8K sources can
require significant download time and temporary disk space.

### Audio playback and mixing

The preview automatically chooses the most appropriate audio path:

- With one selected track at a normal listening level, audio is sent directly
  through Qt's native output. This keeps it synchronized with the video and
  avoids unnecessary buffering.
- A very small slider overshoot above 100% is kept on the native path and
  clamped to the normal maximum, preventing accidental activation of the
  higher-latency amplifier.
- For actual amplification above the native range, the software gain processor
  remains available.
- When several tracks are selected, the software path remains available to
  apply their individual volume settings and mix them together.

This automatic optimization does not remove any previous functionality. Audio
track selection, individual volume controls, amplification, multi-track mixing,
and audio processing during export are all preserved.

## Requirements

- Windows
- FFmpeg is bundled in the installer
- No manual Qt DLL setup required after installation

## Development and complete installer build

The VS Code configuration uses environment variables instead of machine-specific
Qt paths. Define them for your Qt installation, then restart VS Code:

```powershell
setx QT_SDK_DIR "C:\Qt\6.11.1\mingw_64"
setx QT_MINGW_DIR "C:\Qt\Tools\mingw1310_64"
setx QT_CMAKE_DIR "C:\Qt\Tools\CMake_64"
setx QT_NINJA_DIR "C:\Qt\Tools\Ninja"
```

Qt 6.8 or newer is required with Widgets, Multimedia, and MultimediaWidgets.
The release workflow also requires:

- `ffmpeg.exe` and `ffprobe.exe` in `.cache/ffmpeg/bin`
- Inno Setup 6 installed in one of the standard locations detected by
  `scripts/build-installer.ps1`

`yt-dlp.exe` is downloaded from the official yt-dlp GitHub release into
`.cache/yt-dlp` the first time the Release folder is prepared, then bundled
beside the application executable.

To build and debug the application, press `F5` in VS Code.

To rebuild the application, deploy all Qt and MinGW runtime dependencies, copy
FFmpeg, and generate the complete installer:

1. Open **Terminal > Run Task...** in VS Code.
2. Run **Build Installer (Release)**.
3. Wait for every dependent task to finish.

This task performs the complete sequence:

1. Removes the previous `build-release` directory.
2. Configures and compiles a fresh Release build with CMake and Ninja.
3. Runs `windeployqt` to copy the required Qt and compiler runtime files.
4. Copies `ffmpeg.exe`, `ffprobe.exe`, and `yt-dlp.exe` into the release
   directory (downloading yt-dlp first if it is not cached).
5. Removes the CMake build metadata that should not be distributed.
6. Invokes Inno Setup to create `dist/VideoCutCompress-Setup.exe`.

The resulting standalone application is `build-release/videocut.exe`, and the
installer to distribute is `dist/VideoCutCompress-Setup.exe`.

## Output

Exported files are created next to the original video with an auto-generated name based on:

- source file name
- start / end time
- selected export mode
