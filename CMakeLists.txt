cmake_minimum_required(VERSION 3.29)

project(DalRinneUtopiaRecomp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/CompilerChecks.cmake)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

enable_testing()

add_executable(dal_runtime runtime/src/main.cpp)
target_compile_definitions(dal_runtime PRIVATE DAL_VERSION="${PROJECT_VERSION}")
target_compile_options(dal_runtime PRIVATE -Wall -Wextra)
target_include_directories(dal_runtime PRIVATE runtime/include)

add_test(NAME runtime_version COMMAND dal_runtime --version)
set_tests_properties(runtime_version PROPERTIES
  PASS_REGULAR_EXPRESSION "dal_runtime 0\\.1\\.0")

add_test(NAME runtime_musttail COMMAND dal_runtime --selftest-musttail)
set_tests_properties(runtime_musttail PROPERTIES
  PASS_REGULAR_EXPRESSION "musttail ok")

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(DAL_EBOOT_ELF "" CACHE FILEPATH "Decrypted EBOOT.elf for optional tests")

add_test(NAME tools_unittest
  COMMAND ${Python3_EXECUTABLE} -B -m unittest discover -s tools/tests -t tools
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(tools_unittest PROPERTIES
  ENVIRONMENT "DAL_EBOOT_ELF=${DAL_EBOOT_ELF}")
