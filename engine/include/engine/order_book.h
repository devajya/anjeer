#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// AGENT-CTX: engine headers must never include network, I/O, or server headers.
// The engine is a pure-logic library. All coupling to uWS, nlohmann, or sockets
// belongs in server/. This boundary is what makes actor-model wrapping possible
// in Slice 6 — see dev-environment.md "Slice 6" and cpp_performance_rules.md
// "Separate the network thread from the game loop thread".

namespace anjeer::engine {

// ---------------------------------------------------------------------------
// Side
// AGENT-CTX: Declared at namespace scope (not inside OrderBook) because Side
// will be reused by GameState, ScoringEngine, and eval modules in later slices.
// Using enum class (scoped) to prevent implicit int conversion bugs.
// ---------------------------------------------------------------------------
enum class Side { Buy, Sell };

// ---------------------------------------------------------------------------
// Event types returned by OrderBook operations.
//
// Design choice — std::variant over virtual dispatch:
// AGENT-CTX: cpp_performance_rules.md: "avoid virtual dispatch in hot paths".
// std::variant + std::visit compiles to a jump table or inline branches; vtable
// dispatch requires an indirect call that blocks inlining. For the small fixed
// set of event types here, variant is the right tool.
// If the event type set ever grows beyond ~8 variants, profile before keeping
// this shape — very large variants can increase binary size noticeably.
//
// Ownership: the server owns serialisation. Events carry only engine-domain data.
// AGENT-CTX: Never add JSON, string formatting, or WS handles to event structs.
// That coupling would make the engine un-testable and un-threadable.
//
// Threading (Slice 6+): events will be produced on the game-session thread and
// posted back to the uWS event-loop thread via Loop::defer(). The event structs
// are trivially copyable (OrderAckEvent, TradeEvent, BookUpdateEvent) or have
// value semantics (OrderErrorEvent has a std::string), so posting copies is safe.
// ---------------------------------------------------------------------------

// Confirms a successfully placed order. Sent only to the submitting client.
// AGENT-CTX: suit is included so the server can route this event without a
// separate lookup when multiple books are active in Slice 3+.
struct OrderAckEvent {
    int64_t     order_id;
    int32_t     player_id;
    Side        side;
    int32_t     price;
    std::string suit;
};

// Emitted when a buy and sell cross. Sent to all clients; the server personalises
// your_side per recipient before serialising.
// AGENT-CTX: qty is intentionally absent. Every trade is for exactly 1 card in
// the current game mechanic (Slice 2 resolution). To add multi-card support:
//   1. Insert `int32_t qty` here.
//   2. Add qty to the wire protocol (server → client "trade" message).
//   3. Update matching logic in OrderBook::try_match() for partial fills.
//   4. Update all callers and tests.
// Until then, qty=1 is implied and the wire message omits the field.
struct TradeEvent {
    std::string suit;
    int32_t     price;
    int32_t     buyer_id;
    int32_t     seller_id;
    Side        aggressor_side;
};

// Broadcast to all clients after every mutation that changes best bid or ask.
// best_bid / best_ask are nullopt when no orders exist on that side.
// AGENT-CTX: Only best bid and best ask are broadcast in Slice 2 (per resolution 5).
// Full depth was deliberately deferred — at 5 players each with 1 resting order
// per suit, depth adds nothing over best-price. If Slice 8 introduces a depth
// display, add `std::vector<PriceLevel> bid_depth, ask_depth` here and update
// the wire protocol.
struct BookUpdateEvent {
    std::string            suit;
    std::optional<int32_t> best_bid;
    std::optional<int32_t> best_ask;
};

// Confirms a successfully cancelled order. Sent only to the cancelling client.
struct OrderCancelAckEvent {
    int64_t order_id;
};

// Returned when an operation fails validation. Sent only to the relevant client.
// AGENT-CTX: error codes are a closed enum (not strings) so the server can
// serialise them to stable wire-protocol strings and callers can switch exhaustively.
// Add codes here when new engine-level validation rules are introduced.
// Server-layer rejections (unknown suit, malformed JSON) are handled by the server
// before the engine is called and are sent as literal strings — they do not belong
// in this enum.
struct OrderErrorEvent {
    enum class Code {
        PriceOutOfRange,
        OrderNotFound,
        NotYourOrder,
    };
    Code        code;
    std::string message;  // human-readable; may change — do not parse on the client
};

// AGENT-CTX: OrderEvent is the complete set of results an OrderBook operation can
// produce. A single call to submit() / nudge() / cancel() returns a
// std::vector<OrderEvent> which may contain multiple events
// (e.g. OrderAck + TradeEvent + BookUpdateEvent).
// Callers use std::visit or std::get_if to handle each event type.
// The set of variants is closed — add a new struct above and a new variant here
// whenever a new event type is needed. Do not use inheritance.
using OrderEvent = std::variant<
    OrderAckEvent,
    TradeEvent,
    BookUpdateEvent,
    OrderCancelAckEvent,
    OrderErrorEvent
>;

// ---------------------------------------------------------------------------
// OrderBook — single-suit, price-time priority limit order book.
//
// Invariants (must hold after every public method call):
//   - bids_ is sorted descending by price, ascending by id on ties (FIFO within price).
//   - asks_ is sorted ascending by price, ascending by id on ties.
//   - No order in bids_ has price >= any order in asks_ (i.e. no resting crossed book).
//     The only time bid >= ask is transiently during try_match(), which resolves it
//     immediately.
//   - Every order has a unique id > 0.
//   - All prices satisfy min_price <= price <= max_price.
//
// WIPE CONTRACT (global mechanic, Slice 2–?):
// AGENT-CTX: When any submit() or nudge() result vector contains a TradeEvent, the
// CALLER must call wipe() on ALL active OrderBook instances (one per suit).
// The engine does NOT self-wipe after a trade; it only removes the two matched
// orders. The global wipe is a game-mechanic policy triggered by the server (or game
// session in Slice 6+), but executed by the engine via wipe().
// wipe() clears all resting orders and returns a BookUpdateEvent{suit, nullopt, nullopt}
// so the server only needs to dispatch those events — it never constructs post-wipe
// book updates itself. This separation means:
//   - Future: per-suit-only wipe = caller skips wipe() on other books.
//   - Future: multi-card partial fill = caller decides whether wipe is triggered.
// Never make OrderBook auto-wipe — that would couple game policy to the engine.
//
// THREADING:
// AGENT-CTX: OrderBook is NOT thread-safe. In Slice 2, all calls happen on the uWS
// event-loop thread (safe). In Slice 6+, OrderBook lives inside its GameSession
// thread and is never touched from outside — thread safety is provided by the
// command-queue wrapper, not by internal locking. Do not add mutexes here.
// ---------------------------------------------------------------------------
class OrderBook {
public:
    // AGENT-CTX: Config mirrors ServerConfig::OrderBookConfig but belongs to the
    // engine (no JSON, no std::vector<std::string> active_suits). The server
    // constructs one Config per active suit and passes it here. If a new config
    // knob is added, update ServerConfig::OrderBookConfig and config.cpp too.
    struct Config {
        int32_t     min_price               = 1;
        int32_t     max_price               = 99;
        // AGENT-CTX: nudge_initial prices are the fallback when a player nudges
        // and no orders exist on that side. Keeping them in Config (rather than
        // hard-coding 1/99) lets the game designer control the opening price
        // without a code change.
        int32_t     nudge_initial_buy_price  = 1;
        int32_t     nudge_initial_sell_price = 99;
        // AGENT-CTX: suit is stored in Config so every event produced by this book
        // carries the correct suit label without a caller-side lookup.
        std::string suit                    = "S1";
    };

