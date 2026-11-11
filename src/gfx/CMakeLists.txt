add_library(st_gfx STATIC
    Canvas.cpp Canvas.h
    Device.cpp Device.h
    Icons.cpp Icons.h
    ImageCache.cpp ImageCache.h
    Text.cpp Text.h
    Theme.cpp Theme.h
    Types.cpp Types.h
)
# dcomp: DirectComposition, the swap chain of a window with a system backdrop (Mica, WindowTarget::useComposition).
target_link_libraries(st_gfx PUBLIC st_core dcomp)
