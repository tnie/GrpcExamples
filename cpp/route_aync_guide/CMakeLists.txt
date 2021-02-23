# Minimum CMake required
cmake_minimum_required(VERSION 2.8)

# Project
project(RouteGuide C CXX)

if(NOT MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
else()
  add_definitions(-D_WIN32_WINNT=0x600)
endif()

# Protobuf
# NOTE: we cannot use "CONFIG" mode here because protobuf-config.cmake
# is broken when used with CMAKE_INSTALL_PREFIX
find_package(Protobuf REQUIRED)
message(STATUS "Using protobuf ${protobuf_VERSION}")

# {Protobuf,PROTOBUF}_FOUND is defined based on find_package type ("MODULE" vs "CONFIG").
# For "MODULE", the case has also changed between cmake 3.5 and 3.6.
# We use the legacy uppercase version for *_LIBRARIES AND *_INCLUDE_DIRS variables
# as newer cmake versions provide them too for backward compatibility.
if(Protobuf_FOUND OR PROTOBUF_FOUND)
  if(TARGET protobuf::libprotobuf)
    set(_PROTOBUF_LIBPROTOBUF protobuf::libprotobuf)
  else()
    set(_PROTOBUF_LIBPROTOBUF ${PROTOBUF_LIBRARIES})
    include_directories(${PROTOBUF_INCLUDE_DIRS})
  endif()
  if(TARGET protobuf::protoc)
    set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
  else()
    set(_PROTOBUF_PROTOC ${PROTOBUF_PROTOC_EXECUTABLE})
  endif()
else()
  message(WARNING "Failed to locate libprotobuf and protoc!")
endif()

# gRPC
find_package(gRPC CONFIG REQUIRED)
message(STATUS "Using gRPC ${gRPC_VERSION}")

# gRPC C++ plugin
get_target_property(gRPC_CPP_PLUGIN_EXECUTABLE gRPC::grpc_cpp_plugin
    IMPORTED_LOCATION_DEBUG)

# Proto file
get_filename_component(rg_proto "../../protos/route_guide.proto" ABSOLUTE)
get_filename_component(rg_proto_path "${rg_proto}" PATH)

# Generated sources
protobuf_generate_cpp(rg_proto_srcs rg_proto_hdrs "${rg_proto}")
set(rg_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/route_guide.grpc.pb.cc")
set(rg_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/route_guide.grpc.pb.h")
add_custom_command(
      OUTPUT "${rg_grpc_srcs}" "${rg_grpc_hdrs}"
      COMMAND ${_PROTOBUF_PROTOC}
      ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}" -I "${rg_proto_path}"
        --plugin=protoc-gen-grpc="${gRPC_CPP_PLUGIN_EXECUTABLE}"
        "${rg_proto}"
      DEPENDS "${rg_proto}")

# Generated include directory
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

# Targets greeter_[async_](client|server)
foreach(_target
  route_guide_client route_guide_server)
  #greeter_async_client greeter_async_server)
  add_executable(${_target} "${_target}.cc"
    ${rg_proto_srcs}
    ${rg_grpc_srcs}
	"helper.cc")
  target_link_libraries(${_target}
    ${_PROTOBUF_LIBPROTOBUF}
    gRPC::grpc++_unsecure)
endforeach()
