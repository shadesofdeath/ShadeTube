#include "PlayerSource.hpp"

#include "Cipher/JsScanner.hpp"

#include "../Utils/StringUtils.hpp"

#include <regex>

// ------------------------------------------------------------------------------------------------
// How the player is dissected (see README.md for the long version):
//
// 1. Signature timestamp: `signatureTimestamp:19834` (or `sts:19834`) anywhere in the source.
//
// 2. Global lookup array (newer players): right after "use strict" the player declares
//      var XX="split;reverse;length;...".split(";")      or      var XX=["split","reverse",...]
//    and later refers to strings/method names as XX[12]. We decode that array so that code like
//    `a[XX[3]](XX[0])` can be read as `a["reverse"]("")`.
//
// 3. Signature cipher "callsite": a small function that splits the scrambled signature into
//    characters, applies a sequence of helper calls, and joins it again:
//      Xy=function(a){a=a.split("");Ab.cd(a,3);Ab.ef(a,41);Ab.gh(a,2);return a.join("")}
//    Each statement `Ab.cd(a,3)` is one operation; `Ab` is the helper object.
//
// 4. Helper object: `var Ab={cd:function(a,b){a.splice(0,b)},ef:function(a){a.reverse()},
//    gh:function(a,b){var c=a[0];a[0]=a[b%a.length];a[b%a.length]=c}}`. Each member is classified
//    by what its body does: reverse -> Reverse, splice -> Splice(n), `%`/[0] juggling -> Swap(n).
//
// 5. n-parameter function (throttling): located via the code that reads the "n" query parameter
//    from a stream URL and passes it through a transform, e.g.
//      .get("n"))&&(b=Xy[0](b)          b=String.fromCharCode(110),c=a.get(b))&&(c=Xy[0](c)
//      c=a.get(b))&&(c=Xy(c)            d=Xy[0](c),a.set("n",d)
//    or, as a last resort, by the function whose catch block returns "enhanced_except_..._w8_".
//    If the name has an index (Xy[0]) we resolve `var Xy=[Real]`. The function body is copied
//    verbatim, together with the global array it depends on, and handed to a JS engine.
//
// 6. Whole-player solver (JS engine only; port of yt-dlp's "EJS" solver). Since 2025, YouTube
//    flattens the control flow of the cipher/n code (e.g. `P[l[h^315]](l[h^273])`), so no snippet
//    can be cut out reliably. Instead:
//      - take the body of the player IIFE `(function(g){var window=this; ... })(_yt_player)`,
//      - drop `var window=this` and every top-level expression statement that is not an
//        assignment (those are the side effects that would need a real browser),
//      - find the function whose body contains the statement `x.set("alr","yes")`: it builds a
//        stream URL object and applies the signature transform to the parameter it is given,
//      - generate `_yte_solve(sig, n)` that builds such a URL with `s=<sig>`, sets `n`, and invokes
//        the URL object's first own prototype method (which applies the n transform).
// ------------------------------------------------------------------------------------------------

