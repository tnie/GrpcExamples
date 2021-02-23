#include <memory>
#include <string>
#include <sstream>
#include <fstream>
#include <iostream>
#include <grpc++/grpc++.h>
#include "helloworld.grpc.pb.h"
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using helloworld::HelloRequest;
using helloworld::HelloReply;
using helloworld::Greeter;
class GreeterServiceImpl final : public Greeter::Service
{
    Status SayHello(ServerContext* context,
        const HelloRequest* request,
        HelloReply* reply) override
    {
        std::string prefix("Hello ");
        reply->set_message(prefix + request->name());
        return Status::OK;
    }
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
void
runServer()
{
    /**
    * [!] Be carefull here using one cert with the CN != localhost. [!]
    **/
    std::string server_address("localhost:50051");
    std::string key;
    std::string cert;
    std::string root;
    const std::string dir(R"(..\config\)");
    read(dir + "server.crt", cert);
    read(dir + "server.key", key);
    read(dir + "ca.crt", root);
    /*const std::string dir(R"(E:\gRPC\zhao\cert\)");
    read(dir + "server.crt", cert);
    read(dir + "server.key", key);
    read(dir + "root.crt", root);*/
    ServerBuilder builder;
    grpc::SslServerCredentialsOptions::PemKeyCertPair keycert =
    {
        key,
        cert
    };
    grpc::SslServerCredentialsOptions sslOps;
    sslOps.pem_root_certs = root;
    sslOps.pem_key_cert_pairs.push_back(keycert);
    builder.AddListeningPort(server_address, grpc::SslServerCredentials(sslOps));
    // builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    GreeterServiceImpl service;
    builder.RegisterService(&service);
    std::unique_ptr < Server > server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;
    server->Wait();
}
int
main(int argc, char** argv)
{
    runServer();
    return 0;
}