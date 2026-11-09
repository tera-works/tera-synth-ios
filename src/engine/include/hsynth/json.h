// Tiny JSON DOM (parse + pretty print). Used only off the audio thread
// (presets, metadata export). Objects keep insertion order.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hsynth {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(double d) : type_(Type::Number), num_(d) {}
    Json(float d) : type_(Type::Number), num_(d) {}
    Json(int d) : type_(Type::Number), num_(d) {}
    Json(const char* s) : type_(Type::String), str_(s) {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray() const { return type_ == Type::Array; }
    bool isBool() const { return type_ == Type::Bool; }

    double number(double def = 0) const { return type_ == Type::Number ? num_ : (type_ == Type::Bool ? (bool_ ? 1 : 0) : def); }
    bool boolean(bool def = false) const { return type_ == Type::Bool ? bool_ : (type_ == Type::Number ? num_ != 0 : def); }
    const std::string& string() const { return str_; }

    // Array
    void push(Json v) { items_.push_back(std::move(v)); }
    size_t size() const { return type_ == Type::Object ? members_.size() : items_.size(); }
    const Json& at(size_t i) const { return items_[i]; }

    // Object
    void set(const std::string& key, Json v);
    const Json* find(const std::string& key) const;
    const std::vector<std::pair<std::string, Json>>& members() const { return members_; }

    std::string dump(int indent = 2) const;
    static bool parse(const std::string& text, Json& out, std::string& error);

private:
    void dumpTo(std::string& out, int indent, int level) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> members_;
};

}  // namespace hsynth
