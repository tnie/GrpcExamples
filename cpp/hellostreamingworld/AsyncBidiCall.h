#pragma once
#include <fmt\chrono.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <queue>
#include <mutex>
#include <thread>
#include <spdlog\spdlog.h>
#include <exception>
#include "hellostreamingworld.pb.h"
#include "hellostreamingworld.grpc.pb.h"
//AsyncClientCall Interface
class AsyncClientCall
{
public:
    AsyncClientCall()
    {
        // 所有长连接都需要在退出时主动关闭的
        std::lock_guard<std::mutex> lg(mt_);
        auto success = handle_.insert(this);
        assert("handle_ 插入新的元素失败" && success.second);
    }
    virtual ~AsyncClientCall()
    {
        std::lock_guard<std::mutex> lg(mt_);
        handle_.erase(this);
    }
    // 可以通过 uuid 主动 close 任务
    const uintptr_t get_uuid() const {
        return reinterpret_cast<uintptr_t>(this);
    }
    // 正常流程 && 异常
    //除了在 completion queue 中回调，禁止在其他场景调用
    virtual void HandleResponse(bool eventStatus) = 0;
    // 关闭
    virtual void close()
    {
        context_.TryCancel();
    };
    grpc::ClientContext& context() { return context_; }
    grpc::Status & status() { return status_; }
    static void close(const uintptr_t& uuid)
    {
        try
        {
            auto call = reinterpret_cast<AsyncClientCall*>(uuid);
            std::lock_guard<std::mutex> lg(mt_);
            if (handle_.find(call) != handle_.end())
            {
                call->close();
            }
        }
        catch (const std::exception&)
        {

        }
    }
    static void closeAll()
    {
        try
        {
            std::lock_guard<std::mutex> lg(mt_);
            for (auto & var : handle_)
            {
                var->close();
            }
        }
        catch (const std::exception&)
        {

        }
    }

    static bool empty()
    {
        std::lock_guard<std::mutex> lg(mt_);
        return handle_.empty();
    }
protected:
    enum class CallStatus { CREATE, PROCESS, FINISH };
    CallStatus state_ = CallStatus::CREATE;
    void log_when_finish() const
    {
        auto & status = status_;
        if (status.ok()) {
            spdlog::info("{} finish {}: {}, {}", typeid(*this).name(),
                status.error_code(), status.error_message(), status.error_details());
        }
        else {
            spdlog::warn("{} finish {}: {}, {}", typeid(*this).name(),
                status.error_code(), status.error_message(), status.error_details());
        }
    }
    template<typename T>
    void log_when_finish(const T& label) const
    {
        auto & status = status_;
        if (status.ok()) {
            spdlog::info("{} {} finish {}: {}, {}", typeid(*this).name(), label,
                status.error_code(), status.error_message(), status.error_details());
        }
        else {
            spdlog::warn("{} {} finish {}: {}, {}", typeid(*this).name(), label,
                status.error_code(), status.error_message(), status.error_details());
        }
    }
private:
    grpc::ClientContext context_;
    // used by rpc->Finish()
    grpc::Status status_;
    static std::set<AsyncClientCall*> handle_;
    static std::mutex mt_;
};

using namespace hellostreamingworld;

//TODO 保证（自我约束）对象的创建和销毁都在当前类中，否则更容易错用
//私有化构造函数等
class AsyncBidiCall final : public AsyncClientCall
{
    typedef typename HelloReply ReturnT;
    typedef typename HelloRequest RequestT;
    using rpc_t = ::grpc::ClientAsyncReaderWriter< RequestT, ReturnT>;

