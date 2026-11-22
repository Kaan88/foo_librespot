#include "json.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace flp {
namespace json {

const value* value::find(const char* key) const {
	if (kind != kind_t::object_v || key == nullptr) return nullptr;
	for (const auto& kv : object) {
		if (kv.first == key) return &kv.second;
	}
	return nullptr;
}

bool value::has(const char* key) const {
	const value* v = find(key);
	return v != nullptr && !v->is_null();
}

std::string value::get_string(const char* key, const std::string& def) const {
	const value* v = find(key);
	if (v == nullptr || !v->is_string()) return def;
	return v->string;
}

double value::get_number(const char* key, double def) const {
	const value* v = find(key);
	if (v == nullptr || !v->is_number()) return def;
	return v->number;
}

long long value::get_int64(const char* key, long long def) const {
	const value* v = find(key);
	if (v == nullptr || !v->is_number()) return def;
	return static_cast<long long>(v->number < 0 ? v->number - 0.5 : v->number + 0.5);
}

bool value::get_bool(const char* key, bool def) const {
	const value* v = find(key);
	if (v == nullptr || !v->is_bool()) return def;
	return v->boolean;
}

namespace {

class parser {
public:
	parser(const std::string& text) : m_text(text) {}

	bool run(value& out, std::string& error) {
		skip_space();
		if (!parse_value(out)) {
			error = m_error.empty() ? "invalid JSON" : m_error;
			return false;
		}
		skip_space();
		if (m_pos != m_text.size()) {
			error = "trailing characters after JSON value";
			return false;
		}
		return true;
	}

private:
	const std::string& m_text;
	size_t m_pos = 0;
	std::string m_error;

	bool eof() const { return m_pos >= m_text.size(); }
	char peek() const { return eof() ? '\0' : m_text[m_pos]; }

	void skip_space() {
		while (!eof()) {
			const char c = m_text[m_pos];
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
				++m_pos;
			} else {
				break;
			}
		}
	}

	bool fail(const char* what) {
		if (m_error.empty()) m_error = what;
		return false;
	}

	bool consume(char c) {
		if (peek() != c) return false;
		++m_pos;
		return true;
	}

	bool parse_value(value& out) {
		if (eof()) return fail("unexpected end of input");
		switch (peek()) {
		case '{': return parse_object(out);
		case '[': return parse_array(out);
		case '"':
			out.kind = value::kind_t::string_v;
			return parse_string(out.string);
		case 't':
			if (m_text.compare(m_pos, 4, "true") != 0) return fail("invalid literal");
			m_pos += 4;
			out.kind = value::kind_t::bool_v;
			out.boolean = true;
			return true;
		case 'f':
			if (m_text.compare(m_pos, 5, "false") != 0) return fail("invalid literal");
			m_pos += 5;
			out.kind = value::kind_t::bool_v;
			out.boolean = false;
			return true;
		case 'n':
			if (m_text.compare(m_pos, 4, "null") != 0) return fail("invalid literal");
			m_pos += 4;
			out.kind = value::kind_t::null_v;
			return true;
		default:
			return parse_number(out);
		}
	}

	bool parse_object(value& out) {
		out.kind = value::kind_t::object_v;
		++m_pos; // '{'
		skip_space();
		if (consume('}')) return true;
		for (;;) {
			skip_space();
			if (peek() != '"') return fail("expected object key");
			std::string key;
			if (!parse_string(key)) return false;
			skip_space();
			if (!consume(':')) return fail("expected ':'");
			skip_space();
			value member;
			if (!parse_value(member)) return false;
			out.object.emplace_back(std::move(key), std::move(member));
			skip_space();
			if (consume('}')) return true;
			if (!consume(',')) return fail("expected ',' or '}'");
		}
	}

	bool parse_array(value& out) {
		out.kind = value::kind_t::array_v;
		++m_pos; // '['
		skip_space();
		if (consume(']')) return true;
		for (;;) {
			skip_space();
			value member;
			if (!parse_value(member)) return false;
			out.array.push_back(std::move(member));
			skip_space();
			if (consume(']')) return true;
			if (!consume(',')) return fail("expected ',' or ']'");
		}
	}

