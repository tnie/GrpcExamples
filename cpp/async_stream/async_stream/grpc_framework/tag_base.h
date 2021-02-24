//
// Created by zjf on 2018/2/13.
//

#ifndef QUOTE_SERVER_TAG_BASE_H
#define QUOTE_SERVER_TAG_BASE_H

/// 此框架将gPRC异步操作event's tag封装成一个类，tag_base是这些类的基类。
/// 关于even's tag，请参考grpc::CompletionQueue::Next。
class tag_base{
public:
    /// 事件处理函数。当从CompletetionQueue::Next中读取到正常的事件时，会调用此函数。
    /// 参见server_impl::srv或client_impl::srv
    virtual void process() = 0;

    /// 错误处理函数。当从CompletetionQueue::Next中读取到异常的事件时，会调用此函数。
    /// 参见server_impl::srv或client_impl::srv
    virtual void on_error() = 0;
};

class rpc_base : public tag_base{
public:
    virtual void on_force_close() {};
};


#endif //QUOTE_SERVER_TAG_BASE_H
