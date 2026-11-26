# libogg 1.3.6 (Xiph.Org, BSD-3-Clause, see COPYING): Ogg bit packing and framing, used by libvorbis and opusfile.
# Vendored unchanged from https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.gz
# (SHA-256 83e6704730683d004d20e21b8f7f55dcb3383cdf84c0daedf30bde175f774638); only the sources the build needs.
add_library(xiph_ogg STATIC src/framing.c src/bitwise.c)
target_include_directories(xiph_ogg PUBLIC include)
target_compile_options(xiph_ogg PRIVATE /W0)
