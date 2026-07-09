# Story 001: Resolution change in Video settings has no effect

## Status
Investigated

## Story
**As a** player, **I want** the Resolution dropdown in Settings → Video to actually change the render/display resolution and stick across launches, **so that** I can run the game at a resolution that matches my monitor and performance target the way retail WoW 3.3.5a does.

## Context / Observed Behavior
Selecting a different entry in the **Resolution** dropdown (Settings → Video: 1280x720 / 1600x900 / 1920x1080 / 2560x1440 / 3840x2160) appears to do nothing. The rendered image does not change resolution, and any change that does take is lost on the next launch. Observed on macOS (the reporter's platform, Darwin).

## Expected Behavior
In retail WoW 3.3.5a the Resolution control changes the actual display/render resolution:
- In fullscreen it switches the display mode to the chosen resolution.
- In windowed mode it resizes the render surface to the chosen resolution.
- The choice is written to `Config.wtf` and re-applied automatically on the next launch.

## Acceptance Criteria
1. Changing the Resolution dropdown visibly changes the rendered resolution in **both** windowed and fullscreen modes.
2. The selected resolution survives an app restart (persisted and re-applied at startup).
3. On macOS/Retina, selecting a high resolution actually renders at that pixel count (not silently clamped to a smaller point-space size).
4. The 3D projection (camera aspect ratio) and UI layout stay correct after a resolution change (no stretching), including for any future non-16:9 option.

## Investigation (Dev Notes)

### Where it lives
- UI control: `src/ui/settings_panel.cpp:625` (`kResolutions[][2]` table) and `src/ui/settings_panel.cpp:940-943` — the `ImGui::Combo("Resolution", …)` handler calls `window->applyResolution(w, h)` then `saveCallback()`. Also the "Restore Video Defaults" button at `src/ui/settings_panel.cpp:974`.
- Apply path: `Window::applyResolution(int w, int h)` at `src/core/window.cpp:209-225`.
- Window creation flags: `src/core/window.cpp:101-107` (and `SDL_CreateWindow` at `:109`).
- Swapchain recreation (the part that *does* work): `Renderer::beginFrame()` at `src/rendering/renderer.cpp:863-883` → `VkContext::recreateSwapchain()` at `src/rendering/vk_context.cpp:1460`.
- Camera aspect-ratio update: only in the SDL event loop at `src/core/application.cpp:728-740` (guarded on `SDL_WINDOWEVENT_RESIZED`).
- Persistence: `GameScreen::saveSettings()` at `src/ui/game_screen_minimap.cpp:1408`; startup window size hardcoded at `src/core/application.cpp:121-122`.

### Root-cause hypothesis
There are two distinct defects that each produce "no effect," plus a persistence gap. The exact one the reporter hit depends on their display mode — the fix must cover both.

1. **Fullscreen no-op (primary code defect, HIGH confidence).** `applyResolution()` early-returns while fullscreen:
   ```cpp
   // src/core/window.cpp:212-216
   if (fullscreen) {
       windowedWidth = w;
       windowedHeight = h;
       return;              // <-- swapchain never touched
   }
   ```
   The window is created with `SDL_WINDOW_FULLSCREEN_DESKTOP` (`src/core/window.cpp:103`, `:179`), i.e. borderless fullscreen locked to the desktop resolution. In that mode the dropdown is a literal no-op — nothing is resized and the swapchain is never marked dirty. Since players commonly run fullscreen, this matches the report directly.

2. **Windowed macOS clamping (MEDIUM confidence, display-dependent).** The window is created **without** `SDL_WINDOW_ALLOW_HIGHDPI` (`src/core/window.cpp:101` flags = `SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN`, plus fullscreen/resizable). On a Retina Mac the SDL window and Vulkan surface live in *point space* (half the native pixel count). `applyResolution()` sets the window size via `SDL_SetWindowSize` in points, so large options (1600/1920/2560/3840 wide) exceed the logical screen and Cocoa constrains the window to roughly screen width — several dropdown options collapse to the same actual size and look like they do nothing. It also means the app never renders at true native pixel density.

3. **Persistence gap (contributes to "does nothing").** `saveSettings()` (`src/ui/game_screen_minimap.cpp:1408-1520`) writes UI/audio/gameplay/graphics keys but **no** resolution/fullscreen/vsync keys, and there is no load-path that re-applies a saved resolution. The window is hardcoded to 1280x720 at `src/core/application.cpp:121-122`. So even a successful windowed change is discarded on the next launch — "read once at startup, never re-applied."

### Evidence
- **Windowed apply actually works mechanically** (so the report is not a broken recreate path): `applyResolution()` windowed branch calls `SDL_SetWindowSize` and `vkContext->markSwapchainDirty()` (`src/core/window.cpp:217-224`). Next frame `Renderer::beginFrame()` sees `isSwapchainDirty()` and calls `recreateSwapchain(window->getWidth(), window->getHeight())` (`src/rendering/renderer.cpp:863-866`). vk-bootstrap `set_desired_extent` (`src/rendering/vk_context.cpp:1480`) is clamped to the surface's `currentExtent`, which follows the resized window; and with no upscaler active the scene renders at swapchain extent (`PostProcessPipeline::getSceneRenderExtent()` returns `getSwapchainExtent()` — `src/rendering/post_process_pipeline.cpp:142`). So when the OS window truly resizes, the render output truly resizes. That is precisely why the two failure surfaces are (fullscreen no-op) and (macOS point-space clamping), not a dead swapchain path.
- **Fullscreen early-return**: `src/core/window.cpp:212-216` (quoted above).
- **Missing HIGHDPI flag**: `src/core/window.cpp:101`. Contrast: initial swapchain is sized from `SDL_Vulkan_GetDrawableSize` (`src/rendering/vk_context.cpp:89`) while the resize path uses logical `window->getWidth()/getHeight()` — consistent only because HIGHDPI is off, which is itself the Retina problem.
- **No persistence**: `grep` for `resolution|fullscreen=|vsync=` in `src/ui/game_screen_minimap.cpp` returns nothing in the save block; `pendingFullscreen`/`pendingVsync` are re-read from live window state each time the panel opens (`src/ui/settings_panel.cpp:649-650`), never from disk.
- **Latent, currently invisible aspect-ratio bug**: `applyResolution()` never updates the camera aspect ratio; that only happens in the `SDL_WINDOWEVENT_RESIZED` handler (`src/core/application.cpp:739`). A *programmatic* `SDL_SetWindowSize` emits `SDL_WINDOWEVENT_SIZE_CHANGED`, not `SDL_WINDOWEVENT_RESIZED`, so that handler (and the `DISPLAY_SIZE_CHANGED` addon event at `:743`) never fires on a dropdown-driven change. This is currently harmless only because all five `kResolutions` entries are 16:9 (same aspect); it would distort the moment a non-16:9 option is added.

### Suggested approach
- **Fullscreen**: make resolution meaningful. Either (a) switch to real exclusive fullscreen (`SDL_WINDOW_FULLSCREEN`) and change the SDL display mode / swapchain extent to the chosen resolution, or (b) decouple "Resolution" from window size entirely and implement it as an **internal render-scale / offscreen target** resolution that is blitted/upscaled to the swapchain (this also gives a clean windowed story and reuses the existing FSR/scene-target plumbing in `post_process_pipeline.cpp`). Option (b) is closest to what players expect and avoids OS window-size clamping.
- **macOS**: add `SDL_WINDOW_ALLOW_HIGHDPI` at `src/core/window.cpp:101` and make the resize path consistently use `SDL_Vulkan_GetDrawableSize` (pixels) rather than logical `getWidth()/getHeight()`, so high resolutions render at true pixel density.
- **Persistence**: add `resolution_w`/`resolution_h` (and ideally `fullscreen`, `vsync`) to `saveSettings()` and a load-path that calls `applyResolution()` / `setFullscreen()` at startup instead of hardcoding 1280x720 (`src/core/application.cpp:121-122`).
- **Aspect/UI**: have `applyResolution()` update `camera->setAspectRatio(window->getAspectRatio())` and fire `DISPLAY_SIZE_CHANGED` directly, rather than relying on an SDL event that never arrives for programmatic resizes.
- **Unknowns/risks**: exclusive fullscreen mode-switching behaves differently across macOS/Windows/Linux and MoltenVK; the render-scale approach is more portable but touches post-process target sizing. Confirm against the reporter which mode (windowed vs fullscreen) and monitor they used to know which surface dominated.

### Effort / risk
Persistence + HIGHDPI + fullscreen early-return removal: **medium · medium**. A full render-scale/internal-resolution decoupling (the "correct" WoW-like behavior): **large · medium**.

## Tasks / Subtasks
- [ ] Decide model: OS display-mode switching vs internal render-scale target (recommend the latter for portability and to match player expectations).
- [ ] Fix the fullscreen no-op in `Window::applyResolution()` (`src/core/window.cpp:212-216`) so a resolution change takes effect while fullscreen.
- [ ] Add `SDL_WINDOW_ALLOW_HIGHDPI` (`src/core/window.cpp:101`) and make the swapchain-resize path use drawable (pixel) size consistently.
- [ ] Update `applyResolution()` to refresh camera aspect ratio and fire `DISPLAY_SIZE_CHANGED` (don't depend on `SDL_WINDOWEVENT_RESIZED`).
- [ ] Persist resolution (and fullscreen/vsync) in `GameScreen::saveSettings()` (`src/ui/game_screen_minimap.cpp:1408`).
- [ ] Add a startup load-path that applies the saved resolution instead of the hardcoded 1280x720 (`src/core/application.cpp:121-122`).
- [ ] (Optional) Guard against selecting a resolution larger than the current display; clamp or hide unsupported options.

## Testing
Against a live AzerothCore/TrinityCore 3.3.5a session:
1. **Windowed, non-Retina**: change each dropdown option; confirm the render surface changes size/resolution each time (add a temp log of `swapchainExtent` in `recreateSwapchain`).
2. **Windowed, macOS Retina**: confirm large options actually render at native pixels and are not silently clamped to ~screen width (check `SDL_Vulkan_GetDrawableSize` vs `SDL_GetWindowSize`).
3. **Fullscreen**: enable Fullscreen, then change resolution; confirm the rendered resolution changes (currently a no-op).
4. **Persistence**: set a non-default resolution, restart the app, confirm it comes back at that resolution rather than 1280x720.
5. **Aspect**: after each change confirm no horizontal/vertical stretching of the 3D scene and that UI/nameplates re-layout (validate the future non-16:9 case if such an option is added).

## Change Log
| Date | Change | Author |
|---|---|---|
| 2026-07-09 | Story created + investigated | agent team |

## Dev Agent Record
_(populated during implementation)_
