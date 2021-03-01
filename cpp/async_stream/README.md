从服务端同事获取的 异步流通信框架，包含读写和重连功能。

- `channel_state_monitor` 类意义不大，只是单纯作为观察者，并不介入重连功能
- `channel_state_monitor` ~~定时器超长，怎么取消的？否则 `cq_` 退出时不会崩溃吗？~~ 

	服务端实际在用的代码将时长改为了 5s 并新增了 `switch_` 开关避免超时后再次调用 `NotifyOnStateChange()`， 也没有提前结束 outstanding waiting。可能 grpc 真的不支持。

- 如果 `client_impl::on_channel_state_changed` 不执行 `cq_->Shutdown()`， 则 `cq_->Next()` 肯定不会返回 `false`

	重置 channel/stub/cq 带来唯一的收益，就是可以重置 channel 的参数。借此实现网络重连，反而增加复杂度，直接调用 `GetState(true)` channel 会自动重连的。