cmake_minimum_required(VERSION 3.16)
project(GAMEPROJ CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES )
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

add_library(project_warnings INTERFACE)
target_compile_options(project_warnings INTERFACE -Wall -Wextra -Wshadow -Wconversion "-fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/=") #STRIPS from __FILE__

option(ENABLE_SANITIZERS "Enable ASan and UBsan" OFF)
if (ENABLE_SANITIZERS) 
    target_compile_options(project_warnings INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(project_warnings INTERFACE -fsanitize=address,undefined)
endif()

add_library(server_lib STATIC
    src/log.cpp
    src/socket.cpp
    src/server.cpp
    src/parser.cpp
    src/tools.cpp
    src/threadpool.cpp
    src/handlers.cpp
)

target_include_directories(server_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(server_lib PRIVATE project_warnings argon2)

add_executable(server src/main.cpp)

target_link_libraries(server PRIVATE server_lib project_warnings)

