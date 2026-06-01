#include "exchange/sequencer.h"

namespace anjeer::exchange {

seq_t Sequencer::next_seq() noexcept {
    return ++seq_;
}

void Sequencer::reset() noexcept {
    seq_ = 0;
}

seq_t Sequencer::current_seq() const noexcept {
    return seq_;
}

} // namespace anjeer::exchange
