# Developer test programs. Each module owns a sub-folder with its own CMakeLists.txt.
file(GLOB _test_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/*)
foreach(_dir ${_test_dirs})
    if(IS_DIRECTORY ${_dir} AND EXISTS ${_dir}/CMakeLists.txt)
        add_subdirectory(${_dir})
    endif()
endforeach()
