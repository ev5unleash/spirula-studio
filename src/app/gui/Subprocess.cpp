// Subprocess.cpp -- see Subprocess.h.

#include "app/gui/Subprocess.h"

#include <cctype>


namespace gui {

std::vector<std::string> split_args(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quote = false, have = false;
    char quote = 0;
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        if (in_quote) {
            if (c == quote) in_quote = false;
            else cur += c;
            continue;
        }
        // A line continuation, the way a pasted shell command writes one:
        // neither the backslash nor the break is part of an argument. Any
        // other backslash is a Windows path and stays.
        if (c == '\\') {
            size_t j = i + 1;
            if (j < s.size() && s[j] == '\r') j++;
            if (j < s.size() && s[j] == '\n') { i = j; continue; }
        }
        if (c == '"' || c == '\'') {
            in_quote = true;
            quote = c;
            have = true;
        } else if (std::isspace((unsigned char)c)) {
            if (have) { out.push_back(cur); cur.clear(); have = false; }
        } else {
            cur += c;
            have = true;
        }
    }
    if (have) out.push_back(cur);
    return out;
}


std::string safe_arg(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        // A straight quote ends a JSON string and groups words for a shell;
        // the typographic ones read the same and mean nothing to either.
        if (c == '"') out += "”";
        else if (c == '\'' || c == '`') out += "’";
        // Backslash is JSON's escape character, and a path survives as '/'.
        else if (c == '\\') out += '/';
        else if (c < 0x20 || c == 0x7F) out += ' ';   // would end the line
        else out += (char)c;
    }
    std::string tidy;
    tidy.reserve(out.size());
    for (char c : out)
        if (c != ' ' || (!tidy.empty() && tidy.back() != ' ')) tidy += c;
    while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
    return tidy;
}


std::vector<std::string> command_argv(const std::string& command,
                                      const std::string& token,
                                      const std::string& value) {
    std::vector<std::string> argv = split_args(command);
    if (token.empty()) return argv;
    const std::string safe = safe_arg(value);
    for (std::string& arg : argv)
        for (size_t at = arg.find(token); at != std::string::npos;
             at = arg.find(token, at + safe.size()))
            arg.replace(at, token.size(), safe);
    return argv;
}

}  // namespace gui
