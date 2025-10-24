cmake_minimum_required(VERSION 3.24 FATAL_ERROR)

project(
  lit
  VERSION "0.1.0"
  LANGUAGES CXX C)

string(COMPARE EQUAL ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR} LIT_IS_TOPLEVEL)

option(LIT_WITH_OPENSSL "with openssl" OFF)
option(LIT_USE_LOCAL_PKG "use local packages" OFF)
option(LIT_BUILD_EXAMPLES "Build examples" ${LIT_IS_TOPLEVEL})

include(cmake/cpm.cmake)
if(LIT_USE_LOCAL_PKG)
  CPMAddPackage(NAME cmake-scripts SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../c/cmake-scripts)
else()
  CPMAddPackage("gh:cc-hng/cmake-scripts#v0.1.1")
endif()
include(${cmake-scripts_SOURCE_DIR}/ProjectGuard.cmake)
include(${cmake-scripts_SOURCE_DIR}/core.cmake)

#####################################
# compile & link options
#####################################
cxx_20()

#####################################
# third-party packages
#####################################
find_package(Threads REQUIRED)

set(BOOST_INCLUDE_LIBRARIES "core;asio;beast")
CPMAddPackage(
  NAME Boost
  VERSION 1.89.0 # Versions less than 1.85.0 may need patches for installation targets.
  URL https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-cmake.tar.xz
  URL_HASH SHA256=2c5ec5edcdff47ff55e27ed9560b0a0b94b07bd07ed9928b476150e16b0efc57
  OPTIONS "BOOST_ENABLE_CMAKE ON" "BOOST_SKIP_INSTALL_RULES ON" # Set `OFF` for installation
          "BUILD_SHARED_LIBS OFF" "BUILD_TESTING OFF" "BUILD_GMOCK OFF"
)
CPMAddPackage("gh:microsoft/GSL#v4.2.0")
CPMAddPackage("gh:cc-hng/std_coroutines#v1.0.3")

if (LIT_WITH_OPENSSL)
  find_package(OpenSSL REQUIRED)
endif()

#####################################
# target
#####################################
file(GLOB_RECURSE cxx_sources ${CMAKE_CURRENT_SOURCE_DIR}/src/**.h ${CMAKE_CURRENT_SOURCE_DIR}/src/**.cpp)
list(REMOVE_ITEM cxx_sources "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/pch.h")

if(BUILD_SHARED_LIBS)
  add_library(${PROJECT_NAME} SHARED)
else()
  add_library(${PROJECT_NAME} STATIC)
endif()
target_sources(${PROJECT_NAME} PRIVATE ${cxx_sources})
target_link_libraries(
  ${PROJECT_NAME} PUBLIC
    std::coroutines Microsoft.GSL::GSL Threads::Threads
    Boost::headers Boost::core Boost::asio Boost::beast)
target_include_directories(
  ${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_time_optimization(${PROJECT_NAME} REQUIRED)
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

if (LIT_WITH_OPENSSL)
  target_link_libraries(${PROJECT_NAME} PUBLIC OpenSSL::SSL OpenSSL::Crypto)
  target_compile_definitions(${PROJECT_NAME} PUBLIC  $<$<BOOL:${LIT_WITH_OPENSSL}>:LIT_WITH_OPENSSL>)
endif()

# CPMAddPackage("gh:fmtlib/fmt#11.2.0")
# add_executable(demo ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
# target_link_libraries(demo PRIVATE lit::lit fmt::fmt)
# target_include_directories(demo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# target_precompile_headers(demo PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/pch.h")
# target_link_time_optimization(demo REQUIRED)

if (LIT_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
