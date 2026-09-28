#pragma once

#include "Cipher/CipherManifest.hpp"

#include <optional>
#include <string>
#include <vector>

namespace YoutubeExplode::detail {

/// Parsed YouTube player JavaScript (base.js).
///
/// Everything here is heuristic, reverse-engineered knowledge of how YouTube's minified player
/// is laid out. It is THE part of the library that breaks when YouTube ships a new player
/// structure. See README.md ("Cipher extraction") for a walkthrough of each step.
class PlayerSource {
public:
    PlayerSource(std::string url, std::string content);

    const std::string& url() const noexcept { return url_; }

    /// Signature timestamp ("sts"), sent to the player endpoint so that the returned
    /// ciphers match this player version.
    const std::optional<std::string>& signatureTimestamp() const noexcept { return signatureTimestamp_; }

    /// Signature transform plan (reverse / splice / swap), for the built-in interpreter.
    const std::optional<CipherManifest>& cipherManifest() const noexcept { return cipherManifest_; }

    /// Self-contained script prefix that defines `__yte_sig(s)`, for a JS engine (fallback path).
    const std::optional<std::string>& signatureScript() const noexcept { return signatureScript_; }

    /// Self-contained script prefix that defines `__yte_n(n)`, for a JS engine.
    const std::optional<std::string>& nScript() const noexcept { return nScript_; }

    /// Whole-player solver (for a JS engine). This is the approach yt-dlp uses since 2025: the
    /// player is executed with browser stubs, side-effect statements removed, and the URL helper
    /// that sets `alr=yes` is used to transform both "s" and "n". Defines `_yte_solve(sig, n)`,
    /// which returns `{sig, n}`. Robust against the control-flow flattening used by modern players.
    const std::optional<std::string>& solverScript() const noexcept { return solverScript_; }

    /// Human-readable description of what failed during extraction (for error messages).
    const std::string& diagnostics() const noexcept { return diagnostics_; }

    /// Name of the global lookup array (`var XX="...".split(";")`), if the player has one.
    const std::optional<std::string>& globalArrayName() const noexcept { return globalArrayName_; }
    const std::vector<std::string>& globalArray() const noexcept { return globalArray_; }

private:
    void extractSignatureTimestamp();
    void extractGlobalArray();
    void extractSignatureCipher();
    void extractNFunction();
    void buildPlayerSolver();

    std::string resolveGlobalReferences(const std::string& code) const;
    void note(const std::string& message);

    std::string url_;
    std::string content_;

    std::optional<std::string> signatureTimestamp_;
    std::optional<CipherManifest> cipherManifest_;
    std::optional<std::string> signatureScript_;
    std::optional<std::string> nScript_;
    std::optional<std::string> solverScript_;

    std::optional<std::string> globalArrayName_;
    std::optional<std::string> globalArrayCode_;
    std::vector<std::string> globalArray_;

    std::string diagnostics_;
};

} // namespace YoutubeExplode::detail
