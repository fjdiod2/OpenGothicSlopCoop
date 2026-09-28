cmake_minimum_required(VERSION 3.16)
project(OpenGothicProtocolTests LANGUAGES CXX)

# Standalone: no renderer, engine dependencies, submodules or Gothic assets.
option(COOP_SANITIZERS "Run protocol tests with address/undefined sanitizers" OFF)
add_executable(coop-protocol-tests protocol_test.cpp ../../common/network/protocol.cpp)
target_include_directories(coop-protocol-tests PRIVATE ../../common)
target_compile_features(coop-protocol-tests PRIVATE cxx_std_20)
set_target_properties(coop-protocol-tests PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(coop-protocol-tests PRIVATE /W4 /WX)
else()
  target_compile_options(coop-protocol-tests PRIVATE -Wall -Wextra -Wconversion -Werror)
endif()
if(COOP_SANITIZERS)
  if(MSVC)
    message(FATAL_ERROR "COOP_SANITIZERS requires Clang or GCC")
  endif()
  target_compile_options(coop-protocol-tests PRIVATE -fsanitize=address,undefined
                         -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  target_link_options(coop-protocol-tests PRIVATE -fsanitize=address,undefined)
endif()

enable_testing()
foreach(case golden message-types fragments combined malformed-header malformed-payload
             encode-validation eof receive-limits write-limits partial-writes stream-stress malformed-stress)
  add_test(NAME protocol-${case} COMMAND coop-protocol-tests ${case})
  set_tests_properties(protocol-${case} PROPERTIES TIMEOUT 20)
endforeach()
