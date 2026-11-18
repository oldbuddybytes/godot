#pragma once

#include "../agent_tools.h"

#include "core/object/class_db.h"
#include "tests/test_macros.h"

namespace TestAgentTools {

TEST_CASE("[AgentTools] Module classes are registered") {
	CHECK(ClassDB::class_exists("AgentTools"));
	CHECK(AgentTools::get_version() == "0.1.0");
}

} // namespace TestAgentTools
