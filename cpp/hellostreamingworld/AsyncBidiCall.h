#pragma once
#include <boost\uuid\uuid.hpp>
#include <boost\uuid\uuid_io.hpp>
#include <boost\uuid\random_generator.hpp>
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

#pragma comment(lib, "bcrypt.lib")

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
class AsyncBidiCall final : public AsyncClientCall, std::enable_shared_from_this<AsyncBidiCall>
{
    typedef typename HelloReply ReturnT;
    typedef typename HelloRequest RequestT;
    using rpc_t = ::grpc::ClientAsyncReaderWriter< RequestT, ReturnT>;
    using self_t = AsyncBidiCall;

    // can write by rpc? rpc != nullptr && request_ is idle( need lock).
    //=channel is connected && rpc has been assigned && there is no outstanding write opr
    enum class WriteState { IDLE, WRITING, STOP };
    WriteState wrt_state_ = WriteState::STOP;
    // 私有类。只用于 rpc->write()，不用于 read()/finish() 等异步方法。要求线程安全
    // 为什么要使用 AsyncWriteCall 类型？区分 cq 回调对应的是 rpc->write() 还是 rpc->read()，尤其是 eventStatus(false) 的时候
    class AsyncWriteCall final : public AsyncClientCall
    {
        std::atomic_bool has_response_ = false;
        const std::string uuid_;
        const RequestT request_;
        AsyncBidiCall * owner_ = nullptr;
    public:
        AsyncWriteCall(AsyncBidiCall* owner, std::string uuid, RequestT request) :
            uuid_(std::move(uuid)), request_(std::move(request)), owner_(owner)
        {
            assert(nullptr != owner_);
        }
        const RequestT& request() const { return request_; }
        //除了在 completion queue 中回调，禁止在其他场景调用
        void HandleResponse(bool eventStatus) override
        {
            if (eventStatus)
            {
                spdlog::info("write {} successfully. {}", uuid_, request_.DebugString());
                owner_->write_next();
            }
            else
            {
                spdlog::warn("write {} failed. {}", uuid_, request_.DebugString());
                owner_->stop_write();
            }
            has_response_ = true;
        }
        ~AsyncWriteCall()
        {
            if (!has_response_)
                spdlog::warn("~write {} not executed. {}", uuid_, request_.DebugString());
            else
                spdlog::debug("~write {} executed. {}", uuid_, request_.DebugString());
        }
    };
    std::mutex mt_; // 针对 wrt_call_buffer_, wrt_call_ 和 wrt_state_
    std::unique_ptr<AsyncWriteCall> wrt_call_;
    std::queue<std::unique_ptr<AsyncWriteCall>> wrt_call_buffer_;

    ReturnT reply_; // 只在 cq 线程中使用则无需加锁
    std::unique_ptr< rpc_t> rpc_;
    std::weak_ptr<MultiGreeter::Stub> stub_wptr_;
    std::shared_ptr<AsyncBidiCall> myself_;

    AsyncBidiCall(std::shared_ptr<MultiGreeter::Stub> stub = nullptr) : stub_wptr_{ stub }
    {
        //myself_ = shared_from_this();     // 尚未构造完毕
    }
    void write_next()   // 限于 HandleResponse() 中使用
    {
        std::lock_guard<std::mutex> lg(mt_);
        if (wrt_call_buffer_.empty())
        {
            wrt_state_ = WriteState::IDLE;   // make writable
        }
        else
        {
            wrt_call_.swap(wrt_call_buffer_.front());
            wrt_call_buffer_.pop();
            wrt_state_ = WriteState::WRITING;
            assert(rpc_);
            rpc_->Write(wrt_call_->request(), wrt_call_.get());
        }
    }
    void stop_write()
    {
        do {
            std::lock_guard<std::mutex> lg(mt_);
            wrt_state_ = WriteState::STOP;
        } while (false);
        this->FinishOnce();
    }
    void FinishOnce()   // 限于 HandleResponse() 中使用
    {
        //互斥锁只针对 this->wrt_state
        // 只会用在 cq 的回调中，state_ 不存在竞争，所以 state_ 不用额外的锁
        std::lock_guard<std::mutex> lg(mt_);
        if ((WriteState::WRITING != wrt_state_) && (CallStatus::FINISH == state_))
        {
            auto & status = AsyncClientCall::status();
            rpc_->Finish(&status, this);    // if Finish() again/twice, bang...
        }
        else
        {
            // there is outstanding write/read. waiting...
        }
    }
public:
    static std::shared_ptr<AsyncBidiCall> NewPtr()
    {
        auto ptr = std::shared_ptr<AsyncBidiCall>(new AsyncBidiCall);
        ptr->myself_ = ptr;
        return ptr;
    }
    decltype(rpc_) & rpcRef()
    {
        return rpc_;
    }
    // why is async-write so complex? https://github.com/grpc/grpc/issues/4007#issuecomment-152568219
    //不保证发送成功。如果用户传入 uuid 则返回 uuid，若 uuid 为空，则内部生成 uuid 后返回
    std::string write(RequestT v2, std::string uuid = "")
    {
        if (uuid.empty()) {
            auto tmp = boost::uuids::random_generator();
            uuid = boost::uuids::to_string(tmp());
        }
        const std::string uuidCopy = uuid;
        std::lock_guard<std::mutex> lg(mt_);
        //writable, /wait owner's CREATE event
        if (WriteState::IDLE == wrt_state_)
        {
            wrt_call_.reset(new AsyncWriteCall(this, uuid, v2));
            wrt_state_ = WriteState::WRITING;
            //if rpc_ is nullptr, check (wrt_state_ = WriteState::IDLE)
            assert(rpc_);
            rpc_->Write(wrt_call_->request(), wrt_call_.get());
        }
        else
        {
            wrt_call_buffer_.emplace(new AsyncWriteCall(this, uuid, v2));
        }
        return uuidCopy;
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
                this->write_next();
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
                this->FinishOnce();
            }
            break;
        case CallStatus::FINISH:
            //释放时（比如当 read / write 失败）如果有 outstanding write / read op 就会崩溃
            log_when_finish();
            myself_.reset();
            break;
        default:
            break;
        }
    }
};

