// Game construction and the StateView serialiser.
//
// Split from game.cpp so the rules engine has no dependency on map generation or on JSON.

#include <algorithm>
#include <cmath>

#include "poly/game.hpp"
#include "poly/mapgen.hpp"

namespace poly {
namespace {







// Mirrors verb_name_public in cli/main.cpp; both must track enum Verb exactly. See the note
// there — this table had the same two-entry gap, so `pending_verb` reported the wrong verb and
// indexed past the end for EndTurn and Deselect.



}  // namespace

bool new_game(const GameConfig& cfg, const Rules& rules, GameState& out, std::string& error) {
  if (cfg.player_count < 2 || cfg.player_count > kMaxPlayers) {
    error = "player_count must be between 2 and " + std::to_string(kMaxPlayers);
    return false;
  }
  // Opponents are bounded by settlement sites, not by the array size.
  const int seats = max_players_for_map(cfg.dims, cfg.land_ratio, rules.village_density);
  if (cfg.player_count > seats) {
    error = "this map seats at most " + std::to_string(seats) + " players";
    return false;
  }

  MapConfig map;
  map.dims = cfg.dims;
  map.seed = cfg.seed;
  map.land_ratio = cfg.land_ratio;
  map.village_density = rules.village_density;
  // How many of its guaranteed resource each capital needs to LEVEL UP on turn 1, if the
  // tribe's starting tech can harvest it at all. Bardur opens with Hunting and must therefore
  // find enough Game to reach level 2; Xin-xi's Metal needs Mining, so it gets one for
  // flavour and no guarantee. Computed from Rules rather than tabulated in mapgen, so the
  // thresholds and yields stay in one place.
  for (int p = 0; p < cfg.player_count; ++p) {
    const TribeStats& tribe = rules.tribe(cfg.tribes[p]);
    int count = 1;
    if (tribe.has_start_tech) {
      // Find a resource this tribe can harvest immediately, and size the guarantee to the
      // population needed for level 2.
      const int needed_population = rules.population_threshold[1];
      for (int r = 1; r < static_cast<int>(Resource::Count); ++r) {
        const ResourceStats& res = rules.resource(static_cast<Resource>(r));
        if (res.population <= 0) continue;
        if (res.needs_tech && res.tech != tribe.start_tech) continue;
        // Covers both routes to population: harvesting directly (Fruit, Game, Fish) and
        // building on the resource (Crop needs a Farm, Metal needs a Mine). Zebasi levels its
        // capital by building a Farm, not by harvesting, so restricting this to `harvestable`
        // silently excluded it.
        count = (needed_population + res.population - 1) / res.population;
        break;
      }
    }
    map.guaranteed_count[p] = count;
  }
  map.ruin_density = rules.ruin_density;
  map.player_count = cfg.player_count;
  for (int p = 0; p < cfg.player_count; ++p) map.tribes[p] = cfg.tribes[p];

  // Generation runs FIRST and owns state initialisation -- it resets GameState wholesale.
  // Anything configured before this call would be silently discarded, which is exactly the
  // bug that made turn limits never fire: games ran to turn 521 against a limit of 60.
  if (!generate_map(map, out, error)) return false;

  out.mode = cfg.mode;
  out.turn_limit = cfg.turn_limit;
  out.fog = cfg.fog;
  out.player_count = static_cast<std::int8_t>(cfg.player_count);
  out.turn = 1;
  out.current_player = 0;

  for (int p = 0; p < cfg.player_count; ++p) {
    Player& player = out.players[p];
    const TribeStats& tribe = rules.tribe(cfg.tribes[p]);
    player.tribe = cfg.tribes[p];
    player.is_human = cfg.is_human[p];
    player.difficulty = cfg.difficulty[p];
    player.alive = true;
    // Starting stars vary BY TRIBE and were previously not set at all, so every tribe began
    // on income alone (resolutions.md C2). Luxidoor's 2 buys its level-3 capital.
    player.stars = tribe.start_stars;
    // A tribe's starting tech is GRANTED FREE, not discounted (live-rules.md §3.3).
    // Luxidoor has none.
    if (tribe.has_start_tech) player.grant_tech(tribe.start_tech);
    player.mark_met(static_cast<PlayerId>(p));  // you always know yourself
  }

  for (PlayerId p = 0; p < out.player_count; ++p) recompute_visibility(out, rules, p);

  // The opening player's turn has to be STARTED, not just entered. Income is credited at
  // turn start (resolutions.md R1), and nothing has changed hands yet, so without this the
  // first player begins the game on zero stars and cannot act at all on turn 1.
  begin_turn(out, rules);

  out.phase = Phase::Select;
  out.selected = kNoTile;
  return true;
}


}  // namespace poly
