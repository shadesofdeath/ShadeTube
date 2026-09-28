#pragma once

#include <string>

namespace YoutubeExplode::Common {

/// Language information.
class Language {
public:
    Language() = default;
    Language(std::string code, std::string name) : code_(std::move(code)), name_(std::move(name)) {}

    /// ISO 639-1 code of the language (may contain a region suffix, e.g. "en-US").
    const std::string& code() const noexcept { return code_; }
    /// Full English name of the language.
    const std::string& name() const noexcept { return name_; }

    std::string toString() const { return code_ + " (" + name_ + ")"; }

    /// Languages are compared by code, case-insensitively.
    friend bool operator==(const Language& a, const Language& b);
    friend bool operator!=(const Language& a, const Language& b) { return !(a == b); }

private:
    std::string code_;
    std::string name_;
};

} // namespace YoutubeExplode::Common
