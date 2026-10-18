cmake_minimum_required(VERSION 3.10)
project(WtMsgQue LANGUAGES CXX)
find_package(nng CONFIG REQUIRED)
find_package(Threads REQUIRED)
file(GLOB SRCS *.cpp)
add_library(WtMsgQue SHARED ${SRCS})
target_include_directories(WtMsgQue PRIVATE ${INCS})
target_compile_features(WtMsgQue PRIVATE cxx_std_17)
target_link_libraries(WtMsgQue PRIVATE nng::nng Threads::Threads)
set_target_properties(WtMsgQue PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
