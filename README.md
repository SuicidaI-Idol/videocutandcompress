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

- Video playback with seek bar and relative skip controls
- `Start` and `End` trim markers displayed on the timeline
- Toggleable preview mode for virtual segment playback
- Output folder shortcut after a successful export
- Three export modes:
- `Max size`: compress to a target file size in MB
- `Preset (editable)`: use customizable bitrate and width settings
- `Input (cut only)`: cut the selected segment without recompression

## Requirements

- Windows
- FFmpeg is bundled in the installer
- No manual Qt DLL setup required after installation

## Development

The VS Code configuration uses environment variables instead of machine-specific
Qt paths. Define them for your Qt installation, then restart VS Code:

```powershell
setx QT_SDK_DIR "C:\Qt\6.11.1\mingw_64"
setx QT_MINGW_DIR "C:\Qt\Tools\mingw1310_64"
setx QT_CMAKE_DIR "C:\Qt\Tools\CMake_64"
setx QT_NINJA_DIR "C:\Qt\Tools\Ninja"
```

Qt 6.8 or newer is required with Widgets, Multimedia, and MultimediaWidgets.
Press `F5` to build and debug, or run the `Build Installer (Release)` VS Code
task to create the Windows installer.

## Output

Exported files are created next to the original video with an auto-generated name based on:

- source file name
- start / end time
- selected export mode
