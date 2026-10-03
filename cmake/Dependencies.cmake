include(FetchContent)

# Use extraction time for downloaded archives when supported by CMake.
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# Download only the two headers the application uses. Hashes pin their contents.
FetchContent_Declare(httplib
  URL https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.3/httplib.h
  URL_HASH SHA256=a0a0c13dc086663863dbe6730a19716f7d3744904b6205496e8301750814dbd5
  DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_Declare(json
  URL https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
  URL_HASH SHA256=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
  DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(httplib json)

if(BUILD_TESTING)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()
