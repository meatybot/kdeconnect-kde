# Releasing this fork

This fork publishes its own releases on GitHub, separately from the KDE release service. Pushing a tag that starts with `v` runs `.github/workflows/release.yml`, which does three things in order:

1. Builds the tagged commit on Arch Linux and runs the full ctest suite. A failing test stops the release.
2. Builds the packages in parallel:
   - a Linux Flatpak bundle from `packaging/flatpak/org.kde.kdeconnect.json` on the KDE 6.10 runtime
   - a Windows x86_64 NSIS installer (`.exe`) and portable `.7z` archive, built with KDE Craft
   - a macOS arm64 `.dmg`, built with KDE Craft
3. Creates the GitHub release with generated notes and attaches the packages, a source tarball made by `git archive`, and `SHA256SUMS`.

The Android app lives in the separate `kdeconnect-android` repository and is released from there with its own tag. See `RELEASING.md` in that repository.

## Version and tag

The version comes from `RELEASE_SERVICE_VERSION` in the top-level `CMakeLists.txt`. Tags use that version with a `-fork.N` suffix, so fork releases never collide with upstream tags. N starts at 1 and increases for each release of the same upstream version.

```sh
grep -m3 'RELEASE_SERVICE_VERSION_' CMakeLists.txt
```

## Making a release

1. Make sure `master` is green in the CI workflow on GitHub.
2. Tag the commit and push the tag:

   ```sh
   git checkout master && git pull
   git tag -a v26.11.70-fork.2 -m "Short summary of the release"
   git push origin v26.11.70-fork.2
   ```

3. Watch the run until all jobs succeed:

   ```sh
   gh run list -R meatybot/kdeconnect-kde -w Release -L 1
   gh run watch <run-id> -R meatybot/kdeconnect-kde --exit-status
   ```

4. Check the published assets:

   ```sh
   gh release download v26.11.70-fork.2 -R meatybot/kdeconnect-kde -D /tmp/rel
   cd /tmp/rel && sha256sum -c SHA256SUMS
   ```

The run takes about 30 minutes. The Windows Craft build is the longest job at about 20 minutes, after the 6 minute test job.

## Re-running a failed release

If the release job fails after the tag is pushed, fix the problem on `master` and choose one of two options:

- Rebuild the existing tag without moving it. Run the Release workflow from the Actions tab with **Run workflow** and enter the tag, or use `gh workflow run Release -R meatybot/kdeconnect-kde -f tag=<tag>`. The publish step reuses an existing release and replaces its assets.
- Move the tag to the fixed commit with `git tag -f -a <tag> -m "..."` and `git push -f origin <tag>`. Only do this when nobody has downloaded the release yet. Otherwise, tag the next `-fork.N` instead.

## Building the Flatpak locally

flatpak-builder needs the KDE SDK and runtime 6.10 from Flathub:

```sh
flatpak install flathub org.kde.Sdk//6.10 org.kde.Platform//6.10
flatpak-builder --force-clean --repo=repo build packaging/flatpak/org.kde.kdeconnect.json
flatpak build-bundle repo kdeconnect.flatpak org.kde.kdeconnect
flatpak install --user --bundle kdeconnect.flatpak
```

The manifest builds the current checkout (`"path": "../.."`) plus the libraries the KDE runtime does not ship: libfakekey, libevdev, libei, plasma-wayland-protocols, pulseaudio-qt and kpeople. Each one is pinned by tag and commit. When you bump the runtime version, bump kpeople to the KDE Frameworks version that the new SDK ships.

The bundle has full session D-Bus access, like a native install, because notification forwarding, MPRIS control and call handling talk to arbitrary services on the session bus.

## Windows and macOS builds

`.github/workflows/craft.yml` builds both platforms with [KDE Craft](https://community.kde.org/Craft), the same tool the KDE CI uses for its `craft-windows-x86-64-qt6` and `craft-macos-arm64-qt6` jobs. CI runs it on every push, and the release workflow calls it for the tag. Qt, KDE Frameworks and the other dependencies come prebuilt from KDE's binary cache at `files.kde.org/craft/Qt6`, so only kdeconnect itself is compiled. The build recipe is the `kde/applications/kdeconnect-kde` blueprint from KDE's craft-blueprints-kde repository, combined with `.craft.ini` of this repository.

The CraftMaster configuration is `.github/craft/CraftConfig.ini`. It is a copy of KDE's `craft-ci/qt6/CraftConfig.ini` with the paths changed for GitHub-hosted runners and code signing turned off.

The Craft blueprint builds without PulseAudio, so neither package contains the audio streaming plugin, which needs PulseAudio. The Windows package also lacks the `kdeconnect-relayserver` binary, because the blueprint's package filter only keeps a fixed list of executables. The macOS package ships it. Connecting through a relay works on both platforms, because the relay link provider is part of `kdeconnectcore`.

## Installing on Windows and macOS

Neither package is code signed, because the KDE signing services are only reachable from KDE's own CI.

- Windows: run `kdeconnect-kde-<tag>-windows-cl-msvc2022-x86_64.exe`. SmartScreen warns about an unknown publisher. Choose **More info**, then **Run anyway**. The `.7z` archive is a portable alternative that needs no installation.
- macOS (Apple silicon only): open the `.dmg` and drag KDE Connect into Applications. Gatekeeper blocks unsigned apps on first launch. Allow it in **System Settings > Privacy & Security**, or run `xattr -dr com.apple.quarantine "/Applications/KDE Connect.app"`.

## Installing a release

```sh
flatpak install --user --bundle kdeconnect-<tag>-x86_64.flatpak
flatpak run org.kde.kdeconnect
```

The bundle uses the app id `org.kde.kdeconnect`. Uninstall any other KDE Connect build first, including the distribution package, because two `kdeconnectd` instances on one session conflict.
