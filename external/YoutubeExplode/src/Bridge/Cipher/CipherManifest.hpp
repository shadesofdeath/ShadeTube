#pragma once

#include <string>
#include <vector>

namespace YoutubeExplode::detail {

/// Single step of YouTube's signature transform plan.
struct CipherOperation {
    enum class Kind { Reverse, Splice, Swap };

    Kind kind = Kind::Reverse;
    int index = 0;

    /// reverse: a.reverse()
    /// splice:  a.splice(0, index)            -> drops the first `index` characters
    /// swap:    var c=a[0]; a[0]=a[index % a.length]; a[index % a.length]=c
    std::string apply(std::string input) const;
    std::string toString() const;
};

/// Ordered list of operations that turns an encrypted signature ("s") into the real one.
class CipherManifest {
public:
    CipherManifest(std::string signatureTimestamp, std::vector<CipherOperation> operations)
        : signatureTimestamp_(std::move(signatureTimestamp)), operations_(std::move(operations)) {}

    const std::string& signatureTimestamp() const noexcept { return signatureTimestamp_; }
    const std::vector<CipherOperation>& operations() const noexcept { return operations_; }

    std::string decipher(std::string input) const;
    std::string toString() const;

private:
    std::string signatureTimestamp_;
    std::vector<CipherOperation> operations_;
};

} // namespace YoutubeExplode::detail