namespace YoutubeExplode::detail {

namespace {

constexpr std::size_t npos = std::string::npos;

std::string window(const std::string& s, std::size_t begin, std::size_t end) {
    if (begin > s.size()) begin = s.size();
    if (end > s.size()) end = s.size();
    return s.substr(begin, end - begin);
}

} // namespace

PlayerSource::PlayerSource(std::string url, std::string content) : url_(std::move(url)), content_(std::move(content)) {
    extractSignatureTimestamp();
    extractGlobalArray();
    extractSignatureCipher();
    extractNFunction();
    buildPlayerSolver();
    // The player source is large (several MB); keep only what we extracted.
    content_.clear();
    content_.shrink_to_fit();
}

void PlayerSource::note(const std::string& message) {
    if (!diagnostics_.empty())
        diagnostics_ += " ";
    diagnostics_ += message;
}

// ---- 1. Signature timestamp ---------------------------------------------------------------------

void PlayerSource::extractSignatureTimestamp() {
    for (const char* key : {"signatureTimestamp:", "sts:"}) {
        std::size_t from = 0;
        while (true) {
            auto pos = content_.find(key, from);
            if (pos == npos)
                break;
            from = pos + 1;
            if (pos > 0 && Js::isIdentifierChar(content_[pos - 1]))
                continue;
            auto k = pos + std::char_traits<char>::length(key);
            std::string digits;
            while (k < content_.size() && isAsciiDigit(content_[k]))
                digits += content_[k++];
            if (digits.size() >= 5) {
                signatureTimestamp_ = digits;
                return;
            }
        }
    }
    note("Signature timestamp not found.");
}

// ---- 2. Global lookup array ---------------------------------------------------------------------

void PlayerSource::extractGlobalArray() {
    static const std::regex declaration(R"(^\s*var\s+([\w$]+)\s*=\s*)");
    for (const char* marker : {"\"use strict\";", "'use strict';"}) {
        std::size_t from = 0;
        while (true) {
            auto pos = content_.find(marker, from);
            if (pos == npos)
                break;
            from = pos + 1;
            const auto start = pos + std::char_traits<char>::length(marker);
            const auto head = window(content_, start, start + 64);
            std::smatch m;
            if (!std::regex_search(head, m, declaration))
                continue;
            const auto name = m[1].str();
            const auto valueStart = start + static_cast<std::size_t>(m.length(0));
            if (valueStart >= content_.size())
                continue;

            std::vector<std::string> values;
            std::size_t valueEnd = npos;
            const char c = content_[valueStart];
            if (c == '"' || c == '\'') {
                // "a;b;c".split(";")
                std::size_t afterString = 0;
                auto joined = Js::parseStringLiteral(content_, valueStart, &afterString);
                if (!joined || content_.compare(afterString, 7, ".split(") != 0)
                    continue;
                std::size_t afterSep = 0;
                auto separator = Js::parseStringLiteral(content_, afterString + 7, &afterSep);
                if (!separator || afterSep >= content_.size() || content_[afterSep] != ')')
                    continue;
                if (separator->empty()) {
                    for (char ch : *joined) values.emplace_back(1, ch);
                } else {
                    std::size_t p = 0;
                    while (true) {
                        auto q = joined->find(*separator, p);
                        values.push_back(joined->substr(p, q == npos ? npos : q - p));
                        if (q == npos) break;
                        p = q + separator->size();
                    }
                }
                valueEnd = afterSep + 1;
            } else if (c == '[') {
                auto close = Js::findMatchingBracket(content_, valueStart);
                if (close == npos)
                    continue;
                bool ok = true;
                for (auto item : Js::splitTopLevel(std::string_view(content_).substr(valueStart + 1, close - valueStart - 1), ',')) {
                    auto t = trim(item);
                    auto v = Js::parseStringLiteral(t, 0);
                    if (!v) { ok = false; break; }
                    values.push_back(*v);
                }
                if (!ok)
                    continue;
                valueEnd = close + 1;
            } else {
                continue;
            }

            globalArrayName_ = name;
            globalArray_ = std::move(values);
            globalArrayCode_ = "var " + name + "=" + content_.substr(valueStart, valueEnd - valueStart) + ";";
            return;
        }
    }
    // Older players have no global array; that's fine.
}

std::string PlayerSource::resolveGlobalReferences(const std::string& code) const {
    if (!globalArrayName_)
        return code;
    // Replace NAME[123] with the corresponding string literal.
    const auto& name = *globalArrayName_;
    std::string out;
    std::size_t from = 0;
    while (true) {
        auto pos = Js::findIdentifier(code, name, from);
        if (pos == npos) {
            out.append(code, from, npos);
            break;
        }
        auto k = pos + name.size();
        std::size_t close = npos;
        std::string digits;
        if (k < code.size() && code[k] == '[') {
            std::size_t j = k + 1;
            while (j < code.size() && isAsciiDigit(code[j])) digits += code[j++];
            if (j < code.size() && code[j] == ']') close = j;
        }
        auto idx = tryParseInt(digits);
        if (close == npos || !idx || *idx < 0 || static_cast<std::size_t>(*idx) >= globalArray_.size()) {
            out.append(code, from, k - from);
            from = k;
            continue;
        }
        out.append(code, from, pos - from);
        out += Js::toStringLiteral(globalArray_[static_cast<std::size_t>(*idx)]);
        from = close + 1;
    }
    return out;
}

// ---- 3 & 4. Signature cipher --------------------------------------------------------------------

void PlayerSource::extractSignatureCipher() {
    // Find the callsite: <name>=function(<a>){<a>=<a>.split("")... return <a>.join("")}
    // We anchor on `.split(` occurrences and validate the surrounding code with a small regex,
    // which is far faster and more robust than running a regex over the whole multi-MB file.
    static const std::regex head(
        R"((?:([\w$]+)\s*=\s*function|function\s+([\w$]+))\s*\(\s*([\w$]+)\s*\)\s*\{\s*(?:var\s+)?\3\s*=\s*\3\.split\(\s*$)");

    std::optional<Js::FunctionSpan> callsite;
    std::string argument;
    std::string callsiteName;
    const std::string splitToken = ".split(";
    std::size_t from = 0;
    while (!callsite) {
        auto pos = content_.find(splitToken, from);
        if (pos == npos)
            break;
        from = pos + 1;
        const auto afterSplit = pos + splitToken.size();
        const auto prefixStart = pos >= 160 ? pos - 160 : 0;
        const auto prefix = window(content_, prefixStart, afterSplit);
        std::smatch m;
        if (!std::regex_search(prefix, m, head))
            continue;
        // Argument of split() must be "" or a global-array entry equal to "".
        std::size_t argEnd = content_.find(')', afterSplit);
        if (argEnd == npos)
            continue;
        const auto splitArg = resolveGlobalReferences(trim(content_.substr(afterSplit, argEnd - afterSplit)));
        if (splitArg != "\"\"" && splitArg != "''")
            continue;

        const auto funcStart = prefixStart + static_cast<std::size_t>(m.position(0));
        auto fnPos = content_.find("function", funcStart);
        if (fnPos == npos || fnPos > pos)
            continue;
        auto span = Js::parseFunctionAt(content_, fnPos);
        if (!span)
            continue;
        const auto body = resolveGlobalReferences(span->body);
        const auto arg = m[3].str();
        if (!contains(body, arg + ".join(\"\")") && !contains(body, arg + "[\"join\"](\"\")") &&
            !contains(body, arg + ".join('')"))
            continue;
        span->body = body;
        callsite = span;
        argument = arg;
        callsiteName = m[1].matched ? m[1].str() : m[2].str();
    }

    if (!callsite) {
        note("Signature cipher callsite (a=a.split(\"\")...return a.join(\"\")) not found.");
        return;
    }

    // Parse operations: Obj.fn(a,3)   Obj["fn"](a,3)   Obj.fn(a)
    static const std::regex call(
        R"re(^\s*([\w$]+)\s*(?:\.\s*([\w$]+)|\[\s*"([\w$]+)"\s*\])\s*\(\s*([\w$]+)\s*(?:,\s*(\d+)\s*)?\)\s*$)re");
    struct Call {
        std::string container, function;
        int index;
    };
    std::vector<Call> calls;
    std::string containerName;
    for (auto statementView : Js::splitTopLevel(callsite->body, ';')) {
        // Statements may also be comma-joined: Ab.cd(a,3),Ab.ef(a,41)
        for (auto expressionView : Js::splitTopLevel(statementView, ',')) {
            std::string expression(expressionView);
            // A comma split breaks "Ab.cd(a,3)" only at top level, so calls stay intact.
            std::smatch m;
            if (!std::regex_match(expression, m, call) || m[4].str() != argument)
                continue;
            Call c{m[1].str(), m[2].matched ? m[2].str() : m[3].str(), m[5].matched ? std::stoi(m[5].str()) : 0};
            if (containerName.empty())
                containerName = c.container;
            if (c.container == containerName)
                calls.push_back(std::move(c));
        }
    }

    std::optional<std::string> containerCode;
    if (!containerName.empty())
        containerCode = Js::findObjectLiteral(content_, containerName);

    // JS engine script: everything the callsite needs, verbatim.
    if (containerCode) {
        std::string script;
        if (globalArrayCode_)
            script += *globalArrayCode_;
        script += "var " + containerName + "=" + *containerCode + ";";
        script += "var __yte_sig=function(" + callsite->params + "){" + callsite->body + "};";
        signatureScript_ = script;
    }

    if (calls.empty() || !containerCode) {
        note(calls.empty() ? "Signature cipher operations could not be parsed."
                           : "Signature cipher helper object '" + containerName + "' not found.");
        return;
    }

    // Classify helper members.
    enum class Role { Unknown, Reverse, Splice, Swap };
    std::vector<std::pair<std::string, Role>> roles;
    const auto container = resolveGlobalReferences(*containerCode);
    for (auto memberView : Js::splitTopLevel(std::string_view(container).substr(1, container.size() - 2), ',')) {
        std::string member = trim(memberView);
        auto colon = member.find(':');
        if (colon == npos)
            continue;
        std::string key = trim(member.substr(0, colon));
        if (!key.empty() && (key.front() == '"' || key.front() == '\''))
            if (auto parsed = Js::parseStringLiteral(key, 0))
                key = *parsed;
        const std::string value = member.substr(colon + 1);
        Role role = Role::Unknown;
        if (contains(value, "reverse"))
            role = Role::Reverse;
        else if (contains(value, "splice") || contains(value, "slice"))
            role = Role::Splice;
        else if (contains(value, "%") || contains(value, "[0]"))
            role = Role::Swap;
        roles.emplace_back(key, role);
    }

    std::vector<CipherOperation> operations;
    for (const auto& c : calls) {
        Role role = Role::Unknown;
        for (const auto& [name, r] : roles)
            if (name == c.function)
                role = r;
        switch (role) {
            case Role::Reverse: operations.push_back({CipherOperation::Kind::Reverse, 0}); break;
            case Role::Splice: operations.push_back({CipherOperation::Kind::Splice, c.index}); break;
            case Role::Swap: operations.push_back({CipherOperation::Kind::Swap, c.index}); break;
            case Role::Unknown:
                note("Signature cipher helper '" + containerName + "." + c.function + "' could not be classified.");
                return;
        }
    }

    cipherManifest_ = CipherManifest(signatureTimestamp_.value_or(""), std::move(operations));
}

// ---- 5. n-parameter function --------------------------------------------------------------------

void PlayerSource::extractNFunction() {
    struct Candidate {
        std::string name;
        std::optional<int> index;
    };
    std::optional<Candidate> found;

    auto tryPattern = [&](const std::string& anchor, const std::regex& pattern, bool lookBehind) {
        std::size_t from = 0;
        while (!found) {
            auto pos = content_.find(anchor, from);
            if (pos == npos)
                return;
            from = pos + 1;
            std::string text = lookBehind ? window(content_, pos >= 120 ? pos - 120 : 0, pos + anchor.size() + 80)
                                          : window(content_, pos, pos + anchor.size() + 160);
            std::smatch m;
            if (!std::regex_search(text, m, pattern))
                continue;
            Candidate c{m[1].str(), std::nullopt};
            if (m.size() > 2 && m[2].matched)
                c.index = std::stoi(m[2].str());
            found = c;
        }
    };

    // a) .get("n"))&&(b=Xy[0](b)   /   .get("n"))&&(b=Xy(b)
    static const std::regex p1(R"(^\.get\("n"\)\)&&\(\s*[\w$]+\s*=\s*([\w$]+)(?:\[(\d+)\])?\(\s*[\w$]+\s*\))");
    tryPattern(".get(\"n\"))&&(", p1, false);

    // b) b=String.fromCharCode(110),c=a.get(b))&&(c=Xy[0](c)
    //    "nn"[+a.D]),c=a.get(b))&&(c=Xy[0](c)
    static const std::regex p2(
        R"(^(?:String\.fromCharCode\(110\)|"nn"\[\+[\w$.]+\])(?:,[\w$]+\(a\))?,[\w$]+=[\w$]+\.(?:get\([\w$]+\)|[\w$]+\[[\w$]+\]\|\|null)\)&&\(\s*[\w$]+\s*=\s*([\w$]+)(?:\[(\d+)\])?\(\s*[\w$]+\s*\))");
    if (!found) tryPattern("String.fromCharCode(110)", p2, false);
    if (!found) tryPattern("\"nn\"[+", p2, false);

    // c) d=Xy[0](c),a.set("n",d)   /   d=Xy[0](c),a.set(b,d)
    static const std::regex p3(R"(([\w$]+)=([\w$]+)(?:\[(\d+)\])?\([\w$]+\),[\w$]+\.set\((?:"n+"|[\w$]+),\1\))");
    if (!found) {
        std::size_t from = 0;
        while (!found) {
            auto pos = content_.find(".set(", from);
            if (pos == npos)
                break;
            from = pos + 1;
            const auto text = window(content_, pos >= 80 ? pos - 80 : 0, pos + 40);
            std::smatch m;
            if (std::regex_search(text, m, p3)) {
                Candidate c{m[2].str(), std::nullopt};
                if (m[3].matched)
                    c.index = std::stoi(m[3].str());
                found = c;
            }
        }
    }

    // d) Fallback: the function whose catch block returns "..._w8_" + a.
    if (!found) {
        std::size_t from = 0;
        while (!found) {
            auto anchor = content_.find("_w8_", from);
            if (anchor == npos)
                break;
            from = anchor + 1;
            // Walk back over "=function(" occurrences, nearest first.
            std::size_t back = anchor;
            for (int attempts = 0; attempts < 64 && !found; ++attempts) {
                auto fn = content_.rfind("=function(", back);
                if (fn == npos || anchor - fn > 60000)
                    break;
                back = fn == 0 ? 0 : fn - 1;
                auto span = Js::parseFunctionAt(content_, fn + 1);
                if (!span || span->end <= anchor)
                    continue;
                std::size_t nameEnd = fn;
                std::size_t nameStart = nameEnd;
                while (nameStart > 0 && Js::isIdentifierChar(content_[nameStart - 1]))
                    --nameStart;
                if (nameStart < nameEnd)
                    found = Candidate{content_.substr(nameStart, nameEnd - nameStart), std::nullopt};
            }
        }
    }

    if (!found) {
        note("n-parameter function reference not found.");
        return;
    }

    std::string functionName = found->name;
    if (found->index) {
        auto items = Js::findArrayLiteral(content_, found->name);
        if (!items || static_cast<std::size_t>(*found->index) >= items->size()) {
            note("n-parameter function array '" + found->name + "' not found.");
            return;
        }
        functionName = (*items)[static_cast<std::size_t>(*found->index)];
    }

    auto span = Js::findFunctionDefinition(content_, functionName);
    if (!span) {
        note("n-parameter function '" + functionName + "' definition not found.");
        return;
    }

    // Remove the early-return guard `;if(typeof X==="undefined")return a;` that makes the
    // function a no-op when evaluated outside the full player context.
    std::string body = span->body;
    const auto params = Js::splitTopLevel(span->params, ',');
    const std::string firstParam = params.empty() ? std::string() : trim(params.front());
    std::string undefinedRef = "\"undefined\"";
    for (std::size_t i = 0; i < globalArray_.size(); ++i)
        if (globalArray_[i] == "undefined" && globalArrayName_)
            undefinedRef += "|" + *globalArrayName_ + "\\[" + std::to_string(i) + "\\]";
    if (!firstParam.empty()) {
        std::string escapedParam;
        for (char ch : firstParam) {
            if (ch == '$') escapedParam += "\\$";
            else escapedParam += ch;
        }
        std::string escapedUndefined = replaceAll(undefinedRef, "$", "\\$");
        const std::regex guard(R"(;\s*if\s*\(\s*typeof\s+[\w$]+\s*===?\s*(?:)" + escapedUndefined +
                               R"(|'undefined')\s*\)\s*return\s+)" + escapedParam + R"(\s*;)");
        std::string cleaned;
        std::size_t last = 0, search = 0;
        while (true) {
            auto t = body.find("typeof", search);
            if (t == npos)
                break;
            search = t + 1;
            auto semi = body.rfind(';', t);
            if (semi == npos || semi < last || t - semi > 12)
                continue;
            const auto candidate = window(body, semi, semi + 120 + undefinedRef.size());
            std::smatch m;
            if (std::regex_search(candidate, m, guard) && m.position(0) == 0) {
                cleaned.append(body, last, semi - last);
                cleaned += ";";
                last = semi + static_cast<std::size_t>(m.length(0));
                search = last;
            }
        }
        cleaned.append(body, last, npos);
        body = std::move(cleaned);
    }

    std::string script;
    if (globalArrayCode_)
        script += *globalArrayCode_;
    script += "var __yte_n=function(" + span->params + "){" + body + "};";
    nScript_ = script;
}

// ---- 6. Whole-player solver ---------------------------------------------------------------------

namespace {

constexpr const char* kSolverPrelude = R"js(
if (typeof globalThis.XMLHttpRequest === "undefined") { globalThis.XMLHttpRequest = { prototype: {} }; }
if (typeof URL === "undefined") {
  globalThis.location = { hash: "", host: "www.youtube.com", hostname: "www.youtube.com",
    href: "https://www.youtube.com/watch?v=yt-explode", origin: "https://www.youtube.com", password: "",
    pathname: "/watch", port: "", protocol: "https:", search: "?v=yt-explode", username: "" };
} else { globalThis.location = new URL("https://www.youtube.com/watch?v=yt-explode"); }
if (typeof globalThis.document === "undefined") { globalThis.document = Object.create(null); }
if (typeof globalThis.navigator === "undefined") { globalThis.navigator = Object.create(null); }
if (typeof globalThis.self === "undefined") { globalThis.self = globalThis; }
if (typeof globalThis.window === "undefined") { globalThis.window = globalThis; }
var _yte_solvers = [];
)js";

// Calls one candidate URL helper and extracts the transformed parameters.
constexpr const char* kSolverEpilogue = R"js(
globalThis._yte_solve = function (sig, n) {
  var results = [], errors = [];
  for (var i = 0; i < _yte_solvers.length; i++) {
    try {
      var url = _yte_solvers[i]("https://youtube.com/watch?v=yt-explode", "s", sig ? encodeURIComponent(sig) : undefined);
      url.set("n", n);
      var proto = Object.getPrototypeOf(url);
      var keys = Object.keys(proto).concat(Object.getOwnPropertyNames(proto));
      for (var k = 0; k < keys.length; k++) {
        if (["constructor", "set", "get", "clone"].indexOf(keys[k]) < 0) { url[keys[k]](); break; }
      }
      var s = url.get("s");
      results.push(JSON.stringify({ sig: s ? decodeURIComponent(s) : null, n: url.get("n") || null }));
    } catch (e) { errors.push(String(e)); }
  }
  var unique = results.filter(function (v, idx) { return results.indexOf(v) === idx; });
  if (unique.length !== 1) { throw new Error(unique.length ? "ambiguous solutions: " + unique.join(" | ") : "no solutions: " + errors.join(", ")); }
  return JSON.parse(unique[0]);
};
)js";

