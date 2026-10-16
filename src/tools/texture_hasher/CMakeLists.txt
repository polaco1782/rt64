cmake_minimum_required(VERSION 3.20)
project(texture_hasher)
set(CMAKE_CXX_STANDARD 17)

include_directories("../../contrib")

add_executable(texture_hasher "texture_hasher.cpp")

# Applications that accept the Rice implementation's GPL license can call it
# directly. The standalone tool remains available for RT64's offline workflow.
add_library(texture_hashing STATIC "texture_hasher.cpp")
target_compile_definitions(texture_hashing PRIVATE RT64_TEXTURE_HASHER_LIBRARY)
target_include_directories(texture_hashing SYSTEM PRIVATE "../../contrib/hlslpp/include")
target_link_libraries(texture_hashing PUBLIC rt64)
