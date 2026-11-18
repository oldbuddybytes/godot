#include "register_types.h"

#include "agent_tools.h"

#include "core/object/class_db.h"

void initialize_agent_tools_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(AgentTools);
}

void uninitialize_agent_tools_module(ModuleInitializationLevel p_level) {
}
