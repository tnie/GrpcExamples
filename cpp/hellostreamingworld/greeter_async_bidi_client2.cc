/*
 *
 * Copyright 2016, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <grpc++/grpc++.h>
#include "../AsyncBidiCall.h"
#include "hellostreamingworld.grpc.pb.h"

using grpc::Channel;
using grpc::ClientAsyncReaderWriter;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Status;
using hellostreamingworld::HelloRequest;
using hellostreamingworld::HelloReply;
using hellostreamingworld::MultiGreeter;

std::set<AsyncClientCall*> AsyncClientCall::handle_;
std::mutex AsyncClientCall::mt_;
std::atomic<size_t> AsyncBidiCall::instance_count_;

// NOTE: This is a complex example for an asynchronous, bidirectional streaming
// client. For a simpler example, start with the
// greeter_client/greeter_async_client first.
class AsyncBidiGreeterClient {
    AsyncBidiCall* call_ = nullptr;
 public:
  explicit AsyncBidiGreeterClient(std::shared_ptr<Channel> channel)
      : stub_(MultiGreeter::NewStub(channel)) {
    grpc_thread_.reset(
        new std::thread(std::bind(&AsyncBidiGreeterClient::GrpcThread, this)));
    auto monitor = new Monitor(channel, &cq_);
    constexpr auto second1s = std::chrono::seconds(1);
    auto deadline = std::chrono::system_clock::now() + second1s;
    channel->NotifyOnStateChange(GRPC_CHANNEL_READY, deadline, &cq_, monitor);
  }

  // Similar to the async hello example in greeter_async_client but does not
  // wait for the response. Instead queues up a tag in the completion queue
  // that is notified when the server responds back (or when the stream is
  // closed). Returns false when the stream is requested to be closed.
  bool AsyncSayHello(const std::string& user) {
      if (0 == AsyncBidiCall::instance_count_)
      {
          call_ = new AsyncBidiCall(stub_);
          auto rpc = stub_->PrepareAsyncSayHello(&call_->context(), &cq_);
          rpc->StartCall(reinterpret_cast<void*>(call_));
          call_->set_rpc(move(rpc));
      }
      call_->write(user);
      return true;
  }

  ~AsyncBidiGreeterClient() {
    std::cout << "Shutting down client...." << std::endl;
    cq_.Shutdown();
    grpc_thread_->join();
  }

 private:
  // Runs a gRPC completion-queue processing thread. Checks for 'Next' tag
  // and processes them until there are no more (or when the completion queue
  // is shutdown).
  void GrpcThread() {
    while (true) {
      void* got_tag;
      bool ok = false;
      // Block until the next result is available in the completion queue "cq".
      // The return value of Next should always be checked. This return value
      // tells us whether there is any kind of event or the cq_ is shutting
      // down.
      if (!cq_.Next(&got_tag, &ok)) {
        std::cerr << "Client stream closed. Quitting" << std::endl;
        break;
      }
      AsyncClientCall* ptr = reinterpret_cast<AsyncClientCall*>(got_tag);
      ptr->HandleResponse(ok);

    }
  }


  // The producer-consumer queue we use to communicate asynchronously with the
  // gRPC runtime.
  CompletionQueue cq_;

  // Out of the passed in Channel comes the stub, stored here, our view of the
  // server's exposed services.
  std::shared_ptr<MultiGreeter::Stub> stub_;

  // Thread that notifies the gRPC completion queue tags.
  std::unique_ptr<std::thread> grpc_thread_;
};

int main(int argc, char** argv) {
  AsyncBidiGreeterClient greeter(grpc::CreateChannel(
      "localhost:50051", grpc::InsecureChannelCredentials()));

  std::string text;
  while (true) {
    std::cout << "Enter text (type quit to end): ";
    std::cin >> text;

    // Async RPC call that sends a message and awaits a response.
    if (!greeter.AsyncSayHello(text)) {
      std::cout << "Quitting." << std::endl;
      break;
    }
  }
  return 0;
}
