# Song preview: the press-and-hold gesture (ui/HoldGesture) and the session state machine (app/PreviewSession) stepped
# with explicit times and a fake player, the start point, then a real Player (AudioEngine + WASAPI at volume 0) on
# generated WAVs: the preview voice holds and resumes the current track. Compiles the standalone files directly.
add_executable(preview_test preview_test.cpp ${CMAKE_SOURCE_DIR}/src/app/PreviewSession.cpp
                            ${CMAKE_SOURCE_DIR}/src/ui/HoldGesture.cpp)
target_link_libraries(preview_test PRIVATE st_player st_catalog st_core mfplat mfreadwrite mfuuid ole32)
