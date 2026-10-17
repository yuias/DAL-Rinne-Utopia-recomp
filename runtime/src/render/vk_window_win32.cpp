// Win32 window on a UI thread. Presenter::Window forwards to Impl, which owns the HWND and the message loop.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// WIN32_LEAN_AND_MEAN leaves the IME header out.
#include <imm.h>

#include "render/vk_window.h"

// Not through volk's platform switch: volk is built without VK_USE_PLATFORM_WIN32_KHR, so the surface
// entry point is fetched by name below. The header only adds types here (VK_NO_PROTOTYPES).
#include <vulkan/vulkan_win32.h>

#include <atomic>
#include <cstring>
#include <format>
#include <future>
#include <thread>

namespace dal {
namespace {

constexpr wchar_t kWindowClass[] = L"DalGameWindow";
constexpr wchar_t kWindowTitle[] = L"デート・ア・ライブ 凜祢ユートピア";

}  // namespace

std::optional<KeyMessage> keyMessageFromWin32(std::uint32_t message, std::uint64_t wparam, std::uint64_t lparam) {
    bool down = false;
    switch (message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: down = true; break;
        case WM_KEYUP:
        case WM_SYSKEYUP: break;
        default: return std::nullopt;
    }
    std::uint32_t scan_code = static_cast<std::uint32_t>((lparam >> 16) & 0xFF);
    if (scan_code == 0) {
        // Not every extended key comes back with 0xE0 (the arrows map to the numpad codes), so the
        // message's own extended bit decides below.
        scan_code = MapVirtualKeyW(static_cast<UINT>(wparam), MAPVK_VK_TO_VSC_EX) & 0xFF;
    }
    if (scan_code != 0 && (lparam & (1u << 24)) != 0) scan_code |= 0xE000u;
    return KeyMessage{scan_code, down, down && (lparam & (1u << 30)) != 0};
}

class Presenter::Window::Impl {
public:
    static std::unique_ptr<Impl> create(const PresenterOptions& options, std::string& error) {
        std::unique_ptr<Impl> window(new Impl(options));
        std::future<std::string> ready = window->ready_.get_future();
        window->thread_ = std::thread([w = window.get()] { w->run(); });
        error = ready.get();
        if (!error.empty()) {
            window->thread_.join();
            return nullptr;
        }
        return window;
    }

    ~Impl() {
        if (thread_.joinable()) {
            closing_.store(true);
            // DestroyWindow only works on the owning thread.
            if (hwnd_ != nullptr) PostMessageW(hwnd_, kDestroyMessage, 0, 0);
            thread_.join();
        }
    }

    HWND hwnd() const { return hwnd_; }
    bool minimized() const {
        return width_.load(std::memory_order_relaxed) == 0 || height_.load(std::memory_order_relaxed) == 0;
    }
    VkExtent2D clientSize() const {
        return {width_.load(std::memory_order_relaxed), height_.load(std::memory_order_relaxed)};
    }

private:
    static constexpr UINT kDestroyMessage = WM_APP;

    explicit Impl(const PresenterOptions& options)
        : on_close_(options.on_close),
          on_message_(options.on_message),
          on_key_(options.on_key),
          on_focus_lost_(options.on_focus_lost),
          client_width_(options.client_width),
          client_height_(options.client_height) {}

