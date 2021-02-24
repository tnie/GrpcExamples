//
// Created by zjf on 2018/3/9.
//

#ifndef QUOTE_SERVER_CLIENT_RPC_H
#define QUOTE_SERVER_CLIENT_RPC_H

#include "client_impl.h"
#include "tag_base.h"
#include "rpc_reader.h"
#include "rpc_writer.h"

#include <grpc++/grpc++.h>
using namespace grpc;

enum class ClientRPCStatus { CREATE, READ, WRITE, WORKING, FINISH, DESTORY, ERR };

/// 对[Server streaming RPC](https://grpc.io/docs/guides/concepts.html#server-streaming-rpc)的客户端异步抽象封装。
/// client_uni_stream_rpc对应protobuf文件中某个service的rpc方法。此类为抽象类。具体实现步骤如下：
/// \li 子类须调用stub的异步RPC，并将tag指向自己。并且根据返回的ClientAsyncReader初始化reader_。
/// \li 在process()方法，调用reader_->read()触发读操作。
/// \tparam W 写出的数据类型。
/// \tparam R 读取的数据类型。
template <typename W, typename R>
class client_uni_stream_rpc
        : public tag_base , public reader_callback{
public:
    client_uni_stream_rpc() {
        status = ClientRPCStatus::CREATE;
    };

    /// 继承自tag_base。子类需要处理来着CompletionQueue的回调事件。
    /// 事件主要有2类：一类是客户端的异步RPC调用开始，另一类是网络异常或服务器发送的Cancel命令。
    virtual void process() override = 0;

    /// 继承自tag_base。错误处理函数。
    virtual void on_error() override {
        status = ClientRPCStatus::FINISH;
        Status rpc_status;
        context.TryCancel();
//        stream->Finish(&rpc_status, this);
        gpr_log(GPR_DEBUG, "on_error %p %d %s", this, rpc_status.error_code(), rpc_status.error_message().c_str()    );
        this->process();
    };

    /// 继承自reader_callback。读事件回调。
    /// \param message 读取到的数据的地址。
    virtual void on_read(void* message) override = 0;

    /// 继承自reader_callback。读取失败事件回调。
    virtual void on_read_error() override {
        on_error();
    };
protected:
//    client_impl<SERVICE>* client;
    ClientContext context;
    ClientRPCStatus status;

    std::unique_ptr<ClientAsyncReader<R>> stream;
    typedef reader<R, ClientAsyncReader<R>> reader_t;
    std::unique_ptr<reader_t> reader_;

    W request;
};

/// 对[Bidirectional streaming RPC](https://grpc.io/docs/guides/concepts.html#bidirectional-streaming-rpc)的客户端异步抽象封装。
/// client_bi_stream_rpc对应protobuf文件中某个service的rpc方法。此类为抽象类。具体实现步骤如下：
/// \li 子类须调用stub的异步RPC，并将tag指向自己。并且根据返回的ClientAsyncReader初始化reader_和writer_。
/// \li 在process()方法，调用reader_->read()或(和）writer_->start()触发读操作。
/// \tparam W 写出的数据类型。
/// \tparam R 读取的数据类型。
template <typename W, typename R>
class client_bi_stream_rpc
        : public tag_base
        , public reader_callback
        , public writer_callback
{
public:

    client_bi_stream_rpc() {
        status = ClientRPCStatus::CREATE;
    };

    /// 继承自tag_base。子类需要处理来着CompletionQueue的回调事件。
    virtual void process() override = 0;

    /// 继承自tag_base。错误处理函数。
    virtual void on_error() override {
        status = ClientRPCStatus::FINISH;
        Status rpc_status;
//        stream->Finish(&rpc_status, this);
        context.TryCancel();
        gpr_log(GPR_DEBUG, "on_error %p %d %s", this, rpc_status.error_code(), rpc_status.error_message().c_str()    );
        this->process();
    };

    /// 继承自reader_callback。读事件回调。
    /// \param req_ptr 读取到的数据的地址。
    virtual void on_read(void * req_ptr) override = 0;

    /// 继承自reader_callback。读取失败事件回调。
    virtual void on_read_error() override {
        on_error();
    };

    /// 继承自writer_callback。写操作事件回调。
    /// \param write_id 写操作的ID，具体见class writer
    virtual void on_write(int write_id) override = 0;

    /// 继承自writer_callback。写操作失败事件回调。
    virtual void on_write_error() override {
        on_error();
    };

    ///
    /// \param w
    /// \return
    int write(const W& w){
        return writer_->write(w);
    };

protected:
    ClientContext context;
    ClientRPCStatus status;

    std::unique_ptr<ClientAsyncReaderWriter<W,R>> stream;
    typedef reader<R, ClientAsyncReaderWriter<W,R>> reader_t;
    typedef writer<W, ClientAsyncReaderWriter<W,R>> writer_t;
    std::unique_ptr<reader_t> reader_;
    std::unique_ptr<writer_t> writer_;
};
#endif //QUOTE_SERVER_CLIENT_RPC_H
