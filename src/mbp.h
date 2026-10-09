#pragma once

// MBP-1 helpers on top of AZ's DataWindow.

#include "azbacktest.h"

#include <chrono>
#include <cstdio>
#include <vector>

#ifdef _WIN32
extern "C" __declspec(dllimport) void* __stdcall GetCurrentProcess();
extern "C" __declspec(dllimport) int __stdcall SetProcessWorkingSetSize(void*, std::size_t, std::size_t);
#endif

constexpr long long MS = 1'000'000LL, SEC = 1'000'000'000LL;

// one row. on T rows bid/ask is the book before the trade
struct Row {
    long long ts = 0;           // ts_event ns
    double px = 0, bid = 0, ask = 0;
    bool trade = false;
    char side = 'N';            // B / A / N
};

inline Row rowFromWindow(const DataWindow& w, std::size_t b) {
    const char s = w.side[b][0];
    return {w.tsEventNanos[b], w.prices[b], w.bidPrices[b], w.askPrices[b],
            w.action[b][0] == kCSVMapping.actionTradeAlias[0],
            s == kCSVMapping.buySideAggressorAlias[0] ? 'B' : s == kCSVMapping.sellSideAggressorAlias[0] ? 'A' : 'N'};
}

struct Book {
    double bid = 0, ask = 0;
    bool valid() const { return bid > 0 && ask > 0; }
};

// passive limit, touch fills like the prop sims: any print at our price fills us, no queue
struct PassiveOrder {
    int s = 0;                  // +1 buy, -1 sell
    double L = 0;               // limit price
    bool live = false, filled = false;
    long long fillTs = 0, deadline = 0;
    double fillPx = 0;

    PassiveOrder(int side, double price, const Book& b, long long t, long long dl) : s(side), L(price), live(price > 0), deadline(dl) {
        const double opp = s > 0 ? b.ask : b.bid;
        if (live && opp > 0 && s * (opp - L) <= 0) { live = false; filled = true; fillTs = t; fillPx = opp; }
    }

    void onRow(const Row& x) {
        if (!live) return;
        if (x.ts > deadline) { live = false; return; }
        const double opp = s > 0 ? x.ask : x.bid;
        if (x.trade ? s * (L - x.px) >= 0 : opp > 0 && s * (opp - L) <= 0) { live = false; filled = true; fillTs = x.ts; fillPx = L; }
    }
};

class Progress {
public:
    explicit Progress(double totalBytes) : _total(totalBytes) {}

    void update(double doneBytes) {
        const int pct = (int)(100.0 * doneBytes / _total);
        while (pct >= _next && _next <= 100) {
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - _start).count();
            std::printf("[pass] %3d%% | elapsed %s | eta %s\n", _next, fmt(el).c_str(), fmt(el * (100.0 - _next) / _next).c_str());
            std::fflush(stdout);
#ifdef _WIN32
            SetProcessWorkingSetSize(GetCurrentProcess(), (std::size_t)-1, (std::size_t)-1);
#endif
            _next += 5;
        }
    }

private:
    static std::string fmt(double s) {
        char b[32];
        const int m = (int)(s / 60);
        if (m >= 60) std::snprintf(b, sizeof b, "%dh%02dm", m / 60, m % 60);
        else std::snprintf(b, sizeof b, "%dm%02ds", m, (int)s % 60);
        return b;
    }
    double _total;
    int _next = 5;
    std::chrono::steady_clock::time_point _start = std::chrono::steady_clock::now();
};
