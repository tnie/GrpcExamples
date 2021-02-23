# secure_helloworld

此项目源自 Quote-Demo 中的 [某条 Issue][1]，源码来自 [ Secure helloworld][2]（有小幅度修改）。通过 vcpkg 使用 grpc 库。

仔细阅读 [CMakeLists.txt][cm]：

> This branch ~~assumes~~ **requires** gRPC and all its dependencies are already installed on this system, so they can be located by find_package().
> 
> `# ${_GRPC_GRPCPP_UNSECURE}	# 引入此库会莫名出错`

fixbug xxx_unsecure.lib 库不能随意引用，原因未知。只知道加上不对，删了通信就能成功。


[1]:https://github.com/tnie/quote-demo/issues/9
[2]:https://github.com/grpc/grpc/issues/9593#issuecomment-277946137
[cm]:CMakeLists.txt
