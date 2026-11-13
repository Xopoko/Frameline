#include "windows_shortcuts.h"

#include <QMetaObject>
#include <QObject>
#include <qt_windows.h>

#include <future>
#include <system_error>
#include <thread>
#include <utility>

WindowsRegionKeyState::WindowsRegionKeyState(bool alreadyDown)
    : state_(alreadyDown ? State::Passed : State::Released)
{
}

WindowsRegionKeyState::Decision WindowsRegionKeyState::onS(bool pressed, bool exactChord)
{
    if (!pressed) {
        const bool captured = std::exchange(state_, State::Released) == State::Captured;
        return captured ? Decision::Suppress : Decision::Pass;
    }
    if (state_ == State::Captured)
        return Decision::Suppress;
    if (state_ == State::Released && exactChord) {
        state_ = State::Captured;
        return Decision::Capture;
    }
    state_ = State::Passed;
    return Decision::Pass;
}

namespace {
bool keyDown(int key)
{
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

void preventStartMenu()
{
    // A swallowed S can leave Windows treating Win as a standalone key press.
    // Send the same unassigned key-up that PowerToys uses to mark Win as used.
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = 0xff;
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(input));
}
}

class WindowsRegionShortcut::Hook {
public:
    explicit Hook(std::function<void()> handler) : handler_(std::move(handler)) {}

    ~Hook()
    {
        if (thread_.joinable()) {
            if (threadId_)
                PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
            thread_.join();
        }
    }

    bool start()
    {
        if (threadId_)
            return true;
        std::promise<DWORD> ready;
        auto started = ready.get_future();
        try {
            thread_ = std::thread([this, ready = std::move(ready)]() mutable {
                run(ready);
            });
        } catch (const std::system_error&) {
            return false;
        }
        threadId_ = started.get();
        if (!threadId_)
            thread_.join();
        return threadId_ != 0;
    }

private:
    void run(std::promise<DWORD>& ready)
    {
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        current_ = this;
        keyState_ = WindowsRegionKeyState(keyDown('S'));
        const auto hook = SetWindowsHookExW(WH_KEYBOARD_LL, &keyboardEvent, GetModuleHandleW(nullptr), 0);
        ready.set_value(hook ? GetCurrentThreadId() : 0);
        if (hook) {
            // Keeping this loop off the GUI thread avoids hook timeouts during capture and export.
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            UnhookWindowsHookEx(hook);
        }
        current_ = nullptr;
    }

    static LRESULT CALLBACK keyboardEvent(int code, WPARAM message, LPARAM data)
    {
        if (code == HC_ACTION && current_) {
            const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
            if (key->vkCode == 'S' && !(key->flags & LLKHF_INJECTED)) {
                const bool pressed = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
                const bool exactChord = pressed && (keyDown(VK_LWIN) || keyDown(VK_RWIN))
                    && !(key->flags & LLKHF_ALTDOWN) && !keyDown(VK_MENU)
                    && !keyDown(VK_CONTROL) && !keyDown(VK_SHIFT);
                const auto decision = current_->keyState_.onS(pressed, exactChord);
                if (decision == WindowsRegionKeyState::Decision::Capture) {
                    preventStartMenu();
                    auto* owner = current_;
                    QMetaObject::invokeMethod(&owner->dispatch_, [owner] { owner->handler_(); },
                                              Qt::QueuedConnection);
                }
                if (decision != WindowsRegionKeyState::Decision::Pass)
                    return 1;
            }
        }
        return CallNextHookEx(nullptr, code, message, data);
    }

    static thread_local Hook* current_;
    QObject dispatch_;
    std::function<void()> handler_;
    std::thread thread_;
    DWORD threadId_ = 0;
    WindowsRegionKeyState keyState_;
};

thread_local WindowsRegionShortcut::Hook* WindowsRegionShortcut::Hook::current_ = nullptr;

WindowsRegionShortcut::WindowsRegionShortcut(std::function<void()> handler)
    : hook_(std::make_unique<Hook>(std::move(handler)))
{
}

WindowsRegionShortcut::~WindowsRegionShortcut() = default;

bool WindowsRegionShortcut::start()
{
    return hook_->start();
}