    // 私有类。只用于 rpc->write()，不用于 read()/finish() 等异步方法。要求线程安全
    // 为什么要使用 AsyncWriteCall 类型？区分 cq 回调对应的是 rpc->write() 还是 rpc->read()，尤其是 eventStatus(false) 的时候
    class AsyncWriteCall final : public AsyncClientCall
    {
        // can write by rpc? rpc != nullptr && request_ is idle( need lock).
        //=channel is connected && rpc has been assigned && there is no outstanding write opr
        enum class WriteState { IDLE, WRITING, STOP };
        WriteState wrt_state_ = WriteState::STOP;
    public:
        AsyncWriteCall(AsyncBidiCall* owner) : owner_(owner), rpc_{ owner->rpcRef() }
        {
            assert(nullptr != owner_);
        }
        void write(RequestT v2)
        {
            std::lock_guard<std::mutex> lg(mt_);
            //writable, /wait owner's CREATE event
            if (WriteState::IDLE == wrt_state_)
            {
                request_.Swap(&v2);
                wrt_state_ = WriteState::WRITING;
                //if rpc_ is nullptr, check (wrt_state_ = WriteState::IDLE)
                assert(rpc_);
                rpc_->Write(request_, this);
            }
            else
            {
                request_buffer_.push(v2);
            }
        }
        void write_next()   // 限于 HandleResponse() 中使用
        {
            std::lock_guard<std::mutex> lg(mt_);
            if (request_buffer_.empty())
            {
                wrt_state_ = WriteState::IDLE;   // make writable
            }
            else
            {
                request_.Swap(&request_buffer_.front());
                request_buffer_.pop();
                wrt_state_ = WriteState::WRITING;
                assert(rpc_);
                rpc_->Write(request_, this);
            }
        }
        //除了在 completion queue 中回调，禁止在其他场景调用
        void HandleResponse(bool eventStatus) override
        {
            if (eventStatus)
            {
                write_next();
            }
            else
            {
                do {
                    std::lock_guard<std::mutex> lg(mt_);
                    wrt_state_ = WriteState::STOP;
                } while (false);
                FinishOnce();
            }
        }

        // 因为需要使用嵌套类的锁保证线程安全，所以没有作为 owner's member function
        // 只会用在 cq 的回调中，所以 owner's state 不用额外的锁
        void FinishOnce()   // 限于 HandleResponse() 中使用
        {
            //互斥锁只针对 this->wrt_state
            std::lock_guard<std::mutex> lg(mt_);
            // owner's state 不存在竞争
            if ((WriteState::WRITING != wrt_state_) && (CallStatus::FINISH == owner_->state()))
            {
                auto & status = owner_->status();
                rpc_->Finish(&status, owner_);    // if Finish() again/twice, bang...
            }
            else
            {
                // there is outstanding write/read. waiting...
            }
        }

    private:
        AsyncBidiCall * owner_ = nullptr;
        std::unique_ptr<rpc_t>& rpc_;   // owner_->rpcRef();
        std::mutex mt_; // 针对 wrt_state_, request_ 和 request_buffer_
        RequestT request_;
        std::queue<RequestT> request_buffer_;
    };
    std::unique_ptr<AsyncWriteCall> wrt_call_;

    ReturnT reply_; // 只在 cq 线程中使用则无需加锁
    std::unique_ptr< rpc_t> rpc_;
    std::weak_ptr<MultiGreeter::Stub> stub_wptr_;
public:
    //谨慎调用接口，对象随时可能在另一线程释放
    static std::atomic<size_t> instance_count_;
    decltype(rpc_) & rpcRef()
    {
        return rpc_;
    }
    CallStatus state() const
    {
        return state_;
    }

