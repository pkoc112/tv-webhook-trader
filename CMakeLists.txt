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
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -DNDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer")
elseif(MSVC)
    add_compile_options(/W4 /permissive-)
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
