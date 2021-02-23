#include <memory>
#include <string>
#include <sstream>
#include <fstream>
#include <iostream>
#include <grpc++/grpc++.h>
#include "helloworld.grpc.pb.h"
using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using helloworld::HelloRequest;
using helloworld::HelloReply;
using helloworld::Greeter;
class GreeterClient
{
public:
    GreeterClient(const std::string& server,
        const std::string& root = "",
        const std::string& cert = "",
        const std::string& key = "")
    {
        grpc::SslCredentialsOptions opts =
        {
            root,
            key,
            cert
        };

        /*auto args = grpc::ChannelArguments();
        args.SetSslTargetNameOverride("server.dev.yuanda.com");*/

        //stub_ = Greeter::NewStub(grpc::CreateCustomChannel(server, grpc::SslCredentials(opts), args));
        stub_ = Greeter::NewStub(grpc::CreateChannel(server, grpc::SslCredentials(opts)));
        //stub_ = Greeter::NewStub(grpc::CreateChannel(server,  grpc::InsecureChannelCredentials()));
    }
    std::string
        SayHello(const std::string& user)
    {
        HelloRequest request;
        request.set_name(user);
        HelloReply reply;
        ClientContext context;
        Status status = stub_->SayHello(&context, request, &reply);
        if (status.ok())
        {
            return reply.message();
        }
        else
        {
            std::cout << status.error_code() << ": "
                << status.error_message() << std::endl;
            return "RPC failed";
        }
    }
private:
    std::unique_ptr<Greeter::Stub> stub_;
};
void
read(const std::string& filename, std::string& data)
{
    std::ifstream file(filename.c_str(), std::ios::in);
    if (file.is_open())
    {
        std::stringstream ss;
        ss << file.rdbuf();
        file.close();
        data = ss.str();
    }
    else
    {
        std::cerr << "open file Failed! " << filename << std::endl;
    }
    return;
}
int
main(int argc, char** argv)
{
    std::string root;
    std::string server{ "localhost:50051" };
    read(R"(..\config\ca.crt)", root);
    //read(R"(E:\gRPC\zhao\cert\root.crt)", root);

    GreeterClient greeter(server, root);
    std::string user("world");
    std::string reply = greeter.SayHello(user);
    std::cout << "Greeter received: " << reply << std::endl;
    getchar();
    return 0;
}