# Unit tests for the plugin's safeguards (src/sr-guard.c). Plain C with no
# OBS or FFmpeg dependency, so this builds on its own:
#   cmake -S tests -B build_tests && cmake --build build_tests && ctest --test-dir build_tests
cmake_minimum_required(VERSION 3.20)

project(sports-replay-tests LANGUAGES C)

enable_testing()

add_executable(test-guard test-guard.c ../src/sr-guard.c)
target_include_directories(test-guard PRIVATE ../src)
set_target_properties(test-guard PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

if(MSVC)
  target_compile_options(test-guard PRIVATE /W4 /WX)
else()
  target_compile_options(test-guard PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME guard COMMAND test-guard)
