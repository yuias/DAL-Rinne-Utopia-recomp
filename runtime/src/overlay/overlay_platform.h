#pragma once

// What the debug overlay needs from the window system: the display scale, the platform ImGui backend and
// the translation of window messages. Implemented per platform (overlay_platform_win32.cpp,
// overlay_platform_macos.mm). `native_window` is the HWND on Windows and the content NSView on macOS.
// ImGui keeps its current context in a process global, so the calls that touch a context take it explicitly
// and use it under imguiMutex() (imgui_context.h).

#include <optional>

#include "render/vk_window.h"

struct ImGuiContext;

namespace dal::overlay_platform {

// Pixels per layout unit of the window: the DPI scale on Windows, the backing scale on macOS. 1 when
// `native_window` is null. Renderer's thread.
float windowScale(void* native_window);

// False: ImGui coordinates are pixels (Windows). True: they are points and the framebuffer is `scale`
// times larger (macOS).
bool usesPointCoordinates();

// Starts or stops the platform backend of `context` for the window. The implementation makes `context`
// current under imguiMutex() on the thread that runs the backend (the main thread on macOS, reached with
// dispatch_sync), so the caller must not hold imguiMutex(): the main thread takes it while it dispatches
// window events. Renderer's thread.
bool initBackend(void* native_window, ImGuiContext* context);
void shutdownBackend(ImGuiContext* context);

std::optional<KeyMessage> keyOf(const WindowMessage& message);
bool isFocusLoss(const WindowMessage& message);
// A key press, which is withheld from the pad while ImGui wants the keyboard.
bool isKeyDown(const WindowMessage& message);

// Window thread, with the overlay's ImGuiContextScope open. Feeds the message to the backend and sets `route.handled`
// and `route.result` when the backend consumed it.
void forwardMessage(const WindowMessage& message, MessageRoute& route);

}  // namespace dal::overlay_platform
