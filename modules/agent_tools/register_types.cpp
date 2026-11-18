#include "register_types.h"

#include "agent_inspector.h"
#include "agent_tools.h"
#include "agent_validator.h"

#include "core/object/class_db.h"

void initialize_agent_tools_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(AgentTools);
	GDREGISTER_CLASS(AgentValidator);
	GDREGISTER_CLASS(AgentInspector);
}

void uninitialize_agent_tools_module(ModuleInitializationLevel p_level) {
}
