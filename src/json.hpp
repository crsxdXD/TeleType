#pragma once
#include <cctype>
#include <cstdio>
#include <string>

namespace tinyjson {
inline std::string escape(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char buf[7]; std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else out += static_cast<char>(c);
    }
    return out;
}
inline void put_utf8(std::string& s, unsigned cp) {
    if (cp <= 0x7F) s += static_cast<char>(cp);
    else if (cp <= 0x7FF) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 63));
    } else if (cp <= 0xFFFF) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 63));
        s += static_cast<char>(0x80 | (cp & 63));
    } else if (cp <= 0x10FFFF) {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 63));
        s += static_cast<char>(0x80 | ((cp >> 6) & 63));
        s += static_cast<char>(0x80 | (cp & 63));
    }
}
inline int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
inline bool parse_hex4(const std::string& s, size_t at, unsigned& cp) {
    if (at + 4 > s.size()) return false;
    cp = 0;
    for (int i = 0; i < 4; ++i) {
        int h = hex(s[at + i]); if (h < 0) return false;
        cp = (cp << 4) | static_cast<unsigned>(h);
    }
    return true;
}
inline bool parse_string(const std::string& s, size_t& pos, std::string& value) {
    if (pos >= s.size() || s[pos++] != '"') return false;
    value.clear();
    while (pos < s.size()) {
        char c = s[pos++];
        if (c == '"') return true;
        if (c != '\\') { value += c; continue; }
        if (pos == s.size()) return false;
        char e = s[pos++];
        switch(e) {
            case '"': value += '"'; break;
            case '\\': value += '\\'; break;
            case '/': value += '/'; break;
            case 'b': value += '\b'; break;
            case 'f': value += '\f'; break;
            case 'n': value += '\n'; break;
            case 'r': value += '\r'; break;
            case 't': value += '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!parse_hex4(s, pos, cp)) return false;
                pos += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (pos + 6 > s.size() || s[pos] != '\\' || s[pos + 1] != 'u') return false;
                    unsigned low = 0;
                    if (!parse_hex4(s, pos + 2, low) || low < 0xDC00 || low > 0xDFFF) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    pos += 6;
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) return false;
                put_utf8(value, cp); break;
            }
            default: return false;
        }
    }
    return false;
}
// Small parser specifically for an OpenAI-compatible chat/completions response.
// Locates choices[0].message.content without a heavyweight JSON dependency.
inline bool completion_text(const std::string& json, std::string& out) {
    size_t choices = json.find("\"choices\"");
    if (choices == std::string::npos) return false;
    size_t start = json.find('[', choices + 9);
    if (start == std::string::npos) return false;
    size_t message = json.find("\"message\"", start);
    if (message == std::string::npos) return false;
    size_t content = json.find("\"content\"", message + 9);
    if (content == std::string::npos) return false;
    size_t colon = json.find(':', content + 9);
    if (colon == std::string::npos) return false;
    size_t p = json.find_first_not_of(" \r\n\t", colon + 1);
    if (p == std::string::npos) return false;
    return parse_string(json, p, out);
}
// Ollama /api/chat returns {"message":{"role":"assistant","content":"..."}}.
// Uses the existing minimal JSON string decoder; no heavyweight library.
inline bool ollama_chat_text(const std::string& json, std::string& out) {
    const size_t message = json.find("\"message\"");
    if (message == std::string::npos) return false;
    const size_t obj = json.find('{', message + 9);
    if (obj == std::string::npos) return false;
    const size_t content = json.find("\"content\"", obj + 1);
    if (content == std::string::npos) return false;
    const size_t colon = json.find(':', content + 9);
    if (colon == std::string::npos) return false;
    const size_t p0 = json.find_first_not_of(" \r\n\t", colon + 1);
    if (p0 == std::string::npos) return false;
    size_t p = p0;
    return parse_string(json, p, out);
}
}  // namespace tinyjson
