// Cocoa window with a CAMetalLayer for MoltenVK, and the host event loop. AppKit objects may only be
// touched on the process main thread, so every Cocoa call below runs there; the size and the minimized
// state are published in atomics for the thread that owns the renderer.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include "render/vk_window.h"

// Not through volk's platform switch: volk is built without VK_USE_PLATFORM_METAL_EXT, so the surface
// entry point is fetched by name below. The header only adds types here (VK_NO_PROTOTYPES).
#include <vulkan/vulkan_metal.h>

#include <dispatch/dispatch.h>
#include <pthread.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>

#include "core/log.h"
#include "core/threads.h"
#include "input/mac_key_codes.h"

namespace {

constexpr char kWindowTitle[] = "デート・ア・ライブ 凜祢ユートピア";

// What the Objective-C classes below call into; implemented by Presenter::Window::Impl. All calls happen
// on the main thread.
struct WindowEvents {
    virtual ~WindowEvents() = default;
    // Returns true when the event must not reach AppKit's own dispatch.
    virtual bool onEvent(NSEvent* event) = 0;
    // The user asked to close the window. Returns true when the client took over (the window stays open).
    virtual bool requestClose() = 0;
    virtual void onFocus(bool gained) = 0;
    // The frame, the backing scale or the minimized state changed.
    virtual void onLayoutChanged() = 0;
};

}  // namespace

// AppKit does not deliver the key-up of a key that went down with Command held to the key window (it
// ends the key equivalent search), so a key pressed together with Command would stay down for the pad.
// Handing those key-ups to the window makes up for it.
@interface DalApplication : NSApplication
@end

@implementation DalApplication
- (void)sendEvent:(NSEvent*)event {
    if (event.type == NSEventTypeKeyUp && (event.modifierFlags & NSEventModifierFlagCommand) != 0) {
        [self.keyWindow sendEvent:event];
        return;
    }
    [super sendEvent:event];
}
@end

@interface DalWindow : NSWindow
@property(nonatomic, assign) WindowEvents* events;
@end

@implementation DalWindow
// The single routing point for window input: every event sent to this window reaches the client first.
// Key equivalents (Cmd+Q and other menu shortcuts) are handled by NSApplication before they get here, so
// the client never sees those key-downs.
- (void)sendEvent:(NSEvent*)event {
    if (self.events != nullptr && self.events->onEvent(event)) return;
    [super sendEvent:event];
}
@end

@interface DalMetalView : NSView
@property(nonatomic, assign) WindowEvents* events;
@end

@implementation DalMetalView
- (CALayer*)makeBackingLayer {
    return [CAMetalLayer layer];
}
- (BOOL)wantsUpdateLayer {
    return YES;
}
- (BOOL)isOpaque {
    return YES;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    return YES;
}
// Keys are not text input here: the view is no NSTextInputClient and never calls interpretKeyEvents:, so
// no input method opens. The empty overrides keep the keys that still reach the responder chain (unmapped
// keys, Command combinations) from beeping.
- (void)keyDown:(NSEvent*)event {
}
- (void)keyUp:(NSEvent*)event {
}
- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    if (self.events != nullptr) self.events->onLayoutChanged();
}
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.events != nullptr) self.events->onLayoutChanged();
}
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (self.events != nullptr) self.events->onLayoutChanged();
}
@end

@interface DalWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) WindowEvents* events;
@end

@implementation DalWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    return self.events == nullptr || !self.events->requestClose();
}
- (void)windowDidBecomeKey:(NSNotification*)notification {
    if (self.events != nullptr) self.events->onFocus(true);
}
- (void)windowDidResignKey:(NSNotification*)notification {
    if (self.events != nullptr) self.events->onFocus(false);
}
- (void)windowDidMiniaturize:(NSNotification*)notification {
    if (self.events != nullptr) self.events->onLayoutChanged();
}
- (void)windowDidDeminiaturize:(NSNotification*)notification {
    if (self.events != nullptr) self.events->onLayoutChanged();
}
- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
    if (self.events != nullptr) self.events->onLayoutChanged();
}
@end

namespace {

// The window that is open, for Cmd+Q. Main thread only.
WindowEvents* g_open_window = nullptr;

}  // namespace

@interface DalAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation DalAppDelegate
// Cmd+Q and Quit end the run through the client's close handling, like the close button, and never
// terminate the process from here: AppKit would call exit() while the emulation thread still runs. Before a
// window is open (or while it closes) Quit does nothing.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    if (g_open_window != nullptr) g_open_window->requestClose();
    return NSTerminateCancel;
}
@end

