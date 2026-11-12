// Spam Buffer - evens out uneven click spam (alternate spam, wave spam, ship straight-fly)
//
// How it works:
//  1. Every jump input goes through GJBaseGameLayer::handleButton.
//  2. If the last few inputs came in faster than the "spam threshold", we treat it as spam.
//  3. While spamming, inputs are held in a small queue and released at the average
//     spacing of your recent inputs, so uneven timing (10ms, 60ms, 10ms, 60ms...)
//     becomes steady timing (35ms, 35ms, 35ms...).
//  4. The delay is capped by "max delay" so it never feels like input lag.
//  5. When you stop spamming, inputs pass straight through again.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <chrono>
#include <deque>

using namespace geode::prelude;

namespace {

double nowSec() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

struct PendingInput {
    bool down;
    int button;
    bool player1;
    double due;
};

struct BufferState {
    std::deque<double> arrivals;   // arrival times of recent inputs
    std::deque<PendingInput> queue;
    double lastOut = 0.0;          // when we last let an input through
    bool replaying = false;        // true while we flush the queue (avoids re-buffering)

    void reset() {
        arrivals.clear();
        queue.clear();
        lastOut = 0.0;
        replaying = false;
    }

    // average gap between the last few inputs
    double meanGap() const {
        size_t n = arrivals.size();
        if (n < 2) return 0.0;
        size_t start = n > 5 ? n - 5 : 0;
        return (arrivals.back() - arrivals[start]) / double(n - 1 - start);
    }

    // true if the last 3 gaps were all shorter than the threshold
    bool isSpamming(double thresholdSec) const {
        size_t n = arrivals.size();
        if (n < 4) return false;
        for (size_t i = n - 3; i < n; ++i) {
            if (arrivals[i] - arrivals[i - 1] > thresholdSec) return false;
        }
        return true;
    }
};

BufferState g;

bool shouldBuffer(GJBaseGameLayer* layer, int button, bool isPlayer1) {
    auto mod = Mod::get();
    if (!mod->getSettingValue<bool>("enabled")) return false;
    if (button != 1) return false; // jump button only, ignore platformer left/right

    auto pl = PlayLayer::get();
    if (!pl || static_cast<GJBaseGameLayer*>(pl) != layer) return false; // not the editor

    if (mod->getSettingValue<bool>("practice-only") && !pl->m_isPracticeMode) return false;

    if (mod->getSettingValue<bool>("fly-only")) {
        auto p = isPlayer1 ? layer->m_player1 : layer->m_player2;
        if (!p || !(p->m_isShip || p->m_isDart)) return false; // ship or wave only
    }
    return true;
}

} // namespace

class $modify(SBGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (g.replaying || !shouldBuffer(this, button, isPlayer1)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        auto mod = Mod::get();
        double thresholdSec = mod->getSettingValue<int64_t>("spam-threshold") / 1000.0;
        double maxDelaySec = mod->getSettingValue<int64_t>("max-delay") / 1000.0;
        double now = nowSec();

        // In dual mode both players get the same press at the same moment.
        // Treat those as one input so they don't look like spam.
        bool sameMoment = !g.arrivals.empty() && (now - g.arrivals.back()) < 0.002;
        if (!sameMoment) {
            g.arrivals.push_back(now);
            if (g.arrivals.size() > 8) g.arrivals.pop_front();
        }

        bool spamming = g.isSpamming(thresholdSec);

        // Not spamming and nothing waiting: pass straight through, no added latency.
        if (!spamming && g.queue.empty()) {
            g.lastOut = now;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        double due;
        if (sameMoment && !g.queue.empty()) {
            due = g.queue.back().due;
        } else {
            due = std::max(now, g.lastOut + g.meanGap());
        }
        due = std::min(due, now + maxDelaySec);
        if (!g.queue.empty()) due = std::max(due, g.queue.back().due); // keep order

        // Already due and nothing ahead of it: no reason to wait a frame.
        if (g.queue.empty() && due <= now) {
            g.lastOut = now;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        g.queue.push_back({down, button, isPlayer1, due});
    }

    void update(float dt) {
        if (!g.queue.empty()) {
            double now = nowSec();
            g.replaying = true;
            while (!g.queue.empty() && g.queue.front().due <= now) {
                PendingInput e = g.queue.front();
                g.queue.pop_front();
                GJBaseGameLayer::handleButton(e.down, e.button, e.player1);
                g.lastOut = now;
            }
            g.replaying = false;
        }
        GJBaseGameLayer::update(dt);
    }
};

class $modify(SBPlayLayer, PlayLayer) {
    void resetLevel() {
        g.reset();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        g.reset();
        PlayLayer::onQuit();
    }
};
