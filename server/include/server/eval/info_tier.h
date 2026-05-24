#pragma once

namespace anjeer::server::eval {

// Visibility tier for data fields used in eval computations.
//
// Before using a field in a cross-player computation or in any output routed
// to a slot other than the field's owner, verify the field's tier is at most
// ObserverDerived. PrivateServer data must only feed self-targeted outputs
// (target_slot == owner slot).
//
// Tiers:
//   Public          — order book, trade suit/price/slots; anyone watching can see it.
//   ObserverDerived — derivable from the public feed by an attentive watcher
//                     (e.g. net delta per player per suit, mirroring the DeltaTable).
//   PrivateServer   — known only to the server (actual hand contents, goal suit,
//                     exact balances). Never use for cross-player inference.
enum class InfoTier {
    Public,
    ObserverDerived,
    PrivateServer,
};

} // namespace anjeer::server::eval
