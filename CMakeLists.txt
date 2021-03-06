# CMake build script for UPA interactive provider
# x64 Windows Server-only
# 2013/02/07 -- Steven.McCoy@thomsonreuters.com

cmake_minimum_required (VERSION 2.8.8)

project (Chainy)

# Thomson Reuters Transport API
if (MSVC12)     
	set(UPA_BUILD_COMPILER "VS120")
## CMake 3.2.3: no support MSVC 2013 for Boost so explicitly set compiler flag.
	set(Boost_COMPILER "-vc120")
elseif (MSVC11)     
	set(UPA_BUILD_COMPILER "VS110")
elseif (MSVC10)
	set(UPA_BUILD_COMPILER "VS100")
endif ()        
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(UPA_BUILD_TYPE "Debug_MDd")
else (CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(UPA_BUILD_TYPE "Release_MD")
endif (CMAKE_BUILD_TYPE STREQUAL "Debug")
set(UPA_ROOT D:/upa8.0.0.L1.win.rrg)
set(UPA_INCLUDE_DIRS
	${UPA_ROOT}/Include
	${UPA_ROOT}/ValueAdd/Include
)
set(UPA_LIBRARY_DIR ${UPA_ROOT}/Libs/WIN_64_${UPA_BUILD_COMPILER}/${UPA_BUILD_TYPE})
set(UPA_LIBRARY_DIRS
	${UPA_LIBRARY_DIR}
	${UPA_ROOT}/ValueAdd/Libs/WIN_64_${UPA_BUILD_COMPILER}/${UPA_BUILD_TYPE}
)
set(UPA_LIBRARIES
	librsslData
	librsslMessages
	librsslTransport
# UPA ValueAdd except reactor
	librsslRDM
	librsslVACache
	librsslVAUtil
)

# Boost headers plus built libraries
set(BOOST_ROOT D:/boost_1_58_0)
set(BOOST_LIBRARYDIR ${BOOST_ROOT}/stage/lib)
set(Boost_USE_STATIC_LIBS ON)
find_package (Boost 1.50 COMPONENTS atomic chrono thread REQUIRED)

#-----------------------------------------------------------------------------
# force off-tree build

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
message(FATAL_ERROR "CMake generation is not allowed within the source directory!
Remove the CMakeCache.txt file and try again from another folder, e.g.:

   del CMakeCache.txt
   mkdir build
   cd build
   cmake ..
")
endif(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})

#-----------------------------------------------------------------------------
# default to Release build

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING
      "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel."
      FORCE)
endif(NOT CMAKE_BUILD_TYPE)

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)
set(LIBRARY_OUTPUT_PATH  ${CMAKE_BINARY_DIR}/lib)

#-----------------------------------------------------------------------------
# platform specifics

add_definitions(
	-DWIN32
	-DWIN32_LEAN_AND_MEAN
# Windows Server 2008 R2
	-D_WIN32_WINNT=0x0601
# UPA version
        -DUPA_LIBRARY_VERSION="8.0.0."
# std::make_shared<t> limits above default of 5.
	-D_VARIADIC_MAX=10
# production release
##	-DOFFICIAL_BUILD
##	-DCONFIG_SECONDS_RESOLUTION
)

# SEH Exceptions.
string(REGEX REPLACE "/EHsc" "/EHa" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

# Parallel make.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")

# Optimization flags.
# http://msdn.microsoft.com/en-us/magazine/cc301698.aspx
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /GL")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /LTCG")
set(CMAKE_MODULE_LINKER_FLAGS_RELEASE "${CMAKE_MODULE_LINKER_FLAGS_RELEASE} /LTCG")

# Eliminate duplicate strings.
# http://msdn.microsoft.com/en-us/library/s0s0asdt.aspx
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /GF")

# Enable function-level linking.
# http://msdn.microsoft.com/en-us/library/xsa71f43.aspx
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Gy")

if (MSVC12)
# REF and ICF linker optimisations to also work on identical data COMDATs.
# http://blogs.msdn.com/b/vcblog/archive/2013/09/11/introducing-gw-compiler-switch.aspx
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Gw")
endif ()

# Disable buffer security check.
# http://msdn.microsoft.com/en-us/library/8dbf701c(v=vs.80).aspx
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /GS-")

# Debug optimized builds.
# http://randomascii.wordpress.com/2013/09/11/debugging-optimized-codenew-in-visual-studio-2012/
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /d2Zi+")

#-----------------------------------------------------------------------------
# source files

