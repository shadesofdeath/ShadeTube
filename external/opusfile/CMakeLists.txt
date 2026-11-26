# opusfile 0.12 (Xiph.Org, BSD-3-Clause, see COPYING): Ogg Opus files over our own read / seek callbacks (no http.c;
# stream.c only because op_open_file / op_open_memory reference it). Vendored unchanged from https://downloads.xiph.org/releases/opus/opusfile-0.12.tar.gz
# (SHA-256 118d8601c12dd6a44f52423e68ca9083cc9f2bfe72da7a8c1acb22a80ae3550b); only the sources the build needs.
add_library(xiph_opusfile STATIC src/info.c src/internal.c src/opusfile.c src/stream.c)
target_include_directories(xiph_opusfile PUBLIC include)
target_link_libraries(xiph_opusfile PUBLIC xiph_opus xiph_ogg)
target_compile_definitions(xiph_opusfile PRIVATE OP_DISABLE_HTTP OP_DISABLE_DOCS OP_HAVE_LRINTF)
target_compile_options(xiph_opusfile PRIVATE /W0)
