# Gleem for OBS

An OBS Studio plugin that adds a **Gleem IRL Sidekick** source: sign in to Gleem once, pick your IRL Sidekick from a list, and its stream plays in your scene. No URL to copy from the dashboard.

## Features

- **Gleem IRL Sidekick source.** It behaves like a Media Source, and decoding is done by OBS's own Media Source. Audio filters, monitoring, the mixer and sync offset work as usual.
- **Sign in with Gleem.** Uses the OAuth device flow: the plugin opens `gleem.gg/activate`, you approve, and that is all. Tokens refresh on their own.
- **Waits for your stream.** The source only connects while ingest has your stream. When you go live again it picks the stream back up within a few seconds, with nothing to click.
- **Your stream URL stays private.**
  - The pull URL holds a secret ticket, so the plugin keeps it in memory only. It is never written to the scene collection.
  - The ticket is replaced with `[redacted]` in the OBS log file.
  - If you ask for a new URL in the dashboard, the source fetches it by itself.
- **Rented Gleem OBS machines** sign in automatically through the token the machine provides.

## Installation

Download the build for your platform from the [releases](https://github.com/gleem-gg/obs-gleem/releases):

| Platform | File |
| --- | --- |
| Windows | `obs-gleem-*-windows-x64.exe` (installer) or `.zip` |
| macOS | `obs-gleem-*-macos-universal.pkg` |
| Ubuntu / Debian | `obs-gleem-*-x86_64-linux-gnu.deb` |
| Flatpak OBS | build it with `./flatpak/build.sh` (see below) |

It needs OBS Studio 31 or later. Restart OBS after installing.

On Windows, the installer puts the plugin in `C:\ProgramData\obs-studio\plugins\obs-gleem`. To use the `.zip` instead, extract it to `C:\ProgramData\obs-studio\plugins`; for a portable OBS, extract it to the `plugins` folder of that installation (OBS 33 or later).

## Usage

1. In a scene, add a source with **+** and choose **Gleem IRL Sidekick**.
2. Click **Sign in with Gleem**. Your browser opens gleem.gg; check that it shows the same code, then approve.
3. Pick your IRL Sidekick from the list and click **OK**.

The source's properties show its current status, such as "Waiting for the IRL Sidekick to go live". You can also open **Tools → Gleem Account** to see who is signed in or to sign out. Signing out revokes the sign-in on gleem.gg.

| Setting | What it does |
| --- | --- |
| IRL Sidekick | The device to show. Offline ones are marked. |
| Use hardware decoding when available | Passed on to OBS's media decoder. |
| Disconnect when not visible | Only pull the stream while the source is shown in program or preview. Off by default, so switching to the scene is instant. |

### Rented Gleem OBS machines

When OBS gets a token in `GLEEM_API_TOKEN` (and optionally `GLEEM_API_URL`), the plugin uses it and hides sign-in. That token needs the `irl:read` scope.

## Development

The build is based on the [OBS plugin template](https://github.com/obsproject/obs-plugintemplate). It uses libobs, obs-frontend-api, Qt 6 and libcurl.

### Linux, against the Flatpak OBS

This uses the OBS Flatpak's own SDK and installs into the sandbox's per-user plugin directory:

```bash
flatpak install flathub org.freedesktop.Sdk//25.08   # must match `flatpak info com.obsproject.Studio`
flatpak run --devel --filesystem=$PWD --command=bash com.obsproject.Studio -c '
  cmake -S . -B build-flatpak -G Ninja -DCMAKE_PREFIX_PATH=/app -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLINUX_PORTABLE=ON &&
  cmake --build build-flatpak &&
  cmake --install build-flatpak --component portable'
```

This installs to `~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/obs-gleem/`. Alternatively, `./flatpak/build.sh` builds and installs a proper Flatpak extension; it needs `flatpak-builder`.

### Windows, macOS, Ubuntu

Use the template's presets, e.g. `cmake --preset windows-x64` or `cmake --preset macos`, then build. CI does the same in `.github/workflows/build-project.yaml`.

## Releasing

1. Set the version in `buildspec.json`, then tag `vX.Y.Z` and push the tag. CI builds every platform and creates a draft release. The draft has no Windows files yet: the build CI keeps as an artifact is unsigned.
2. On the Windows machine with the Certum card plugged in, run `.\packaging\windows\release.ps1 vX.Y.Z` and enter the card's PIN when asked. It signs the DLL, the installer and its uninstaller, verifies the DLL's and the installer's signatures, and attaches the installer and zip to the draft. It needs [gh](https://cli.github.com), `signtool` from the Windows SDK, [Inno Setup](https://jrsoftware.org/isinfo.php) and proCertumCardManager with the card's certificate registered. `-DryRun` signs without uploading.
3. Check the draft and publish it.

CI runs on these triggers:
- `v*` tags build every platform and create a draft release;
- pull requests build when opened;
- the **Dispatch** workflow builds on demand.

### Against a local gleem.gg

| Variable | Purpose |
| --- | --- |
| `GLEEM_API_URL` | Base URL, e.g. `http://127.0.0.1:8000`. Tokens are kept per base URL. |
| `GLEEM_OAUTH_CLIENT_ID` | The `gla_…` id of a public app with the device flow enabled on that instance. The default is the production "Gleem for OBS" app, set at build time with `-DGLEEM_OAUTH_CLIENT_ID=`. |

To run a second OBS with its own settings next to your usual one, set the variable inside the sandbox; `flatpak run --env=` does not override it:

```bash
flatpak run --command=sh com.obsproject.Studio -c 'export XDG_CONFIG_HOME=/tmp/obs-test; exec obs --multi'
```

### How it works

| File | Role |
| --- | --- |
| `src/sidekick-source.cpp` | The source. It wraps a private `ffmpeg_source`, renders its video, and passes its audio on through `obs_source_output_audio`, so filters apply. A worker thread connects only while gleem.gg reports the device as `ingest.publishing`, and stops the child's reconnect attempts otherwise. |
| `src/gleem-account.cpp` | Sign-in, token refresh (the refresh token rotates on each use, so refreshes are serialised), and one shared poll of `GET /api/v1/irl/devices` every 5 seconds while any source exists. |
| `src/log-redact.cpp` | Wraps OBS's log handler and redacts play tickets. FFmpeg's own messages that bypass OBS's log, written straight to stderr, are not covered. |

The pull URL comes from `GET /api/v1/irl/devices/{uuid}/playback` (scope `irl:read`).

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
