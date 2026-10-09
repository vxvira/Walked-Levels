#pragma once

#include "mbp.h"

#include <cstdint>
#include <random>
#include <vector>

struct Sweep {
    long long ts0 = 0;
    long long ta = 0;
    int dir = 1;
    int levels = 0;
};

class SweepDetector {
public:
    SweepDetector(int minLevels, long long delayMinMs, long long delayMaxMs)
        : _minLevels(minLevels), _delayMin(delayMinMs * MS), _delayMax(delayMaxMs * MS) {}

    const std::vector<Sweep>& onRow(const Row& r) {
        _ready.clear();
        if (_open && !(r.ts == _ts && (!r.trade || (r.side == _side
                && (_side == 'N' || (_side == 'B' ? r.px >= _back : r.px <= _back)))))) {
            _open = false;
            complete();
        }
        if (r.trade) {
            if (!_open) { _open = true; _side = r.side; _ts = r.ts; _levels = 0; }
            if (_levels == 0 || r.px != _back) { _levels++; _back = r.px; }
        }
        for (auto it = _pending.begin(); it != _pending.end();) {
            if (r.ts > it->ta) { _ready.push_back(*it); it = _pending.erase(it); }
            else ++it;
        }
        return _ready;
    }

private:
    void complete() {
        if (_levels < _minLevels || _side == 'N') return;
        std::mt19937_64 rng((uint64_t)_ts);
        const long long delay = std::uniform_int_distribution<long long>(_delayMin, _delayMax)(rng);
        _pending.push_back({_ts, _ts + delay, _side == 'B' ? 1 : -1, _levels});
    }

    int _minLevels;
    long long _delayMin, _delayMax;
    bool _open = false;
    char _side = 'N';
    long long _ts = 0;
    int _levels = 0;
    double _back = 0;
    std::vector<Sweep> _pending, _ready;
};
