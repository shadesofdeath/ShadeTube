#pragma once

#include <string>

namespace YoutubeExplode::JavaScript {

/// Optional hook for a real JavaScript engine (QuickJS, Duktape, V8, a Node/Deno process...).
///
/// The library itself never needs a JS engine for the default code path:
///  - the primary innertube clients return plain, directly playable URLs;
///  - the signature cipher of the fallback client is solved by a built-in minimal
///    interpreter that understands the reverse / splice / swap transform plan.
///
/// The only thing that *requires* real JavaScript is the "n" throttling parameter on
/// URLs returned by web/TV clients, because YouTube's n-transform is a large obfuscated
/// function with loops, arrays and closures. When an engine is supplied, the library
/// extracts that function from the player source and asks the engine to evaluate it.
/// It is also used as a fallback for the signature cipher when the transform plan
/// cannot be extracted.
///
/// Without an engine, such URLs are returned with the original n value: they usually
/// still play, but may be throttled to real-time speed or, on some players, rejected.
class IJsEngine {
public:
    virtual ~IJsEngine() = default;

    /// Evaluates a self-contained script and returns the value of its last expression
    /// converted to a string. Throw any std::exception on failure.
    virtual std::string evaluate(const std::string& script) = 0;
};

} // namespace YoutubeExplode::JavaScript
