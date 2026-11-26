# libvorbis 1.3.7 (Xiph.Org, BSD-3-Clause, see COPYING): the Vorbis decoder and vorbisfile (Ogg Vorbis files over our own
# read / seek callbacks). No encoder (vorbisenc.c and its mode / codebook tables are left out).
# Vendored unchanged from https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.gz
# (SHA-256 0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab); only the sources the build needs.
add_library(xiph_vorbis STATIC
    lib/analysis.c lib/bitrate.c lib/block.c lib/codebook.c lib/envelope.c lib/floor0.c lib/floor1.c lib/info.c
    lib/lookup.c lib/lpc.c lib/lsp.c lib/mapping0.c lib/mdct.c lib/psy.c lib/registry.c lib/res0.c lib/sharedbook.c
    lib/smallft.c lib/synthesis.c lib/vorbisfile.c lib/window.c)
target_include_directories(xiph_vorbis PUBLIC include PRIVATE lib)
target_link_libraries(xiph_vorbis PUBLIC xiph_ogg)
target_compile_options(xiph_vorbis PRIVATE /W0)
