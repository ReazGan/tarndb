cmake_minimum_required(VERSION 3.16)
project(tarn VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(TARN_WERROR "Treat warnings as errors" OFF)
option(TARN_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(TARN_FUZZ "Build the libFuzzer target (clang)" OFF)

if(MSVC)
  add_compile_options(/W4)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
  if(TARN_WERROR)
    add_compile_options(/WX)
  endif()
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wvla)
  if(TARN_WERROR)
    add_compile_options(-Werror)
  endif()
endif()

if(TARN_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

if(TARN_FUZZ)
  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
endif()

find_package(Threads REQUIRED)

add_library(tarn
  src/tarn.c
  src/os.c
  src/crc32c.c
  src/siphash.c
  src/index.c)
target_include_directories(tarn PUBLIC include PRIVATE src)
target_link_libraries(tarn PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(tarn PRIVATE bcrypt)
endif()

add_executable(tarn_cli tools/tarn.c)
set_target_properties(tarn_cli PROPERTIES OUTPUT_NAME tarn)
target_link_libraries(tarn_cli PRIVATE tarn)

add_executable(tarn_bench bench/bench.c)
target_link_libraries(tarn_bench PRIVATE tarn)

enable_testing()
add_executable(tarn_test tests/test.c)
target_include_directories(tarn_test PRIVATE src)
target_link_libraries(tarn_test PRIVATE tarn)
add_test(NAME tarn_test COMMAND tarn_test ${CMAKE_CURRENT_BINARY_DIR}/test-tmp)

if(TARN_FUZZ)
  add_executable(fuzz_open fuzz/fuzz_open.c)
  target_include_directories(fuzz_open PRIVATE src)
  target_link_libraries(fuzz_open PRIVATE tarn)
  target_link_options(fuzz_open PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

install(TARGETS tarn tarn_cli)
install(FILES include/tarn.h DESTINATION include)