namespace dal {
namespace {

// Set once the main thread runs (or is about to run) [NSApp run], which finishes the launch itself.
bool g_loop_started = false;
bool g_launched = false;
DalAppDelegate* g_app_delegate = nil;

void runOnMain(const std::function<void()>& fn) {
    if (pthread_main_np() != 0) {
        fn();
        return;
    }
    const std::function<void()>* target = &fn;
    dispatch_sync(dispatch_get_main_queue(), ^{
      (*target)();
    });
}

// Creates NSApp with a minimal menu. Main thread only; the first successful call does the work.
bool ensureApplication(std::string& error) {
    if (g_app_delegate == nil) {
        // Without a window server connection NSApplication aborts the process instead of failing.
        CFDictionaryRef session = CGSessionCopyCurrentDictionary();
        if (session == nullptr) {
            error = "no GUI session (the window server is not reachable)";
            return false;
        }
        CFRelease(session);

        [DalApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        g_app_delegate = [[DalAppDelegate alloc] init];
        [NSApp setDelegate:g_app_delegate];

        NSMenu* menu_bar = [[NSMenu alloc] init];
        NSMenuItem* app_item = [[NSMenuItem alloc] init];
        [menu_bar addItem:app_item];
        NSMenu* app_menu = [[NSMenu alloc] init];
        NSString* quit_title = [@"Quit " stringByAppendingString:[[NSProcessInfo processInfo] processName]];
        [app_menu addItem:[[NSMenuItem alloc] initWithTitle:quit_title
                                                     action:@selector(terminate:)
                                              keyEquivalent:@"q"]];
        [app_item setSubmenu:app_menu];
        [NSApp setMainMenu:menu_bar];
    }
    if (!g_launched && !g_loop_started) {
        [NSApp finishLaunching];
        g_launched = true;
    }
    return true;
}

}  // namespace

class Presenter::Window::Impl final : public WindowEvents {
public:
    // Opens the window on the main thread; from another thread the main thread must be running its event
    // loop (runWithHostEventLoop).
    static std::unique_ptr<Impl> create(const PresenterOptions& options, std::string& error) {
        std::unique_ptr<Impl> window(new Impl(options));
        bool opened = false;
        runOnMain([&] { opened = window->open(error); });
        if (!opened) return nullptr;
        return window;
    }

    ~Impl() override {
        // Callbacks that arrive from now on find the window closing and do not reach the client.
        closing_.store(true);
        runOnMain([this] { close(); });
    }

    void* view() const { return (__bridge void*)view_; }
    bool minimized() const {
        return width_.load(std::memory_order_relaxed) == 0 || height_.load(std::memory_order_relaxed) == 0;
    }
    VkExtent2D clientSize() const {
        return {width_.load(std::memory_order_relaxed), height_.load(std::memory_order_relaxed)};
    }