    // AGENT-CTX: Constructor takes Config by value. At game scale this is called
    // once per suit at session start — not a hot path. std::string in Config makes
    // it non-trivial; taking by value allows the caller to std::move if desired.
    explicit OrderBook(Config cfg);

    // Submit a new limit order.
    // Returns: [OrderAckEvent] + optional [TradeEvent, BookUpdateEvent]
    //       or: [OrderErrorEvent] on validation failure.
    // AGENT-CTX: If the result contains a TradeEvent, the caller MUST invoke
    // wipe() on ALL OrderBook instances (see WIPE CONTRACT above).
    // The matched orders are already removed from this book; wipe() removes
    // any remaining resting orders from all books and returns BookUpdateEvents.
    // [[nodiscard]] enforces that callers process the events — silently dropping
    // a TradeEvent would leave a globally inconsistent book state.
    [[nodiscard]] std::vector<OrderEvent> submit(int32_t player_id, Side side, int32_t price);

    // Cancel a resting order by ID.
    // Returns: [OrderCancelAckEvent, BookUpdateEvent]
    //       or: [OrderErrorEvent] (OrderNotFound or NotYourOrder).
    // AGENT-CTX: player_id is required so the engine can enforce that only the
    // order's owner can cancel it. Server must pass the authenticated player_id,
    // never the client-reported value.
    [[nodiscard]] std::vector<OrderEvent> cancel(int64_t order_id, int32_t player_id);

