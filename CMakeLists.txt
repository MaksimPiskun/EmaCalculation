cmake_minimum_required(VERSION 3.12)

project(EmaCalculation VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
# Find the necessary packages
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)

add_executable(EmaCalculation main.cpp)

target_link_libraries(EmaCalculation
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
)

target_include_directories(EmaCalculation PRIVATE
    ${Boost_INCLUDE_DIRS}      # Boost headers
    ${OPENSSL_INCLUDE_DIR}     # OpenSSL headers
    nlohmann_json::nlohmann_json  # nlohmann::json header (header-only)
)

# Debug message to show the libraries and include directories
message(STATUS "Boost Include Directories: ${Boost_INCLUDE_DIRS}")
message(STATUS "OpenSSL Include Directories: ${OPENSSL_INCLUDE_DIR}")
