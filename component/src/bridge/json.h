// Minimal JSON reader/writer used by the librespot bridge client.
// Dependency-free (std::string only) so the same code can be built into the
// console test tools without any foobar2000 SDK linkage.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace flp {
namespace json {

// A parsed JSON value. Objects keep their key order, which is handy when
// debugging a bridge reply; lookup is a linear scan over a handful of keys.
struct value {
	enum class kind_t { null_v, bool_v, number_v, string_v, array_v, object_v };

	kind_t kind = kind_t::null_v;
	bool boolean = false;
	double number = 0.0;
	std::string string;
	std::vector<value> array;
	std::vector<std::pair<std::string, value>> object;

	bool is_null() const { return kind == kind_t::null_v; }
	bool is_bool() const { return kind == kind_t::bool_v; }
	bool is_number() const { return kind == kind_t::number_v; }
	bool is_string() const { return kind == kind_t::string_v; }
	bool is_array() const { return kind == kind_t::array_v; }
	bool is_object() const { return kind == kind_t::object_v; }

	const value* find(const char* key) const;

	// Convenience accessors used on bridge replies; all are tolerant of a
	// missing or mistyped member and return the supplied default.
	std::string get_string(const char* key, const std::string& def = std::string()) const;
	double get_number(const char* key, double def = 0.0) const;
	long long get_int64(const char* key, long long def = 0) const;
	bool get_bool(const char* key, bool def = false) const;

	// True when the member exists and is not JSON null.
	bool has(const char* key) const;
};

// Parses one complete JSON text. Returns false and fills `error` on failure.
bool parse(const std::string& text, value& out, std::string& error);

// Returns `s` as a quoted JSON string, including the surrounding quotes.
std::string quote(const std::string& s);

} // namespace json
} // namespace flp