bool isAlrYesStatement(std::string_view statement) {
    static const std::regex pattern(R"re(^[\w$]+\s*(?:\.\s*[\w$]+|\[[^\]]+\])\s*\(\s*"alr"\s*,\s*"yes"\s*\)$)re");
    if (statement.find("\"alr\"") == std::string_view::npos)
        return false;
    const std::string text(statement);
    return std::regex_match(text, pattern);
}

bool bodyHasAlrYesStatement(const std::string& body) {
    if (body.find("\"alr\"") == std::string::npos)
        return false;
    for (auto statement : Js::splitStatements(body))
        if (isAlrYesStatement(statement))
            return true;
    return false;
}

} // namespace

void PlayerSource::buildPlayerSolver() {
    // Locate the player IIFE: var _yt_player={};(function(g){var window=this;...})(_yt_player);
    const auto iife = content_.find("(function(");
    if (iife == npos || iife > 512) {
        note("Player IIFE not found (whole-player solver unavailable).");
        return;
    }
    auto wrapper = Js::parseFunctionAt(content_, iife + 1);
    if (!wrapper) {
        note("Player IIFE could not be parsed (whole-player solver unavailable).");
        return;
    }
    const std::string prefix = content_.substr(0, iife + 1);  // keeps the opening "("
    const std::string suffix = content_.substr(wrapper->end);

    auto statements = Js::splitStatements(wrapper->body);
    std::string body;
    body.reserve(wrapper->body.size());
    std::vector<std::string> candidates;
    bool first = true;

    for (auto statement : statements) {
        if (first) {
            first = false;
            if (statement.substr(0, 16) == "var window=this" || statement == "var window=this")
                continue;  // `window` must resolve to our global stub
        }
        const auto word = Js::leadingWord(statement);
        const char c0 = statement.empty() ? '\0' : statement.front();
        const bool isDeclarationOrControl =
            word == "var" || word == "let" || word == "const" || word == "function" || word == "class" ||
            word == "if" || word == "for" || word == "while" || word == "do" || word == "try" || word == "switch" ||
            c0 == '{';
        const bool isLiteral = c0 == '"' || c0 == '\'';
        if (!isDeclarationOrControl && !isLiteral && !Js::isAssignmentStatement(statement))
            continue;  // side-effect expression statement: drop it

        body.append(statement.data(), statement.size());
        body += word == "function" ? "\n" : ";\n";

        // Candidate URL helpers: NAME=function(...){...}, function NAME(...){...}, var NAME=function(...){...}
        if (statement.find("\"alr\"") == std::string_view::npos)
            continue;
        if (word == "function") {
            if (auto span = Js::parseFunctionAt(statement, 0); span && bodyHasAlrYesStatement(span->body)) {
                auto open = statement.find('(');
                candidates.push_back(trim(statement.substr(8, open - 8)));
            }
            continue;
        }
        std::vector<std::string_view> declarators;
        if (word == "var" || word == "let" || word == "const")
            declarators = Js::splitTopLevel(statement.substr(word.size()), ',');
        else
            declarators.push_back(statement);
        for (auto declarator : declarators) {
            auto eq = declarator.find('=');
            if (eq == std::string_view::npos)
                continue;
            auto target = trim(declarator.substr(0, eq));
            auto value = trim(declarator.substr(eq + 1));
            if (!startsWith(value, "function"))
                continue;
            if (auto span = Js::parseFunctionAt(value, 0); span && bodyHasAlrYesStatement(span->body))
                candidates.push_back(target);
        }
    }

    if (candidates.empty()) {
        note("URL helper setting alr=yes not found (whole-player solver unavailable).");
        return;
    }

    std::string registration;
    for (const auto& candidate : candidates)
        registration += "_yte_solvers.push(" + candidate + ");\n";

    std::string script;
    script.reserve(content_.size() + 4096);
    script += kSolverPrelude;
    script += prefix;
    script += "function(" + wrapper->params + "){\n" + body + registration + "}";
    script += suffix;
    script += "\n";
    script += kSolverEpilogue;
    solverScript_ = std::move(script);
}

} // namespace YoutubeExplode::detail
