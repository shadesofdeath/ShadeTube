add_library(st_catalog STATIC Models.cpp Models.h Credits.cpp Credits.h)
target_link_libraries(st_catalog PUBLIC st_core)
