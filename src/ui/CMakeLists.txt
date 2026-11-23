add_library(st_ui STATIC
    Acrylic.cpp Acrylic.h
    Anim.cpp Anim.h
    Controls.cpp Controls.h
    Layout.cpp Layout.h
    Popups.cpp Popups.h
    ScrollDemo.cpp
    ScrollPhysics.cpp ScrollPhysics.h
    TextBox.cpp TextBox.h
    Widget.cpp Widget.h
    Window.cpp Window.h
)
target_link_libraries(st_ui PUBLIC st_gfx)
