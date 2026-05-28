#include "exchange/sequencer.h"

namespace anjeer::exchange {

seq_t Sequencer::next_seq() noexcept {
    return ++seq_;
}

void Sequencer::reset() noexcept {
    seq_ = 0;
}

} // namespace anjeer::exchange
