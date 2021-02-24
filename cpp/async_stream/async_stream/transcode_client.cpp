//
// Created by zjf on 2018/2/13.
//

#include <grpc++/create_channel.h>
#include "transcode_client.h"
#include "transcode_call.h"
#include "util/logger.hpp"
#include "RedisCenter.h"
using namespace grpc;
using namespace yuanda;

transcode_client::transcode_client() {

}


void transcode_client::on_run() {
    new Transcode_PushQuote(this);
}

void transcode_client::on_exit() {

}

void transcode_client::on_push_quote_read(void *message) {
    const MultiQuote* quotes = static_cast<MultiQuote*>(message);
	for (auto i = 0; i < quotes->entities_size(); ++i) {
		const auto& quote = quotes->entities(i);
		RedisCenter::ins()->SetDayKeyByQuote(quote);
	}
    this->push_signal(quotes);
}