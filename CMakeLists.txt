cmake_minimum_required(VERSION 3.13)

# create your C++ project
project(MyProject VERSION 0.1.0 LANGUAGES CXX)

# get the relevant libraries to use from github. FetchContent is great because it pulls the most recent version of a library and will add it to your project.
include(FetchContent)
# go through recursively
# MOE
FetchContent_Declare(MOE 
                     GIT_REPOSITORY https://github.com/mahilab/MOE.git
                     GIT_TAG        origin/main)
FetchContent_MakeAvailable(MOE)

# add your include directories
include_directories("include")

# create an app from your source files
add_executable(ex_skye_demo
    "src/ex_skye_demo.cpp"
)
# link your app to MEL libaries
target_link_libraries(ex_skye_demo moe mahi::daq mahi::robo mahi::com)

add_executable(ex_rom_CSU
    "src/ex_rom_CSU.cpp")
# link your app to MEL libaries
target_link_libraries(ex_rom_CSU moe mahi::daq mahi::robo mahi::com)