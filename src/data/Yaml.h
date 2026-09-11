#pragma once

// Yaml.h -- a practical YAML subset on top of Json.h's value model, plus the
// two writers, so a file this program reads can be hand-edited or generated.
//
// YAML 1.2 is a JSON superset, so `yaml_parse` accepts JSON verbatim: a
// document opening with `{` or `[` is handed to json_parse, which is exact
// about it. Everything else is read as block YAML.
//
// Supported: block mappings and sequences, single-line flow collections,
// plain / 'single' / "double" scalars, `|` and `>` block scalars, `#`
// comments, a leading `---`. NOT supported, and an error rather than a
// silent misreading: anchors, aliases, tags, multiple documents, and flow
// collections spanning lines (use JSON for those).

#include "data/Json.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace yaml_detail {

struct Line {
    std::string text;    // comments stripped, right-trimmed
    int indent = 0;      // spaces before the first token
    int number = 1;      // 1-based, for error messages
    bool blank = true;
};

[[noreturn]] inline void fail(int line, const std::string& why) {
    throw std::runtime_error("YAML line " + std::to_string(line) + ": " + why);
}

// Everything from an unquoted '#' that follows whitespace or starts the line.
inline std::string strip_comment(const std::string& s) {
    char quote = 0;
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        if (quote) {
            if (c == '\\' && quote == '"' && i + 1 < s.size()) i++;
            else if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
            return s.substr(0, i);
        }
    }
    return s;
}

inline std::string rtrim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.pop_back();
    return s;
}

inline std::vector<Line> split(const std::string& text) {
    std::vector<Line> out;
    size_t i = 0;
    int n = 1;
    while (i <= text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string raw = text.substr(i, e - i);
        Line l;
        l.number = n++;
        // A tab in the indent is never valid YAML and misreads silently.
        for (char c : raw) {
            if (c == ' ') continue;
            if (c == '\t') fail(l.number, "tab in indentation");
            break;
        }
        const std::string body = rtrim(strip_comment(raw));
        size_t ind = 0;
        while (ind < body.size() && body[ind] == ' ') ind++;
        l.indent = (int)ind;
        l.text = body.substr(ind);
        l.blank = l.text.empty();
        // The raw line is what a block scalar keeps, indentation and all.
        if (!l.blank) l.text = rtrim(l.text);
        out.push_back(std::move(l));
        if (e == text.size()) break;
        i = e + 1;
    }
    return out;
}

// A scalar as YAML types it: `null`/`~`/empty, true/false, a number, or text.
inline JsonValue plain_scalar(const std::string& s, int line) {
    JsonValue v;
    if (s.empty() || s == "~" || s == "null" || s == "Null" || s == "NULL")
        return v;                                  // Null
    if (s == "true" || s == "True" || s == "TRUE" ||
        s == "false" || s == "False" || s == "FALSE") {
        v.type = JsonValue::Type::Bool;
        v.b = s[0] == 't' || s[0] == 'T';
        return v;
    }
    // Numbers only when the whole token is one; "1.2.3" and "3 apples" stay text.
    const char* b = s.c_str();
    char* endp = nullptr;
    const double d = std::strtod(b, &endp);
    if (endp && *endp == '\0' && endp != b) {
        v.type = JsonValue::Type::Number;
        v.num = d;
        return v;
    }
    (void)line;
    v.type = JsonValue::Type::String;
    v.str = s;
    return v;
}

inline std::string unescape_double(const std::string& s, int line) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\') { out += s[i]; continue; }
        if (++i >= s.size()) fail(line, "trailing backslash");
        switch (s[i]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '0': out += '\0'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            default: fail(line, std::string("unsupported escape \\") + s[i]);
        }
    }
    return out;
}

// One scalar or single-line flow collection, from `s` (already comment-free).
inline JsonValue parse_inline(const std::string& s, int line);

