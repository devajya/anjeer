#pragma once

// Single public façade for the engine library.
// Consumers include only this header — never individual module headers directly.

#include "engine/order_book.h"
#include "engine/suit.h"           // Slice 3: Suit enum, color helpers, suit_name
#include "engine/game_state.h"     // Slice 3: GameState, PlayerHand, DealResult
#include "engine/scoring_engine.h" // Slice 4: ScoringConfig, PlayerResult, RoundResult, score_round
