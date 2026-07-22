# AGENTS.md

Guide for AI agents (and humans) working on `nativekit`.

## Project

`nativekit` is a cross-platform N-API native addon (C++) that gives Electron
desktop apps OS-level capabilities: floating overlays, native window awareness,
and app icon extraction.

- **Platforms**: macOS (arm64, x64), Windows (x64)
- **Binding**: C++ via `node-addon-api` (N-API v8)
- **Native tails**: macOS uses Objective-C++ (`.mm`) calling AppKit / CoreGraphics;
  Windows uses C++ (`.cpp`) calling Win32 / Shell.
- **Build**: CMake + `cmake-js` (primary), `binding.gyp` / `node-gyp` (fallback)
- **Distribution**: prebuilt `.node` binaries via `prebuildify`, resolved at
  runtime by `node-gyp-build`. Published to npm.

## Repository layout

```
src/
  binding.cpp                 # N-API entry — registers all modules
  common/                     # shared C++ types & helpers
  overlay/                    # floating panel system
    mac/*.mm  win/*.cpp
  windows/                    # system window query
    mac/*.mm  win/*.cpp
  apps/                       # app icon extraction
    mac/*.mm  win/*.cpp
js/
  index.ts                    # TS wrapper: platform guard, validation, types
docs/
  architecture.md             # design, cross-platform strategy, layout
  api-reference.md            # full JS API
package.json
CMakeLists.txt
binding.gyp
```

## Conventions

### Code

- **Language**: C++17. Comments and identifiers in English.
- **N-API**: use `node-addon-api` C++ wrappers (`Napi::CallbackInfo`,
  `Napi::ObjectWrap`, `Napi::ThreadSafeFunction`). Never call JS from a non-main
  thread directly — always marshal via `ThreadSafeFunction`.
- **macOS**: Objective-C++ (`.mm`). Prefer AppKit / CoreGraphics. Use ARC unless
  integrating with a framework that forbids it.
- **Windows**: C++ with `windows.h`, DWM, Shell API, and WIC. Target Windows 10
  1809+.
- **Object lifetime**: `Napi::ObjectWrap` owns native handles; destructors
  release them. Never leak `NSWindow*` / `HWND`.
- **Error handling**: throw `Napi::Error` for JS-visible failures; never return
  garbage on partial failure. Fail fast on invalid input.

### Naming

- **C++**: `snake_case` for files, functions, variables; `PascalCase` for
  classes/structs.
- **JS/TS API**: camelCase methods, PascalCase types. Module names are
  lowercase single words (`overlay`, `windows`, `apps`).

### JS wrapper (`js/index.ts`)

- Enforce **main-process only** — throw if `process.type !== 'browser'` is
  detectable, or if `BrowserWindow` is absent.
- Validate args before crossing into native.
- Promisify all async native calls.

## Build & test

```bash
# install deps (builds from source if no prebuild found)
pnpm install

# build native addon
pnpm build              # = cmake-js build

# build for a specific arch (macOS)
pnpm build:mac          # arm64 + x64

# build for windows
pnpm build:win         # x64

# create prebuilt binaries
pnpm prebuild

# run whatever tests exist
pnpm test
```

Native build requires:
- **macOS**: Xcode CLT, Swift toolchain (comes with Xcode)
- **Windows**: Visual Studio Build Tools 2019+ (Desktop C++ workload)
- **Both**: Node.js 18+, CMake 3.22+

## CI / Release

GitHub Actions (`.github/workflows/`):
1. `build.yml` — matrix builds `.node` on `macos-14` (arm64, cross x64),
   `windows-2022` (x64). Uploads each as a workflow artifact.
2. `release.yml` — runs on `v*` tag pushes or manual dispatch with an existing
   tag, downloads all build artifacts, assembles
   `prebuilds/<platform>-<arch>/nativekit.node`, runs `prebuildify --napi`, and
   publishes to npm through Trusted Publishing.

See `docs/architecture.md` §9 for the distribution flow.

## What to do / not do

**Do**:
- Keep platform tails symmetrical: every method in a module's cross-platform
  interface must exist on both macOS and Windows.
- Add/expand docs when you add an API surface.
- Run the relevant platform build after native changes.
- Use `Napi::ThreadSafeFunction` for any callback originating off the main thread.

**Don't**:
- Don't introduce a third platform binding framework (no napi-rs, no Neon). The
  C++ + ObjC++/Win32 split is deliberate — see `docs/architecture.md` §2.
- Don't call AppKit / Win32 from non-UI threads. Marshal to the main thread.
- Don't add new top-level exports without updating `docs/api-reference.md`.
- Don't commit secrets, tokens, or `.env` files.
- Don't commit built `.node` binaries or `prebuilds/` — CI generates them.
- Don't use `git add -A`. Stage explicit paths.
