#include "agent_tools.h"

#include "core/object/class_db.h"

String AgentTools::get_version() {
	return "0.1.0";
}

void AgentTools::_bind_methods() {
	ClassDB::bind_static_method("AgentTools", D_METHOD("get_version"), &AgentTools::get_version);
}
