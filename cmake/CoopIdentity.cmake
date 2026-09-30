find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
get_filename_component(COOP_SOURCE "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
add_custom_target(coop-build-identity
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${COOP_SOURCE}"
          "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/coop_build.h"
          -P "${CMAKE_CURRENT_LIST_DIR}/GenerateCoopIdentity.cmake"
  BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/generated/coop_build.h"
  VERBATIM)
function(coop_identity target)
  add_dependencies(${target} coop-build-identity)
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_link_libraries(${target} OpenSSL::Crypto)
endfunction()