	bool parse_number(value& out) {
		const size_t start = m_pos;
		if (peek() == '-') ++m_pos;
		while (!eof() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') ++m_pos;
		if (peek() == '.') {
			++m_pos;
			while (!eof() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') ++m_pos;
		}
		if (peek() == 'e' || peek() == 'E') {
			++m_pos;
			if (peek() == '+' || peek() == '-') ++m_pos;
			while (!eof() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') ++m_pos;
		}
		if (m_pos == start) return fail("invalid value");
		const std::string text = m_text.substr(start, m_pos - start);
		char* end = nullptr;
		const double d = std::strtod(text.c_str(), &end);
		if (end == nullptr || *end != '\0') return fail("invalid number");
		out.kind = value::kind_t::number_v;
		out.number = d;
		return true;
	}

	static void append_utf8(std::string& out, unsigned cp) {
		if (cp <= 0x7f) {
			out.push_back(static_cast<char>(cp));
		} else if (cp <= 0x7ff) {
			out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
		} else if (cp <= 0xffff) {
			out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
		} else {
			out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
		}
	}

	bool parse_hex4(unsigned& out) {
		if (m_pos + 4 > m_text.size()) return fail("truncated \\u escape");
		out = 0;
		for (int i = 0; i < 4; ++i) {
			const char c = m_text[m_pos++];
			out <<= 4;
			if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
			else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
			else return fail("invalid \\u escape");
		}
		return true;
	}

	bool parse_string(std::string& out) {
		++m_pos; // opening quote
		out.clear();
		while (!eof()) {
			const unsigned char c = static_cast<unsigned char>(m_text[m_pos++]);
			if (c == '"') return true;
			if (c != '\\') {
				out.push_back(static_cast<char>(c));
				continue;
			}
			if (eof()) return fail("truncated escape");
			const char esc = m_text[m_pos++];
			switch (esc) {
			case '"': out.push_back('"'); break;
			case '\\': out.push_back('\\'); break;
			case '/': out.push_back('/'); break;
			case 'b': out.push_back('\b'); break;
			case 'f': out.push_back('\f'); break;
			case 'n': out.push_back('\n'); break;
			case 'r': out.push_back('\r'); break;
			case 't': out.push_back('\t'); break;
			case 'u': {
				unsigned cp = 0;
				if (!parse_hex4(cp)) return false;
				if (cp >= 0xd800 && cp <= 0xdbff) {
					// High surrogate; expect a low one to follow.
					unsigned low = 0;
					if (m_pos + 2 <= m_text.size() && m_text[m_pos] == '\\' && m_text[m_pos + 1] == 'u') {
						m_pos += 2;
						if (!parse_hex4(low)) return false;
						if (low >= 0xdc00 && low <= 0xdfff) {
							cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
						} else {
							append_utf8(out, 0xfffd);
							cp = low;
						}
					} else {
						cp = 0xfffd;
					}
				} else if (cp >= 0xdc00 && cp <= 0xdfff) {
					cp = 0xfffd; // lone low surrogate
				}
				append_utf8(out, cp);
				break;
			}
			default:
				return fail("invalid escape");
			}
		}
		return fail("unterminated string");
	}
};

} // namespace

bool parse(const std::string& text, value& out, std::string& error) {
	out = value();
	parser p(text);
	return p.run(out, error);
}

std::string quote(const std::string& s) {
	static const char* hex = "0123456789abcdef";
	std::string out;
	out.reserve(s.size() + 2);
	out.push_back('"');
	for (const unsigned char c : s) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				out += "\\u00";
				out.push_back(hex[(c >> 4) & 0xf]);
				out.push_back(hex[c & 0xf]);
			} else {
				out.push_back(static_cast<char>(c));
			}
		}
	}
	out.push_back('"');
	return out;
}

} // namespace json
} // namespace flp
