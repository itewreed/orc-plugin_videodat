/*
 * File:        vd_json.h
 * Module:      orc-stage-plugin-videodat (VideoDat decoder)
 * Purpose:     Minimal ordered JSON value, serialised like Python's
 *              json.dumps (default separators, optional indent)
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace videodat {

class Json {
public:
    enum class Kind { Null, Bool, Int, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : kind_(Kind::Bool), b_(b) {}
    Json(int v) : kind_(Kind::Int), i_(v) {}
    Json(long v) : kind_(Kind::Int), i_(v) {}
    Json(long long v) : kind_(Kind::Int), i_(v) {}
    Json(unsigned v) : kind_(Kind::Int), i_(v) {}
    Json(unsigned long v) : kind_(Kind::Int), i_(static_cast<int64_t>(v)) {}
    Json(unsigned long long v) : kind_(Kind::Int), i_(static_cast<int64_t>(v)) {}
    Json(const char* s) : kind_(Kind::String), s_(s) {}
    Json(std::string s) : kind_(Kind::String), s_(std::move(s)) {}

    static Json array() { Json j; j.kind_ = Kind::Array; return j; }
    static Json object() { Json j; j.kind_ = Kind::Object; return j; }

    Kind kind() const { return kind_; }
    bool is_null() const { return kind_ == Kind::Null; }

    // Object access: sets (keeping the original position) or appends a key.
    Json& operator[](const std::string& key);
    const Json* find(const std::string& key) const;
    void push_back(Json v) { items_.emplace_back(std::string(), std::move(v)); }
    // dict.update(): later keys overwrite in place, new keys are appended.
    void update(const Json& other);

    bool as_bool() const { return b_; }
    int64_t as_int() const { return i_; }
    const std::string& as_string() const { return s_; }
    const std::vector<std::pair<std::string, Json>>& items() const { return items_; }

    // json.dumps(value, ensure_ascii=..., indent=...). indent < 0: compact
    // with ", " and ": " separators.
    std::string dump(bool ensure_ascii = true, int indent = -1) const;

private:
    void write(std::string& out, bool ensure_ascii, int indent, int level) const;

    Kind kind_{Kind::Null};
    bool b_{false};
    int64_t i_{0};
    std::string s_;
    std::vector<std::pair<std::string, Json>> items_;  // array: keys empty
};

} // namespace videodat