    AsyncBidiCall(std::shared_ptr<MultiGreeter::Stub> stub) : stub_wptr_{ stub },
        wrt_call_{ new AsyncWriteCall(this) }
    {
        assert(nullptr != wrt_call_);
        ++instance_count_;
    }
    ~AsyncBidiCall()
    {
        --instance_count_;
    }
    // 若返回失败，请延迟重试。why is async-write so complex? https://github.com/grpc/grpc/issues/4007#issuecomment-152568219
    void write(const RequestT& v2)
    {
        assert(wrt_call_);
        wrt_call_->write(v2);
    }
    //除了在 completion queue 中回调，禁止在其他场景调用
    void HandleResponse(bool eventStatus) override
    {
        using namespace std::chrono_literals;
        auto & status = AsyncClientCall::status();

        //即便 waiting outstanding read（比如盘后不推送行情的时候），有新的 request 也要及时发送出去
        switch (state_)
        {
        case CallStatus::CREATE:
            if (eventStatus)
            {
                // 避免直接使用 AsyncRpc() 接口直接赋值，使用 PrepareAsyncRpc() 接口赋值后，再调用其 StartCall()
                assert(nullptr != rpc_);
                if (nullptr == rpc_) {
                    throw std::logic_error("rpc_ is nullptr. You should replace AsyncRpc() with PrepareAsyncRpc().");
                }
                assert(wrt_call_);
                wrt_call_->write_next();
                state_ = CallStatus::PROCESS;
                rpc_->Read(&reply_, this);
            }
            else
            {
                state_ = CallStatus::FINISH;
                rpc_->Finish(&status, this);
            }
            break;
        case CallStatus::PROCESS:
            if (eventStatus)
            {
                // you're only allowed to have one outstanding at a time
                spdlog::info("reply is:\n***************\n{}*************", reply_.DebugString());
                rpc_->Read(&reply_, this);
            }
            else
            {
                state_ = CallStatus::FINISH;
                wrt_call_->FinishOnce();
            }
            break;
        case CallStatus::FINISH:
            //释放时（比如当 read / write 失败）如果有 outstanding write / read op 就会崩溃
            log_when_finish();
            // 多线程竞争。另一线程执行 this->write() 可能崩溃...
            // 通过错误配置 meta 可以复现
            delete this;
            break;
        default:
            break;
        }
    }
};

class ChannelMonitor : public AsyncClientCall
{
    std::shared_ptr<grpc::Channel> _channel;
    grpc::CompletionQueue* cq_;
    grpc_connectivity_state state_ = GRPC_CHANNEL_READY;
    std::atomic<bool> run_;
    // TODO 改用五秒甚至五分钟后，如何取消此定时器呢？如何 shutdown channel?
    constexpr static std::chrono::seconds second1s_ = std::chrono::seconds(10);
public:
    ChannelMonitor(std::shared_ptr<grpc::Channel> ch, grpc::CompletionQueue* cq) :
        _channel(ch), cq_(cq), run_(true)
    {
        auto current_state = _channel->GetState(true);
        auto deadline = std::chrono::system_clock::now() + second1s_;
        _channel->NotifyOnStateChange(current_state, deadline, cq_, this);
    }
    void close() override
    {
        run_ = false;
    }
    void HandleResponse(bool eventStatus) override
    {
        if (false == run_)
        {
            delete this;
            return;
        }
        if (eventStatus)
        {
            // channel's state changed.
            state_ = _channel->GetState(true);
            switch (state_)
            {
            case GRPC_CHANNEL_IDLE:
                spdlog::warn("channel is idle");
                break;
            case GRPC_CHANNEL_CONNECTING:
                spdlog::info("channel is connecting");
                break;
            case GRPC_CHANNEL_READY:
                // 回调
                spdlog::info("channel is ready for work");
                break;
            case GRPC_CHANNEL_TRANSIENT_FAILURE:
                spdlog::warn("channel has seen a failure but expects to recover");
                break;
            case GRPC_CHANNEL_SHUTDOWN: // TODO shutdown 之后不应再调用 NotifyOnStateChange，但没有找到方法 shutdown channel
                spdlog::error("channel has seen a failure that it cannot recover from");
                break;
            default:
                break;
            }
        }
        else
        {
            // timeout
            spdlog::info("timeout after {}.", second1s_);
        }
        auto deadline = std::chrono::system_clock::now() + second1s_;
        _channel->NotifyOnStateChange(state_, deadline, cq_, this);
    }
};


