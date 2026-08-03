// Tests that the tech tree encoded in content.hpp reproduces live-rules.md §3.1 exactly.
//
// tech_tier() and tech_parent() are COMPUTED from declaration order rather than tabulated.
// That is a deliberate trade: the tree is perfectly regular (5 branches x 5), so exploiting
// the regularity removes 25 hand-written parent entries that could each be wrong. But it
// means a single misplaced enumerator silently corrupts the whole tree -- so the structure
// is asserted here against the wiki's shape, edge by edge.

#include "poly/content.hpp"
#include "testing.hpp"

using poly::Tech;
using poly::tech_branch;
using poly::tech_is_root;
using poly::tech_parent;
using poly::tech_tier;

TEST("tech: the tree is 25 techs in 5 branches of 5") {
  CHECK_EQ(poly::kTechCount, 25);
  CHECK_EQ(poly::kTechBranches * poly::kTechsPerBranch, 25);
}

TEST("tech: every branch root is tier 1 and parentless") {
  const Tech roots[] = {Tech::Climbing, Tech::Fishing, Tech::Hunting, Tech::Organization,
                        Tech::Riding};
  for (const Tech root : roots) {
    CHECK(tech_is_root(root));
    CHECK_EQ(tech_tier(root), 1);
    CHECK(tech_parent(root) == root);
  }
}

TEST("tech: tiers match the wiki tree") {
  const Tech tier2[] = {Tech::Mining,   Tech::Meditation, Tech::Sailing, Tech::Ramming,
                        Tech::Archery,  Tech::Forestry,   Tech::Farming, Tech::Strategy,
                        Tech::Roads,    Tech::FreeSpirit};
  for (const Tech t : tier2) CHECK_EQ(tech_tier(t), 2);

  const Tech tier3[] = {Tech::Smithery,   Tech::Philosophy, Tech::Navigation, Tech::Aquatism,
                        Tech::Spiritualism, Tech::Mathematics, Tech::Construction,
                        Tech::Diplomacy,  Tech::Trade,      Tech::Chivalry};
  for (const Tech t : tier3) CHECK_EQ(tech_tier(t), 3);
}

TEST("tech: every parent edge matches live-rules.md 3.1") {
  struct Edge {
    Tech child;
    Tech parent;
  };
  // Transcribed directly from the tree diagram in the research doc.
  const Edge edges[] = {
      {Tech::Mining, Tech::Climbing},       {Tech::Smithery, Tech::Mining},
      {Tech::Meditation, Tech::Climbing},   {Tech::Philosophy, Tech::Meditation},

      {Tech::Sailing, Tech::Fishing},       {Tech::Navigation, Tech::Sailing},
      {Tech::Ramming, Tech::Fishing},       {Tech::Aquatism, Tech::Ramming},

      {Tech::Archery, Tech::Hunting},       {Tech::Spiritualism, Tech::Archery},
      {Tech::Forestry, Tech::Hunting},      {Tech::Mathematics, Tech::Forestry},

      {Tech::Farming, Tech::Organization},  {Tech::Construction, Tech::Farming},
      {Tech::Strategy, Tech::Organization}, {Tech::Diplomacy, Tech::Strategy},

      {Tech::Roads, Tech::Riding},          {Tech::Trade, Tech::Roads},
      {Tech::FreeSpirit, Tech::Riding},     {Tech::Chivalry, Tech::FreeSpirit},
  };

  // 20 edges: 25 techs minus 5 parentless roots.
  CHECK_EQ(static_cast<int>(std::size(edges)), 20);

  for (const Edge& e : edges) {
    CHECK(tech_parent(e.child) == e.parent);
    // A parent is always exactly one tier shallower.
    CHECK_EQ(tech_tier(e.child), tech_tier(e.parent) + 1);
    // And always in the same branch.
    CHECK_EQ(tech_branch(e.child), tech_branch(e.parent));
  }
}

TEST("tech: two edges the research flagged as needing independent confirmation") {
  // polyenv-audit.md called these out as the only edges not triple-confirmed. Pinning them
  // here means that if either turns out to be wrong, exactly one test fails and says so,
  // rather than the error diffusing through the economy.
  CHECK(tech_parent(Tech::Aquatism) == Tech::Ramming);
  CHECK(tech_parent(Tech::Spiritualism) == Tech::Archery);
}

TEST("tech: walking parents from any tech reaches its root") {
  // The property backwards research relies on (resolutions.md R30): the tree is only three
  // deep, so owning a T3 transitively opens its whole branch upward.
  for (int i = 0; i < poly::kTechCount; ++i) {
    const auto t = static_cast<Tech>(i);
    Tech cursor = t;
    int steps = 0;
    while (!tech_is_root(cursor)) {
      cursor = tech_parent(cursor);
      ++steps;
      CHECK(steps <= 2);  // never more than T3 -> T2 -> T1
    }
    CHECK_EQ(tech_branch(cursor), tech_branch(t));
    CHECK_EQ(steps, tech_tier(t) - 1);
  }
}

TEST("content: removed and out-of-scope members are absent") {
  // Regression guards. Each of these was present in at least one source consulted and is
  // wrong for live v2.16.3; a future edit re-adding one should have to delete a test.
  //
  // Whale -> replaced by Starfish. Boat/Ship/Battleship -> replaced by the Path of the Ocean
  // naval line. Whaling/Engineering -> removed techs.
  CHECK_EQ(static_cast<int>(poly::Resource::Count), 10);
  CHECK_EQ(poly::kTribeCount, 12);
  CHECK_EQ(static_cast<int>(poly::UnitType::Count), 19);
}

TEST("content: skills compose and test as a bitmask") {
  using poly::has_skill;
  using poly::Skill;
  // Knight: Dash, Persist, Fortify (live-rules.md §2.1).
  const Skill knight = Skill::Dash | Skill::Persist | Skill::Fortify;
  CHECK(has_skill(knight, Skill::Dash));
  CHECK(has_skill(knight, Skill::Persist));
  CHECK(has_skill(knight, Skill::Fortify));
  CHECK(!has_skill(knight, Skill::Escape));
  // Swordsman lost Fortify in Path of the Ocean 2.8 -- the single most commonly stale
  // unit fact in the sources consulted.
  const Skill swordsman = Skill::Dash;
  CHECK(!has_skill(swordsman, Skill::Fortify));
}
