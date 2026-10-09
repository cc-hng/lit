# fmt 12 auto-enables C++20 module scanning (CMAKE_CXX_SCAN_FOR_MODULES),
# needs clang-scan-deps which zig does not ship. Nothing here uses modules.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF CACHE BOOL "No C++20 modules" FORCE)
set(FMT_MODULE OFF CACHE BOOL "Disable fmt C++20 module library" FORCE)
CPMAddPackage("gh:fmtlib/fmt#12.2.0")


macro(add_example TARGET)
  set(multiValueArgs SRCS DEPENDS)
  cmake_parse_arguments(EX "" "" "${multiValueArgs}" ${ARGN})
  list(LENGTH EX_SRCS SRCS_CNT)
  if(${SRCS_CNT})
    add_executable(${TARGET} ${EX_SRCS})
  else()
    add_executable(${TARGET} ${TARGET}.cpp)
  endif()
  target_link_libraries(${TARGET} PRIVATE lit::lit fmt::fmt ${EX_DEPENDS})
  #target_link_time_optimization(${TARGET} REQUIRED)
endmacro()

add_example(httpserver)
add_example(httpclient)
# add_example(wsserver)
# add_example(wsclient)
