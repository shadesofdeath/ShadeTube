add_library(st_core STATIC
    Async.h
    CrashHandler.cpp CrashHandler.h
    Dispatcher.cpp Dispatcher.h
    Http.cpp Http.h
    I18n.cpp I18n.h
    Log.cpp Log.h
    MemoryStats.cpp MemoryStats.h
    Paths.cpp Paths.h
    Resources.cpp Resources.h
    Settings.cpp Settings.h
    ThreadPool.cpp ThreadPool.h
    Utf.cpp Utf.h
)
# dbghelp: MiniDumpWriteDump (crash reports).
target_link_libraries(st_core PUBLIC st_options YoutubeExplode::YoutubeExplode PRIVATE dbghelp psapi)
