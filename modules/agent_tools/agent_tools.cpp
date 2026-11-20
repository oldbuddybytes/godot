#include "agent_tools.h"

#include "agent_inspector.h"
#include "agent_motion_analyzer.h"
#include "agent_validator.h"

#include "core/object/class_db.h"

String AgentTools::get_version() {
	return "0.1.0";
}

int AgentTools::run_command(const String &p_command) {
	if (p_command == "--validate-project") {
		return AgentValidator::run_cli();
	}
	if (p_command == "--analyze-animation") {
		return AgentMotionAnalyzer::run_cli();
	}
	return AgentInspector::run_cli(p_command);
}

void AgentTools::_bind_methods() {
	ClassDB::bind_static_method("AgentTools", D_METHOD("get_version"), &AgentTools::get_version);
}
