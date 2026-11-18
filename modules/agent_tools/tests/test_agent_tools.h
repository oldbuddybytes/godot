#pragma once

#include "../agent_tools.h"
#include "../agent_validator.h"

#include "core/object/class_db.h"
#include "tests/test_macros.h"

namespace TestAgentTools {

TEST_CASE("[AgentTools] Module classes are registered") {
	CHECK(ClassDB::class_exists("AgentTools"));
	CHECK(AgentTools::get_version() == "0.1.0");
}

TEST_CASE("[AgentTools] Validator text report groups and truncates") {
	Dictionary err;
	err["code"] = "SCRIPT_PARSE_ERROR";
	err["severity"] = "error";
	err["path"] = "res://a.gd";
	err["line"] = 5;
	err["column"] = 2;
	err["message"] = "Expected \")\"";
	err["affected"] = PackedStringArray({ "res://1.tscn", "res://2.tscn", "res://3.tscn", "res://4.tscn" });
	Dictionary warn;
	warn["code"] = "INVALID_UID";
	warn["severity"] = "warning";
	warn["path"] = "res://b.tscn";
	warn["message"] = "uid://x is unknown";

	Dictionary result;
	result["ok"] = false;
	result["errors"] = 1;
	result["warnings"] = 1;
	result["files"] = 6;
	result["diagnostics"] = Array({ err, warn });

	const String full = AgentValidator::format_text(result);
	CHECK(full.begins_with("FAIL: 1 error, 1 warning in 6 files"));
	CHECK(full.contains("ERROR res://a.gd:5:2 SCRIPT_PARSE_ERROR"));
	CHECK(full.contains("(affects 4: res://1.tscn, res://2.tscn, res://3.tscn, +1)"));
	CHECK(full.contains("WARNING res://b.tscn INVALID_UID"));

	const String errors_only = AgentValidator::format_text(result, 20, true);
	CHECK_FALSE(errors_only.contains("INVALID_UID"));

	const String capped = AgentValidator::format_text(result, 1);
	CHECK(capped.contains("(+1 more not shown"));
}

} // namespace TestAgentTools