// 有没有必要显式重连呢？——如果上层不监控网络变更，就没必要显式重连。甚至没有必要存在此类型。
//每次 rpc 调用都会先建立连接 GetState(true) 的，但用户怎么知道要重新请求呢？
//用户一直重试吗（或者某层的 write() 持续重试）？还是等底层事件通知呢？
//如果等事件的话，那还是要底层做重连的呀
class ChannelStateMonitor final: public AsyncClientCall
{
    std::shared_ptr<grpc::Channel> _channel;
    grpc::CompletionQueue* cq_;
    grpc_connectivity_state state_ = GRPC_CHANNEL_READY;
    std::atomic<bool> run_;
    // to be a channel state observer when try_to_connect_ is false.
    const bool try_to_connect_;
    std::shared_ptr<ChannelStateMonitor> myself_;
    // 改用五秒甚至五分钟后，如何取消此定时器呢？取消不了，只能 short timeout + loop
    constexpr static std::chrono::seconds second1s_ = std::chrono::seconds(2);
    using monitor_func_t = std::function<void(grpc_connectivity_state old_state, grpc_connectivity_state new_state)>;
    ChannelStateMonitor(std::shared_ptr<grpc::Channel> ch, grpc::CompletionQueue* cq, monitor_func_t m = nullptr) :
        _channel(ch), cq_(cq), on_channel_state_change_(m), try_to_connect_(m != nullptr)
    {
        // 底层重连尝试的间隔是 1-2-4-8-16- 增加的，考虑是否使用
        auto current_state = _channel->GetState(try_to_connect_);
        auto deadline = std::chrono::system_clock::now() + second1s_;
        _channel->NotifyOnStateChange(current_state, deadline, cq_, this);
    }
public:
    static std::shared_ptr<ChannelStateMonitor> NewPtr(std::shared_ptr<grpc::Channel> ch, grpc::CompletionQueue* cq)
    {
        auto ptr = std::shared_ptr<ChannelStateMonitor>(new ChannelStateMonitor{ ch, cq });
        ptr->myself_ = ptr;
        return ptr;
    }
    void close() override
    {
        run_ = false;
    }
    void HandleResponse(bool eventStatus) override
    {
        if (false == run_)
        {
            myself_.reset();
            return;
        }
        if (eventStatus)
        {
            // channel's state changed.
            auto current_state = _channel->GetState(try_to_connect_);
            spdlog::info("channel_state_change: {}->{}. {}", state_, current_state, comment(current_state));
            if (on_channel_state_change_)
                on_channel_state_change_(state_, current_state);
            state_ = current_state;
        }
        else
        {
            // timeout
            spdlog::info("NotifyOnStateChange() timeout after {}.", second1s_);
        }
        auto deadline = std::chrono::system_clock::now() + second1s_;
        _channel->NotifyOnStateChange(state_, deadline, cq_, this);
    }
private:
    static std::string comment(grpc_connectivity_state state)
    {
        //Copy from grpc_connectivity_state
        static std::map<int, char const * const> state2comment = {
            { GRPC_CHANNEL_IDLE, "channel is idle" },
            { GRPC_CHANNEL_CONNECTING, "channel is connecting" },
            { GRPC_CHANNEL_READY,"channel is ready for work " },
            { GRPC_CHANNEL_TRANSIENT_FAILURE, "channel has seen a failure but expects to recover" },
            { GRPC_CHANNEL_SHUTDOWN, "channel has seen a failure that it cannot recover from " }
        };
        auto ctor = state2comment.find(state);
        if (ctor != state2comment.end())
            return ctor->second;
        return std::string{};
    }
    monitor_func_t on_channel_state_change_ ;   // 暂时未使用
};

// 使 ch 自动重连。返回其之前是否启用了自动重连。要求 ChannelStateMonitor 支持自动重连才行
static bool enable_auto_reconnect(std::shared_ptr<grpc::Channel> ch, grpc::CompletionQueue* cq)
{
    // TODO 是否会影响 channel 析构
    static std::map<std::shared_ptr<grpc::Channel>, std::weak_ptr<ChannelStateMonitor>> channel2monitor;
    auto ctor = channel2monitor.find(ch);
    if (ctor != channel2monitor.cend())
    {
        if (auto ptr = ctor->second.lock())
        {
            return true;
        }
        else
        {
            channel2monitor.erase(ctor);
        }
    }
    channel2monitor[ch] = ChannelStateMonitor::NewPtr(ch, cq);
    return false;
}

