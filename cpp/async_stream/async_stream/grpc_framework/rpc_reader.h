//
// Created by zjf on 2018/2/12.
//

#ifndef QUOTE_SERVER_RPC_READER_H
#define QUOTE_SERVER_RPC_READER_H

#include <google/protobuf/arena.h>
#include <grpc/support/log.h>

#define RPC_READER_LOG 0


using google::protobuf::Arena;

template<typename R, typename READER>
class reader;

/// 异步读操作的回调接口。
class reader_callback{
    template<typename R, typename READER>
    friend class reader;
protected:
    /// 读取操作失败的回调接口
    virtual void on_read_error() = 0;

    /// 读取操作成功的回调接口
    /// \param req 读取到的数据的地址。
    virtual void on_read(void* req) = 0;
};

/// 对gRPC异步读操作的封装。
/// \tparam R 读取的数据类型
/// \tparam READER 具体的执行读取操作的对象，通常为ServerAsyncReaderWriter<W,R>或ClientAsyncReaderWriter<W,R>
template<typename R, typename READER>
class reader : public tag_base{
public:

    reader(reader_callback* cb, READER& async_reader)
            :callback_(*cb), reader_impl_(async_reader), auto_read_(true){
        arena_ = std::unique_ptr<Arena>(new Arena());
    };

    /// 设置是否自动读取下一个消息。
    /// \param auto_read 当等于true，程序会在收到读取事件后，自动触发下一次读操作；否则，不触发。
    void set_auto(bool auto_read){
        auto_read_ = auto_read;
    }

    /// 触发异步读操作。
    void read(){
#if RPC_READER_LOG
        gpr_log(GPR_DEBUG, "start reading");
#endif
        arena_->Reset();
        req_ = Arena::Create<R>(arena_.get());
        reader_impl_.Read(req_, this);
    };

    /// 继承自tag_base。CompletionQueue的回调函数，表示一次读操作完成。
    virtual void process(){
#if RPC_READER_LOG
        gpr_log(GPR_DEBUG, "end reading %p", this);
#endif
        callback_.on_read((void*)req_);
        if(auto_read_){
            read();
        }

    };

    /// 继承自tag_base。读操作发生错误。出现的情况有以下几种：
    /// \li - When client's writer call method Finish, server will read with ok equaling false.
    /// \li - When client call method TryCancel, server will read with ok=false, and at
    /// \li    the next cq->Next, ServerContext::IsCancelled()==true.
    /// - So we can terminate the client when ok is false .
    virtual void on_error(){
        callback_.on_read_error();
    }
private:
    reader_callback& callback_;
    READER& reader_impl_;
    bool auto_read_;
    R* req_;

    std::unique_ptr<Arena> arena_;
};
#endif //QUOTE_SERVER_RPC_READER_H
