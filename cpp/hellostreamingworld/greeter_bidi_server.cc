/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <iostream>
#include <memory>
#include <string>
#include <spdlog/spdlog.h>
#include <grpcpp/grpcpp.h>
#include <chrono>

#ifdef BAZEL_BUILD
#include "examples/protos/helloworld.grpc.pb.h"
#else
#include "hellostreamingworld.grpc.pb.h"
#endif

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using hellostreamingworld::HelloRequest;
using hellostreamingworld::HelloReply;
using hellostreamingworld::MultiGreeter;
using grpc::ServerReaderWriter;

// Logic and data behind the server's behavior.
class GreeterServiceImpl final : public MultiGreeter::Service {
    Status SayHello(ServerContext* context,
                   ServerReaderWriter<HelloReply, HelloRequest>* stream) override {
    HelloRequest note;
    std::list<std::thread> threads;
    std::atomic<bool> done = false;
    while (stream->Read(&note)) {
        spdlog::info("Read: {}@{}", note.name(), note.num_greetings());

        auto wrth = std::thread([stream, note, &done]() {
            const size_t num = note.num_greetings();
            size_t i = 0;
            HelloReply reply;
            reply.set_message(fmt::format("Hello {}@{}/{}", note.name(), i, note.num_greetings()));
            // TODO Write() 是否线程安全？
            while (i < num && stream->Write(reply))
            {
                spdlog::info("Write: {}", reply.message());
                i++;
                reply.set_message(fmt::format("Hello {}@{}/{}", note.name(), i, note.num_greetings()));
                using namespace std::chrono_literals;
                std::this_thread::sleep_for(3s);
            }
            if (i>= num) {
                spdlog::info("Write all. {}/{}", i, num);
            }
            else {
                spdlog::warn("Write failed. {}/{}", i, num);
            }
        });
        threads.push_back(std::move(wrth));
    }
    spdlog::warn("Read failed");
    done.store(true);
    for (auto & wrth : threads) {
        if (wrth.joinable())
        {
            wrth.join();
        }
    }

    return Status::OK;
  }
};

void RunServer() {
  std::string server_address("0.0.0.0:50051");
  GreeterServiceImpl service;

  ServerBuilder builder;
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to an *synchronous* service.
  builder.RegisterService(&service);

  //builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 2*60*60*1000/*default:2h*/);
  //builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 20*1000/*default:20s*/);
  //builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 0);
  //builder.AddChannelArgument(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 2);
  //builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_SENT_PING_INTERVAL_WITHOUT_DATA_MS, 5*60*1000);
  builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 20 * 1000);
  builder.AddChannelArgument(GRPC_ARG_HTTP2_MAX_PING_STRIKES, 5);

  // Finally assemble the server.
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
}

int main(int argc, char** argv) {
  RunServer();

  return 0;
}
