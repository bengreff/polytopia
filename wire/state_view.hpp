// The wire-protocol projection of a GameState. NOT part of the engine's public surface --
// see cli/state_view.cpp for why it moved out of poly/game.hpp.
#pragma once

#include <string>

#include "poly/game.hpp"

namespace poly {

// Serialises a StateView per docs/wire-protocol.md. `viewer` = kNoPlayer for full
// observability. Fog is applied HERE, at serialisation, so a UI bug cannot leak the map.
std::string state_view_json(const GameState& s, const Rules& rules, PlayerId viewer);

// One engine Event as the sentence a player reads.
//
// The wording lives HERE, not in game.cpp. The engine reports what happened as data; deciding
// how to say it -- how much detail, what language, whether to say it at all -- is presentation,
// and it should not require rebuilding the rules to change. The browser previously recovered
// structured fields by regex-matching these sentences, which made an English string with a
// double hyphen in it the real engine/UI contract.
std::string event_text(const Event& e, const Rules& rules);

// The structured form, so a client can act on the fields instead of parsing the prose. Both are
// sent: `text` for display, the rest for logic.
std::string event_json(const Event& e, const Rules& rules);

}  // namespace poly
