# Roadmap

Development is phased by impact and risk. Each phase is independently shippable.

## Phase 0 — Foundation (P0)

> Lowest risk, highest enabling value. Everything else builds on the window
> abstraction.

### Native Window Awareness (`windows`)

- [ ] Cross-platform `window_query.h` interface (`Rect`, `SystemWindow`)
- [ ] macOS: `CGWindowListCopyWindowInfo` — `frontmost`, `list`, `find`, `atPoint`
- [ ] Windows: `EnumWindows` + `GetWindowRect` + `GetForegroundWindow`
- [ ] Frontmost-window app identity: bundle id / exe path + display name + title
- [ ] TS wrapper: main-process guard, promise normalization, typed results
- [ ] Prebuilt binaries for `darwin-arm64`, `darwin-x64`, `win32-x64`

**Deliverable**: `windows` module fully usable on both platforms.

---

## Phase 1 — Headline Feature (P0)

> The floating overlay system — the capability that Electron genuinely cannot
> do (app-scoped windows vanish on Space switch; no always-on-top-across-everything).

### Overlay System (`overlay`)

- [ ] Cross-platform `overlay_manager.h` interface (Host, Presentation, Session,
      Stack, Anchor)
- [ ] macOS:
  - [ ] `NSWindow` subclass — floating level, all-Spaces, stationary, clear bg
  - [ ] Stack layout engine (cascade + expand) with edge anchoring
  - [ ] Frame rendering from `data:image/...;base64` payloads (N-API path)
  - [ ] Control panel — hide / relocate buttons, SF-Symbol icons, tooltips,
        luminance-adaptive contrast
  - [ ] Session lifecycle: attach → visible → complete → removed
  - [ ] Suppress / activate / invalidate session operations
  - [ ] Events: `maxSizeChanged`, `activate`, `visibilityRequest`
- [ ] Windows:
  - [ ] Layered `HWND` (`WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST`)
  - [ ] `DwmExtendFrameIntoClientArea` for translucent edges
  - [ ] Message-pump thread + `ThreadSafeFunction` for JS callbacks
  - [ ] Stack layout + anchoring (parity with macOS)
  - [ ] Frame rendering (GDI / Direct2D from base64)
- [ ] Cross-platform TS API parity verification

**Deliverable**: `overlay` module on both platforms, full API parity.

---

## Phase 2 — Secure IPC (P1)

> Isolated, verified inter-process communication for sensitive work (automation,
  credential handling, untrusted code). Decoupled from but composable with the
  overlay.

### Secure Channel (`secureChannel`)

- [ ] Cross-platform `secure_channel.h` interface
- [ ] macOS:
  - [ ] `posix_spawn` for child process
  - [ ] `NSXPCConnection` host + worker protocols
  - [ ] Audit-token verification — `verify(pid, path)`
  - [ ] `wasTerminatedByPrivacy()` via TCC / privacy termination signal
  - [ ] Frame streaming: worker frames → `overlay.pushImage` bridge
- [ ] Windows:
  - [ ] `CreateProcess` with restricted token
  - [ ] Named pipe (verified `GetNamedPipeClientProcessId` + full-path check)
  - [ ] Privacy-termination detection (job-object / restricted-token exit reason)
  - [ ] Frame streaming bridge to overlay
- [ ] Events: `data`, `exit`

**Deliverable**: sandboxed worker that streams verified frames into an overlay.

---

## Phase 3 — Polish Features (P2)

> Convenience capabilities that round out the "doesn't feel like a web app"
  experience.

### App Icons (`apps`)

- [ ] macOS: `NSWorkspace.shared.icon(forFile:)` → PNG data URL
- [ ] Windows: `SHGetFileInfo` / `ExtractIconEx` → PNG data URL
- [ ] Size variants: `small`, `medium`

### File Drag-Out (`drag`)

- [ ] macOS: `NSDraggingSource` protocol implementation
- [ ] Windows: OLE `IDropSource` + `IDataObject` (CF_HDROP)
- [ ] `ended` event with drop result + coordinates
- [ ] Cross-platform `DragConfig` parity

**Deliverable**: `apps` + `drag` modules shipped.

---

## Phase 4 — Animation Toolkit (Deferred)

> Deferred **unless** a comprehensive, reusable physics system can be delivered.
  A partial animation API that only covers one or two cases is actively harmful:
  it fragments expectations and sets a low ceiling.

Criteria to green-light:

- [ ] Spring physics model (damping, stiffness, mass, initial velocity)
- [ ] Display-link / message-pump driven 60fps on both platforms
- [ ] Supports all interactive modes simultaneously:
  - [ ] drag (follow finger with spring lag)
  - [ ] resize (spring-scaled bounds)
  - [ ] programmatic move (animated re-anchor)
- [ ] Reusable across modules (overlay, future window-effects)
- [ ] Cross-platform parity

If only a subset is feasible, this phase stays deferred and overlays use
immediate (non-physical) transitions instead.

---

## Release cadence

| Milestone | Modules | Target |
|---|---|---|
| `0.1.0` | `windows` | Phase 0 |
| `0.2.0` | `windows` + `overlay` | Phase 1 |
| `0.3.0` | + `secureChannel` | Phase 2 |
| `0.4.0` | + `apps` + `drag` | Phase 3 |
| `1.0.0` | API-stable, full parity | Phase 3 complete |
| `1.x+` | `animation` (if criteria met) | Phase 4 |

Each milestone ships prebuilt binaries for all three targets
(`darwin-arm64`, `darwin-x64`, `win32-x64`) and is published to npm.