    VkResult createSurface(VkInstance instance, VkSurfaceKHR* surface, std::string& error) const {
        const auto create_surface = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT"));
        if (create_surface == nullptr) {
            error = "vkCreateMetalSurfaceEXT is not available";
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        VkMetalSurfaceCreateInfoEXT surface_info{};
        surface_info.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
        surface_info.pLayer = layer_;
        const VkResult result = create_surface(instance, &surface_info, nullptr, surface);
        if (result != VK_SUCCESS) error = std::format("vkCreateMetalSurfaceEXT failed ({})", static_cast<int>(result));
        return result;
    }

private:
    explicit Impl(const PresenterOptions& options)
        : on_close_(options.on_close),
          on_message_(options.on_message),
          on_key_(options.on_key),
          on_focus_lost_(options.on_focus_lost),
          client_width_(options.client_width),
          client_height_(options.client_height) {}

    // Main thread.
    bool open(std::string& error) {
        @autoreleasepool {
            if (!ensureApplication(error)) return false;
            // The content size is in points; the backing scale makes it pixels in updateSurface().
            const NSRect content = NSMakeRect(0, 0, client_width_, client_height_);
            const NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
            window_ = [[DalWindow alloc] initWithContentRect:content
                                                   styleMask:style
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
            if (window_ == nil) {
                error = "NSWindow creation failed";
                return false;
            }
            // ARC owns the window; closing it must not release it a second time.
            [window_ setReleasedWhenClosed:NO];
            [window_ setTitle:[NSString stringWithUTF8String:kWindowTitle]];
            // The debug overlay hovers its widgets, which needs the pointer position without a button down.
            [window_ setAcceptsMouseMovedEvents:YES];

            view_ = [[DalMetalView alloc] initWithFrame:content];
            [view_ setWantsLayer:YES];
            layer_ = [view_.layer isKindOfClass:[CAMetalLayer class]] ? (CAMetalLayer*)view_.layer : nil;
            if (layer_ == nil) {
                error = "the content view has no CAMetalLayer";
                window_ = nil;
                view_ = nil;
                return false;
            }
            [layer_ setOpaque:YES];

            delegate_ = [[DalWindowDelegate alloc] init];
            window_.events = this;
            view_.events = this;
            delegate_.events = this;
            [window_ setDelegate:delegate_];
            [window_ setContentView:view_];
            [window_ makeFirstResponder:view_];
            [window_ center];
            updateSurface();
            g_open_window = this;
            [window_ makeKeyAndOrderFront:nil];
            [NSApp activate];
        }
        return true;
    }

    // Main thread.
    void close() {
        @autoreleasepool {
            if (g_open_window == this) g_open_window = nullptr;
            if (window_ == nil) return;
            window_.events = nullptr;
            view_.events = nullptr;
            delegate_.events = nullptr;
            [window_ setDelegate:nil];
            [window_ close];
            // Released on the next turn of the main queue: close() may run from one of this window's own
            // callbacks, and ARC does not keep the window alive while it is still inside sendEvent:.
            DalWindow* closed = window_;
            dispatch_async(dispatch_get_main_queue(), ^{
              (void)closed;
            });
            window_ = nil;
            view_ = nil;
            layer_ = nil;
            delegate_ = nil;
        }
    }

    // Publishes the pixel size and keeps the layer's drawable the same size.
    void updateSurface() {
        if (window_ == nil || layer_ == nil) return;
        const CGFloat scale = window_.backingScaleFactor;
        const NSSize points = view_.bounds.size;
        const double pixels_w = std::round(points.width * scale);
        const double pixels_h = std::round(points.height * scale);
        const bool empty = pixels_w < 1 || pixels_h < 1;
        if (!empty) {
            layer_.contentsScale = scale;
            layer_.drawableSize = CGSizeMake(pixels_w, pixels_h);
        }
        const bool hidden = empty || window_.miniaturized;
        width_.store(hidden ? 0 : static_cast<std::uint32_t>(pixels_w));
        height_.store(hidden ? 0 : static_cast<std::uint32_t>(pixels_h));
    }

    // The key message of a key event, or nullopt for any other event and for keys without a scan code.
    static std::optional<KeyMessage> keyMessageFromEvent(NSEvent* event) {
        const NSEventType type = event.type;
        if (type != NSEventTypeKeyDown && type != NSEventTypeKeyUp && type != NSEventTypeFlagsChanged) {
            return std::nullopt;
        }
        const std::optional<std::uint32_t> scan_code = scanCodeFromMacKeyCode(event.keyCode);
        if (!scan_code) return std::nullopt;
        if (type == NSEventTypeFlagsChanged) {
            // A modifier has no key-down or key-up event, only the new flag state. Its own device-dependent
            // bit tells which key changed, even while the other key of the pair is held.
            const std::optional<std::uint64_t> mask = macModifierDownMask(event.keyCode);
            if (!mask) return std::nullopt;
            return KeyMessage{*scan_code, (event.modifierFlags & *mask) != 0, false};
        }
        // isARepeat raises an exception for events that are not key-down or key-up.
        const bool down = type == NSEventTypeKeyDown;
        return KeyMessage{*scan_code, down, down && event.isARepeat};
    }

    bool onEvent(NSEvent* event) override {
        if (closing_.load()) return false;
        const std::optional<KeyMessage> key = keyMessageFromEvent(event);
        bool withhold_key = false;
        if (on_message_) {
            WindowMessage message;
            message.view = view();
            message.event = (__bridge void*)event;
            if (key) {
                message.kind = WindowEventKind::Key;
                message.key = key;
            }
            const MessageRoute route = on_message_(message);
            if (route.handled) return true;
            withhold_key = route.withhold_key;
        }
        if (!key) return false;
        if (on_key_ && !withhold_key) on_key_(*key);
        // The client has the key. Command combinations still go on to AppKit for the system shortcuts that
        // are not menu items; other keys end here, so nothing beeps.
        return (event.modifierFlags & NSEventModifierFlagCommand) == 0 || event.type == NSEventTypeFlagsChanged;
    }

    bool requestClose() override {
        if (!on_close_ || closing_.load()) return false;
        on_close_();  // ends the process
        return true;
    }

    void onFocus(bool gained) override {
        if (closing_.load()) return;
        if (on_message_) {
            WindowMessage message;
            message.view = view();
            message.kind = gained ? WindowEventKind::FocusGained : WindowEventKind::FocusLost;
            on_message_(message);
        }
        if (!gained && on_focus_lost_) on_focus_lost_();
    }

    void onLayoutChanged() override { updateSurface(); }

    std::function<void()> on_close_;
    std::function<MessageRoute(const WindowMessage&)> on_message_;
    std::function<void(const KeyMessage&)> on_key_;
    std::function<void()> on_focus_lost_;
    std::uint32_t client_width_;
    std::uint32_t client_height_;
    DalWindow* window_ = nil;
    DalMetalView* view_ = nil;
    CAMetalLayer* layer_ = nil;
    DalWindowDelegate* delegate_ = nil;
    std::atomic<bool> closing_{false};
    std::atomic<std::uint32_t> width_{0};
    std::atomic<std::uint32_t> height_{0};
};

Presenter::Window::Window(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Presenter::Window::~Window() = default;

std::unique_ptr<Presenter::Window> Presenter::Window::create(const PresenterOptions& options, std::string& error) {
    std::unique_ptr<Impl> impl = Impl::create(options, error);
    if (!impl) return nullptr;
    return std::unique_ptr<Window>(new Window(std::move(impl)));
}

void* Presenter::Window::nativeHandle() const { return impl_->view(); }

bool Presenter::Window::minimized() const { return impl_->minimized(); }

VkExtent2D Presenter::Window::clientSize() const { return impl_->clientSize(); }

VkResult Presenter::Window::createSurface(VkInstance instance, VkSurfaceKHR* surface, std::string& error) const {
    return impl_->createSurface(instance, surface, error);
}

void runWithHostEventLoop(std::function<void()> body, bool user_interface) {
    if (pthread_main_np() == 0) throw std::logic_error("runWithHostEventLoop must be called on the main thread");

    // Keeps App Nap and timer coalescing from slowing the emulation down while the window is hidden or
    // unfocused.
    id activity =
            [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                                                           reason:@"Running the emulated game"];

    bool ui = user_interface;
    if (ui) {
        g_loop_started = true;  // [NSApp run] finishes the launch itself
        std::string error;
        if (!ensureApplication(error)) {
            // Presenter::create reports the same problem when a window is requested.
            logWarn("dal: window: {}; running without a user interface", error);
            g_loop_started = false;
            ui = false;
        }
    }

    struct Shared {
        std::atomic<bool> done{false};
        std::exception_ptr error;
    };
    const auto shared = std::make_shared<Shared>();
    // The body replaces what ran on the process main thread, whose stack is 8 MiB.
    startHostThread(
            [shared, ui, body = std::move(body)]() mutable {
                try {
                    // Destroyed here, before the main thread is told: once it returns, the caller may be gone.
                    auto run = std::move(body);
                    run();
                } catch (...) {
                    shared->error = std::current_exception();
                }
                dispatch_async(dispatch_get_main_queue(), ^{
                  shared->done.store(true);
                  if (ui) {
                      [NSApp stop:nil];
                      // stop: only takes effect once the loop handles another event.
                      [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                                          location:NSZeroPoint
                                                     modifierFlags:0
                                                         timestamp:0
                                                      windowNumber:0
                                                           context:nil
                                                           subtype:0
                                                             data1:0
                                                             data2:0]
                               atStart:YES];
                  } else {
                      CFRunLoopStop(CFRunLoopGetMain());
                  }
                });
            },
            8u << 20);

    while (!shared->done.load()) {
        if (ui) {
            [NSApp run];
        } else {
            CFRunLoopRun();
        }
    }
    [[NSProcessInfo processInfo] endActivity:activity];
    if (shared->error) std::rethrow_exception(shared->error);
}

void pumpHostEvents() {
    if (pthread_main_np() == 0 || NSApp == nil) return;
    @autoreleasepool {
        std::string error;
        ensureApplication(error);
        for (;;) {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:nil
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (event == nil) break;
            [NSApp sendEvent:event];
        }
        [NSApp updateWindows];
    }
}

}  // namespace dal
