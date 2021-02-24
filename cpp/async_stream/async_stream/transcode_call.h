//
// Created by zjf on 2018/3/12.
//

#ifndef PROVIDER_TRANSCODE_CALL_H
#define PROVIDER_TRANSCODE_CALL_H

#include "grpc_framework/client_rpc.h"
#include "transcode_client.h"

using namespace yuanda;
using namespace std;

class transcode_client;

class Transcode_PushQuote
        : public client_uni_stream_rpc<EmptyMessage, MultiQuote>{
public:
    typedef client_uni_stream_rpc<EmptyMessage, MultiQuote> super;
    Transcode_PushQuote(transcode_client* client);

    virtual void on_read(void* message) override;

    virtual void process() override;
private:
    transcode_client* client;
};

#endif //PROVIDER_TRANSCODE_CALL_H
