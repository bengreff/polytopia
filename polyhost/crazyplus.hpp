#pragma once
// Two operations for py/crazyplus (the CrazyPlus evaluation bot, docs/design/crazyplus.md). Both run
// only the game's own commands; neither changes any rule.
//
//   op 40 BOTTURN u32 slot, u32 max  -> u32 n commands played, n x (i16 type, a, b, arg), status block
//         The built-in AI plays the current player's commands until its turn ends (the current
//         player changes), the game ends or `max` commands were played; then bot seats move as
//         after any step. Same result as repeated STEP(STEP_BOT_OWN), without listing the legal
//         commands after every command (the cost of a step), for fast rollouts on cloned states.
//   op 41 STAY    u32 slot, i16 tile -> u8 ok, status block
//         Plays the game's StayCommand for the current player's unit on `tile`: the unit does
//         nothing more this turn (the AI's own way to finish with a unit; a person simply leaves
//         the unit alone). Lets the bot veto the AI's command for a unit and let the AI continue
//         with the other units.

#include <cstdint>
#include <string>
#include <vector>

#include "env.hpp"

namespace crazyplus {

inline bool handles(uint8_t op) { return op == 40 || op == 41; }

template <class Reader, class Writer, class SlotFn, class StatusFn>
void handle(uint8_t op, Reader& r, Writer& w, SlotFn slot, StatusFn status) {
    env::Match& m = slot(r.template get<uint32_t>());
    if (op == 40) {
        uint32_t max = r.template get<uint32_t>();
        uint8_t p = m.currentPlayer();
        uint32_t n = 0;
        std::vector<env::Cmd> played;
        while (!m.ended() && m.currentPlayer() == p && n < max) {
            if (m.finalTurn() && !m.choicePending()) {
                m.endMatch();
                continue;
            }
            Object* cmd = m.botCommand();
            if (!cmd) throw std::runtime_error("BOTTURN: the AI returned no command");
            played.push_back(m.encode(cmd));
            if (!m.run(cmd)) throw std::runtime_error("BOTTURN: AI command rejected: " + m.lastError);
            ++n;
        }
        m.playBots();  // the bot seats' turns, then the legal list
        w.template put<uint32_t>(n);
        for (const env::Cmd& c : played) w.raw(&c, sizeof c);
        status(w, m);
    } else if (op == 41) {
        int16_t tile = r.template get<int16_t>();
        uint8_t p = m.currentPlayer();
        static Class* cStay = cls("GameLogicAssembly", "", "StayCommand");
        static const Method* newStay = method(cStay, ".ctor", 2);
        bool ok = tile >= 0 && tile < m.width * m.height;
        if (ok) {  // only the current player's own unit (the command's own check doesn't look)
            Object* u = nullptr;
            for (Object* t : m.tiles())
                if (m.index(env::at<env::Coord>(t, m.api.tCoords)) == tile) u = env::at<Object*>(t, m.api.tUnit);
            ok = u && env::at<uint8_t>(u, m.api.uOwner) == p;
        }
        env::Coord c{tile % m.width, tile / m.width};
        Object* cmd = object_new(cStay);
        Object* res = nullptr;
        ok = ok && tryCall(newStay, cmd, {&p, &c}, &res) && m.valid(cmd) && m.run(cmd);
        if (ok) m.playBots();
        else m.computeLegal();
        w.template put<uint8_t>(ok);
        status(w, m);
    }
}

}  // namespace crazyplus
