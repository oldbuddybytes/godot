#include "register_types.h"

#include "agent_inspector.h"
#include "agent_test.h"
#include "agent_test_runner.h"
#include "agent_tools.h"
#include "agent_unattended.h"
#include "agent_validator.h"

#include "core/object/class_db.h"

void initialize_agent_tools_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(AgentTools);
	GDREGISTER_CLASS(AgentValidator);
	GDREGISTER_CLASS(AgentInspector);
	GDREGISTER_CLASS(AgentWait);
	GDREGISTER_CLASS(AgentTest);
	GDREGISTER_CLASS(AgentTestRunner);
	GDREGISTER_CLASS(AgentUnattended);
}

void uninitialize_agent_tools_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		AgentUnattended::cleanup();
	}
}
