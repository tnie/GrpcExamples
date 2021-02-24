//
// Created by zjf on 2018/3/9.
//

#ifndef PROVIDER_CLIENT_IMPL_H
#define PROVIDER_CLIENT_IMPL_H

#include "tag_base.h"

#include <string>
#include <thread>
#include <set>
#include <grpc++/grpc++.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>
#include <atomic>

using grpc::Channel;
using grpc::ChannelCredentials;
using grpc::ClientAsyncResponseReader;
using grpc::ClientContext;
using grpc::ClientAsyncReader;
using grpc::ClientAsyncWriter;
using grpc::ClientAsyncReaderWriter;
using grpc::CompletionQueue;
using grpc::Status;

using namespace std;
using namespace std::chrono;


/// channel状态变化的回调接口。
class channel_state_callback{
public:
    virtual void on_channel_state_changed(grpc_connectivity_state old_state,
                                          grpc_connectivity_state new_state) = 0;
};

/// channel状态的观察者。用来观察channel的状态变化。
class channel_state_monitor : public tag_base{
public:
    /// 构造函数。
    /// \param channel 被观察的channel
    /// \param cq 观察时使用的完成队列。
    /// \param minutes 超时的时间。
    channel_state_monitor(std::shared_ptr<Channel> channel,
                          CompletionQueue* cq,
                          int minutes,
                          channel_state_callback* cb)
    :channel_(channel), cq_(cq), minutes_(minutes){
        state_ = channel_->GetState(false);
        system_clock::time_point deadline = system_clock::now() + chrono::minutes(minutes_);
        channel_->NotifyOnStateChange(state_,  deadline, cq_, this );
        callback_ = cb;
    };

    /// 继承自tag_base, 用于处理channel的状态变化。
    virtual void process() {
        auto current_state = channel_->GetState(false);
        gpr_log(GPR_DEBUG, "channel state changed from %d to %d", state_, current_state);
        callback_->on_channel_state_changed(state_, current_state);
        state_ = current_state;
    };

    /// 继承自tag_base, 用于处理超时，会自动重新注册一次观察。
    virtual void on_error() {
        // time out
        gpr_log(GPR_DEBUG, "channel_state_monitor time out, re-monite it");
        system_clock::time_point deadline = system_clock::now() + minutes(minutes_);
        channel_->NotifyOnStateChange(state_,  deadline, cq_, this );
    };

private:
    std::shared_ptr<Channel> channel_;
    CompletionQueue* cq_;
    int minutes_;
    grpc_connectivity_state state_;

    channel_state_callback* callback_;
};


/// 对gRPC客户端的抽象基类。采用异步模式来响应客户端。
/// \tparam SERVICE 服务的类型。
template<typename SERVICE>
class client_impl : public channel_state_callback{
public:
    typedef client_impl<SERVICE> this_type;

    client_impl(){
    }

    /// 获得创建channel用的ChannelCredentials。默认使用grpc::InsecureChannelCredentials。
    /// 如果需要TLS/SSL或其他混合的ChannelCredentials，需重装此函数。
    /// \return
    virtual std::shared_ptr<ChannelCredentials> get_credental() {
        return grpc::InsecureChannelCredentials();
    }

    /// 注册tag_base, 客户端只处理注册过的tag_base，参考srv()函数。
    /// \param tags 需要注册的tag_base的集合。
    void add_tag(std::vector<tag_base*> tags){
        tags_.insert(tags.begin(), tags.end());
    };

    /// 注销tag_base, 注销后，再处理这些tag_base，参考srv()函数。
    /// \param tags 需要注册的tag_base的集合。
    void remove_tag(std::vector<tag_base*> tags){
        std::for_each(tags.begin(), tags.end(), [this](tag_base* tag){
            this->tags_.erase(tag);
        });
    }

    /// 启动客户端
    /// \param address 客户端的地址(ip and port)
    void run(string address){
        server_addr = address;

        runnig.store(true, std::memory_order_relaxed);
        thread = std::thread(&this_type::srv, this);
    }

    /// 客户端启动的回调函数。子类可以在这里建立RPC调用。
    virtual void on_run() {};

    /// 退出客户端。
    void exit(){
        runnig.store(false, std::memory_order_relaxed);
        if( thread.joinable() ){
            thread.join();
        }
    }

    /// 客户端退出的回调函数。
    virtual void on_exit() {};

    /// 获得对应服务的stub。
    typename SERVICE::Stub* stub(){
        return stub_.get();
    }

    /// 获得客户端的完成队列。
    CompletionQueue* cq(){
        return cq_.get();
    }
protected:
    /// 客户端的工作线程。支持channel状态监控和服务器重连。
    void srv(){

        while( true ){
            if( !runnig.load(std::memory_order_relaxed) ){
                cq_->Shutdown();
                on_exit();
                return;
            }

            credential = get_credental();
            grpc::ChannelArguments channel_args;
            channel_args.SetCompressionAlgorithm(GRPC_COMPRESS_GZIP);
            channel = grpc::CreateCustomChannel(server_addr, credential, channel_args);
            //channel = grpc::CreateChannel(server_addr, credential);
            stub_ = SERVICE::NewStub(channel);
            cq_ = std::unique_ptr<CompletionQueue>(new CompletionQueue());


            system_clock::time_point deadline =
                    system_clock::now() + std::chrono::seconds(5);
            if( !channel->WaitForConnected( deadline )) {
                gpr_log(GPR_DEBUG, "channel connected failed, continue");
                continue;
            }

            channel_state_monitor_ = std::unique_ptr<channel_state_monitor>(
                    new channel_state_monitor(channel, cq_.get(), 60*24, this));
            add_tag({channel_state_monitor_.get()});
            gpr_log(GPR_DEBUG, "channel_state_listener_ is %p", channel_state_monitor_.get());


            on_run();

            void* got_tag;
            bool ok = false;
            while(cq_->Next(&got_tag, &ok))
            {
                tag_base* call = static_cast<tag_base*>(got_tag);

                //gpr_log(GPR_DEBUG, "tag is %p, ok == %d", tag, ok);
                if( tags_.find(static_cast<tag_base*>(got_tag)) == tags_.end() ){
                    gpr_log(GPR_DEBUG, "invalid tag: %p", got_tag);
                    continue;
                }

                if( ok ){
//                    gpr_log(GPR_DEBUG, "tag process: %p", got_tag);
                    call->process();
                } else {
//                    gpr_log(GPR_DEBUG, "tag error: %p", got_tag);
//                    gpr_log(GPR_DEBUG, "channele stats: %d", channel->GetState(false));
                    call->on_error();
                }
            }
            std::cout << "Completion queue is shutting down. Restart it" << std::endl;
            remove_tag({channel_state_monitor_.get()});

            std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(1000*10));
        }

    }

    /// channel状态变化回调函数。
    /// \param old_state 原始状态
    /// \param new_state 最新状态
    virtual void on_channel_state_changed(grpc_connectivity_state old_state,
                                          grpc_connectivity_state new_state){
        if( new_state != GRPC_CHANNEL_READY){
            cq_->Shutdown();
        }

    }
protected:
    string server_addr;
    std::shared_ptr<Channel> channel;
    std::shared_ptr<ChannelCredentials> credential;

    std::unique_ptr<CompletionQueue> cq_;
    std::unique_ptr<typename SERVICE::Stub> stub_;

    std::atomic<bool> runnig;
    std::thread thread;

    std::set<tag_base*> tags_;
    std::unique_ptr<channel_state_monitor> channel_state_monitor_;

};
#endif //PROVIDER_CLIENT_IMPL_H
