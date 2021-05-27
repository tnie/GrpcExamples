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
#include <sstream>
#include <grpcpp/grpcpp.h>
#include <thread>
#include <atomic>
#include <spdlog/spdlog.h>

#ifdef BAZEL_BUILD
#include "examples/protos/helloworld.grpc.pb.h"
#else
#include "hellostreamingworld.grpc.pb.h"
#endif

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using hellostreamingworld::HelloRequest;
using hellostreamingworld::HelloReply;
using hellostreamingworld::MultiGreeter;
using grpc::ClientReaderWriter;

class GreeterClient {
    struct StreamPkg {
        ClientContext context;
        std::shared_ptr<ClientReaderWriter<HelloRequest, HelloReply> > stream;
        std::thread thRecv;
    };

    std::shared_ptr<StreamPkg> pkg;

 public:
  GreeterClient(std::shared_ptr<Channel> channel)
      : _channel(channel), stub_(MultiGreeter::NewStub(channel))
  {
      _monitor.swap(std::thread([this]() {monitor(); }));
  }

  ~GreeterClient()
  {
      if (pkg && pkg->thRecv.joinable())
      {
          pkg->thRecv.join();
      }
      _run.store(false);
      if (_monitor.joinable())
      {
          _monitor.join();
      }
  }

  void SayHello(size_t i) {
      if (nullptr == pkg)
          pkg = std::make_shared<StreamPkg>();
      if (nullptr == pkg->stream)
      {
          pkg->stream = stub_->SayHello(&(pkg->context));

          /*if (_channel->GetState(false) == GRPC_CHANNEL_READY)
          pkg.stream = stub_->SayHello(&(pkg.context));
          else
          return;*/
      }


      HelloRequest msg;
      msg.set_name("niel");
      msg.set_num_greetings(i);
      if (pkg->stream->Write(msg))
      {
          spdlog::info("write {}@{}", msg.name(), i);
      }
      else
      {
          spdlog::error("write failed: {}@{}", msg.name(), i);
          if (pkg) {
              pkg->stream->WritesDone();
              pkg->context.TryCancel();
          }
          pkg = nullptr;
          return;
      }

    if (pkg->thRecv.get_id() == std::thread::id())
    {

    }

    /*using namespace std::chrono_literals;
    std::this_thread::sleep_for(1s);
    pkg.stream->WritesDone();*/
    //pkg.context.TryCancel();
  }

private:
  void monitor()
  {
      bool shutdn = false;
      while (_run.load())
      {
          auto current_state = _channel->GetState(true);
          switch (current_state)
          {
          case GRPC_CHANNEL_IDLE:
              spdlog::warn(">> GRPC_CHANNEL_IDLE");
              if (shutdn == false)
              {
              }
              shutdn = true;
              break;
          case GRPC_CHANNEL_CONNECTING:
              spdlog::info(">> GRPC_CHANNEL_CONNECTING");
              break;
          case GRPC_CHANNEL_READY:
              // 回调
              spdlog::info(">> GRPC_CHANNEL_READY");
              if (shutdn)
              {
                  spdlog::info("channel is ready for work");
                  shutdn = false;
              }
              break;
          case GRPC_CHANNEL_TRANSIENT_FAILURE:
              spdlog::warn(">> GRPC_CHANNEL_TRANSIENT_FAILURE");
              break;
          case GRPC_CHANNEL_SHUTDOWN:
              spdlog::error(">> GRPC_CHANNEL_SHUTDOWN");
              break;
          default:
              break;
          }
          auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(10);
          spdlog::info("wait for state change or timeout 10s...");
          _channel->WaitForStateChange(current_state, deadline);
          spdlog::info("state change / 10s.");
      }
  }
 private:
  std::unique_ptr<MultiGreeter::Stub> stub_;
  std::shared_ptr<grpc::Channel> _channel;
  std::atomic<bool> _run;
  std::thread _monitor;
};

int main(int argc, char** argv) {
  // Instantiate the client. It requires a channel, out of which the actual RPCs
  // are created. This channel models a connection to an endpoint (in this case,
  // localhost at port 50051). We indicate that the channel isn't authenticated
  // (use of InsecureChannelCredentials()).
    auto args = grpc::ChannelArguments();
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 1000 * 13);
    //GRPC_ARG_KEEPALIVE_TIMEOUT_MS // ignore，无异议
    //args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    //args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);    // 不发数据帧之前的最大 ping 次数。限于周期性接收时，也会循环 ping ?
    // 1 不发数据帧之前? 2 不收数据帧之前? 的最小 ping 间隔
    args.SetInt(GRPC_ARG_HTTP2_MIN_SENT_PING_INTERVAL_WITHOUT_DATA_MS, 1000 * 21);
  GreeterClient greeter(grpc::CreateCustomChannel(
      "8.131.114.171:50051", grpc::InsecureChannelCredentials(), args)); // 192.168.40.130
  //8.131.114.171
  //127.0.0.1
  size_t i = 0;
  while (true)
  {
      ++i;
      greeter.SayHello(i);
      using namespace std::chrono_literals;
      std::this_thread::sleep_for(7s);
  }

  getchar();
  return 0;
}
