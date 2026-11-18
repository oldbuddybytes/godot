#pragma once

#include "core/object/object.h"

class AgentTools : public Object {
	GDCLASS(AgentTools, Object);

protected:
	static void _bind_methods();

public:
	static String get_version();
	// Runs a command-line flag such as "--validate-project"; returns the exit code.
	static int run_command(const String &p_command);
};
