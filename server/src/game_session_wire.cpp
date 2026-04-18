#include "server/game_session_wire.h"

namespace anjeer::server {

WsErrorCode to_ws_error_code(engine::OrderErrorEvent::Code c) noexcept {
    switch (c) {
        case engine::OrderErrorEvent::Code::PriceOutOfRange: return WsErrorCode::PriceOutOfRange;
        case engine::OrderErrorEvent::Code::OrderNotFound:   return WsErrorCode::OrderNotFound;
        case engine::OrderErrorEvent::Code::NotYourOrder:    return WsErrorCode::NotYourOrder;
    }
    return WsErrorCode::PriceOutOfRange;  // unreachable; silences -Wreturn-type
}

namespace serialise {

std::string error_code_str(WsErrorCode c) noexcept {
    switch (c) {
        case WsErrorCode::PriceOutOfRange:     return "PRICE_OUT_OF_RANGE";
        case WsErrorCode::OrderNotFound:       return "ORDER_NOT_FOUND";
        case WsErrorCode::NotYourOrder:        return "NOT_YOUR_ORDER";
        case WsErrorCode::UnknownSuit:         return "UNKNOWN_SUIT";
        case WsErrorCode::MalformedMessage:    return "MALFORMED_MESSAGE";
        case WsErrorCode::ServerFull:          return "SERVER_FULL";
        case WsErrorCode::RoundNotActive:      return "ROUND_NOT_ACTIVE";
        case WsErrorCode::InsufficientBalance: return "INSUFFICIENT_BALANCE";
    }
    return "UNKNOWN_ERROR";
}

std::string side(engine::Side s) noexcept {
    return s == engine::Side::Buy ? "buy" : "sell";
}

std::string opt_price(std::optional<int32_t> p) noexcept {
    return p.has_value() ? std::to_string(*p) : "null";
}

std::string book_update_payload(
        const std::string&     suit,
        std::optional<int32_t> best_bid,
        std::optional<int32_t> best_ask) {
    const nlohmann::json bid_val =
        best_bid.has_value() ? nlohmann::json(*best_bid) : nlohmann::json(nullptr);
    const nlohmann::json ask_val =
        best_ask.has_value() ? nlohmann::json(*best_ask) : nlohmann::json(nullptr);
    return nlohmann::json{
        {"type",     "book_update"},
        {"suit",     suit},
        {"best_bid", bid_val},
        {"best_ask", ask_val},
    }.dump();
}

void error(WsHandle ws,
           WsErrorCode code,
           std::string_view msg,
           Logger& slog) {
    const std::string code_str = error_code_str(code);
    const std::string payload =
        nlohmann::json{{"type","error"},{"code",code_str},{"message",msg}}.dump();
    ws->send(payload, uWS::OpCode::TEXT);
    slog.warn("send",
              "player=" + std::to_string(ws->getUserData()->player_slot) +
              " type=error code=" + code_str +
              " message=" + std::string(msg));
}

void book_update(
        const std::set<WsHandle>& conns,
        const std::string&        suit,
        std::optional<int32_t>    best_bid,
        std::optional<int32_t>    best_ask,
        Logger&                   slog) {
    const std::string payload = book_update_payload(suit, best_bid, best_ask);
    for (WsHandle c : conns) c->send(payload, uWS::OpCode::TEXT);
    slog.info("broadcast",
              "type=book_update suit=" + suit +
              " best_bid=" + opt_price(best_bid) +
              " best_ask=" + opt_price(best_ask) +
              " recipients=" + std::to_string(conns.size()));
}

void trade(
        const std::set<WsHandle>& conns,
        const engine::TradeEvent& t,
        Logger&                   slog) {
    slog.info("broadcast",
              "type=trade suit=" + t.suit +
              " price=" + std::to_string(t.price) +
              " aggressor=" + side(t.aggressor_side) +
              " buyer_id=" + std::to_string(t.buyer_id) +
              " seller_id=" + std::to_string(t.seller_id) +
              " recipients=" + std::to_string(conns.size()));

    for (WsHandle c : conns) {
        const int32_t pid = c->getUserData()->player_slot;
        nlohmann::json your_side = nullptr;
        if (pid == t.buyer_id)  your_side = "buy";
        if (pid == t.seller_id) your_side = "sell";

        const std::string payload = nlohmann::json{
            {"type",           "trade"},
            {"suit",           t.suit},
            {"price",          t.price},
            {"aggressor_side", side(t.aggressor_side)},
            {"your_side",      your_side},
        }.dump();
        c->send(payload, uWS::OpCode::TEXT);
    }
}

std::string round_end_payload(
        const engine::RoundResult& result,
        const std::vector<int>&    available_cash) {
    auto arr = nlohmann::json::array();
    for (const auto& pr : result.player_results) {
        arr.push_back({
            {"player_slot",     pr.player_slot},
            {"goal_cards_held", pr.goal_cards_held},
            {"payout",          pr.payout},
            {"balance",         available_cash[pr.player_slot]},
            {"disconnected",    pr.disconnected},
        });
    }
    return nlohmann::json{
        {"type",      "round_end"},
        {"goal_suit", std::string(engine::suit_name(result.goal_suit))},
        {"results",   arr},
    }.dump();
}

std::string round_start_payload(
        int                       slot,
        const engine::PlayerHand& hand,
        const std::string&        round_end_at,
        int                       effective_balance) {
    return nlohmann::json{
        {"type",         "round_start"},
        {"player_slot",  slot},
        {"round_end_at", round_end_at},
        {"balance",      effective_balance},
        {"hand", {
            {"clubs",    hand.suit_counts[engine::suit_index(engine::Suit::Clubs)]},
            {"diamonds", hand.suit_counts[engine::suit_index(engine::Suit::Diamonds)]},
            {"hearts",   hand.suit_counts[engine::suit_index(engine::Suit::Hearts)]},
            {"spades",   hand.suit_counts[engine::suit_index(engine::Suit::Spades)]},
        }},
    }.dump();
}

} // namespace serialise

namespace parse {

std::optional<SubmitOrderFields> submit_order(const nlohmann::json& j) {
    try {
        return SubmitOrderFields{
            j.at("suit").get<std::string>(),
            j.at("side").get<std::string>(),
            j.at("price").get<int32_t>(),
        };
    } catch (const nlohmann::json::exception&) { return std::nullopt; }
}

std::optional<NudgeFields> nudge(const nlohmann::json& j) {
    try {
        return NudgeFields{
            j.at("suit").get<std::string>(),
            j.at("side").get<std::string>(),
        };
    } catch (const nlohmann::json::exception&) { return std::nullopt; }
}

std::optional<CancelFields> cancel_order(const nlohmann::json& j) {
    try {
        return CancelFields{ j.at("order_id").get<int64_t>() };
    } catch (const nlohmann::json::exception&) { return std::nullopt; }
}

std::optional<engine::Side> side(const std::string& s) noexcept {
    if (s == "buy")  return engine::Side::Buy;
    if (s == "sell") return engine::Side::Sell;
    return std::nullopt;
}

} // namespace parse

} // namespace anjeer::server
