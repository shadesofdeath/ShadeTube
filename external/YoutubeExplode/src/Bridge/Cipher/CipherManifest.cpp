#include "CipherManifest.hpp"

#include <algorithm>
#include <utility>

namespace YoutubeExplode::detail {

std::string CipherOperation::apply(std::string input) const {
    switch (kind) {
        case Kind::Reverse:
            std::reverse(input.begin(), input.end());
            return input;
        case Kind::Splice:
            if (index <= 0)
                return input;
            return static_cast<std::size_t>(index) >= input.size() ? std::string() : input.substr(static_cast<std::size_t>(index));
        case Kind::Swap:
            if (!input.empty()) {
                const auto target = static_cast<std::size_t>(index) % input.size();
                std::swap(input[0], input[target]);
            }
            return input;
    }
    return input;
}

std::string CipherOperation::toString() const {
    switch (kind) {
        case Kind::Reverse: return "Reverse";
        case Kind::Splice: return "Splice (" + std::to_string(index) + ")";
        case Kind::Swap: return "Swap (" + std::to_string(index) + ")";
    }
    return "?";
}

std::string CipherManifest::decipher(std::string input) const {
    for (const auto& op : operations_)
        input = op.apply(std::move(input));
    return input;
}

std::string CipherManifest::toString() const {
    std::string out = "sts=" + signatureTimestamp_ + " [";
    for (std::size_t i = 0; i < operations_.size(); ++i) {
        if (i) out += ", ";
        out += operations_[i].toString();
    }
    return out + "]";
}

} // namespace YoutubeExplode::detail