    // Place a new order at best_bid+1 (buy) or best_ask-1 (sell).
    // Falls back to nudge_initial_buy_price (if no bids) or nudge_initial_sell_price
    // (if no asks). Price is clamped to [min_price, max_price].
    // AGENT-CTX: Nudge always creates a NEW order (Option B per Slice 2 resolution).
    // It does NOT modify an existing order. The resulting OrderAckEvent contains the
    // new order_id which the client must store for any future cancel. This guarantees
    // every resting order has a known owner and is cancellable, even after nudging
    // into an empty book.
    // Returns same shape as submit().
    [[nodiscard]] std::vector<OrderEvent> nudge(Side side, int32_t player_id);

    // Remove all resting orders from this book and return a BookUpdateEvent
    // with null best_bid / best_ask (the post-wipe state).
    // Called by the server on ALL active books after any TradeEvent (global wipe).
    // AGENT-CTX: wipe() owns both the state mutation (clearing bids/asks) and the
    // event emission (BookUpdateEvent{suit, nullopt, nullopt}). The server calls
    // wipe() on each book and dispatches the returned events to clients — it never
    // constructs post-wipe book updates itself. [[nodiscard]] ensures the caller
    // broadcasts the events rather than silently discarding them.
    [[nodiscard]] std::vector<OrderEvent> wipe();

    // AGENT-CTX: best_bid / best_ask return by value (std::optional<int32_t> is
    // cheap to copy). [[nodiscard]] prevents accidental discard of the result.
    [[nodiscard]] std::optional<int32_t> best_bid() const noexcept;
    [[nodiscard]] std::optional<int32_t> best_ask() const noexcept;

private:
    // AGENT-CTX: Order is a private implementation detail. Callers never construct
    // Orders directly — they call submit()/nudge() and receive an order_id via
    // OrderAckEvent. This keeps the public interface clean and prevents callers
    // from bypassing price validation.
    // qty is omitted (always 1 in current mechanic). See TradeEvent AGENT-CTX.
    struct Order {
        int64_t id;
        int32_t player_id;
        Side    side;
        int32_t price;
    };

    Config  cfg_;
    // AGENT-CTX: next_id_ starts at 1. ID 0 is reserved as a sentinel for
    // "no order" in future data structures. Increment before assigning so the
    // first issued ID is 1, not 0.
    int64_t next_id_{1};

    // AGENT-CTX: Bids and asks stored as sorted std::vector (not std::map or
    // std::set). Rationale from cpp_performance_rules.md:
    //   - "prefer std::vector — contiguous, cache-friendly, fast iteration"
    //   - "use sorted std::vector + std::binary_search instead of std::set for
    //     read-heavy sets"
    // At game scale (≤5 players, ≤1 resting order per player per suit) the book
    // has at most ~5 entries. Linear scan is faster than tree traversal at this
    // size due to cache locality. If depth grows to hundreds of orders (unlikely
    // for this game), profile before switching to a priority_queue or map.
    //
    // Sort invariant: bids_ descending price, ascending id on ties.
    //                 asks_ ascending price, ascending id on ties.
    std::vector<Order> bids_;
    std::vector<Order> asks_;

    // Check whether best_bid >= best_ask and return a TradeEvent if so.
    // Removes the matched orders from bids_ and asks_.
    // AGENT-CTX: Returns std::optional (not a bool + out-param) to keep the
    // call site readable and avoid two-phase "did it match?" + "get the event".
    std::optional<TradeEvent> try_match();

    // Construct a BookUpdateEvent snapshot for the current book state.
    // Called after every mutation so the server has up-to-date data to broadcast.
    [[nodiscard]] BookUpdateEvent make_book_update() const;

    // Returns true if price is within [min_price, max_price].
    [[nodiscard]] bool is_valid_price(int32_t price) const noexcept;
};

} // namespace anjeer::engine
