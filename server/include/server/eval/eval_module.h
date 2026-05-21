#pragma once

#include "engine/game_snapshot.h"
#include "server/eval/eval_types.h"

namespace anjeer::server::eval {

class EvalModule {
public:
    virtual ~EvalModule() = default;

    virtual void on_round_start(const engine::GameStateSnapshot&) = 0;
    virtual void on_trade_event(const EvalTradeEvent&)            = 0;
    virtual void on_book_update(const EvalBookUpdate&)            = 0;
    virtual void on_round_end  (const engine::GameStateSnapshot&) = 0;
};

} // namespace anjeer::server::eval
