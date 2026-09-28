#pragma once

// Example IJsEngine implementation that shells out to Node.js (or Deno/Bun with small changes).
// It is NOT part of the library: it exists to show how to plug a JS engine in. For a product,
// embedding QuickJS-ng (MIT) behind the same interface avoids the external process.

#include <YoutubeExplode/JavaScript/IJsEngine.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define YTE_POPEN _popen
#define YTE_PCLOSE _pclose
#else
#define YTE_POPEN popen
#define YTE_PCLOSE pclose
#endif

class NodeJsEngine final : public YoutubeExplode::JavaScript::IJsEngine {
public:
    explicit NodeJsEngine(std::string nodeExecutable = "node") : node_(std::move(nodeExecutable)) {}

    std::string evaluate(const std::string& script) override {
        // Write the script to a temp file; the runner evaluates it in a fresh VM context and prints
        // the completion value of the last statement.
        std::mt19937_64 rng(std::random_device{}());
        const std::string path = tempDirectory() + "yte-" + std::to_string(rng()) + ".js";
        {
            std::ofstream file(path, std::ios::binary);
            if (!file)
                throw std::runtime_error("Failed to create " + path);
            file << script;
        }
        const std::string runner =
            "const fs=require('fs'),vm=require('vm');"
            "const v=vm.runInContext(fs.readFileSync(process.argv[1],'utf8'),vm.createContext({}));"
            "process.stdout.write(String(v));";
        std::string command = quote(node_) + " -e \"" + runner + "\" " + quote(path);
#ifdef _WIN32
        // cmd.exe strips the outermost quotes of a command line that starts with a quote.
        command = "\"" + command + "\"";
#endif

        std::string output;
        FILE* pipe = YTE_POPEN(command.c_str(), "r");
        if (!pipe) {
            std::remove(path.c_str());
            throw std::runtime_error("Failed to start Node.js.");
        }
        char buffer[4096];
        std::size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0)
            output.append(buffer, n);
        const int status = YTE_PCLOSE(pipe);
        std::remove(path.c_str());
        if (status != 0)
            throw std::runtime_error("Node.js exited with status " + std::to_string(status) + ".");
        return output;
    }

private:
    static std::string quote(const std::string& s) { return "\"" + s + "\""; }

    static std::string tempDirectory() {
#ifdef _WIN32
        const char* dir = std::getenv("TEMP");
        return dir ? std::string(dir) + "\\" : std::string(".\\");
#else
        const char* dir = std::getenv("TMPDIR");
        return dir ? std::string(dir) + "/" : std::string("/tmp/");
#endif
    }

    std::string node_;
};
