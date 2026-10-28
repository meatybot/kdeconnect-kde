# Releasing this fork

This fork publishes its own releases on GitHub, separately from the KDE release service. Pushing a tag that starts with `v` runs `.github/workflows/release.yml`, which does three things in order:

1. Builds the tagged commit on Arch Linux and runs the full ctest suite. A failing test stops the release.
2. Builds a Flatpak bundle from `packaging/flatpak/org.kde.kdeconnect.json` on the KDE 6.10 runtime.
3. Creates the GitHub release with generated notes and attaches the bundle, a source tarball made by `git archive`, and `SHA256SUMS`.

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

3. Watch the run until all three jobs succeed:

   ```sh
   gh run list -R meatybot/kdeconnect-kde -w Release -L 1
   gh run watch <run-id> -R meatybot/kdeconnect-kde --exit-status
   ```

4. Check the published assets:

   ```sh
   gh release download v26.11.70-fork.2 -R meatybot/kdeconnect-kde -D /tmp/rel
   cd /tmp/rel && sha256sum -c SHA256SUMS
   ```

The run takes about 25 minutes, most of it the Flatpak build.

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

## Installing a release

```sh
flatpak install --user --bundle kdeconnect-<tag>-x86_64.flatpak
flatpak run org.kde.kdeconnect
```

The bundle uses the same app id as the official KDE Connect Flatpak, `org.kde.kdeconnect`. Uninstall any other KDE Connect build first, including the distribution package, because two `kdeconnectd` instances on one session conflict.
