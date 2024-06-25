include(FetchContent)

get_property(languages GLOBAL PROPERTY ENABLED_LANGUAGES)
if(NOT "CXX" IN_LIST languages)
    check_language(CXX)
    if(CMAKE_CXX_COMPILER)
      enable_language(CXX)
    else()
      message(STATUS "No CXX support -- perfetto is disabled")
      return()
    endif()
endif()

# We do not want to use FetchContent_Declare/FetchContent_MakeAvailable
# because we do not want to configure and/or compile perfetto here.
# We just need the SDK; however, the SDK is only made available via
# the entire git repository, and the https protocol of android.googlesource.com
# does not allow to download only a single file from a repository.
# FetchContent_Populate just downloads the git repository in the
# perfetto subdirectory (in the current build directory)
# NB: you can deactivate this download by defining FETCHCONTENT_FULLY_DISCONNECTED
FetchContent_Populate(
  perfetto
  GIT_REPOSITORY https://android.googlesource.com/platform/external/perfetto
  GIT_TAG        v45.0
)

if (EXISTS "${CMAKE_CURRENT_BINARY_DIR}/perfetto-src/sdk/perfetto.cc" AND
    EXISTS "${CMAKE_CURRENT_BINARY_DIR}/perfetto-src/sdk/perfetto.h")
    message(STATUS "Perfetto is downloaded in ${CMAKE_CURRENT_BINARY_DIR}/perfetto-src. Found SDK files.")
    add_library(perfettosdk STATIC)
    target_sources(perfettosdk PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/perfetto-src/sdk/perfetto.cc")
    target_include_directories(perfettosdk INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/perfetto-src/")
    target_compile_features(perfettosdk INTERFACE cxx_std_17)

    add_executable(parsec-dbp2pf dbp2pf.cc ../dbpreader.c)
    set_target_properties(parsec-dbp2pf PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(parsec-dbp2pf parsec-base perfettosdk)
    install(TARGETS parsec-dbp2pf RUNTIME DESTINATION ${PARSEC_INSTALL_BINDIR})
else()
    message(FATAL_ERROR "Could not download perfetto in ${CMAKE_CURRENT_BINARY_DIR}/perfetto-src")
endif()