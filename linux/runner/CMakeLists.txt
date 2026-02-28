cmake_minimum_required(VERSION 3.13)
project(runner LANGUAGES CXX)

# Define the application target. To change its name, change BINARY_NAME in the
# top-level CMakeLists.txt, not the value here, or `flutter run` will no longer
# work.
#
# Any new source files that you add to the application should be added here.
add_executable(${BINARY_NAME}
  "main.cc"
  "my_application.cc"
  "${FLUTTER_MANAGED_DIR}/generated_plugin_registrant.cc"
)

# Apply the standard set of build settings. This can be removed for applications
# that need different build settings.
apply_standard_settings(${BINARY_NAME})

# Add preprocessor definitions for the application ID.
add_definitions(-DAPPLICATION_ID="${APPLICATION_ID}")

# Add dependency libraries. Add any application-specific dependencies here.
target_link_libraries(${BINARY_NAME} PRIVATE flutter)
target_link_libraries(${BINARY_NAME} PRIVATE PkgConfig::GTK)

# On modern GCC (9+), std::filesystem is part of libstdc++ and does not need
# a separate -lstdc++fs.  When building with clang on Ubuntu the GCC dev
# library path may not be on the default search path, so add it explicitly.
if(EXISTS "/usr/lib/gcc/x86_64-linux-gnu")
  file(GLOB _gcc_libdirs "/usr/lib/gcc/x86_64-linux-gnu/*")
  foreach(_dir ${_gcc_libdirs})
    if(IS_DIRECTORY ${_dir})
      target_link_directories(${BINARY_NAME} PRIVATE ${_dir})
    endif()
  endforeach()
endif()

target_include_directories(${BINARY_NAME} PRIVATE "${CMAKE_SOURCE_DIR}")
