//
// Created by zjf on 2018/3/12.
//

#include "transcode_call.h"
#include "util/logger.hpp"

Transcode_PushQuote::Transcode_PushQuote(transcode_client *client)
        :client(client)
{
    stream = client->stub()->AsyncPushQuote(&context, request, client->cq(), this);
    reader_ = unique_ptr<super::reader_t>(new super::reader_t(this, *(stream.get()) ));
    client->add_tag({this, reader_.get()});
    LOG_INFO("Transcode_PushQuote addr: {:p} , reader's addr: {:p}", (void*)this, (void*)reader_.get())
}

void Transcode_PushQuote::on_read(void *message) {
    client->on_push_quote_read(message);
}

void Transcode_PushQuote::process() {
    if( status == ClientRPCStatus::CREATE ){
        LOG_INFO("Transcode_PushQuote CREATE")
        status = ClientRPCStatus::READ;
        reader_->read();
    } else if( status == ClientRPCStatus::FINISH ){
        LOG_INFO("Transcode_PushQuote FINISH");
        client->remove_tag({this, reader_.get()});
        delete this;
    }
}