set(chromium-sources
# base/
	src/chromium/base64.cc
	src/chromium/chromium_switches.cc
	src/chromium/command_line.cc
	src/chromium/debug/stack_trace.cc
	src/chromium/debug/stack_trace_win.cc
	src/chromium/files/file.cc
	src/chromium/files/file_util.cc
	src/chromium/files/file_util_win.cc
	src/chromium/json/json_reader.cc
	src/chromium/json/json_writer.cc
	src/chromium/json/string_escape.cc
	src/chromium/md5.cc
	src/chromium/memory/singleton.cc
	src/chromium/message_loop/incoming_task_queue.cc
	src/chromium/message_loop/message_pump.cc
	src/chromium/logging.cc
	src/chromium/pending_task.cc
	src/chromium/sha1_portable.cc
	src/chromium/strings/string_number_conversions.cc
	src/chromium/strings/string_piece.cc
	src/chromium/strings/string_split.cc
	src/chromium/strings/string_util.cc
	src/chromium/strings/stringprintf.cc
	src/chromium/strings/utf_string_conversion_utils.cc
	src/chromium/synchronization/lock.cc
	src/chromium/synchronization/lock_impl_win.cc
	src/chromium/time/time.cc
	src/chromium/time/time_win.cc
	src/chromium/values.cc
	src/chromium/vlog.cc
# net/
        src/net/base/ip_endpoint.cc
        src/net/base/net_errors.cc
        src/net/base/net_errors_win.cc
        src/net/base/net_util.cc
        src/net/http/http_byte_range.cc
        src/net/http/http_request_headers.cc
        src/net/http/http_status_code.cc
        src/net/http/http_util.cc
        src/net/socket/socket_descriptor.cc
        src/net/socket/stream_listen_socket.cc
        src/net/socket/tcp_listen_socket.cc
        src/net/io_buffer.cc
# http_server static library
        src/net/server/http_connection.cc
        src/net/server/http_server.cc
        src/net/server/http_server_request_info.cc
        src/net/server/http_server_response_info.cc
        src/net/server/web_socket.cc
# url/
        src/url/gurl.cc
        src/url/url_canon_etc.cc
        src/url/url_canon_filesystemurl.cc
        src/url/url_canon_fileurl.cc
        src/url/url_canon_host.cc
        src/url/url_canon_internal.cc
        src/url/url_canon_ip.cc
        src/url/url_canon_mailtourl.cc
        src/url/url_canon_path.cc
        src/url/url_canon_pathurl.cc
        src/url/url_canon_query.cc
        src/url/url_canon_stdstring.cc
        src/url/url_canon_stdurl.cc
        src/url/url_constants.cc
        src/url/url_parse_file.cc
        src/url/url_util.cc
# url/third_party/mozilla/
        src/googleurl/url_parse.cc
# base/third_party/icu/
	src/icu/icu_utf.cc
# base/third_party/nspr/
	src/nspr/prtime.cc
# third_party/modp_b64/
	src/modp_b64/modp_b64.cc
)

set(cxx-sources
	src/client.cc
	src/config.cc
	src/consumer.cc
	src/chainy_http_server.cc
	src/main.cc
	src/message_loop.cc
	src/chainy.cc
	src/provider.cc
	src/upa.cc
	src/upaostream.cc
)

include_directories(
	include
	src
	${CMAKE_CURRENT_BINARY_DIR}
	${UPA_INCLUDE_DIRS}
	${Boost_INCLUDE_DIRS}
)

link_directories(
	${UPA_LIBRARY_DIRS}
	${Boost_LIBRARY_DIRS}
)

#-----------------------------------------------------------------------------
# source generators

add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/index.html.h
	COMMAND ${PERL_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/convert_to_macro.pl ${CMAKE_CURRENT_SOURCE_DIR}/htdocs/index.html > ${CMAKE_CURRENT_BINARY_DIR}/index.html.h
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/htdocs/index.html
)
add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/poll.js.h
	COMMAND ${PERL_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/convert_to_macro.pl ${CMAKE_CURRENT_SOURCE_DIR}/htdocs/poll.js > ${CMAKE_CURRENT_BINARY_DIR}/poll.js.h
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/htdocs/poll.js
)

set(generated-sources
	${CMAKE_CURRENT_BINARY_DIR}/index.html.h
	${CMAKE_CURRENT_BINARY_DIR}/poll.js.h
)

#-----------------------------------------------------------------------------
# output

add_executable(Chainy
	${cxx-sources}
	${generated-sources}
	${chromium-sources}
)

target_link_libraries(Chainy
	${UPA_LIBRARIES}
	${Boost_LIBRARIES}
	ws2_32.lib
	wininet.lib
	dbghelp.lib	
)

# end of file
