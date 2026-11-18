#pragma once

#include <windows.h>
#include <imm.h>

// This window borrows the thread's IME context. Remember the exact association;
// the IME language, open state and other windows' contexts belong to the user.
class WindowDisplayIme {
public:
    void SetDisplayOnly(HWND window, bool enabled) {
        if (!enabled || window != window_) Restore();
        if (!enabled || !window || window == window_) return;
        window_ = window;
        previous_ = ImmAssociateContext(window, nullptr);
    }

    void Restore() {
        if (!window_) return;
        const HWND window = window_;
        const HIMC previous = previous_;
        window_ = nullptr;
        previous_ = nullptr;
        ImmAssociateContext(window, previous);
    }

private:
    HWND window_ = nullptr;
    HIMC previous_ = nullptr;
};
