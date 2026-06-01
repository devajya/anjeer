#pragma once

#include "exchange/exchange_types.h"

namespace anjeer::exchange {

// Per-session monotonically-increasing sequence counter.
// Stamps every outbound MarketDataEvent so consumers can detect gaps and enforce ordering.
// Not thread-safe — all calls must happen on the ExchangeSession owner's thread.
class Sequencer {
public:
    // Returns the next sequence number. First call after construction or reset() returns 1.
    [[nodiscard]] seq_t next_seq() noexcept;

    // Resets the counter to 0. next_seq() will return 1 on the following call.
    // Called by ExchangeSession at begin_round so seq restarts from 1 each round.
    void reset() noexcept;

    // Returns the last value returned by next_seq(), or 0 if never called / after reset().
    [[nodiscard]] seq_t current_seq() const noexcept;

private:
    seq_t seq_ = 0;
};

} // namespace anjeer::exchange
