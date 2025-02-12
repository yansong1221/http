cmake_minimum_required(VERSION 3.18 FATAL_ERROR)
project(http)

get_directory_property(_HAS_PARENT PARENT_DIRECTORY)
if(_HAS_PARENT)
    set(IS_ROOT_PROJECT OFF)
else()
    set(IS_ROOT_PROJECT ON)
endif()

if(IS_ROOT_PROJECT)

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(OUTPUT_DIR_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/bin/x64/Debug)
        set(OUTPUT_DIR_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/bin/x64/Release)
    else () 
        set(OUTPUT_DIR_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/bin/x86/Debug)
        set(OUTPUT_DIR_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/bin/x86/Release)
    endif()

    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${OUTPUT_DIR_DEBUG})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG ${OUTPUT_DIR_DEBUG})
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${OUTPUT_DIR_DEBUG})

    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${OUTPUT_DIR_RELEASE})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE ${OUTPUT_DIR_RELEASE})
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${OUTPUT_DIR_RELEASE})

    set_property(GLOBAL PROPERTY USE_FOLDERS ON)

endif(IS_ROOT_PROJECT)

if (MSVC)
    add_compile_options(/bigobj)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED COMPONENTS beast)
find_package(OpenSSL REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_executable(${PROJECT_NAME} 
main.cxx
server.hpp
use_awaitable.hpp
variant_stream.hpp
websocket_conn.hpp
ssl_stream.hpp
proxy_conn.hpp
request.hpp
)
target_link_libraries(${PROJECT_NAME} 
PRIVATE Boost::beast
PRIVATE OpenSSL::SSL OpenSSL::Crypto
PRIVATE spdlog::spdlog
)