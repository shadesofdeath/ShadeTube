# How the lyrics views draw and move their lines (app/LyricsRender, gfx/TextShape): the motion model, shapes on the real
# graphics device (rows, refusals, the idle release) and their pixels against DirectWrite's. LyricsRender is compiled
# straight in; it needs gfx / ui only.
add_executable(lyricsview_test lyricsview_test.cpp ${CMAKE_SOURCE_DIR}/src/app/LyricsRender.cpp)
target_include_directories(lyricsview_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lyricsview_test PRIVATE st_ui st_core)
