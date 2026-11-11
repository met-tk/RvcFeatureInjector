cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/src/PluginVersion.h" RVC_VERSION_DEFINE REGEX "^#define RVC_PLUGIN_VERSION ")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" RVC_RELEASE_VERSION "${RVC_VERSION_DEFINE}")
project(RvcFeatureInjectorVST3 VERSION ${RVC_RELEASE_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Standard Steinberg VST3 Bundle output structure for Windows x64
set(VST3_BUNDLE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/dist/RvcFeatureInjector.vst3")
set(VST3_BIN_DIR "${VST3_BUNDLE_ROOT}/Contents/x86_64-win")
set(VST3_RES_DIR "${VST3_BUNDLE_ROOT}/Contents/Resources")

add_library(RvcFeatureInjector SHARED
    src/inference/RvcModelEngine.cpp
    src/dsp/RvcFeatureInjector.cpp
    src/ui/PluginEditorView.cpp
    src/vst3/RvcVst3Plugin.cpp
    src/vst3/vst3_exports.def
)

target_include_directories(RvcFeatureInjector PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(RvcFeatureInjector PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(RvcFeatureInjector PRIVATE /W3 /O2 /EHsc /utf-8 /arch:AVX2)
    # The scanner must load the plugin without a global VC++ runtime install.
    set_property(TARGET RvcFeatureInjector PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Link only standard Windows OS libraries so Cubase's vstscanner never fails on external DLL search paths
target_link_libraries(RvcFeatureInjector PRIVATE
    user32
    gdi32
    comdlg32
    shell32
    ole32
    uuid
    winmm
    d2d1
    dwrite
    windowscodecs
)

set_target_properties(RvcFeatureInjector PROPERTIES
    OUTPUT_NAME "RvcFeatureInjector"
    PREFIX ""
    SUFFIX ".vst3"
    LIBRARY_OUTPUT_DIRECTORY "${VST3_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${VST3_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${VST3_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${VST3_BIN_DIR}"
)

# Bundle RVC Daemon Worker into Contents/Resources/tools/
add_custom_command(TARGET RvcFeatureInjector POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${VST3_BIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${VST3_RES_DIR}/tools"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/tools/rvc_daemon_worker.py" "${VST3_RES_DIR}/tools/rvc_daemon_worker.py"
    COMMENT "Packaging RVC Daemon Worker into RvcFeatureInjector.vst3 bundle..."
)

# Automated VST3 Host & Real-Time DSP Benchmark Executable
add_executable(vst3_host_verify
    tests/vst3_host_verify.cpp
)
if(MSVC)
    target_compile_options(vst3_host_verify PRIVATE /W3 /O2 /EHsc /utf-8)
endif()
add_dependencies(vst3_host_verify RvcFeatureInjector)

add_executable(dsp_regressions tests/dsp_regressions.cpp src/dsp/RvcFeatureInjector.cpp)
target_compile_definitions(dsp_regressions PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(dsp_regressions PRIVATE /W3 /O2 /EHsc /utf-8)
endif()
enable_testing()
add_test(NAME dsp_regressions COMMAND dsp_regressions)
add_executable(dsp_seam_regressions tests/dsp_seam_regressions.cpp src/dsp/RvcFeatureInjector.cpp)
target_compile_definitions(dsp_seam_regressions PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(dsp_seam_regressions PRIVATE /W3 /O2 /EHsc /utf-8)
endif()
add_test(NAME dsp_seam_regressions COMMAND dsp_seam_regressions)
add_executable(dsp_cache_regressions tests/dsp_cache_regressions.cpp src/dsp/RvcFeatureInjector.cpp)
target_compile_definitions(dsp_cache_regressions PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(dsp_cache_regressions PRIVATE /W3 /O2 /EHsc /utf-8)
endif()
add_test(NAME dsp_cache_regressions COMMAND dsp_cache_regressions)
add_executable(dsp_capture_startup_regressions tests/dsp_capture_startup_regressions.cpp src/dsp/RvcFeatureInjector.cpp)
target_compile_definitions(dsp_capture_startup_regressions PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(dsp_capture_startup_regressions PRIVATE /W3 /O2 /EHsc /utf-8)
endif()
add_test(NAME dsp_capture_startup_regressions COMMAND dsp_capture_startup_regressions)
add_executable(rvc_engine_regression tests/rvc_engine_regression.cpp src/inference/RvcModelEngine.cpp)
target_compile_definitions(rvc_engine_regression PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX RVC_ENGINE_REGRESSION)
target_link_libraries(rvc_engine_regression PRIVATE shell32 ole32 uuid)
if(MSVC)
    target_compile_options(rvc_engine_regression PRIVATE /W3 /O2 /EHsc /utf-8)
endif()

add_executable(rvc_state_cleanup src/installer/RvcStateCleanup.cpp)
set_target_properties(rvc_state_cleanup PROPERTIES OUTPUT_NAME "rvc-state-cleanup")
target_compile_definitions(rvc_state_cleanup PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(rvc_state_cleanup PRIVATE /W3 /O2 /EHsc /utf-8)
    set_property(TARGET rvc_state_cleanup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
