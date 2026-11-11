# Nayuki's QR Code generator (C++), vendored unchanged from https://github.com/nayuki/QR-Code-generator (cpp/),
# commit bcfe1b0 (2026-10-29). MIT license: see LICENSE. Draws the pairing QR code of the phone remote control.
add_library(qrcodegen STATIC qrcodegen.cpp qrcodegen.hpp)
target_include_directories(qrcodegen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(qrcodegen PRIVATE /utf-8)
