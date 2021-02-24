//
// Created by zjf on 2018/4/19.
//

#ifndef PROVIDER_MESSAGE_QUEUE_H
#define PROVIDER_MESSAGE_QUEUE_H

#include <google/protobuf/arena.h>

#include <thread>
#include <list>
#include <algorithm>
#include <mutex>
#include <condition_variable>

using google::protobuf::Arena;
using google::protobuf::ArenaOptions;



template<typename MSG>
class msg_queue{
public:
    msg_queue(size_t buffer_size = 1024*1024*64)
            : max_arena_size_ (buffer_size), working_(true) {
        ArenaOptions opt;
        arena_ = std::unique_ptr<Arena>(new Arena(opt));
        working_arena_ = std::unique_ptr<Arena>(new Arena(opt));

        work_thrd_ = std::thread(&msg_queue::work_thrd_func, this);
    };

    virtual ~msg_queue(){
        push(static_cast<MSG*>(nullptr));
        working_ = false;
        if( work_thrd_.joinable() ){
            work_thrd_.join();
        }
    };

    int push(const MSG* msg){
        if( !working_ ){
            return -1;
        }

        lock_t lock(mtx_list_);

        if( msg == nullptr){
            msg_list_.push_back(static_cast<MSG*>(nullptr));
            cond_.notify_one();
            return 0;
        }

        if( arena_->SpaceUsed() > max_arena_size_ ){
            return -2;
        }

		MSG* new_msg = Arena::CreateMessage<MSG>(arena_.get());
		*new_msg = *msg;
        msg_list_.push_back(new_msg);

        // merge message
        if( arena_->SpaceUsed() > max_arena_size_ ){
            std::list<MSG*> merged_list;
            std::unique_ptr<Arena> merged_arena( new Arena() );

            this->merge_msg(msg_list_, merged_list, merged_arena.get());

            if( merged_list.size() > 0){
                //exam memory allocation
                for(auto msg_ptr : merged_list){
                    if( msg_ptr->GetArena() !=  merged_arena.get()){
                        throw std::logic_error("object should be allocated by Arena");
                    }
                }

                msg_list_.swap(merged_list);
                arena_.swap(merged_arena);
            }
        }

        cond_.notify_one();
        return 0;
    }

protected:
    //virtual void handle_msg(const MSG* msg) = 0;
	virtual void handle_msg(const std::list<MSG*>& msg_list) = 0;

    virtual void merge_msg(const std::list<MSG*>& src, std::list<MSG*>& dest, Arena* arena){};
private:
    void work_thrd_func(){
        lock_t lock(mtx_list_);
        while(true){
            cond_.wait(lock,  [this](){return msg_list_.size()>0; });

            msg_list_.swap(working_msg_list_);
            arena_.swap(working_arena_);

            lock.unlock();

            bool quit = working_msg_list_.back() == nullptr;
            if( quit ){
                working_msg_list_.pop_back();
            }

            handle_msg(working_msg_list_);
            working_msg_list_.clear();
            working_arena_->Reset();

            if( quit ){
                return;
            }

            lock.lock();
        }
    };

private:
    typedef std::mutex mutex_t;
    typedef std::unique_lock<mutex_t> lock_t;

    std::thread work_thrd_;

    mutex_t mtx_list_;
    std::condition_variable cond_;

    std::list<MSG*> msg_list_;
    std::unique_ptr<Arena> arena_;

    std::list<MSG*> working_msg_list_;
    std::unique_ptr<Arena> working_arena_;

    size_t max_arena_size_; // max bytes of arena_

    bool working_;
};
#endif //PROVIDER_MESSAGE_QUEUE_H
