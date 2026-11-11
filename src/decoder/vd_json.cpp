/*
 * File:        vd_json.cpp
 * Module:      orc-stage-plugin-videodat (VideoDat decoder)
 * Purpose:     Minimal ordered JSON value
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include "vd_json.h"

#include <cstdio>

namespace videodat {

Json& Json::operator[](const std::string& key)
{
    kind_ = Kind::Object;
    for (auto& kv : items_) {
        if (kv.first == key) return kv.second;
    }
    items_.emplace_back(key, Json());
    return items_.back().second;
}

const Json* Json::find(const std::string& key) const
{
    for (const auto& kv : items_) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

void Json::update(const Json& other)
{
    for (const auto& kv : other.items_) {
        (*this)[kv.first] = kv.second;
    }
}

namespace {
void write_string(std::string& out, const std::string& s, bool ensure_ascii)
{
    out.push_back('"');
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
        case '"': out += "\\\""; continue;
        case '\\': out += "\\\\"; continue;
        case '\n': out += "\\n"; continue;
        case '\r': out += "\\r"; continue;
        case '\t': out += "\\t"; continue;
        case '\b': out += "\\b"; continue;
        case '\f': out += "\\f"; continue;
        default: break;
        }
        if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else if (c < 0x80 || !ensure_ascii) {
            out.push_back(static_cast<char>(c));
        } else {
            // decode UTF-8 and escape as \uXXXX (with surrogates)
            uint32_t cp = 0;
            int extra = 0;
            if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
            else { cp = c & 0x07; extra = 3; }
            for (int k = 0; k < extra && i + 1 < s.size(); ++k) {
                cp = (cp << 6) | (static_cast<unsigned char>(s[++i]) & 0x3F);
            }
            char buf[16];
            if (cp >= 0x10000) {
                cp -= 0x10000;
                std::snprintf(buf, sizeof buf, "\\u%04x\\u%04x", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
            } else {
                std::snprintf(buf, sizeof buf, "\\u%04x", cp);
            }
            out += buf;
        }
    }
    out.push_back('"');
}
} // namespace

void Json::write(std::string& out, bool ensure_ascii, int indent, int level) const
{
    switch (kind_) {
    case Kind::Null: out += "null"; return;
    case Kind::Bool: out += b_ ? "true" : "false"; return;
    case Kind::Int: out += std::to_string(i_); return;
    case Kind::String: write_string(out, s_, ensure_ascii); return;
    case Kind::Array:
    case Kind::Object: break;
    }
    const bool obj = kind_ == Kind::Object;
    if (items_.empty()) {
        out += obj ? "{}" : "[]";
        return;
    }
    out.push_back(obj ? '{' : '[');
    for (size_t i = 0; i < items_.size(); ++i) {
        if (indent >= 0) {
            out += i ? ",\n" : "\n";
            out.append(static_cast<size_t>(indent * (level + 1)), ' ');
        } else if (i) {
            out += ", ";
        }
        if (obj) {
            write_string(out, items_[i].first, ensure_ascii);
            out += ": ";
        }
        items_[i].second.write(out, ensure_ascii, indent, level + 1);
    }
    if (indent >= 0) {
        out.push_back('\n');
        out.append(static_cast<size_t>(indent * level), ' ');
    }
    out.push_back(obj ? '}' : ']');
}

std::string Json::dump(bool ensure_ascii, int indent) const
{
    std::string out;
    write(out, ensure_ascii, indent, 0);
    return out;
}

} // namespace videodat
