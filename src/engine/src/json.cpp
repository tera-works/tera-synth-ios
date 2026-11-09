#include "hsynth/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace hsynth {

void Json::set(const std::string& key, Json v) {
    for (auto& m : members_)
        if (m.first == key) { m.second = std::move(v); return; }
    members_.emplace_back(key, std::move(v));
}

const Json* Json::find(const std::string& key) const {
    for (auto& m : members_)
        if (m.first == key) return &m.second;
    return nullptr;
}

namespace {

void escape(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    out.push_back('"');
}

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string err;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool fail(const std::string& m) {
        if (err.empty()) err = m + " at offset " + std::to_string(i);
        return false;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o.push_back((char)cp);
        else if (cp < 0x800) { o.push_back((char)(0xC0 | (cp >> 6))); o.push_back((char)(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) {
            o.push_back((char)(0xE0 | (cp >> 12)));
            o.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            o.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            o.push_back((char)(0xF0 | (cp >> 18)));
            o.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            o.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            o.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    bool str(std::string& o) {
        if (s[i] != '"') return fail("expected string");
        ++i;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { o.push_back(c); continue; }
            if (i >= s.size()) return fail("bad escape");
            c = s[i++];
            switch (c) {
                case 'n': o.push_back('\n'); break;
                case 't': o.push_back('\t'); break;
                case 'r': o.push_back('\r'); break;
                case 'b': o.push_back('\b'); break;
                case 'f': o.push_back('\f'); break;
                case 'u': {
                    if (i + 4 > s.size()) return fail("bad \\u escape");
                    unsigned cp = (unsigned)std::strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        unsigned lo = (unsigned)std::strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
                        i += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(o, cp);
                    break;
                }
                default: o.push_back(c);
            }
        }
        if (i >= s.size()) return fail("unterminated string");
        ++i;
        return true;
    }
    bool value(Json& out, int depth) {
        if (depth > 64) return fail("nesting too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        const char c = s[i];
        if (c == '{') {
            ++i;
            out = Json::object();
            ws();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            while (true) {
                ws();
                std::string key;
                if (i >= s.size() || !str(key)) return fail("expected key");
                ws();
                if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                ++i;
                Json v;
                if (!value(v, depth + 1)) return false;
                out.set(key, std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++i;
            out = Json::array();
            ws();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            while (true) {
                Json v;
                if (!value(v, depth + 1)) return false;
                out.push(std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string v;
            if (!str(v)) return false;
            out = Json(std::move(v));
            return true;
        }
        if (s.compare(i, 4, "true") == 0) { i += 4; out = Json(true); return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; out = Json(false); return true; }
        if (s.compare(i, 4, "null") == 0) { i += 4; out = Json(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* begin = s.c_str() + i;
            char* end = nullptr;
            const double d = std::strtod(begin, &end);
            if (end == begin) return fail("bad number");
            i += (size_t)(end - begin);
            out = Json(d);
            return true;
        }
        return fail(std::string("unexpected character '") + c + "'");
    }
};

}  // namespace

bool Json::parse(const std::string& text, Json& out, std::string& error) {
    Parser p{text, 0, {}};
    if (!p.value(out, 0)) { error = p.err; return false; }
    p.ws();
    if (p.i != text.size()) { error = "trailing characters at offset " + std::to_string(p.i); return false; }
    return true;
}

void Json::dumpTo(std::string& out, int indent, int level) const {
    auto nl = [&](int lv) {
        if (indent <= 0) return;
        out.push_back('\n');
        out.append((size_t)(indent * lv), ' ');
    };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            if (!std::isfinite(num_)) { out += "0"; break; }
            char buf[32];
            if (num_ == std::floor(num_) && std::fabs(num_) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", num_);
            else std::snprintf(buf, sizeof buf, "%.7g", num_);
            out += buf;
            break;
        }
        case Type::String: escape(out, str_); break;
        case Type::Array:
            out.push_back('[');
            for (size_t k = 0; k < items_.size(); ++k) {
                if (k) out.push_back(',');
                nl(level + 1);
                items_[k].dumpTo(out, indent, level + 1);
            }
            if (!items_.empty()) nl(level);
            out.push_back(']');
            break;
        case Type::Object:
            out.push_back('{');
            for (size_t k = 0; k < members_.size(); ++k) {
                if (k) out.push_back(',');
                nl(level + 1);
                escape(out, members_[k].first);
                out += indent > 0 ? ": " : ":";
                members_[k].second.dumpTo(out, indent, level + 1);
            }
            if (!members_.empty()) nl(level);
            out.push_back('}');
            break;
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

}  // namespace hsynth
