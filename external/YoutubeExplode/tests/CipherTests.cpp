#include "Test.hpp"

#include "Bridge/Cipher/CipherManifest.hpp"
#include "Bridge/Cipher/JsScanner.hpp"
#include "Bridge/PlayerSource.hpp"
#include "NodeJsEngine.hpp"
#include "fixtures/SyntheticPlayer.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace YoutubeExplode::detail;

namespace {

const std::string kSignature =
    "NJAJEij0EwRgIhAI0KExTgjfPk-MPM9MAdzyyPRt=BM8-XO5tm5hlMCSVpAiEAv7eP3CURqZNSPow8BXXAoazVoXgeMP7gH9BdylHCwgw=gwzz";

std::string reference(std::string s) {
    // Straightforward re-implementation of the synthetic player's Zq() for cross-checking.
    auto swap = [](std::string& a, int b) { std::swap(a[0], a[static_cast<std::size_t>(b) % a.size()]); };
    swap(s, 3);
    std::reverse(s.begin(), s.end());
    s = s.substr(2);
    swap(s, 61);
    return s;
}

std::optional<std::string> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

bool nodeAvailable() {
    const char* flag = std::getenv("YTE_TEST_NODE");
    return flag && std::string(flag) == "1";
}

} // namespace

TEST(Cipher_operations) {
    CHECK_EQ((CipherOperation{CipherOperation::Kind::Reverse, 0}.apply("abcdef")), std::string("fedcba"));
    CHECK_EQ((CipherOperation{CipherOperation::Kind::Splice, 2}.apply("abcdef")), std::string("cdef"));
    CHECK_EQ((CipherOperation{CipherOperation::Kind::Swap, 3}.apply("abcdef")), std::string("dbcaef"));
    CHECK_EQ((CipherOperation{CipherOperation::Kind::Swap, 8}.apply("abcdef")), std::string("cbadef"));  // 8 % 6
}

TEST(JsScanner_brackets_literals_statements) {
    const std::string code = R"(a={b:"}",c:'{',d:/[}]/g,e:`${f({})}`};x=1;function y(){return 1}if(a)b();else/x/.test(c);z())";
    CHECK_EQ(Js::findMatchingBracket(code, 2), code.find("};") );
    auto statements = Js::splitStatements(code);
    CHECK_EQ(statements.size(), std::size_t(5));
    CHECK_EQ(std::string(statements[2]), std::string("function y(){return 1}"));
    CHECK_EQ(std::string(statements[3]), std::string("if(a)b();else/x/.test(c)"));
    CHECK(Js::isAssignmentStatement("a.b[c]=function(){}"));
    CHECK(Js::isAssignmentStatement("x+=1"));
    CHECK(!Js::isAssignmentStatement("f(a=1)"));
    CHECK(!Js::isAssignmentStatement("a=1,b()"));
    CHECK(!Js::isAssignmentStatement("a&&(b=1)"));
    CHECK(!Js::isAssignmentStatement("a==b"));
    CHECK_EQ(Js::parseStringLiteral(R"("a\x41\u0042\n")", 0).value(), std::string("aAB\n"));
}

TEST(PlayerSource_synthetic_player) {
    PlayerSource player("synthetic", kSyntheticPlayer);
    CHECK_EQ(player.signatureTimestamp().value_or(""), std::string("20668"));
    CHECK_EQ(player.globalArrayName().value_or(""), std::string("Q"));
    CHECK_EQ(player.globalArray().size(), std::size_t(4));
    CHECK(player.cipherManifest().has_value());
    CHECK_EQ(player.cipherManifest()->toString(), std::string("sts=20668 [Swap (3), Reverse, Splice (2), Swap (61)]"));
    CHECK_EQ(player.cipherManifest()->decipher(kSignature), reference(kSignature));
    CHECK(player.signatureScript().has_value());
    CHECK(player.nScript().has_value());
    // The `typeof Kx===Q[3]` early-return guard must have been stripped.
    CHECK(player.nScript()->find("typeof") == std::string::npos);
    CHECK(player.solverScript().has_value());
    // Side-effect statements are dropped, assignments and control statements are kept.
    CHECK(player.solverScript()->find("appendChild") == std::string::npos);
    CHECK(player.solverScript()->find("_yte_solvers.push(kG)") != std::string::npos);
    CHECK(player.solverScript()->find("g.ok=1") != std::string::npos);
}

