#pragma once

#include <memory>
#include <string>

#include "agents/agent.hpp"

namespace poly::agents {

// Constructs a baseline agent by name, or nullptr if unknown.
std::unique_ptr<Agent> make_agent(const std::string& name);

extern const char* const kAgentNames[];
int agent_count();

}  // namespace poly::agents
