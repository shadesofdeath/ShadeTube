# Memory housekeeping (docs/DEVELOPMENT.md 3.2) on the real graphics device: gfx::ImageCache buckets and expiry,
# gfx::Icons drawing large artwork without cached bitmaps, gfx::Device::trimPool, core/MemoryStats' report and
# app/ListenStats leaving no capacity slack after a read. Links st_ui (gfx, core); ListenStats is standalone and compiled
# straight in. The one asset it draws (a cover placeholder) is embedded like in the app (RCDATA named by asset path).
set(_svg placeholders/album-placeholder.svg)
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/memory_test.rc
     CONTENT "\"${_svg}\" RCDATA \"${CMAKE_SOURCE_DIR}/assets/${_svg}\"\n")
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/memory_test.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/assets/${_svg})
add_executable(memory_test memory_test.cpp ${CMAKE_SOURCE_DIR}/src/app/ListenStats.cpp ${CMAKE_CURRENT_BINARY_DIR}/memory_test.rc)
target_link_libraries(memory_test PRIVATE st_ui st_catalog st_core)
