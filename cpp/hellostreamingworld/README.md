# hellostreamingworld

此双向流 & 异步的 demo 摘自：https://github.com/perumaalgoog/grpc/tree/perugrpc , commit b52b3362d6805daf04a587d26d19e906559192fc the latest release at 2021年2月23日。略有改动。

通过 vcpkg 使用 grpc 库。

仔细阅读 [CMakeLists.txt][cm]：

> This branch ~~assumes~~ **requires** gRPC and all its dependencies are already installed on this system, so they can be located by find_package().

- 在同一接口 read 未返回之前再次 read ，client 崩溃
- hello-server 写两次时，async-client demo 根本就处理不了
- 调试 grpc 心跳机制，调试其联网状态

## NotifyOnStateChange

调用 `NotifyOnStateChange()` 或者 `WaitForConnected()` 接口输入较大超时时长的时候，如何取消（退出）呢？ 现在只能阻塞其所在线程等待其返回。

尝试通过 shutdown channel 来回调也是无解的：

> shutdown channel? 但没有接口 shutdown，只能依赖其析构。但获取 state 需要有效的 channel，所以无解。

从互联网上、从服务端同事那里都没有找到有效的解决方案，从 grpc-issues 里找到了 grpc 目前的确不支持相关特性的描述：

-  [C++ API to close/disconnect a grpc::Channel, canceling all calls][gi21926] 
-  从 [Provide a way to cancel NotifyOnStateChange ][gi21948]，
-  再追到 [Afford a means of cancelling an in-progress watch_connectivity_state][gi3064]

我自己摸索的，服务端同事在使用的，和上述 issue 里提到的 workaround 都是：使用小的时间间隔，轮询判断。

[1]:https://github.com/tnie/quote-demo/issues/9
[2]:https://github.com/grpc/grpc/issues/9593#issuecomment-277946137
[cm]:CMakeLists.txt
[gi21926]:https://github.com/grpc/grpc/issues/21926
[gi21948]:https://github.com/grpc/grpc/issues/21948
[gi3064]:https://github.com/grpc/grpc/issues/3064

# gRPC C++ Hello World Tutorial

