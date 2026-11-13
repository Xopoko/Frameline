#pragma once

#include <functional>
#include <memory>

class WindowsRegionKeyState {
public:
    enum class Decision { Pass, Suppress, Capture };

    explicit WindowsRegionKeyState(bool alreadyDown = false);
    Decision onS(bool pressed, bool exactChord);

private:
    enum class State { Released, Passed, Captured };
    State state_;
};

class WindowsRegionShortcut {
public:
    explicit WindowsRegionShortcut(std::function<void()> handler);
    ~WindowsRegionShortcut();
    bool start();

private:
    class Hook;
    std::unique_ptr<Hook> hook_;
};
