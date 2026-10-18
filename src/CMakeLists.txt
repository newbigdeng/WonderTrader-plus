CMAKE_MINIMUM_REQUIRED(VERSION 3.10)

PROJECT(WonderTrader)

#3. 环境判断
IF (WIN32)
	MESSAGE(STATUS "Operation System is windows")
ELSEIF (UNIX)
	MESSAGE(STATUS "Operation System is UNIX-like OS's")
ENDIF ()

MESSAGE(STATUS "Generator is ${CMAKE_GENERATOR}")

IF (MSVC)
	ADD_DEFINITIONS (-D_CRT_SECURE_NO_WARNINGS)
	IF(CMAKE_CL_64)
		SET(INCS $ENV{MyDepends141}/include)
		SET(LNKS $ENV{MyDepends141}/lib/x64)

		MESSAGE(STATUS "MyDepends is at $ENV{MyDepends141}")
		MESSAGE(STATUS "Platform is x64")
		SET(PLATFORM x64)
	ELSE()
		SET(INCS $ENV{MyDepends141}/include)
		SET(LNKS $ENV{MyDepends141}/lib/x86)

		MESSAGE(STATUS "MyDepends is at $ENV{MyDepends141}")
		MESSAGE(STATUS "Platform is x86")
		SET(PLATFORM x86)
	ENDIF()
	SET(PREFIX )
	SET(SUFFIX .dll)
ELSE (GNUCC)
	IF(WIN32)
		SET (MyDeps $ENV{MyDependsGcc})
		SET(SUFFIX .dll)

		MESSAGE(STATUS "MyDepends is at $ENV{MyDependsGcc}")
	ELSE(UNIX)
		SET(MyDeps "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/compat" CACHE PATH "Compatibility headers; system libraries are linked on Linux")
		SET(SUFFIX .so)

		MESSAGE(STATUS "MyDepends is at ${MyDeps}")
	ENDIF()

	SET(INCS ${MyDeps}/include)
	IF(WIN32)
        SET(LNKS ${MyDeps}/lib)
    ELSE()
        SET(LNKS "")
    ENDIF()

	MESSAGE(STATUS "Platform is x64")
	SET(PLATFORM x64)
	SET(PREFIX lib)
ENDIF()

SET(CMAKE_CXX_STANDARD 17)

