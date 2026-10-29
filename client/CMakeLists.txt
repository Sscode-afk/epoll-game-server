add_library(client_lib STATIC
    src/game.cpp
)
target_include_directories(client_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

include(FetchContent)

set(SFML_BUILD_AUDIO   OFF)
set(SFML_BUILD_NETWORK OFF)

set(SFML_FETCH_EXTRA_ARGS "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    set(SFML_FETCH_EXTRA_ARGS EXCLUDE_FROM_ALL SYSTEM)
endif()

FetchContent_Declare(SFML
    GIT_REPOSITORY https://github.com/SFML/SFML.git
    GIT_TAG        3.0.2
    GIT_SHALLOW    ON
    ${SFML_FETCH_EXTRA_ARGS}
)
FetchContent_MakeAvailable(SFML)

add_executable(client src/main.cpp)
target_link_libraries(client_lib PUBLIC common_proto project_warnings)
target_link_libraries(client_lib PUBLIC SFML::Graphics)
target_link_libraries(client PRIVATE client_lib)