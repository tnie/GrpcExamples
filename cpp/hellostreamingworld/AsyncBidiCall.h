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

class AsyncClientCall
{
protected:
    virtual ~AsyncClientCall()
    {
        std::lock_guard<std::mutex> lg(mt_);
        handle_.erase(this);
    }
public:
    enum CallStatus
    {
        CREATE,
        READY,
        PROCESS,
        FINISH
    };
    AsyncClientCall()
    {
        // 所有长连接都需要在退出时主动关闭的
        std::lock_guard<std::mutex> lg(mt_);
        auto success = handle_.insert(this);
        assert("handle_ 插入新的元素失败" && success.second);
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
inline void set_dummy(HelloReply& reply)
{
    // 通过默认值或业务中无效值或约定等（总之，保证服务端不会返回当前值）
    reply.set_message("__invalid_value_ that server never use.");
}
inline bool is_dummy(const HelloReply& reply)
{
    return reply.message() == "__invalid_value_ that server never use.";
}

inline void set_dummy(HelloRequest& request) // set request un idle indicating un writable.
{
    // 先理解 writable() 的概念。约定使用空 request 代表 writable 的概念，所以赋任意值使其内容不为空
    request.set_name("__dummy_value_");
}

//TODO 保证（自我约束）对象的创建和销毁都在当前类中，否则更容易错用
//私有化构造函数等
class AsyncBidiCall final : public AsyncClientCall
{
    enum CallStatus state = CREATE;
    typedef typename std::string LabelT;
    typedef typename HelloReply ReturnT;
    typedef typename HelloRequest RequestT;
    ReturnT reply_; // 若只在 cq 线程中使用则无需加锁
    std::mutex mt_; // 针对条件变量，request_ 和 requests_
    std::condition_variable cv_;    //等待 rpc_ 的赋值
    std::unique_ptr< ::grpc::ClientAsyncReaderWriter< RequestT, ReturnT>> rpc_;
    std::weak_ptr<MultiGreeter::Stub> stub_wptr_;
    //
    RequestT request_;
    std::queue<RequestT> requests_;
    std::atomic<int> ref_count_ = 0;    // count of outstanding op, use zero value when finish()

    static void callback(const ReturnT& from)
    {
       spdlog::info(from.DebugString());
    }

    bool writable() const
    {
        //TODO can write by rpc? rpc != nullptr && request is idle( need lock).
        //=channel is connected && rpc has been assigned && there is no outstanding write opr
        return (request_.name().empty());
    }

public:
    //谨慎调用接口，对象随时可能在另一线程释放
    static std::atomic<size_t> instance_count_;
    decltype(reply_)& reply() { return reply_; }
    void set_rpc(decltype(rpc_) && r) {
        std::lock_guard<std::mutex> lg(mt_);
        rpc_.swap(r);
        cv_.notify_one();
    }

    AsyncBidiCall(std::shared_ptr<MultiGreeter::Stub> stub) : stub_wptr_{stub}
    {
        set_dummy(reply_);
        set_dummy(request_);
        ++instance_count_;
    }
    ~AsyncBidiCall()
    {
        --instance_count_;
    }
    // 若返回失败，请延迟重试。why is async-write so complex? https://github.com/grpc/grpc/issues/4007#issuecomment-152568219
    void write(const LabelT& name)
    {
        RequestT v2;
        v2.set_name(name);
        v2.set_num_greetings("hello world");
        if (!v2.name().empty() /*有效性校验*/)
        {
            std::unique_lock<std::mutex> lg(mt_);
            if (writable())  //writable, /wait CREATE event
            {
                request_.Swap(&v2);
                // 三合一操作
                ref_count_ += 1;
                rpc_->Write(request_, this);
            }
            else
            {
                requests_.push(v2);
            }
        }
    }

    //除了在 completion queue 中回调，禁止在其他场景调用
    void HandleResponse(bool eventStatus) override
    {
        auto & status = AsyncClientCall::status();

        // 状态机 state/reply_/request_/ref_count_
        //即便 waiting outstanding read（比如盘后不推送行情的时候），有新的 request 也要及时发送出去
        //思考状态机能否不再使用 ref_count_ 引用计数来避免 rpc 多次 Finish()，如何避免 Finish() with outstanding read / write op 的场景 ?
        switch (state)
        {
        case AsyncClientCall::CREATE:
            if (eventStatus)
            {
                std::unique_lock<std::mutex> lg2(mt_);
                constexpr auto second3s = std::chrono::seconds(3);
                while (!cv_.wait_for(lg2, second3s, [this]() { return (nullptr != rpc_); }))
                {
                    spdlog::warn("Waiting for `rpc_[{}]` assignment. {}:{}", typeid(*this).name(), __FILE__, __LINE__);
                }
                state = AsyncClientCall::PROCESS;
                if (requests_.empty())
                {
                    request_.Clear();   // make writable
                }
                else
                {
                    // 针对在 CREATE 期间入队列的个例
                    request_.Swap(&requests_.front());
                    requests_.pop();
                    ref_count_ += 1;
                    rpc_->Write(request_, this);
                }
                ref_count_ += 1;
                rpc_->Read(&reply_, this);
            }
            else
            {
                state = FINISH;
                rpc_->Finish(&status, this);
            }
            break;
        case AsyncClientCall::PROCESS:
            --ref_count_;
            assert(ref_count_ >= 0);
            if (eventStatus)
            {
                if (is_dummy(reply_))   // 若 reply_ 未变更，则此次返回对应 rpc_->Write()
                {
                    std::lock_guard<std::mutex> lg(mt_);
                    if (requests_.empty())
                    {
                        request_.Clear();   // make writable
                    }
                    else
                    {
                        request_.Swap(&requests_.front());
                        requests_.pop();
                        ref_count_ += 1;
                        rpc_->Write(request_, this);
                    }
                }
                else
                {
                    // you're only allowed to have one outstanding at a time
                    callback(reply_);
                    set_dummy(reply_);
                    ref_count_ += 1;
                    rpc_->Read(&reply_, this);
                }
            }
            else if (ref_count_ == 0)
            {
                std::lock_guard<std::mutex> lg(mt_);
                set_dummy(request_);
                state = FINISH;
                rpc_->Finish(&status, this);    // if Finish() again/twice, bang...
            }
            else
            {
                // there is outstanding write/read. waiting...
            }

            break;
        case AsyncClientCall::FINISH:
            //释放时（比如当 read / write 失败）如果有 outstanding write / read op 就会崩溃
            log_when_finish();
            delete this;    // 多线程竞争。另一线程执行 this->write() 可能崩溃...
                            // 通过错误配置 meta 可以复现

            break;
        default:
            break;
        }
    }
};

class Monitor : public AsyncClientCall
{
    std::shared_ptr<grpc::Channel> _channel;
    grpc::CompletionQueue* cq_;
    grpc_connectivity_state state_ = GRPC_CHANNEL_READY;
    std::atomic<bool> run_;
    // TODO 改用五秒甚至五分钟后，如何取消此定时器呢？如何 shutdown channel?
    constexpr static std::chrono::seconds second1s_ = std::chrono::seconds(10);
public:
    Monitor(std::shared_ptr<grpc::Channel> ch, grpc::CompletionQueue* cq) :
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
            case GRPC_CHANNEL_SHUTDOWN:
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


