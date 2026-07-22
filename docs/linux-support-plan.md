# Linux support plan

## 1. Decision

The first Linux implementation targets X11, including Electron applications
running through XWayland with `--ozone-platform=x11`. The public TypeScript API
will remain unchanged.

This supports the mainstream GNOME, KDE Plasma, Cinnamon, Xfce, and MATE
desktop environments when they expose an X11-compatible session. Native
Wayland sessions are explicitly outside the first implementation because a
regular Wayland client cannot inspect other clients' surfaces or their global
coordinates, and compositors do not allow arbitrary top-level window
positioning. Electron documents the same constraint and recommends XWayland
when an application needs programmatic positioning.

The implementation will not add compositor-specific GNOME Shell extensions,
KWin scripts, or private D-Bus APIs. Those approaches would create different
installation and security contracts for each desktop and would not preserve a
single package API.

Primary references:

- [Electron `BaseWindow.getNativeWindowHandle()`](https://www.electronjs.org/docs/latest/api/base-window#wingetnativewindowhandle) defines the Linux handle as an X11 `Window`.
- [Electron Wayland platform notices](https://www.electronjs.org/docs/latest/api/browser-window#platform-notices) document the global-coordinate and positioning limits and the XWayland fallback.
- [Wayland's protocol model](https://wayland.freedesktop.org/docs/book/Protocol.html) states that clients do not know surface global positions and cannot access other clients' surfaces.
- [EWMH 1.5](https://specifications.freedesktop.org/wm/latest-single/) standardizes active-window and stacking-order properties for X11 window managers.
- [Desktop Entry 1.5](https://specifications.freedesktop.org/desktop-entry/latest-single/) and the [Icon Theme specification](https://specifications.freedesktop.org/icon-theme/latest/) define cross-desktop application icon discovery.

## 2. Native architecture

Linux will use the existing shared managers and add one symmetric platform tail
per module.

```text
nativekit.node
  ├── shared Node-API managers
  └── Linux tail
      ├── windows: XCB + EWMH
      ├── overlay: GTK 3 + GDK X11
      └── apps: GIO desktop entries + GTK icon themes + GdkPixbuf
```

### Window queries

Use XCB instead of Xlib so stale or invalid cross-process window identifiers
produce request errors rather than invoking Xlib's process-global error
handler.

- Read `_NET_CLIENT_LIST_STACKING` and reverse its bottom-to-top order.
- Fall back to the root window tree when the window manager does not publish
  EWMH stacking data.
- Read `_NET_ACTIVE_WINDOW`, `_NET_WM_NAME`, `_NET_WM_PID`, and
  `_NET_WM_STATE_HIDDEN` when available.
- Fall back to ICCCM `WM_NAME` and `/proc/<pid>` metadata.
- Keep `relativeTo`, `belowId`, and public front-to-back `level` semantics
  identical to macOS and Windows.
- Treat an unavailable X11 display as an explicit runtime error rather than
  returning fabricated window data.

### Application icons

Resolve `.desktop` entries and executable matches through GIO, then use the
active GTK icon theme. Fall back to the file's GIO icon for paths without a
matching desktop entry. Rasterize through GdkPixbuf to the existing exact
16×16 or 32×32 PNG data URL contract.

Linux `appPath` will accept an absolute `.desktop`, executable, AppImage, or
other file path. A generic file icon is an acceptable fallback when no desktop
entry identifies the application.

### Overlay

Create one undecorated GTK top-level window per presentation on the Electron
main thread. Each window will:

- stay above normal windows without accepting keyboard focus;
- be hidden from taskbar and pager lists and request all-workspace visibility;
- preserve image aspect ratio and enforce the existing decode limits;
- render the optional application icon and hide/relocate controls;
- support double-click activation and window-manager-assisted dragging;
- retain and clamp manual placement to the selected monitor work area; and
- use the Electron host X11 handle for monitor and transient-parent hints.

GTK 3 is preferred over a new rendering or binding framework because Electron
Linux already integrates a GLib event source, GTK provides the required X11
window behavior, and GdkPixbuf covers PNG/JPEG decode and PNG encode without a
second image stack.

## 3. Build and package changes

- Add Linux sources symmetrically to CMake and `binding.gyp`.
- Discover `gtk+-3.0`, `gio-unix-2.0`, and `xcb` through `pkg-config`.
- Add `linux` to the npm OS allowlist and add a `build:linux` script.
- Produce build artifacts named `linux-x64` and `linux-arm64`.
- Document source-build packages for Debian/Ubuntu and equivalent development
  packages for other distributions.

The Linux native addon will dynamically depend on GTK 3, GLib/GIO, GdkPixbuf,
and XCB. These libraries are standard in the supported desktop sessions, but
minimal/headless installations must install them explicitly.

## 4. Build-only CI

Extend `.github/workflows/build.yml`; do not change `release.yml` in this work.
The Linux jobs will use GitHub-hosted Ubuntu 22.04 x64 and arm64 runners. Each
job will:

1. install GTK 3/XCB development packages, Xvfb, Openbox, and X11 test tools;
2. build the native addon and JavaScript package;
3. start Xvfb, an EWMH window manager, and a fixture X11 client;
4. run the current native integration suite;
5. run the Electron demo smoke flow to exercise window enumeration, icon
   extraction, image overlay rendering, and cleanup;
6. verify the npm package contents; and
7. upload a build artifact without publishing it.

The feature branch will be pushed and the build workflow manually dispatched
against that ref. Every Linux matrix job must pass before the pull request is
opened. CI failures will be fixed in focused follow-up commits and the workflow
will be dispatched again.

## 5. Acceptance criteria

- CMake and node-gyp source lists remain symmetric across all three platforms.
- Linux x64 and arm64 addons compile on Ubuntu 22.04.
- The native integration suite passes under Xvfb and Openbox.
- The Electron smoke demo exits successfully under the same X11 session.
- The package dry run contains the expected Linux-compatible sources and
  metadata.
- Existing macOS and Windows build jobs remain unchanged and build successfully.
- README, architecture, API reference, and integration documentation describe
  Linux dependencies, X11 support, and native Wayland limitations.

## 6. Deferred work

- Native Wayland window enumeration, hit testing, and absolute overlay
  placement remain unsupported until a stable cross-compositor protocol exists.
- Compositor-specific integrations may be proposed later as optional adapters,
  but they must not silently change the shared API contract.
- Linux prebuild publication remains disabled until runtime validation is done
  on real GNOME and KDE machines in addition to the build-only CI coverage.
- Sandboxed Flatpak/Snap applications may require packaged GTK/X11 libraries
  and filesystem permissions for desktop-entry or executable icon lookup.
