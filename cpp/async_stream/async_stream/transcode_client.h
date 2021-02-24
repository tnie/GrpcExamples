//
// Created by zjf on 2018/2/13.
//

#ifndef QUOTE_SERVER_TRANSCODE_CLIENT_H
#define QUOTE_SERVER_TRANSCODE_CLIENT_H

#include "grpc_framework/client_impl.h"
#include "util/singleton.h"
#include "data_define.pb.h"
#include "transcode.grpc.pb.h"

#include <grpc/grpc.h>
#include <grpc++/channel.h>
#include <grpc++/client_context.h>

#include <boost/signals2.hpp>

#include <thread>

using namespace grpc;
using namespace ::yuanda;
using namespace boost;

class Transcode_PushQuote;

class transcode_client
: public client_impl<Transcode>, public singleton<transcode_client>
{
public:
    transcode_client();

    virtual void on_run() override;
    virtual void on_exit() override;


    // push method callback
    void on_push_quote_read(void* message);
    typedef boost::signals2::signal <void (const MultiQuote* )> signal_t;
    signals2::connection reg_push_callback(const signal_t::slot_type& slot){
        return push_signal.connect(slot);
    }

protected:
    friend class Provider_PushQuote;
    typedef std::mutex mutex_t;
    typedef std::unique_lock<mutex_t> lock_t;

    signal_t push_signal;
};


#endif //QUOTE_SERVER_TRANSCODE_CLIENT_H