#basic libraries
option(WT_PLUS_RUNTIME_ONLY "Build the supported runtime, backtest, CTP and mock adapters" ON)
if(WT_PLUS_RUNTIME_ONLY)
    foreach(module WTSUtils WTSTools WtMsgQue WtCore WtUftCore WtBtCore WtDtCore
            WtDtHelper WtDtServo WtDataStorage WtDataStorageAD CTPLoader LoaderRunner ParserCTP ParserUDP ParserShm TraderCTP
            TraderMocker WtExeFact WtRiskMonFact WtPorter WtBtPorter WtDtPorter WtUftStraFact WtUftRunner)
        add_subdirectory(${module})
    endforeach()
    option(WT_PLUS_BUILD_TESTS "Build focused native TraderMocker finance tests" OFF)
    if(WT_PLUS_BUILD_TESTS AND UNIX)
        enable_testing()
        add_executable(wt_plus_finance_test
            ${CMAKE_CURRENT_SOURCE_DIR}/../tests/native/trader_mocker_finance.cpp)
        target_include_directories(wt_plus_finance_test PRIVATE ${INCS} ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(wt_plus_finance_test PRIVATE -UNDEBUG)
        target_link_libraries(wt_plus_finance_test PRIVATE dl pthread)
        add_dependencies(wt_plus_finance_test TraderMocker)
        add_test(NAME trader_mocker_finance COMMAND wt_plus_finance_test
            $<TARGET_FILE:TraderMocker> ${CMAKE_BINARY_DIR}/test-results/finance)
    endif()
    option(WT_PLUS_BUILD_PROBES "Build existing controlled native notifier probe" OFF)
    if(WT_PLUS_BUILD_PROBES)
        add_executable(wt_plus_notifier_probe
            ${CMAKE_CURRENT_SOURCE_DIR}/../tests/native/native_mq_probe.cpp)
        target_include_directories(wt_plus_notifier_probe PRIVATE ${INCS} ${CMAKE_CURRENT_SOURCE_DIR}/WtCore)
        target_link_libraries(wt_plus_notifier_probe PRIVATE WtCore WTSTools WTSUtils
            dl pthread boost_filesystem boost_thread)
        set_target_properties(wt_plus_notifier_probe PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/probes)
    endif()
    return()
endif()

ADD_SUBDIRECTORY(WTSUtils)
ADD_SUBDIRECTORY(WTSTools)

#contract Loade through ctp channel
ADD_SUBDIRECTORY(CTPLoader)
ADD_SUBDIRECTORY(CTPOptLoader)
ADD_SUBDIRECTORY(LoaderRunner)

#parser for quote date from ctp channel
ADD_SUBDIRECTORY(ParserCTP)
ADD_SUBDIRECTORY(ParserCTPMini)
ADD_SUBDIRECTORY(ParserCTPOpt)
ADD_SUBDIRECTORY(ParserFemas)
ADD_SUBDIRECTORY(ParserXTP)
ADD_SUBDIRECTORY(ParserShm)
ADD_SUBDIRECTORY(ParserXeleSkt)
ADD_SUBDIRECTORY(WtDataStorage)
ADD_SUBDIRECTORY(WtDataStorageAD)
ADD_SUBDIRECTORY(WtDtCore)
ADD_SUBDIRECTORY(WtDtHelper)
ADD_SUBDIRECTORY(WtDtPorter)
ADD_SUBDIRECTORY(WtDtServo)
#quote date processor
ADD_SUBDIRECTORY(QuoteFactory)

#back test core library
ADD_SUBDIRECTORY(WtBtCore)
#back test c porter for outter libraries
ADD_SUBDIRECTORY(WtBtPorter)
ADD_SUBDIRECTORY(WtBtRunner)

ADD_SUBDIRECTORY(ParserUDP)
ADD_SUBDIRECTORY(TraderCTP)
ADD_SUBDIRECTORY(TraderCTPMini)
ADD_SUBDIRECTORY(TraderCTPOpt)
ADD_SUBDIRECTORY(TraderFemas)
ADD_SUBDIRECTORY(TraderMocker)
ADD_SUBDIRECTORY(TraderXTP)
ADD_SUBDIRECTORY(TraderYD)
ADD_SUBDIRECTORY(WtExeFact)
ADD_SUBDIRECTORY(WtRiskMonFact)

ADD_SUBDIRECTORY(WtMsgQue)
ADD_SUBDIRECTORY(WtCore)
ADD_SUBDIRECTORY(WtPorter)
ADD_SUBDIRECTORY(WtExecMon)
ADD_SUBDIRECTORY(WtRunner)

ADD_SUBDIRECTORY(TraderDumper)

ADD_SUBDIRECTORY(WtShareHelper)

#uft
ADD_SUBDIRECTORY(WtUftCore)
ADD_SUBDIRECTORY(WtUftStraFact)
ADD_SUBDIRECTORY(WtUftRunner)

ADD_SUBDIRECTORY(WtLatencyHFT)
ADD_SUBDIRECTORY(WtLatencyUFT)

#test projects
ADD_SUBDIRECTORY(TestBtPorter)
ADD_SUBDIRECTORY(TestDtPorter)
ADD_SUBDIRECTORY(TestExecPorter)
ADD_SUBDIRECTORY(TestPorter)
ADD_SUBDIRECTORY(TestTrader)
ADD_SUBDIRECTORY(TestParser)
ADD_SUBDIRECTORY(TestUnits)
