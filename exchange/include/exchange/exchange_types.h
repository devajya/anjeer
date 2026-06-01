#pragma once

#include <cstdint>

namespace anjeer::exchange {

using instrument_id_t = uint8_t;
using order_id_t      = int64_t;
using price_t         = int32_t;
using seq_t           = uint64_t;

// Side is defined in exchange/ so all market participants follow exchange semantics.
enum class Side { Buy, Sell };

} // namespace anjeer::exchange
