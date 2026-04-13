#pragma once

// AGENT-CTX: engine.h is the single public façade for the engine library.
// Consumers should include only this header — never individual module headers.
// Add a new #include here whenever a new engine module is introduced (Slice 3+
// will add game_state.h, Slice 7 will add scoring_engine.h, etc.).
// This keeps consumer include lists stable as the engine grows.

#include "engine/order_book.h"
