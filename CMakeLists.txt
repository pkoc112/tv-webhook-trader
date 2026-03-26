cmake_minimum_required(VERSION 3.20)
project(tv_webhook_trader VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Cross-platform compile flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -DNDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer")
elseif(MSVC)
    add_compile_options(/W4 /permissive- /bigobj)
    add_compile_definitions(_WIN32_WINNT=0x0A00)  # Windows 10+
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")
endif()

find_package(Boost 1.74 REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)

include(FetchContent)
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(json)

# -- 공통 인클루드 디렉토리 --
set(COMMON_INCLUDES ${CMAKE_SOURCE_DIR}/include)

# -- 공통 라이브러리 --
set(COMMON_LIBS
    Boost::system OpenSSL::SSL OpenSSL::Crypto
    Threads::Threads fmt::fmt spdlog::spdlog
    nlohmann_json::nlohmann_json
)

# ======== 메인 트레이딩 서버 ========
add_executable(${PROJECT_NAME} src/main.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE ${COMMON_INCLUDES})
target_link_libraries(${PROJECT_NAME} PRIVATE ${COMMON_LIBS})

# ======== 백테스트 & WF 최적화 ========
add_executable(backtest src/backtest_main.cpp)
target_include_directories(backtest PRIVATE ${COMMON_INCLUDES})
target_link_libraries(backtest PRIVATE
    fmt::fmt spdlog::spdlog nlohmann_json::nlohmann_json Threads::Threads
)

install(TARGETS ${PROJECT_NAME} backtest DESTINATION bin)
install(DIRECTORY config/ DESTINATION etc/tv-webhook-trader)

# ======== Unit Tests (Google Test) ========
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
        GIT_SHALLOW TRUE
    )
    # Prevent GoogleTest from overriding compiler/linker settings on Windows
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    set(TEST_LIBS
        GTest::gtest_main
        spdlog::spdlog fmt::fmt
        nlohmann_json::nlohmann_json
        Threads::Threads
    )

    # test_signal_parser
    add_executable(test_signal_parser tests/test_signal_parser.cpp)
    target_include_directories(test_signal_parser PRIVATE ${COMMON_INCLUDES})
    target_link_libraries(test_signal_parser PRIVATE ${TEST_LIBS})
    add_test(NAME test_signal_parser COMMAND test_signal_parser)

    # test_position_sizer
    add_executable(test_position_sizer tests/test_position_sizer.cpp)
    target_include_directories(test_position_sizer PRIVATE ${COMMON_INCLUDES})
    target_link_libraries(test_position_sizer PRIVATE ${TEST_LIBS})
    add_test(NAME test_position_sizer COMMAND test_position_sizer)

    # test_risk_manager
    add_executable(test_risk_manager tests/test_risk_manager.cpp)
    target_include_directories(test_risk_manager PRIVATE ${COMMON_INCLUDES})
    target_link_libraries(test_risk_manager PRIVATE ${TEST_LIBS})
    add_test(NAME test_risk_manager COMMAND test_risk_manager)

    # test_portfolio_risk
    add_executable(test_portfolio_risk tests/test_portfolio_risk.cpp)
    target_include_directories(test_portfolio_risk PRIVATE ${COMMON_INCLUDES})
    target_link_libraries(test_portfolio_risk PRIVATE ${TEST_LIBS})
    add_test(NAME test_portfolio_risk COMMAND test_portfolio_risk)

    # Convenience target: build all tests
    add_custom_target(all_tests DEPENDS
        test_signal_parser test_position_sizer
        test_risk_manager test_portfolio_risk
    )
endif()
