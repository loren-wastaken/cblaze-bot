cmake_minimum_required(VERSION 3.16)

project(CBlaze LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

link_directories(/usr/local/lib)

add_executable(cblaze
    main.cpp
    com.cpp
    rulebreak-detec.cpp
)

target_include_directories(cblaze PRIVATE .)

target_link_libraries(cblaze PRIVATE dpp)
