#pragma once

// Single public façade for the engine library.
// Consumers include only this header — never individual module headers directly.

#include "engine/order_book.h"
#include "engine/suit.h"           // Slice 3: Suit enum, color helpers, suit_name
#include "engine/game_state.h"     // Slice 3: GameState, PlayerHand, DealResult
#include "engine/scoring_engine.h" // Slice 4: ScoringConfig, PlayerResult, RoundResult, score_round
#include "engine/game_snapshot.h"  // Slice 11: DeckSpec, TradeRecord, BookSnapshot, GameStateSnapshot
#include "engine/bots/bot_types.h" // Slice 10: BotConfig, BotAction, BotEvent
#include "engine/bots/bot_agent.h" // Slice 10: BotAgent base class + make_bot() factory
