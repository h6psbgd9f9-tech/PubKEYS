#pragma once
#include <chrono>

// Owned by the single console reader. Confirmation never pauses computation.
class StopConfirmation {
public:
    using Clock = std::chrono::steady_clock;
    enum class Action { pass, prompt, cancel, confirm, ignore };
    Action key(int ch, Clock::time_point now = Clock::now()) {
        if (ch == 3) return Action::ignore; // raw Ctrl+C with processed input off
        if (pending_ && now >= deadline_) pending_ = false;
        if (pending_) {
            pending_ = false;
            return ch == 'y' || ch == 'Y' ? Action::confirm : Action::cancel;
        }
        if (ch == 'q' || ch == 'Q') {
            pending_ = true;
            deadline_ = now + std::chrono::seconds(10);
            return Action::prompt;
        }
        return Action::pass;
    }
private:
    bool pending_ = false;
    Clock::time_point deadline_{};
};
