cmake_minimum_required(VERSION 3.25)

# The legacy application keeps its compiler-default language mode. New domain
# targets use C++20; their adapter exercises the legacy test harness in C++20.
project(HPetriSim LANGUAGES CXX)

option(HPETRISIM_BUILD_MFC_APP "Build the legacy Windows MFC application and adapter" ${WIN32})
set(CMAKE_MFC_FLAG 0)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
add_subdirectory(src/cpp/domain)

if(HPETRISIM_BUILD_MFC_APP)
if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "HPetriSim requires Windows, MSVC, Windows SDK and the MFC component.")
endif()
if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio")
    message(FATAL_ERROR "Use a Visual Studio generator (see CMakePresets.json) for the MFC application.")
endif()
enable_language(RC)

# Match the legacy Win32 baseline: static MFC and static debug/release CRT.
set(CMAKE_MFC_FLAG 1)
include(cmake/LegacySources.cmake)
add_executable(HPetriSim WIN32 ${HPETRISIM_LEGACY_SOURCES}
    src/cpp/hpetrisim/HPetriSim.rc
)
target_include_directories(HPetriSim PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/hpetrisim"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/3rd-party"
)
target_compile_definitions(HPetriSim PRIVATE UNICODE _UNICODE WIN32 _WINDOWS)
target_compile_options(HPetriSim PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W3>"
    "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>"
    # Existing sources take addresses of temporary GDI+ drawing objects.
    "$<$<COMPILE_LANGUAGE:CXX>:/permissive>"
)
target_precompile_headers(HPetriSim PRIVATE src/cpp/hpetrisim/stdafx.h)
target_link_libraries(HPetriSim PRIVATE gdiplus winmm)
set_target_properties(HPetriSim PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    DEBUG_POSTFIX D
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

# Build alongside the application, but do not make the UI use it yet.
add_library(hps_legacy_adapter STATIC src/cpp/adapters/LegacyNetAdapter.cpp)
target_include_directories(hps_legacy_adapter PUBLIC src/cpp PRIVATE src/cpp/hpetrisim src/cpp/3rd-party)
target_compile_definitions(hps_legacy_adapter PRIVATE UNICODE _UNICODE WIN32 _WINDOWS)
target_compile_options(hps_legacy_adapter PRIVATE /W4 /EHsc)
target_precompile_headers(hps_legacy_adapter PRIVATE src/cpp/hpetrisim/stdafx.h)
target_link_libraries(hps_legacy_adapter PUBLIC hps_domain)
endif()

include(CTest)
if(BUILD_TESTING)
    # Catch2 and the MFC test executable must use the same CRT as the application.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    include(FetchContent)
    FetchContent_Declare(Catch2
        URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.8.1.tar.gz
        URL_HASH SHA256=18b3f70ac80fccc340d8c6ff0f339b2ae64944782f8d2fca2bd705cf47cadb79
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    add_subdirectory(tests/domain)
    if(HPETRISIM_BUILD_MFC_APP)
    add_executable(hps_archive_header_tests tests/ArchiveHeaderTests.cpp)
    target_include_directories(hps_archive_header_tests PRIVATE src/cpp/hpetrisim)
    target_compile_definitions(hps_archive_header_tests PRIVATE UNICODE _UNICODE _CONSOLE)
    target_compile_options(hps_archive_header_tests PRIVATE /EHsc)
    target_link_libraries(hps_archive_header_tests PRIVATE Catch2::Catch2WithMain)
    add_test(NAME persistence.archive-header COMMAND hps_archive_header_tests)

    # Link the actual legacy implementations, including their MFC application
    # lifetime objects. The custom console main does not initialize the UI.
    add_executable(hps_simulation_tests ${HPETRISIM_LEGACY_SOURCES}
        tests/SimulationTestMain.cpp tests/SimulationCharacterization.cpp tests/LegacyNetAdapterTests.cpp)
    target_include_directories(hps_simulation_tests PRIVATE src/cpp/hpetrisim src/cpp/3rd-party)
    target_compile_definitions(hps_simulation_tests PRIVATE UNICODE _UNICODE WIN32 _WINDOWS _CONSOLE)
    target_compile_options(hps_simulation_tests PRIVATE /W3 /EHsc /permissive)
    target_precompile_headers(hps_simulation_tests PRIVATE src/cpp/hpetrisim/stdafx.h)
    target_link_libraries(hps_simulation_tests PRIVATE Catch2::Catch2 hps_legacy_adapter gdiplus winmm)
    add_test(NAME simulation.characterization COMMAND hps_simulation_tests "[simulation]")
    set_tests_properties(simulation.characterization PROPERTIES
        TIMEOUT 30 RUN_SERIAL TRUE LABELS "simulation;characterization")
    add_test(NAME domain.legacy-adapter COMMAND hps_simulation_tests "[adapter]")
    set_tests_properties(domain.legacy-adapter PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
    set_tests_properties(domain.legacy-adapter PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
endif()
option(HPETRISIM_UI_TESTS "Enable desktop startup tests (requires an interactive Windows session)" OFF)
if(BUILD_TESTING AND HPETRISIM_BUILD_MFC_APP AND HPETRISIM_UI_TESTS)
    find_program(HPETRISIM_POWERSHELL NAMES powershell REQUIRED)
    add_test(NAME startup.new-document COMMAND "${HPETRISIM_POWERSHELL}"
        -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/StartupSmoke.ps1"
        -Executable "$<TARGET_FILE:HPetriSim>")
    foreach(sample IN ITEMS TokenGame time_sample demo_net Philosophenproblem prozess_connect prozess_simple_connect)
        add_test(NAME "startup.${sample}" COMMAND "${HPETRISIM_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/StartupSmoke.ps1"
            -Executable "$<TARGET_FILE:HPetriSim>" -Document "${CMAKE_CURRENT_SOURCE_DIR}/sample/${sample}.hps")
    endforeach()
    set_tests_properties(startup.new-document startup.TokenGame startup.time_sample startup.demo_net
        startup.Philosophenproblem startup.prozess_connect startup.prozess_simple_connect
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 45 LABELS "ui;characterization")
endif()
