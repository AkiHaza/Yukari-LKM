cmake_minimum_required(VERSION 3.22)
project(yukari_service_filter LANGUAGES CXX)

option(YUKARI_REQUIRE_ZYGISK "Require Magisk's zygisk.hpp for the injected Android target" OFF)
set(MAGISK_INCLUDE "" CACHE PATH "Magisk include directory containing zygisk.hpp")
if((ANDROID OR YUKARI_REQUIRE_ZYGISK) AND (NOT MAGISK_INCLUDE OR NOT EXISTS "${MAGISK_INCLUDE}/zygisk.hpp"))
  message(FATAL_ERROR "MAGISK_INCLUDE must contain zygisk.hpp for the Android Zygisk target")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(yukari_filter SHARED
  src/service_filter.cpp
  src/native_hooks.cpp
  src/zygisk_module.cpp
)
target_include_directories(yukari_filter PUBLIC include)
if(MAGISK_INCLUDE)
  target_include_directories(yukari_filter PUBLIC "${MAGISK_INCLUDE}")
endif()
target_link_libraries(yukari_filter PRIVATE dl)
target_compile_options(yukari_filter PRIVATE -Wall -Wextra -Werror=return-type)
set_target_properties(yukari_filter PROPERTIES OUTPUT_NAME yukari)

add_executable(service-proxy ../service-proxy/service_proxy.cpp src/service_filter.cpp)
target_include_directories(service-proxy PRIVATE include)
target_compile_options(service-proxy PRIVATE -Wall -Wextra -Werror=return-type)

option(YUKARI_BUILD_TESTS "Build host-side ServiceFilter tests" OFF)
if(YUKARI_BUILD_TESTS)
  enable_testing()
  add_executable(service_filter_test tests/service_filter_test.cpp src/service_filter.cpp)
  target_include_directories(service_filter_test PRIVATE include)
  add_test(NAME service_filter_test COMMAND service_filter_test)
endif()