// Split a flow collection's body on top-level commas.
inline std::vector<std::string> flow_items(const std::string& s, int line) {
    std::vector<std::string> out;
    int depth = 0;
    char quote = 0;
    std::string cur;
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        if (quote) {
            cur += c;
            if (c == '\\' && quote == '"' && i + 1 < s.size()) cur += s[++i];
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
        if (c == '[' || c == '{') depth++;
        if (c == ']' || c == '}') depth--;
        if (c == ',' && depth == 0) { out.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    if (quote) fail(line, "unterminated quote");
    if (depth) fail(line, "unbalanced flow collection");
    // "[]" and "{}" have no items; "[a,]" has one.
    std::string tail = cur;
    size_t b = tail.find_first_not_of(" \t");
    if (b != std::string::npos || !out.empty()) out.push_back(cur);
    return out;
}

inline std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

// A mapping key, unquoted or quoted, followed by ':'. npos when the text does
// not open a `key:` pair at all.
inline size_t key_end(const std::string& s) {
    char quote = 0;
    int depth = 0;
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        if (quote) {
            if (c == '\\' && quote == '"' && i + 1 < s.size()) i++;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '[' || c == '{') depth++;
        if (c == ']' || c == '}') depth--;
        // ": " or a ':' at end of line ends a key; "a:b" is a plain scalar,
        // which is what a bare URL or a time of day needs.
        if (c == ':' && depth == 0 && (i + 1 == s.size() || s[i + 1] == ' '))
            return i;
    }
    return std::string::npos;
}

inline JsonValue parse_inline(const std::string& s, int line) {
    const std::string t = trim(s);
    if (t.empty()) return JsonValue{};
    if (t.front() == '[' || t.front() == '{') {
        const char close = t.front() == '[' ? ']' : '}';
        if (t.back() != close)
            fail(line, "flow collection must close on the same line");
        JsonValue v;
        const std::string body = t.substr(1, t.size() - 2);
        if (close == ']') {
            v.type = JsonValue::Type::Array;
            for (const std::string& item : flow_items(body, line))
                v.arr.push_back(parse_inline(item, line));
        } else {
            v.type = JsonValue::Type::Object;
            for (const std::string& item : flow_items(body, line)) {
                const std::string it = trim(item);
                const size_t k = key_end(it);
                if (k == std::string::npos) fail(line, "flow mapping needs 'key: value'");
                JsonValue key = parse_inline(it.substr(0, k), line);
                v.obj.emplace_back(key.type == JsonValue::Type::String ? key.str
                                                                       : trim(it.substr(0, k)),
                                   parse_inline(it.substr(k + 1), line));
            }
        }
        return v;
    }
    if (t.front() == '"') {
        if (t.size() < 2 || t.back() != '"') fail(line, "unterminated \"");
        JsonValue v;
        v.type = JsonValue::Type::String;
        v.str = unescape_double(t.substr(1, t.size() - 2), line);
        return v;
    }
    if (t.front() == '\'') {
        if (t.size() < 2 || t.back() != '\'') fail(line, "unterminated '");
        JsonValue v;
        v.type = JsonValue::Type::String;
        std::string body = t.substr(1, t.size() - 2);
        for (size_t i = 0; i < body.size(); i++) {
            v.str += body[i];
            if (body[i] == '\'' && i + 1 < body.size() && body[i + 1] == '\'') i++;
        }
        return v;
    }
    return plain_scalar(t, line);
}

struct Reader {
    std::vector<Line> lines;
    size_t i = 0;

    bool at_end() {
        while (i < lines.size() && lines[i].blank) i++;
        return i >= lines.size();
    }
    const Line& cur() { return lines[i]; }

    // `|` / `>` keep the lines below at deeper indentation, joined by newlines
    // (literal) or spaces (folded).
    JsonValue block_scalar(char kind, int parent_indent) {
        i++;
        std::string out;
        int base = -1;
        for (; i < lines.size(); i++) {
            const Line& l = lines[i];
            if (l.blank) { out += '\n'; continue; }
            if (l.indent <= parent_indent) break;
            if (base < 0) base = l.indent;
            std::string t = l.text;
            if (l.indent > base) t.insert(0, (size_t)(l.indent - base), ' ');
            out += t;
            out += kind == '|' ? '\n' : ' ';
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
        if (kind == '|') out += '\n';
        JsonValue v;
        v.type = JsonValue::Type::String;
        v.str = out;
        return v;
    }

    // The value that follows `key:` or `-`: what is on the rest of the line,
    // or the block indented under it.
    JsonValue value_after(const std::string& rest, int parent_indent) {
        const std::string t = trim(rest);
        if (t == "|" || t == ">") return block_scalar(t[0], parent_indent);
        if (!t.empty()) { i++; return parse_inline(t, lines[i - 1].number); }
        i++;
        if (at_end()) return JsonValue{};
        if (cur().indent > parent_indent) return parse_block(cur().indent);
        // A sequence may sit at its own key's indent rather than under it,
        // which is what most YAML writers emit and what yaml_write does.
        if (cur().indent == parent_indent &&
            (cur().text == "-" || cur().text.compare(0, 2, "- ") == 0))
            return parse_block(parent_indent);
        return JsonValue{};
    }

    JsonValue parse_block(int indent) {
        if (at_end()) return JsonValue{};
        if (cur().indent < indent) return JsonValue{};
        const bool seq = cur().text == "-" || cur().text.compare(0, 2, "- ") == 0;
        JsonValue v;
        v.type = seq ? JsonValue::Type::Array : JsonValue::Type::Object;
        while (!at_end() && cur().indent == indent) {
            const Line& l = cur();
            if (seq) {
                // A sequence written at its key's own indent ends where the
                // enclosing mapping's next key begins.
                if (l.text != "-" && l.text.compare(0, 2, "- ") != 0) break;
                const std::string rest = l.text.size() > 1 ? l.text.substr(2) : "";
                // "- key: value" opens a mapping whose later keys line up
                // under the key, not under the dash.
                const size_t k = key_end(trim(rest));
                if (k != std::string::npos && !trim(rest).empty() &&
                    trim(rest)[0] != '[' && trim(rest)[0] != '{') {
                    const int inner = indent + 2 + (int)(rest.size() - trim(rest).size());
                    // Rewrite the line so the mapping parser sees just the pair.
                    lines[i].text = trim(rest);
                    lines[i].indent = inner;
                    v.arr.push_back(parse_block(inner));
                    continue;
                }
                v.arr.push_back(value_after(rest, indent));
                continue;
            }
            // Likewise a mapping ends where an enclosing sequence resumes.
            if (l.text == "-" || l.text.compare(0, 2, "- ") == 0) break;
            const size_t k = key_end(l.text);
            if (k == std::string::npos) fail(l.number, "expected 'key: value'");
            JsonValue key = parse_inline(l.text.substr(0, k), l.number);
            const std::string name = key.type == JsonValue::Type::String
                                         ? key.str
                                         : trim(l.text.substr(0, k));
            const std::string rest = l.text.substr(k + 1);
            v.obj.emplace_back(name, value_after(rest, indent));
        }
        return v;
    }
};

}  // namespace yaml_detail

// YAML, or JSON when the document opens with a flow collection.
inline JsonValue yaml_parse(const std::string& text) {
    size_t p = text.find_first_not_of(" \t\r\n");
    if (p != std::string::npos && (text[p] == '{' || text[p] == '['))
        return json_parse(text);
    yaml_detail::Reader r{yaml_detail::split(text), 0};
    // A leading `---` is a document start, not content.
    if (!r.at_end() && r.cur().text == "---") r.i++;
    if (r.at_end()) return JsonValue{};
    const int indent = r.cur().indent;
    JsonValue v = r.parse_block(indent);
    if (!r.at_end())
        yaml_detail::fail(r.cur().number,
                          r.cur().text == "---" ? "multiple documents are not supported"
                                                : "unexpected indentation");
    return v;
}

inline JsonValue yaml_parse_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string text(n > 0 ? (size_t)n : 0, '\0');
    size_t got = n > 0 ? std::fread(&text[0], 1, (size_t)n, f) : 0;
    std::fclose(f);
    text.resize(got);
    return yaml_parse(text);
}

