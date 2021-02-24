//
// Created by zjf on 2018/2/12.
//

#ifndef QUOTE_SERVER_RPC_WRITER_H
#define QUOTE_SERVER_RPC_WRITER_H

#include "tag_base.h"

#include <grpc++/server.h>
#include <grpc/support/log.h>
#include <google/protobuf/arena.h>

#include <deque>
#include <thread>
#include <list>

using google::protobuf::Arena;

/// 异步写操作的回调接口。
class writer_callback{
public:
    /// 写操作成功的回调接口
    /// \param write_id 写操作的ID。
    virtual void on_write(int write_id) = 0;
    /// 写操作失败的回调接口
    virtual void on_write_error() = 0;
};


/// 对gRPC异步写操作的封装。内部有一个发送队列，缓存了待写出的数据。
/// \tparam W 写出的数据类型。
/// \tparam WRITER 具体的执行写操作的对象，通常为ServerAsyncWriter<W> 或 ServerAsyncReaderWriter<R,W> 或 ClientAsyncReaderWriter<W,R>
template<typename W, typename WRITER>
class writer : public tag_base{
public:
    writer(writer_callback* cb, WRITER& async_writer, size_t buf_size = 1024 * 1024 * 32)
            : callback_(*cb)
            , writer_impl_(async_writer)

    {
        auto_write_ = true;
        max_buffer_size_ = buf_size;
        cur_buffer_size_ = 0;
        status_ = STOP ;
        input_id = output_id = 0;
    };

    virtual ~writer(){
        stop();
    }

    /// 如果发送队列中有数据，是否自动执行下一个写操作。
    /// \param auto_write 等于true时，自动发送一个数据；否则，不发送。
    void set_auto(bool auto_write){
        auto_write_ = auto_write;
    }

    /// 启动写操作，等待wirte()被调用
    void start(){
        lock_t lock(mtx_);
        if( status_ == STOP ){
            status_ = IDLE;
        }
    }

    /// 停止发送，并清空队列中没有发送的数据。
    void stop(){
        lock_t lock(mtx_);
        status_ = STOP;
        while(write_buffer_.size() > 0){
            W* w = write_buffer_.front();
            write_buffer_.pop_front();
            Arena* arena = w->GetArena();
            if( arena ){
                delete arena;
            }
        }
        cur_buffer_size_ = 0;
    }

    /// 发送数据resp。将resp加入到发送队列，等待处理。
    /// \param resp 待发送的数据。
    /// \return 返回本次操作的ID。当发送完成时，回调writer_callback::on_write(int write_id),
    /// 可以知道那个数据被发送了。
    int write(const W& resp){

        lock_t lock(mtx_);

        if( status_ == STOP ){
            return -1;
        }

        if( cur_buffer_size_ >= max_buffer_size_ ){
            return -1;
        }

		Arena* arena = new Arena();
		W* w= Arena::CreateMessage<W>(arena);
		*w = resp;
		write_buffer_.push_back(w);
        cur_buffer_size_ += arena->SpaceUsed();

        if( status_ == IDLE){
            GPR_ASSERT(write_buffer_.size() == 1);
            status_ = WRITING;
            writer_impl_.Write(*write_buffer_.front(), this);
        }

        return input_id++;
    }

    /// 发送多个数据。[__first,__last)区间的数据会被添加到发送队列，等待处理。
    /// \tparam _InputIter 指向W类型数据的迭代器。
    /// \param __first 待发送数据的起始的迭代器。
    /// \param __last 待发送数据的种植的迭代器。
    /// \return 第一个和最后一个请求的ID。如果当前状态为STOP或__first等于__last时，返回{-1,-1}
    template <class _InputIter>
    std::pair<int, int> write(_InputIter __first, _InputIter __last){
        lock_t lock(mtx_);

        if( status_ == STOP ){
            return {-1, -1};
        }

        if( __first == __last){
            return {-1, -1};
        }

        if( cur_buffer_size_ >= max_buffer_size_ ){
            return {-1, -1};
        }

        for( _InputIter it = __first; it != __last; ++it){
            Arena* arena = new Arena();
            W* w= Arena::CreateMessage<W>(arena);
            *w = *it;
            write_buffer_.push_back(w);
            cur_buffer_size_ += arena->SpaceUsed();
        }

        if( status_ == IDLE){
            GPR_ASSERT(write_buffer_.size() == std::distance(__first, __last));
            status_ = WRITING;
            writer_impl_.Write(*write_buffer_.front(), this);
        }

        int original = input_id;
        input_id += std::distance(__first, __last);
        return {original, input_id - 1};
    }

    /// 发送队列中的下一个数据。仅当set_auto(false)时使用。
    /// 注意：此处没有线程同步。
    void write_next(){
        if( status_ == STOP || status_ == IDLE){
            return;
        }
        GPR_ASSERT(write_buffer_.size() > 0);
        writer_impl_.Write(*write_buffer_.front(), this);
    }

    /// 结束本次RPC调用。
    /// \param status grpc的状态码。
    /// \param tag 回调的tag。
    void finish(const grpc::Status& status, void* tag){
        writer_impl_.Finish(status, tag);
    }

    /// 继承自tag_base。完成队列处理函数。
    virtual void process(){

        lock_t lock(mtx_);
        if( status_ == STOP ){
            return ;
        }
        GPR_ASSERT(write_buffer_.size() >= 1);
        W* w = write_buffer_.front();
        write_buffer_.pop_front();
        Arena* arena = w->GetArena();
        if( arena ){
            cur_buffer_size_ -= arena->SpaceUsed();
            delete arena;
        }

        callback_.on_write(output_id++);


        if(write_buffer_.size()>0){
            if( auto_write_ ) {
                writer_impl_.Write(*write_buffer_.front(), this);
            }
        } else {
            status_ = IDLE;
        }
    };

    /// 继承自tag_base。完成队列处理函数。
    virtual void on_error(){
        callback_.on_write_error();
    }


private:
    enum CallStatus { IDLE, WRITING, STOP };
    CallStatus status_;

    typedef std::mutex mutex_t;
    typedef std::unique_lock<mutex_t> lock_t;
    mutex_t mtx_;

    std::list<W*> write_buffer_;
    size_t max_buffer_size_;
    size_t cur_buffer_size_;

    writer_callback& callback_;
    WRITER& writer_impl_;

    int input_id;
    int output_id;

    bool auto_write_;

};

#endif //QUOTE_SERVER_RPC_WRITER_H