TEST(PlayerSource_synthetic_player_with_js_engine) {
    if (!nodeAvailable())
        throw yte_test::Skip{"set YTE_TEST_NODE=1 to run (requires node on PATH)"};
    PlayerSource player("synthetic", kSyntheticPlayer);
    NodeJsEngine node;

    const auto viaSnippet = node.evaluate(*player.signatureScript() + "__yte_sig(\"" + kSignature + "\")");
    CHECK_EQ(viaSnippet, reference(kSignature));

    const auto viaSolver = node.evaluate(*player.solverScript() + ";_yte_solve(\"" + kSignature + "\").sig");
    CHECK_EQ(viaSolver, reference(kSignature));

    const auto nSnippet = node.evaluate(*player.nScript() + "__yte_n(\"abc\")");
    const auto nSolver = node.evaluate(*player.solverScript() + ";_yte_solve(undefined,\"abc\").n");
    CHECK_EQ(nSnippet, std::string("fdb"));  // a+1, b+2, c+3, reversed
    CHECK_EQ(nSolver, nSnippet);
}

// ---- Real players (optional) ---------------------------------------------------------------------
// Point YTE_TEST_PLAYERS_DIR at a directory containing real player files named
// base.js-<id> (e.g. fetched from https://www.youtube.com/s/player/<id>/player_ias.vflset/en_US/base.js).
// The expected values below were cross-checked with three independent methods (built-in
// interpreter, extracted snippet in Node.js, whole player in Node.js).

namespace {
struct RealPlayerCase {
    const char* file;
    const char* sts;
    const char* manifest;  // nullptr when the player has no extractable transform plan
    const char* sig;
    const char* n;  // for input "IlLiA21ny7gqA2m4p37"
};

const RealPlayerCase kRealPlayers[] = {
    {"base.js-2022-02-04", "19026", "sts=19026 [Splice (1), Reverse, Splice (1)]",
     "zwg=wgwCHlydB9Hg7PMegXoVzaoAXXB8woPSNZqRUC3Pe7vAEiApVSCMlh5mt5OX-8MB=tRPyyzdAM9MPM-kPfjgTxEK0IAhIgRwE0jiEJAJ",
     "oFTghyXoEZ6TFT7b"},
    {"base.js-2022-04-15", "19096", "sts=19096 [Swap (64), Splice (1), Reverse]",
     "zzwg=wgwCHlydB9Hg7PMegXoVzaoAXXB8woPSNZqRUC3PN7vAEiApVSCMlh5mt5OX-8MB=tRPyyzdAM9MPM-kPfjgTxEK0IAhIgRwE0jiEJAJ",
     "twbzVPrPhlX5Sj"},
    {"base.js-854a788e", "20668", nullptr,
     "zzwg=wgwCHlydB9Hg7PMegXoVzaoAXXB8woPSNZqRUC3Pe7vAEiApVSCMlh5mt5OX-NMB=tRPyyzdAM9MPM-kPfjgTxEK0IAhIgRwE0jiEJA",
     "RL7twTMlYAhbWlqMF"},
};
} // namespace

TEST(PlayerSource_real_players) {
    const char* dir = std::getenv("YTE_TEST_PLAYERS_DIR");
    if (!dir)
        throw yte_test::Skip{"set YTE_TEST_PLAYERS_DIR to run"};
    int found = 0;
    for (const auto& c : kRealPlayers) {
        auto content = readFile(std::string(dir) + "/" + c.file);
        if (!content)
            continue;
        ++found;
        PlayerSource player(c.file, *content);
        CHECK_EQ(player.signatureTimestamp().value_or(""), std::string(c.sts));
        CHECK(player.solverScript().has_value());
        if (c.manifest) {
            CHECK(player.cipherManifest().has_value());
            CHECK_EQ(player.cipherManifest()->toString(), std::string(c.manifest));
            CHECK_EQ(player.cipherManifest()->decipher(kSignature), std::string(c.sig));
        }
        if (nodeAvailable()) {
            NodeJsEngine node;
            CHECK_EQ(node.evaluate(*player.solverScript() + ";_yte_solve(\"" + kSignature + "\").sig"), std::string(c.sig));
            CHECK_EQ(node.evaluate(*player.solverScript() + ";_yte_solve(undefined,\"IlLiA21ny7gqA2m4p37\").n"),
                     std::string(c.n));
        }
    }
    if (found == 0)
        throw yte_test::Skip{"no known player files in YTE_TEST_PLAYERS_DIR"};
}