// ---------------------------------------------------------------------------
// Writers
// ---------------------------------------------------------------------------

namespace yaml_detail {

// The shortest form that reads back as the same double: 0.1 stays "0.1"
// rather than becoming 0.10000000000000001.
inline std::string number_text(double d) {
    if (d == (double)(long long)d && d > -1e15 && d < 1e15)
        return std::to_string((long long)d);
    char buf[40];
    for (int prec = 15; prec <= 17; prec++) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    return buf;
}

inline std::string quote_json(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

// Plain where YAML would read it back unchanged, quoted otherwise.
inline std::string scalar_yaml(const std::string& s) {
    if (s.empty()) return "\"\"";
    static const char* kReserved = "#&*!|>'\"%@`,[]{}:";
    if (s.find_first_of(" \t\n") != std::string::npos ||
        std::strchr(kReserved, s[0]) || s.find(": ") != std::string::npos ||
        s.back() == ':' || s.find(" #") != std::string::npos)
        return quote_json(s);
    JsonValue probe = plain_scalar(s, 0);
    return probe.type == JsonValue::Type::String ? s : quote_json(s);
}

inline void write_json(const JsonValue& v, std::string& out, int indent) {
    const std::string pad((size_t)indent * 2 + 2, ' ');
    const std::string pad0((size_t)indent * 2, ' ');
    switch (v.type) {
        case JsonValue::Type::Null: out += "null"; break;
        case JsonValue::Type::Bool: out += v.b ? "true" : "false"; break;
        case JsonValue::Type::Number: out += number_text(v.num); break;
        case JsonValue::Type::String: out += quote_json(v.str); break;
        case JsonValue::Type::Array:
            if (v.arr.empty()) { out += "[]"; break; }
            out += "[\n";
            for (size_t i = 0; i < v.arr.size(); i++) {
                out += pad;
                write_json(v.arr[i], out, indent + 1);
                out += i + 1 < v.arr.size() ? ",\n" : "\n";
            }
            out += pad0 + "]";
            break;
        case JsonValue::Type::Object:
            if (v.obj.empty()) { out += "{}"; break; }
            out += "{\n";
            for (size_t i = 0; i < v.obj.size(); i++) {
                out += pad + quote_json(v.obj[i].first) + ": ";
                write_json(v.obj[i].second, out, indent + 1);
                out += i + 1 < v.obj.size() ? ",\n" : "\n";
            }
            out += pad0 + "}";
            break;
    }
}

// `pad` prefixes every line; `first` prefixes the first one instead, which is
// what folds a mapping's opening key onto its sequence dash.
inline void write_yaml(const JsonValue& v, std::string& out,
                       const std::string& pad, const std::string& first) {
    switch (v.type) {
        case JsonValue::Type::Null:   out += first + "null\n"; return;
        case JsonValue::Type::Bool:   out += first + (v.b ? "true\n" : "false\n"); return;
        case JsonValue::Type::Number: out += first + number_text(v.num) + "\n"; return;
        case JsonValue::Type::String: out += first + scalar_yaml(v.str) + "\n"; return;
        case JsonValue::Type::Array:
            if (v.arr.empty()) { out += first + "[]\n"; return; }
            for (size_t i = 0; i < v.arr.size(); i++)
                write_yaml(v.arr[i], out, pad + "  ", (i ? pad : first) + "- ");
            return;
        case JsonValue::Type::Object:
            if (v.obj.empty()) { out += first + "{}\n"; return; }
            for (size_t i = 0; i < v.obj.size(); i++) {
                const auto& kv = v.obj[i];
                const std::string lead = (i ? pad : first) + scalar_yaml(kv.first);
                const bool block = (kv.second.type == JsonValue::Type::Object &&
                                    !kv.second.obj.empty()) ||
                                   (kv.second.type == JsonValue::Type::Array &&
                                    !kv.second.arr.empty());
                if (block) {
                    out += lead + ":\n";
                    // A sequence sits at the key's own indent, a mapping under it.
                    const std::string inner =
                        kv.second.type == JsonValue::Type::Array ? pad : pad + "  ";
                    write_yaml(kv.second, out, inner, inner);
                } else {
                    write_yaml(kv.second, out, pad, lead + ": ");
                }
            }
            return;
    }
}

}  // namespace yaml_detail

inline std::string json_write(const JsonValue& v) {
    std::string out;
    yaml_detail::write_json(v, out, 0);
    out += "\n";
    return out;
}

inline std::string yaml_write(const JsonValue& v) {
    std::string out;
    yaml_detail::write_yaml(v, out, "", "");
    return out;
}
