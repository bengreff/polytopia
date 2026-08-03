#include "poly/rules.hpp"

#include <cmath>
#include <unordered_map>

#include "poly/json.hpp"

namespace poly {
namespace {

constexpr std::uint32_t kField = terrain_bit(Terrain::Field);
constexpr std::uint32_t kForest = terrain_bit(Terrain::Forest);
constexpr std::uint32_t kMountain = terrain_bit(Terrain::Mountain);
constexpr std::uint32_t kWater = terrain_bit(Terrain::Water);
constexpr std::uint32_t kOcean = terrain_bit(Terrain::Ocean);

constexpr std::uint32_t sk(Skill s) { return static_cast<std::uint32_t>(s); }

void set_unit(Rules& r, UnitType type, UnitStats stats) {
  r.units[static_cast<int>(type)] = stats;
}

void set_improvement(Rules& r, Improvement imp, ImprovementStats stats) {
  r.improvements[static_cast<int>(imp)] = stats;
}

void set_resource(Rules& r, Resource res, ResourceStats stats) {
  r.resources[static_cast<int>(res)] = stats;
}

void set_tribe(Rules& r, Tribe t, Tech start_tech, UnitType start_unit, int capital_level = 1) {
  r.tribes[static_cast<int>(t)] = TribeStats{start_tech, start_unit, capital_level};
}

// --- name tables, so JSON keys map onto enums -------------------------------------------

const std::unordered_map<std::string, UnitType>& unit_names() {
  static const std::unordered_map<std::string, UnitType> m = {
      {"warrior", UnitType::Warrior},   {"rider", UnitType::Rider},
      {"archer", UnitType::Archer},     {"defender", UnitType::Defender},
      {"swordsman", UnitType::Swordsman}, {"mind_bender", UnitType::MindBender},
      {"catapult", UnitType::Catapult}, {"knight", UnitType::Knight},
      {"cloak", UnitType::Cloak},       {"dagger", UnitType::Dagger},
      {"giant", UnitType::Giant},       {"explorer", UnitType::Explorer},
      {"raft", UnitType::Raft},         {"scout", UnitType::Scout},
      {"rammer", UnitType::Rammer},     {"bomber", UnitType::Bomber},
      {"juggernaut", UnitType::Juggernaut}, {"dinghy", UnitType::Dinghy},
      {"pirate", UnitType::Pirate},
  };
  return m;
}

const std::unordered_map<std::string, Tech>& tech_names() {
  static const std::unordered_map<std::string, Tech> m = {
      {"climbing", Tech::Climbing},       {"mining", Tech::Mining},
      {"smithery", Tech::Smithery},       {"meditation", Tech::Meditation},
      {"philosophy", Tech::Philosophy},   {"fishing", Tech::Fishing},
      {"sailing", Tech::Sailing},         {"navigation", Tech::Navigation},
      {"ramming", Tech::Ramming},         {"aquatism", Tech::Aquatism},
      {"hunting", Tech::Hunting},         {"archery", Tech::Archery},
      {"spiritualism", Tech::Spiritualism}, {"forestry", Tech::Forestry},
      {"mathematics", Tech::Mathematics}, {"organization", Tech::Organization},
      {"farming", Tech::Farming},         {"construction", Tech::Construction},
      {"strategy", Tech::Strategy},       {"diplomacy", Tech::Diplomacy},
      {"riding", Tech::Riding},           {"roads", Tech::Roads},
      {"trade", Tech::Trade},             {"free_spirit", Tech::FreeSpirit},
      {"chivalry", Tech::Chivalry},
  };
  return m;
}

const std::unordered_map<std::string, Skill>& skill_names() {
  static const std::unordered_map<std::string, Skill> m = {
      {"dash", Skill::Dash},         {"escape", Skill::Escape},
      {"fortify", Skill::Fortify},   {"persist", Skill::Persist},
      {"heal", Skill::Heal},         {"convert", Skill::Convert},
      {"scout", Skill::Scout},       {"hide", Skill::Hide},
      {"infiltrate", Skill::Infiltrate}, {"creep", Skill::Creep},
      {"stiff", Skill::Stiff},       {"static", Skill::Static},
      {"surprise", Skill::Surprise}, {"independent", Skill::Independent},
      {"stomp", Skill::Stomp},       {"splash", Skill::Splash},
      {"carry", Skill::Carry},       {"navigate", Skill::Navigate},
  };
  return m;
}

const std::unordered_map<std::string, Improvement>& improvement_names() {
  static const std::unordered_map<std::string, Improvement> m = {
      {"lumber_hut", Improvement::LumberHut}, {"sawmill", Improvement::Sawmill},
      {"farm", Improvement::Farm},            {"windmill", Improvement::Windmill},
      {"mine", Improvement::Mine},            {"forge", Improvement::Forge},
      {"port", Improvement::Port},            {"market", Improvement::Market},
      {"temple", Improvement::Temple},        {"forest_temple", Improvement::ForestTemple},
      {"mountain_temple", Improvement::MountainTemple},
      {"water_temple", Improvement::WaterTemple},
  };
  return m;
}

const std::unordered_map<std::string, Resource>& resource_names() {
  static const std::unordered_map<std::string, Resource> m = {
      {"fruit", Resource::Fruit},   {"crop", Resource::Crop},
      {"game", Resource::Game},     {"fish", Resource::Fish},
      {"starfish", Resource::Starfish}, {"metal", Resource::Metal},
      {"ruin", Resource::Ruin},     {"village", Resource::Village},
      {"lighthouse", Resource::Lighthouse},
  };
  return m;
}

const std::unordered_map<std::string, Tribe>& tribe_names() {
  static const std::unordered_map<std::string, Tribe> m = {
      {"xin_xi", Tribe::XinXi},     {"imperius", Tribe::Imperius},
      {"bardur", Tribe::Bardur},    {"oumaji", Tribe::Oumaji},
      {"kickoo", Tribe::Kickoo},    {"hoodrick", Tribe::Hoodrick},
      {"luxidoor", Tribe::Luxidoor}, {"vengir", Tribe::Vengir},
      {"zebasi", Tribe::Zebasi},    {"ai_mo", Tribe::AiMo},
      {"quetzali", Tribe::Quetzali}, {"yadakk", Tribe::Yadakk},
  };
  return m;
}

const std::unordered_map<std::string, Terrain>& terrain_names() {
  static const std::unordered_map<std::string, Terrain> m = {
      {"field", Terrain::Field},     {"forest", Terrain::Forest},
      {"mountain", Terrain::Mountain}, {"water", Terrain::Water},
      {"ocean", Terrain::Ocean},
  };
  return m;
}

template <typename T>
bool lookup(const std::unordered_map<std::string, T>& table, const std::string& key, T& out) {
  const auto it = table.find(key);
  if (it == table.end()) return false;
  out = it->second;
  return true;
}

}  // namespace

Rules Rules::defaults() {
  Rules r;

  // --- Land units (live-rules.md §2.1). These are CORROBORATED: the 9 regular land units
  // show triple agreement across the wiki, PolyEnv's Units.json, and PolyEnv's own test
  // table. Treat as settled.
  set_unit(r, UnitType::Warrior,
           {2, 10, 2.0f, 2.0f, 1, 1, 1, Tech::Climbing, false, true, true,
            sk(Skill::Dash) | sk(Skill::Fortify)});
  set_unit(r, UnitType::Rider,
           {3, 10, 2.0f, 1.0f, 2, 1, 1, Tech::Riding, true, true, true,
            sk(Skill::Dash) | sk(Skill::Escape) | sk(Skill::Fortify)});
  set_unit(r, UnitType::Archer,
           {3, 10, 2.0f, 1.0f, 1, 2, 1, Tech::Archery, true, true, true,
            sk(Skill::Dash) | sk(Skill::Fortify)});
  set_unit(r, UnitType::Defender,
           {3, 15, 1.0f, 3.0f, 1, 1, 1, Tech::Strategy, true, true, true, sk(Skill::Fortify)});
  // Swordsman has NO Fortify -- removed in Path of the Ocean 2.8. The single most commonly
  // stale unit fact in every source consulted.
  set_unit(r, UnitType::Swordsman,
           {5, 15, 3.0f, 3.0f, 1, 1, 1, Tech::Smithery, true, true, true, sk(Skill::Dash)});
  set_unit(r, UnitType::MindBender,
           {5, 10, 0.0f, 1.0f, 1, 1, 1, Tech::Philosophy, true, true, true,
            sk(Skill::Heal) | sk(Skill::Convert) | sk(Skill::Stiff)});
  set_unit(r, UnitType::Catapult,
           {8, 10, 4.0f, 0.0f, 1, 3, 1, Tech::Mathematics, true, true, true, sk(Skill::Stiff)});
  set_unit(r, UnitType::Knight,
           {8, 10, 3.5f, 1.0f, 3, 1, 1, Tech::Chivalry, true, true, true,
            sk(Skill::Dash) | sk(Skill::Persist) | sk(Skill::Fortify)});
  set_unit(r, UnitType::Cloak,
           {8, 5, 2.0f, 0.5f, 2, 1, 2, Tech::Diplomacy, true, true, false,
            sk(Skill::Hide) | sk(Skill::Infiltrate) | sk(Skill::Dash) | sk(Skill::Scout) |
                sk(Skill::Creep) | sk(Skill::Stiff) | sk(Skill::Static)});
  set_unit(r, UnitType::Dagger,
           {2, 10, 2.0f, 2.0f, 1, 1, 1, Tech::Diplomacy, false, false, false,
            sk(Skill::Dash) | sk(Skill::Surprise) | sk(Skill::Independent) | sk(Skill::Static)});
  set_unit(r, UnitType::Giant,
           {10, 40, 5.0f, 4.0f, 1, 1, 1, Tech::Climbing, false, false, false, sk(Skill::Static)});
  set_unit(r, UnitType::Explorer,
           {0, 10, 0.0f, 0.0f, 12, 0, 1, Tech::Climbing, false, false, false, sk(Skill::Static)});

  // --- Naval (live-rules.md §2.2). A hull carries NO HP of its own: an embarked unit is the
  // SAME entity and damage carries both ways (resolutions.md R29), so `hp` here is unused
  // and the land unit's HP is authoritative.
  //
  // Only Bomber attack (3, nerfed in 2.8.5), Dinghy and Pirate are confirmed to me directly;
  // the rest of the naval line comes from the JSON, which is authoritative.
  set_unit(r, UnitType::Raft,
           {0, 0, 0.0f, 1.0f, 2, 0, 1, Tech::Fishing, true, false, false, sk(Skill::Carry)});
  set_unit(r, UnitType::Scout,
           {5, 0, 2.0f, 1.0f, 3, 2, 2, Tech::Sailing, true, false, true,
            sk(Skill::Carry) | sk(Skill::Scout) | sk(Skill::Navigate)});
  set_unit(r, UnitType::Rammer,
           {5, 0, 3.0f, 3.0f, 3, 1, 1, Tech::Ramming, true, false, true,
            sk(Skill::Carry) | sk(Skill::Navigate)});
  set_unit(r, UnitType::Bomber,
           {15, 0, 3.0f, 2.0f, 2, 3, 1, Tech::Aquatism, true, false, true,
            sk(Skill::Carry) | sk(Skill::Splash) | sk(Skill::Navigate) | sk(Skill::Stiff)});
  set_unit(r, UnitType::Juggernaut,
           {10, 40, 4.0f, 4.0f, 2, 1, 1, Tech::Aquatism, false, false, false,
            sk(Skill::Carry) | sk(Skill::Stomp) | sk(Skill::Static) | sk(Skill::Navigate)});
  set_unit(r, UnitType::Dinghy,
           {0, 5, 2.0f, 0.5f, 2, 1, 2, Tech::Fishing, false, false, false,
            sk(Skill::Hide) | sk(Skill::Infiltrate) | sk(Skill::Scout) | sk(Skill::Static)});
  set_unit(r, UnitType::Pirate,
           {0, 10, 2.0f, 2.0f, 2, 1, 1, Tech::Fishing, false, false, false,
            sk(Skill::Surprise) | sk(Skill::Independent) | sk(Skill::Static)});

  // --- Improvements (live-rules.md §5.1) ---
  set_improvement(r, Improvement::LumberHut,
                  {3, 1, Tech::Forestry, true, kForest, false, Resource::None, 0,
                   Improvement::None, 0});
  set_improvement(r, Improvement::Sawmill,
                  {5, 0, Tech::Mathematics, true, kField, false, Resource::None, 1,
                   Improvement::LumberHut, 1});
  set_improvement(r, Improvement::Farm,
                  {5, 2, Tech::Farming, true, kField, true, Resource::Crop, 0,
                   Improvement::None, 0});
  set_improvement(r, Improvement::Windmill,
                  {5, 0, Tech::Construction, true, kField, false, Resource::None, 1,
                   Improvement::Farm, 1});
  set_improvement(r, Improvement::Mine,
                  {5, 2, Tech::Mining, true, kMountain, true, Resource::Metal, 0,
                   Improvement::None, 0});
  // Forge: Field OR Forest, and NO adjacency prerequisite for placement -- the Buildings
  // page's "adjacent to a Lumber Hut" is a copy-paste error from the Sawmill row.
  // Adjacency affects output only.
  set_improvement(r, Improvement::Forge,
                  {5, 0, Tech::Smithery, true, kField | kForest, false, Resource::None, 1,
                   Improvement::Mine, 2});
  // Port's tech is Fishing (T1), NOT Sailing. Two wiki pages say Sailing and both are stale.
  set_improvement(r, Improvement::Port,
                  {7, 1, Tech::Fishing, true, kWater, false, Resource::None, 0,
                   Improvement::None, 0});
  // Market: 1 per city (owner-verified). Income is the SUM OF LEVELS of adjacent
  // Sawmill/Windmill/Forge (resolutions.md R21) -- handled in economy, not as population.
  set_improvement(r, Improvement::Market,
                  {5, 0, Tech::Trade, true, kField, false, Resource::None, 1,
                   Improvement::None, 0});
  set_improvement(r, Improvement::Temple,
                  {20, 1, Tech::FreeSpirit, true, kField, false, Resource::None, 0,
                   Improvement::None, 0});
  set_improvement(r, Improvement::ForestTemple,
                  {15, 1, Tech::Spiritualism, true, kForest, false, Resource::None, 0,
                   Improvement::None, 0});
  set_improvement(r, Improvement::MountainTemple,
                  {20, 1, Tech::Meditation, true, kMountain, false, Resource::None, 0,
                   Improvement::None, 0});
  set_improvement(r, Improvement::WaterTemple,
                  {20, 1, Tech::Aquatism, true, kWater | kOcean, false, Resource::None, 0,
                   Improvement::None, 0});

  // --- Resources (live-rules.md §6.2) ---
  set_resource(r, Resource::Fruit, {2, 1, 0, Tech::Organization, true, true});
  set_resource(r, Resource::Crop, {5, 2, 0, Tech::Farming, true, false});  // via Farm
  set_resource(r, Resource::Game, {2, 1, 0, Tech::Hunting, true, true});
  set_resource(r, Resource::Fish, {2, 1, 0, Tech::Fishing, true, true});
  set_resource(r, Resource::Starfish, {0, 0, 8, Tech::Navigation, true, true});
  set_resource(r, Resource::Metal, {5, 2, 0, Tech::Mining, true, false});  // via Mine
  set_resource(r, Resource::Ruin, {0, 0, 0, Tech::Climbing, false, false});
  set_resource(r, Resource::Village, {0, 0, 0, Tech::Climbing, false, false});
  set_resource(r, Resource::Lighthouse, {0, 1, 0, Tech::Climbing, false, false});

  // --- Tribes (live-rules.md §10.1). Luxidoor's capital starts at level 3 (R12), derived
  // from a turn-0 score identity that reproduces all 16 published turn-0 scores.
  set_tribe(r, Tribe::XinXi, Tech::Climbing, UnitType::Warrior);
  set_tribe(r, Tribe::Imperius, Tech::Organization, UnitType::Warrior);
  set_tribe(r, Tribe::Bardur, Tech::Hunting, UnitType::Warrior);
  set_tribe(r, Tribe::Oumaji, Tech::Riding, UnitType::Rider);
  set_tribe(r, Tribe::Kickoo, Tech::Fishing, UnitType::Warrior);
  set_tribe(r, Tribe::Hoodrick, Tech::Archery, UnitType::Archer);
  set_tribe(r, Tribe::Luxidoor, Tech::Climbing, UnitType::Warrior, 3);
  set_tribe(r, Tribe::Vengir, Tech::Smithery, UnitType::Swordsman);
  set_tribe(r, Tribe::Zebasi, Tech::Farming, UnitType::Warrior);
  set_tribe(r, Tribe::AiMo, Tech::Meditation, UnitType::MindBender);
  set_tribe(r, Tribe::Quetzali, Tech::Strategy, UnitType::Defender);
  set_tribe(r, Tribe::Yadakk, Tech::Roads, UnitType::Warrior);

  return r;
}

namespace {

void load_unit(const json::Value& v, UnitStats& s) {
  s.cost = v["cost"].integer(s.cost);
  s.hp = v["hp"].integer(s.hp);
  s.attack = static_cast<float>(v["attack"].num(s.attack));
  s.defence = static_cast<float>(v["defence"].num(s.defence));
  s.move = v["move"].integer(s.move);
  s.range = v["range"].integer(s.range);
  s.vision = v["vision"].integer(s.vision);
  s.can_promote = v["can_promote"].boolean(s.can_promote);
  s.trainable = v["trainable"].boolean(s.trainable);

  if (v["tech"].is_string()) {
    Tech t{};
    if (lookup(tech_names(), v["tech"].str(), t)) {
      s.tech = t;
      s.needs_tech = true;
    }
  } else if (v.contains("tech") && v["tech"].is_null()) {
    s.needs_tech = false;
  }

  if (v["skills"].is_array()) {
    std::uint32_t mask = 0;
    for (const auto& entry : v["skills"].arr()) {
      Skill sk_value{};
      if (lookup(skill_names(), entry.str(), sk_value)) {
        mask |= static_cast<std::uint32_t>(sk_value);
      }
    }
    s.skills = mask;
  }
}

}  // namespace

bool load_rules(const std::string& path, Rules& out, std::string& error) {
  json::Value root;
  if (!json::parse_file(path, root, error)) return false;

  out = Rules::defaults();
  out.version = root["version"].str().empty() ? out.version : root["version"].str();

  const json::Value& combat = root["combat"];
  out.accelerator = static_cast<float>(combat["accelerator"].num(out.accelerator));
  out.defence_bonus_standard =
      static_cast<float>(combat["defence_bonus_standard"].num(out.defence_bonus_standard));
  out.defence_bonus_wall =
      static_cast<float>(combat["defence_bonus_wall"].num(out.defence_bonus_wall));

  const json::Value& econ = root["economy"];
  out.tech_cost_base = econ["tech_cost_base"].integer(out.tech_cost_base);
  out.literacy_numerator = econ["literacy_numerator"].integer(out.literacy_numerator);
  out.literacy_denominator = econ["literacy_denominator"].integer(out.literacy_denominator);

  for (const auto& [name, value] : root["units"].obj()) {
    UnitType type{};
    if (lookup(unit_names(), name, type)) {
      load_unit(value, out.units[static_cast<int>(type)]);
    }
  }

  for (const auto& [name, value] : root["improvements"].obj()) {
    Improvement imp{};
    if (!lookup(improvement_names(), name, imp)) continue;
    ImprovementStats& s = out.improvements[static_cast<int>(imp)];
    s.cost = value["cost"].integer(s.cost);
    s.population = value["population"].integer(s.population);
    s.per_city_limit = value["per_city_limit"].integer(s.per_city_limit);
    if (value["tech"].is_string()) {
      Tech t{};
      if (lookup(tech_names(), value["tech"].str(), t)) s.tech = t;
    }
    if (value["terrain"].is_array()) {
      std::uint32_t mask = 0;
      for (const auto& entry : value["terrain"].arr()) {
        Terrain terrain{};
        if (lookup(terrain_names(), entry.str(), terrain)) mask |= terrain_bit(terrain);
      }
      if (mask != 0) s.terrain_mask = mask;
    }
  }

  for (const auto& [name, value] : root["resources"].obj()) {
    Resource res{};
    if (!lookup(resource_names(), name, res)) continue;
    ResourceStats& s = out.resources[static_cast<int>(res)];
    s.harvest_cost = value["harvest_cost"].integer(s.harvest_cost);
    s.population = value["population"].integer(s.population);
    s.stars = value["stars"].integer(s.stars);
    if (value["tech"].is_string()) {
      Tech t{};
      if (lookup(tech_names(), value["tech"].str(), t)) s.tech = t;
    }
  }

  for (const auto& [name, value] : root["tribes"].obj()) {
    Tribe tribe{};
    if (!lookup(tribe_names(), name, tribe)) continue;
    TribeStats& s = out.tribes[static_cast<int>(tribe)];
    s.capital_level = value["capital_level"].integer(s.capital_level);
    if (value["start_tech"].is_string()) {
      Tech t{};
      if (lookup(tech_names(), value["start_tech"].str(), t)) s.start_tech = t;
    }
    if (value["start_unit"].is_string()) {
      UnitType u{};
      if (lookup(unit_names(), value["start_unit"].str(), u)) s.start_unit = u;
    }
  }

  const json::Value& temples = root["city"]["temple"];
  out.temple_growth_interval =
      temples["growth_interval"].integer(out.temple_growth_interval);
  out.temple_max_level = temples["max_level"].integer(out.temple_max_level);
  out.temple_score_per_level =
      temples["score_per_level"].integer(out.temple_score_per_level);

  const json::Value& difficulty = root["difficulty"];
  const char* kDifficultyKeys[] = {"easy", "normal", "hard", "crazy"};
  for (int i = 0; i < static_cast<int>(Difficulty::Count); ++i) {
    out.difficulty_capital_bonus[i] =
        difficulty[kDifficultyKeys[i]]["capital_income_bonus"].integer(
            out.difficulty_capital_bonus[i]);
  }

  out.unverified.clear();
  for (const auto& entry : root["_unverified"].arr()) {
    out.unverified.push_back(entry.str());
  }

  return true;
}

}  // namespace poly
