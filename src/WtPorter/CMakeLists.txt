#1. 确定CMake的最低版本需求
CMAKE_MINIMUM_REQUIRED(VERSION 3.10)

#2. 确定工程名
PROJECT(WtPorter LANGUAGES CXX)
SET(CMAKE_CXX_STANDARD 17)

SET(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/WtPorter)

#7. 添加源码
file(GLOB SRCS *.cpp)
file(GLOB HDRS *.h)

IF(MSVC)
	list (APPEND SRCS ../Common/mdump.cpp)
ENDIF()

INCLUDE_DIRECTORIES(${INCS})
LINK_DIRECTORIES(${LNKS})
ADD_LIBRARY(WtPorter SHARED ${SRCS} ${HDRS})

SET(LIBS WtCore WTSTools WTSUtils)

IF (MSVC)
	LIST(APPEND LIBS ws2_32)
ELSE(GNUCC)
	LIST(APPEND LIBS
		dl
		pthread
		boost_filesystem
		boost_thread)
	IF(WIN32)
		LIST(APPEND LIBS
			ws2_32 iconv)
	ENDIF()
ENDIF()

TARGET_LINK_LIBRARIES(WtPorter ${LIBS})
IF (MSVC)
ELSE (GNUCC)
	SET_TARGET_PROPERTIES(WtPorter PROPERTIES
		CXX_VISIBILITY_PRESET hidden
		VISIBILITY_INLINES_HIDDEN 1
        LINK_FLAGS_RELEASE -s)
ENDIF ()

file(MAKE_DIRECTORY ${LIBRARY_OUTPUT_PATH}/parsers/)
file(MAKE_DIRECTORY ${LIBRARY_OUTPUT_PATH}/traders/)
file(MAKE_DIRECTORY ${LIBRARY_OUTPUT_PATH}/executer/)


foreach(module ParserCTP ParserUDP ParserShm)
    if(TARGET ${module})
        add_dependencies(WtPorter ${module})
        add_custom_command(TARGET WtPorter POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${module}> ${LIBRARY_OUTPUT_PATH}/parsers/
            VERBATIM)
    endif()
endforeach()

foreach(module TraderCTP TraderXTP TraderMocker TraderCTPMini TraderCTPOpt TraderFemas)
    if(TARGET ${module})
        add_dependencies(WtPorter ${module})
        add_custom_command(TARGET WtPorter POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${module}> ${LIBRARY_OUTPUT_PATH}/traders/
            VERBATIM)
    endif()
endforeach()

foreach(module WtExeFact)
    if(TARGET ${module})
        add_dependencies(WtPorter ${module})
        add_custom_command(TARGET WtPorter POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${module}> ${LIBRARY_OUTPUT_PATH}/executer/
            VERBATIM)
    endif()
endforeach()

foreach(module WtDataStorage WtDataStorageAD WtRiskMonFact WtMsgQue)
    if(TARGET ${module})
        add_dependencies(WtPorter ${module})
        add_custom_command(TARGET WtPorter POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${module}> ${LIBRARY_OUTPUT_PATH}//
            VERBATIM)
    endif()
endforeach()