### Install gRPC
Make sure you have installed gRPC on your system. Follow the instructions here:
[https://github.com/grpc/grpc/blob/master/INSTALL](../../../INSTALL.md).

### Get the tutorial source code

The example code for this and our other examples lives in the `examples`
directory. Clone this repository to your local machine by running the
following command:


```sh
$ git clone -b $(curl -L https://grpc.io/release) https://github.com/grpc/grpc
```

Change your current directory to examples/cpp/helloworld

```sh
$ cd examples/cpp/helloworld/
```

### Defining a service

The first step in creating our example is to define a *service*: an RPC
service specifies the methods that can be called remotely with their parameters
and return types. As you saw in the
[overview](#protocolbuffers) above, gRPC does this using [protocol
buffers](https://developers.google.com/protocol-buffers/docs/overview). We
use the protocol buffers interface definition language (IDL) to define our
service methods, and define the parameters and return
types as protocol buffer message types. Both the client and the
server use interface code generated from the service definition.

Here's our example service definition, defined using protocol buffers IDL in
[helloworld.proto](../../protos/helloworld.proto). The `Greeting`
service has one method, `hello`, that lets the server receive a single
`HelloRequest`
message from the remote client containing the user's name, then send back
a greeting in a single `HelloReply`. This is the simplest type of RPC you
can specify in gRPC - we'll look at some other types later in this document.

```protobuf
syntax = "proto3";

option java_package = "ex.grpc";

package helloworld;

// The greeting service definition.
service Greeter {
  // Sends a greeting
  rpc SayHello (HelloRequest) returns (HelloReply) {}
}

// The request message containing the user's name.
message HelloRequest {
  string name = 1;
}

// The response message containing the greetings
message HelloReply {
  string message = 1;
}

```

<a name="generating"></a>
### Generating gRPC code

Once we've defined our service, we use the protocol buffer compiler
`protoc` to generate the special client and server code we need to create
our application. The generated code contains both stub code for clients to
use and an abstract interface for servers to implement, both with the method
defined in our `Greeting` service.

To generate the client and server side interfaces:

```sh
$ make helloworld.grpc.pb.cc helloworld.pb.cc
```
Which internally invokes the proto-compiler as:

```sh
$ protoc -I ../../protos/ --grpc_out=. --plugin=protoc-gen-grpc=grpc_cpp_plugin ../../protos/helloworld.proto
$ protoc -I ../../protos/ --cpp_out=. ../../protos/helloworld.proto
```

### Writing a client

- Create a channel. A channel is a logical connection to an endpoint. A gRPC
  channel can be created with the target address, credentials to use and
  arguments as follows

    ```cpp
    auto channel = CreateChannel("localhost:50051", InsecureChannelCredentials());
    ```

- Create a stub. A stub implements the rpc methods of a service and in the
  generated code, a method is provided to created a stub with a channel:

    ```cpp
    auto stub = helloworld::Greeter::NewStub(channel);
    ```

- Make a unary rpc, with `ClientContext` and request/response proto messages.

    ```cpp
    ClientContext context;
    HelloRequest request;
    request.set_name("hello");
    HelloReply reply;
    Status status = stub->SayHello(&context, request, &reply);
    ```

- Check returned status and response.

    ```cpp
    if (status.ok()) {
      // check reply.message()
    } else {
      // rpc failed.
    }
    ```

For a working example, refer to [greeter_client.cc](greeter_client.cc).

### Writing a server

- Implement the service interface

    ```cpp
    class GreeterServiceImpl final : public Greeter::Service {
      Status SayHello(ServerContext* context, const HelloRequest* request,
          HelloReply* reply) override {
        std::string prefix("Hello ");
        reply->set_message(prefix + request->name());
        return Status::OK;
      }
    };

    ```

- Build a server exporting the service

    ```cpp
    GreeterServiceImpl service;
    ServerBuilder builder;
    builder.AddListeningPort("0.0.0.0:50051", grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<Server> server(builder.BuildAndStart());
    ```

For a working example, refer to [greeter_server.cc](greeter_server.cc).

### Writing asynchronous client and server

gRPC uses `CompletionQueue` API for asynchronous operations. The basic work flow
is
- bind a `CompletionQueue` to a rpc call
- do something like a read or write, present with a unique `void*` tag
- call `CompletionQueue::Next` to wait for operations to complete. If a tag
  appears, it indicates that the corresponding operation is complete.

#### Async client

The channel and stub creation code is the same as the sync client.

- Initiate the rpc and create a handle for the rpc. Bind the rpc to a
  `CompletionQueue`.

    ```cpp
    CompletionQueue cq;
    auto rpc = stub->AsyncSayHello(&context, request, &cq);
    ```

- Ask for reply and final status, with a unique tag

    ```cpp
    Status status;
    rpc->Finish(&reply, &status, (void*)1);
    ```

- Wait for the completion queue to return the next tag. The reply and status are
  ready once the tag passed into the corresponding `Finish()` call is returned.

    ```cpp
    void* got_tag;
    bool ok = false;
    cq.Next(&got_tag, &ok);
    if (ok && got_tag == (void*)1) {
      // check reply and status
    }
    ```

For a working example, refer to [greeter_async_client.cc](greeter_async_client.cc).

#### Async server

The server implementation requests a rpc call with a tag and then wait for the
completion queue to return the tag. The basic flow is

- Build a server exporting the async service

    ```cpp
    helloworld::Greeter::AsyncService service;
    ServerBuilder builder;
    builder.AddListeningPort("0.0.0.0:50051", InsecureServerCredentials());
    builder.RegisterService(&service);
    auto cq = builder.AddCompletionQueue();
    auto server = builder.BuildAndStart();
    ```

- Request one rpc

    ```cpp
    ServerContext context;
    HelloRequest request;
    ServerAsyncResponseWriter<HelloReply> responder;
    service.RequestSayHello(&context, &request, &responder, &cq, &cq, (void*)1);
    ```

- Wait for the completion queue to return the tag. The context, request and
  responder are ready once the tag is retrieved.

    ```cpp
    HelloReply reply;
    Status status;
    void* got_tag;
    bool ok = false;
    cq.Next(&got_tag, &ok);
    if (ok && got_tag == (void*)1) {
      // set reply and status
      responder.Finish(reply, status, (void*)2);
    }
    ```

- Wait for the completion queue to return the tag. The rpc is finished when the
  tag is back.

    ```cpp
    void* got_tag;
    bool ok = false;
    cq.Next(&got_tag, &ok);
    if (ok && got_tag == (void*)2) {
      // clean up
    }
    ```

To handle multiple rpcs, the async server creates an object `CallData` to
maintain the state of each rpc and use the address of it as the unique tag. For
simplicity the server only uses one completion queue for all events, and runs a
main loop in `HandleRpcs` to query the queue.

For a working example, refer to [greeter_async_server.cc](greeter_async_server.cc).

The following is copied from https://github.com/perumaalgoog/grpc/tree/perugrpc/examples/cpp/helloworld with greeter_async_bidi_[client/server].cc and hellostreamingworld.proto.

#### Async bi-directional streaming server/client

Bidirectional streaming RPCs allow both client and server to read and write 
streaming data from/to each other, and this is supported by both synchronous and
asynchronous API. The asynchronous API allows these actions to take place
without blocking using the completion queue, just as in the asynchronous code
examples above.
One notable difference from the previous section on async RPCs and async
streaming (bi-directional or otherwise) is the slightly complex nature of
RPC flows and setup necessary:

* Ensure the server calls `ServerContext::AsyncNotifyWhenDone(&tag)` during the
  the stream setup so that you listen to notifications when the client is done 
  with the stream.
  
* Ensure that the client calls `WritesDone` to indicate it is done with the
  stream.

* Ensure that client/server call `Finish` close the stream. This is an 
  asymmetric API: The server `Finish`es the stream while the client listens
  to the `Finish` notification generated by the server. This is an important 
  distinction and is critical that both client/server handle this properly.

A detailed example is in
[greeter_async_bidi_server.cc](greeter_async_bidi_server.cc)
and [greeter_async_bidi_client.cc](greeter_async_bidi_client.cc).


#### Debugging gRPC

To debug issues:

```bash
export GRPC_VERBOSITY=DEBUG
```


#### Completion queue usage options

In the context of streaming servers/clients, several options are possible:

 1 Completion Queue  :: 1 Server :: 1 RPC (Simplest case)
 
 1 Completion Queue  :: 1 Server :: N RPCs (Sharing completion queue)
 
 N Completion Queues :: 1 Server :: N RPCs (Independent completion queue
  per RPC)
  
 A completion queue can also be shared among *different* RPCs as well, not
  just for the same RPC API. In the context of streaming RPCs such as async
  (bi-directional or uni-directional) streams, an RPC refers to a single stream
  that is active until either side calls `Finish`.

  