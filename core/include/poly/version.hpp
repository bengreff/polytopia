#pragma once

namespace poly {

// Identifies the engine build. The ruleset revision is reported separately by the rules
// loader once M2 lands — a build and a ruleset version are independent, and conflating
// them would make trace-harness divergences hard to attribute.
const char* engine_version();

}  // namespace poly
