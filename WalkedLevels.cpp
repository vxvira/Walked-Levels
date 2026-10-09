#include <climits>
#include <deque>
#include <filesystem>
#include <optional>
#include <vector>

#include "mbp.h"
#include "sweeps.h"

// a sweep = one aggressor walking >= 5 levels, we see it 50-100ms late
const int       kMinLevels  = 5;
const long long kDelayMinMs = 50, kDelayMaxMs = 100;

// fire when net levels over the window hits 40, re-arm under 20
const long long kWindowSec = 60;
const double    kNetLevels = 40;

// the trade
const long long kEntryDelaySec = 10;   // wait this long, then rest a limit at our touch
const long long kEntryWaitSec  = 30;   // cancel if nothing fills
const long long kHoldSec       = 300;  // hold time
const long long kGapSec        = 600;  // toss trades that sit through a data gap

const long long kOosCutSec = 1787601600LL;   // IS before, OOS after (2026-08-24 20:00 UTC)

// running net of sweep levels, fires once per cluster
class ClusterDetector {
public:
    // returns +1 / -1 when a cluster fires, else 0
    int onSweep(const Sweep& sw) {
        while (!_q.empty() && _q.front().ts <= sw.ts0 - kWindowSec * SEC) { _net -= _q.front().v; _q.pop_front(); }
        const double v = sw.dir * (double)sw.levels;
        _q.push_back({sw.ts0, v});
        _net += v;
        int fired = 0;
        for (int s : {1, -1}) {
            bool& armed = s > 0 ? _armedUp : _armedDn;
            if (armed && s * _net >= kNetLevels && sw.dir == s) { armed = false; fired = s; }
            else if (!armed && s * _net < kNetLevels / 2) armed = true;
        }
        return fired;
    }

private:
    struct E { long long ts; double v; };
    std::deque<E> _q;
    double _net = 0;
    bool _armedUp = true, _armedDn = true;
};

// trades closed in [fromSec, toSec) with their own net equity
TradesInfo slice(const TradesInfo& all, long long fromSec, long long toSec) {
    const double cost = kCSVMapping.commision + kCSVMapping.spread + kCSVMapping.timingCost;
    TradesInfo out;
    for (const auto& t : all.trades) {
        if (t.closeEpochSec < fromSec || t.closeEpochSec >= toSec) continue;
        out.trades.push_back(t);
        out.realizedProfit += t.profit - cost;
        out.equityCurve.emplace_back(t.closeEpochSec, out.realizedProfit);
    }
    return out;
}

void addStats(const std::string& tag, const TradesInfo& info) {
    PnlAnalytics pnl(info);
    addStat(tag + " trades", (double)info.trades.size());
    addStat(tag + " trades / day", pnl.returnTradesPerDay());
    addStat(tag + " win rate % (gross)", 100.0 * pnl.returnWinrate());
    addStat(tag + " avg win pts (gross)", pnl.returnAverageWinSize());
    addStat(tag + " avg loss pts (gross)", pnl.returnAverageLossSize());
    addStat(tag + " net pts", info.realizedProfit);
    addStat(tag + " net pts / trade", info.trades.empty() ? 0.0 : info.realizedProfit / info.trades.size());
}

int main() {
    loadConfig();

    // TradeApi trades at prices.back(), so this just holds our fill / exit price
    std::vector<double> prices;
    MarketData md(kCSVMapping.path);
    DataApi  dataApi(prices, 0.25, 0.50);
    TradeApi tradeApi(prices, 0.25, 0.50);

    const int batchSize = 4096;  // io detail

    SweepDetector sweeps(kMinLevels, kDelayMinMs, kDelayMaxMs);
    ClusterDetector clusters;
    Book book;                           // as of the last row
    int side = 0;                        // +1 buy, -1 sell, 0 nothing going
    long long placeAt = 0, exitAt = 0, lastTs = 0;
    std::optional<PassiveOrder> entry;
    Progress progress((double)std::filesystem::file_size(kCSVMapping.path));

    int idx = 0;
    for (;;) {
        DataWindow window = dataApi.requestDataWindow(md, batchSize);
        if (window.prices.empty()) break;
        progress.update((double)md.byteOffset());

        for (std::size_t b = 0; b < window.prices.size(); b++, idx++) {
            const Row row = rowFromWindow(window, b);

            // in a trade: bail on a gap, otherwise exit once the hold is up
            if (tradeApi.openTrade) {
                if (row.ts - lastTs > kGapSec * SEC) { tradeApi.openTrade.reset(); tradeApi.inLong = tradeApi.inShort = false; side = 0; }
                else if (row.ts > exitAt) {
                    prices.assign(1, side > 0 ? book.bid : book.ask);
                    tradeApi.tick(exitAt / SEC);
                    tradeApi.closeTrade();
                    side = 0;
                }
                lastTs = row.ts;
            }
            // working an entry: place it after the delay, check for a fill
            else if (side) {
                if (!entry && row.ts > placeAt && book.valid())
                    entry.emplace(side, side > 0 ? book.bid : book.ask, book, row.ts, placeAt + kEntryWaitSec * SEC);
                else if (entry) entry->onRow(row);

                if (entry && entry->filled) {
                    prices.assign(1, entry->fillPx);
                    tradeApi.tick(entry->fillTs / SEC);
                    side > 0 ? tradeApi.openLong(idx) : tradeApi.openShort(idx);
                    exitAt = entry->fillTs + kHoldSec * SEC;
                    lastTs = entry->fillTs;
                    entry.reset();
                } else if (entry && !entry->live) { entry.reset(); side = 0; }
            }

            // new cluster, only if we're flat
            for (const Sweep& s : sweeps.onRow(row)) {
                const int dir = clusters.onSweep(s);
                if (dir && !side) { side = -dir; placeAt = s.ta + kEntryDelaySec * SEC; }
            }
            if (row.bid > 0 && row.ask > 0) book = {row.bid, row.ask};
        }
    }
    // anything still open at the end just gets dropped

    const TradesInfo isInfo = slice(tradeApi.info, 0, kOosCutSec), oosInfo = slice(tradeApi.info, kOosCutSec, LLONG_MAX);
    addStats("IS", isInfo);
    addStats("OOS", oosInfo);

    PnlAnalytics pnl(tradeApi.info);
    addLine("equity, net pts (daily)", std::vector<std::vector<double>>{pnl.returnProfitOverTime(1440)}, {"IS + OOS"},
            RGBA{0.5f, 0.8f, 0.5f, 1.0f});
    addLine("OOS equity, net pts (daily)", std::vector<std::vector<double>>{PnlAnalytics(oosInfo).returnProfitOverTime(1440)},
            {"OOS"}, RGBA{0.9f, 0.6f, 0.3f, 1.0f});

    showConsole("Walked-Levels", skins::toxic);
}
