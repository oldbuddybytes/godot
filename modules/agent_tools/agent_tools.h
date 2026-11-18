#pragma once

#include "core/object/object.h"

class AgentTools : public Object {
	GDCLASS(AgentTools, Object);

protected:
	static void _bind_methods();

public:
	static String get_version();
};
