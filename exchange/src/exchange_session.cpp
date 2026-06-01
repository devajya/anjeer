#include "exchange/exchange_session.h"

#include <stdexcept>

namespace anjeer::exchange {

template<class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;


ExchangeSession::ExchangeSession(std::vector<InstrumentConfig> instruments) {
    books_.reserve(instruments.size());
    for (auto& cfg : instruments) {
        OrderBook::Config book_cfg;
        book_cfg.min_price               = cfg.min_price;
        book_cfg.max_price               = cfg.max_price;
        book_cfg.nudge_initial_buy_price  = cfg.nudge_initial_buy_price;
        book_cfg.nudge_initial_sell_price = cfg.nudge_initial_sell_price;
        book_cfg.suit                    = cfg.label;
        books_.emplace_back(book_cfg);
    }
}


ExchangeResult ExchangeSession::submit_order(
        instrument_id_t instrument_id, Side side, price_t price, int32_t player_slot) {
    if (instrument_id >= static_cast<instrument_id_t>(books_.size())) {
        ExchangeResult r;
        r.feedback.push_back(OrderRejected{OrderRejected::Code::InvalidInstrument,
            "instrument_id " + std::to_string(instrument_id) + " out of range"});
        return r;
    }
    return translate(instrument_id,
                     books_[instrument_id].submit(player_slot, side, price));
}

ExchangeResult ExchangeSession::cancel_order(
        int64_t order_id, instrument_id_t instrument_id, int32_t player_slot) {
    if (instrument_id >= static_cast<instrument_id_t>(books_.size())) {
        ExchangeResult r;
        r.feedback.push_back(OrderRejected{OrderRejected::Code::InvalidInstrument,
            "instrument_id " + std::to_string(instrument_id) + " out of range"});
        return r;
    }
    return translate(instrument_id,
                     books_[instrument_id].cancel(order_id, player_slot));
}

ExchangeResult ExchangeSession::cancel_player(int32_t player_slot) {
    ExchangeResult merged;
    for (instrument_id_t i = 0;
         i < static_cast<instrument_id_t>(books_.size()); ++i) {
        auto r = translate(i, books_[i].cancel_player(player_slot));
        for (auto& f : r.feedback) merged.feedback.push_back(std::move(f));
        for (auto& m : r.market)   merged.market.push_back(std::move(m));
    }
    return merged;
}

std::vector<BookUpdated> ExchangeSession::wipe() {
    std::vector<BookUpdated> updates;
    updates.reserve(books_.size());
    for (instrument_id_t i = 0;
         i < static_cast<instrument_id_t>(books_.size()); ++i) {
        for (auto& ev : books_[i].wipe()) {
            if (auto* bu = std::get_if<BookUpdateEvent>(&ev)) {
                BookUpdated upd;
                upd.instrument_id = i;
                upd.best_bid      = bu->best_bid;
                upd.best_ask      = bu->best_ask;
                upd.best_bid_slot = bu->best_bid_player_id;
                upd.best_ask_slot = bu->best_ask_player_id;
                updates.push_back(upd);
            }
        }
    }
    return updates;
}

void ExchangeSession::reset_seq() {
    sequencer_.reset();
}

seq_t ExchangeSession::current_seq() const noexcept {
    return sequencer_.current_seq();
}


std::optional<price_t> ExchangeSession::best_bid(instrument_id_t instrument_id) const {
    if (instrument_id >= static_cast<instrument_id_t>(books_.size())) return std::nullopt;
    return books_[instrument_id].best_bid();
}

std::optional<price_t> ExchangeSession::best_ask(instrument_id_t instrument_id) const {
    if (instrument_id >= static_cast<instrument_id_t>(books_.size())) return std::nullopt;
    return books_[instrument_id].best_ask();
}

std::optional<int32_t> ExchangeSession::best_bid_slot(instrument_id_t instrument_id) const {
    if (instrument_id >= static_cast<instrument_id_t>(books_.size())) return std::nullopt;
    auto snaps = books_[instrument_id].bids_snapshot();
    if (snaps.empty()) return std::nullopt;
    return snaps.front().player_slot;
}

std::optional<int32_t> ExchangeSession::best_ask_slot(instrument_id_t instrument_id) const {
    if (instrument_id >= static_cast<instrument_id_t>(books_.size())) return std::nullopt;
    auto snaps = books_[instrument_id].asks_snapshot();
    if (snaps.empty()) return std::nullopt;
    return snaps.front().player_slot;
}

std::vector<OrderBook::OrderSnapshot>
ExchangeSession::bids_snapshot(instrument_id_t instrument_id) const {
    if (instrument_id >= static_cast<instrument_id_t>(books_.size())) return {};
    return books_[instrument_id].bids_snapshot();
}

std::vector<OrderBook::OrderSnapshot>
ExchangeSession::asks_snapshot(instrument_id_t instrument_id) const {
    if (instrument_id >= static_cast<instrument_id_t>(books_.size())) return {};
    return books_[instrument_id].asks_snapshot();
}


OrderRejected::Code
ExchangeSession::map_error_code(OrderErrorEvent::Code code) noexcept {
    switch (code) {
        case OrderErrorEvent::Code::PriceOutOfRange: return OrderRejected::Code::PriceOutOfRange;
        case OrderErrorEvent::Code::OrderNotFound:   return OrderRejected::Code::OrderNotFound;
        case OrderErrorEvent::Code::NotYourOrder:    return OrderRejected::Code::NotYourOrder;
        case OrderErrorEvent::Code::SelfTrade:       return OrderRejected::Code::SelfTrade;
    }
    return OrderRejected::Code::OrderNotFound; // unreachable; satisfies compiler
}

ExchangeResult ExchangeSession::translate(
        instrument_id_t instrument_id, std::vector<OrderEvent> events) {
    ExchangeResult result;
    for (auto& ev : events) {
        std::visit(overloaded{
            [&](const OrderAckEvent& ack) {
                seq_t seq = sequencer_.next_seq();
                OrderAck fb;
                fb.order_id      = ack.order_id;
                fb.instrument_id = instrument_id;
                fb.side          = ack.side;
                fb.price         = ack.price;
                fb.player_slot   = ack.player_id;
                result.feedback.push_back(fb);

                OrderAdded added;
                added.order_id      = ack.order_id;
                added.instrument_id = instrument_id;
                added.side          = ack.side;
                added.price         = ack.price;
                added.player_slot   = ack.player_id;
                added.seq           = seq;
                result.market.push_back(added);
            },
            [&](const TradeEvent& trade) {
                seq_t seq = sequencer_.next_seq();
                OrderExecuted exec;
                exec.order_id       = static_cast<int64_t>(seq);
                exec.instrument_id  = instrument_id;
                exec.price          = trade.price;
                exec.aggressor_side = trade.aggressor_side;
                exec.buyer_slot     = trade.buyer_id;
                exec.seller_slot    = trade.seller_id;
                exec.seq            = seq;
                result.market.push_back(exec);
            },
            [&](const BookUpdateEvent& bu) {
                BookUpdated upd;
                upd.instrument_id = instrument_id;
                upd.best_bid      = bu.best_bid;
                upd.best_ask      = bu.best_ask;
                upd.best_bid_slot = bu.best_bid_player_id;
                upd.best_ask_slot = bu.best_ask_player_id;
                result.feedback.push_back(upd);
            },
            [&](const OrderCancelAckEvent& ack) {
                seq_t seq = sequencer_.next_seq();
                CancelAck fb;
                fb.order_id      = ack.order_id;
                fb.instrument_id = instrument_id;
                result.feedback.push_back(fb);

                OrderCancelled cancelled;
                cancelled.order_id      = ack.order_id;
                cancelled.instrument_id = instrument_id;
                cancelled.seq           = seq;
                result.market.push_back(cancelled);
            },
            [&](const OrderErrorEvent& err) {
                OrderRejected rej;
                rej.code    = map_error_code(err.code);
                rej.message = err.message;
                result.feedback.push_back(rej);
            }
        }, ev);
    }
    return result;
}

} // namespace anjeer::exchange