    void run() {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = &Impl::wndProc;
        cls.hInstance = instance;
        cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW, whose macro is the ANSI form here
        cls.lpszClassName = kWindowClass;
        if (RegisterClassExW(&cls) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            ready_.set_value(std::format("RegisterClassExW failed ({})", GetLastError()));
            return;
        }
        const DWORD style = WS_OVERLAPPEDWINDOW;
        // The frame is sized for the DPI of the monitor the window opens on, so the client area is
        // client_width_ x client_height_ physical pixels.
        const auto frameSize = [&](UINT dpi) {
            RECT frame = {0, 0, static_cast<LONG>(client_width_), static_cast<LONG>(client_height_)};
            AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi);
            return SIZE{frame.right - frame.left, frame.bottom - frame.top};
        };
        const UINT system_dpi = GetDpiForSystem();
        const SIZE initial = frameSize(system_dpi);
        hwnd_ = CreateWindowExW(0, kWindowClass, kWindowTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, initial.cx,
                                initial.cy, nullptr, nullptr, instance, this);
        if (hwnd_ == nullptr) {
            ready_.set_value(std::format("CreateWindowExW failed ({})", GetLastError()));
            UnregisterClassW(kWindowClass, instance);
            return;
        }
        // No IME for the game window: an open Japanese IME would otherwise show a composition window over
        // the game and turn keys into composition input. Keys still arrive as plain key messages.
        ImmAssociateContext(hwnd_, nullptr);
        if (const UINT window_dpi = GetDpiForWindow(hwnd_); window_dpi != 0 && window_dpi != system_dpi) {
            const SIZE adjusted = frameSize(window_dpi);
            SetWindowPos(hwnd_, nullptr, 0, 0, adjusted.cx, adjusted.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RECT client{};
        GetClientRect(hwnd_, &client);
        width_.store(static_cast<std::uint32_t>(client.right - client.left));
        height_.store(static_cast<std::uint32_t>(client.bottom - client.top));
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        ready_.set_value({});

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        UnregisterClassW(kWindowClass, instance);
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);
        MessageRoute route;
        if (self->on_message_) {
            route = self->on_message_(WindowMessage{hwnd, message, static_cast<std::uint64_t>(wparam),
                                                    static_cast<std::int64_t>(lparam)});
            if (route.handled) return static_cast<LRESULT>(route.result);
        }
        switch (message) {
            case WM_SIZE:
                // A minimised window reports a 0x0 client area.
                self->width_.store(wparam == SIZE_MINIMIZED ? 0 : LOWORD(lparam));
                self->height_.store(wparam == SIZE_MINIMIZED ? 0 : HIWORD(lparam));
                return 0;
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
                if (self->on_key_ && !route.withhold_key) {
                    if (const auto key = keyMessageFromWin32(message, wparam, static_cast<std::uint64_t>(lparam)))
                        self->on_key_(*key);
                }
                // Alt combinations (Alt+F4, Alt+Space) must still reach DefWindowProc.
                if (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) break;
                return 0;
            case WM_KILLFOCUS:
                if (self->on_focus_lost_) self->on_focus_lost_();
                break;
            case WM_CLOSE:
                if (self->on_close_ && !self->closing_.load()) {
                    self->on_close_();  // ends the process
                    return 0;
                }
                DestroyWindow(hwnd);
                return 0;
            case kDestroyMessage: DestroyWindow(hwnd); return 0;
            case WM_DESTROY: PostQuitMessage(0); return 0;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT:
                // The swapchain draws the client area.
                ValidateRect(hwnd, nullptr);
                return 0;
            default: break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    std::function<void()> on_close_;
    std::function<MessageRoute(const WindowMessage&)> on_message_;
    std::function<void(const KeyMessage&)> on_key_;
    std::function<void()> on_focus_lost_;
    std::uint32_t client_width_;
    std::uint32_t client_height_;
    HWND hwnd_ = nullptr;
    std::thread thread_;
    std::promise<std::string> ready_;  // empty string: the window exists
    std::atomic<bool> closing_{false};
    std::atomic<std::uint32_t> width_{0};
    std::atomic<std::uint32_t> height_{0};
};

Presenter::Window::Window(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Presenter::Window::~Window() = default;

std::unique_ptr<Presenter::Window> Presenter::Window::create(const PresenterOptions& options, std::string& error) {
    // Process-wide and before any window exists, so the surface queries on the calling thread report
    // physical pixels like the UI thread's window. Fails harmlessly when the awareness is already set.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::unique_ptr<Impl> impl = Impl::create(options, error);
    if (!impl) return nullptr;
    return std::unique_ptr<Window>(new Window(std::move(impl)));
}

void* Presenter::Window::nativeHandle() const { return impl_->hwnd(); }

bool Presenter::Window::minimized() const { return impl_->minimized(); }

VkExtent2D Presenter::Window::clientSize() const { return impl_->clientSize(); }

VkResult Presenter::Window::createSurface(VkInstance instance, VkSurfaceKHR* surface, std::string& error) const {
    const auto create_surface =
            reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
    if (create_surface == nullptr) {
        error = "vkCreateWin32SurfaceKHR is not available";
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    VkWin32SurfaceCreateInfoKHR surface_info{};
    surface_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surface_info.hinstance = GetModuleHandleW(nullptr);
    surface_info.hwnd = impl_->hwnd();
    const VkResult result = create_surface(instance, &surface_info, nullptr, surface);
    if (result != VK_SUCCESS) error = std::format("vkCreateWin32SurfaceKHR failed ({})", static_cast<int>(result));
    return result;
}

// The window has its own thread, so the calling thread needs no event loop.
void runWithHostEventLoop(std::function<void()> body, bool /*user_interface*/) { body(); }

void pumpHostEvents() {}

}  // namespace dal
