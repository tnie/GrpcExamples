/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include <grpc/grpc.h>
#include <grpc++/channel.h>
#include <grpc++/client_context.h>
#include <grpc++/create_channel.h>
#include <grpc++/security/credentials.h>
#include "helper.h"
#include "route_guide.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::Status;
using routeguide::Point;
using routeguide::Feature;
using routeguide::Rectangle;
using routeguide::RouteSummary;
using routeguide::RouteNote;
using routeguide::RouteGuide;

Point MakePoint(long latitude, long longitude) {
  Point p;
  p.set_latitude(latitude);
  p.set_longitude(longitude);
  return p;
}

Feature MakeFeature(const std::string& name,
                    long latitude, long longitude) {
  Feature f;
  f.set_name(name);
  f.mutable_location()->CopyFrom(MakePoint(latitude, longitude));
  return f;
}

RouteNote MakeRouteNote(const std::string& message,
                        long latitude, long longitude) {
  RouteNote n;
  n.set_message(message);
  n.mutable_location()->CopyFrom(MakePoint(latitude, longitude));
  return n;
}

class RouteGuideClient {
 public:
  RouteGuideClient(std::shared_ptr<Channel> channel, const std::string& db)
      : stub_(RouteGuide::NewStub(channel)) {
    routeguide::ParseDb(db, &feature_list_);
  }

  void GetFeature() {
    Point point;
    point = MakePoint(409146138, -746188906);
    GetOneFeature(point);
    point = MakePoint(0, 0);
    GetOneFeature(point);
  }

  void ListFeatures() {
    routeguide::Rectangle rect;
    Feature feature;
    ClientContext context;

    rect.mutable_lo()->set_latitude(400000000);
    rect.mutable_lo()->set_longitude(-750000000);
    rect.mutable_hi()->set_latitude(420000000);
    rect.mutable_hi()->set_longitude(-730000000);
    std::cout << "Looking for features between 40, -75 and 42, -73"
              << std::endl;

    std::unique_ptr<ClientReader<Feature> > reader(
        stub_->ListFeatures(&context, rect));
    while (reader->Read(&feature)) {
      std::cout << "Found feature called "
                << feature.name() << " at "
                << feature.location().latitude()/kCoordFactor_ << ", "
                << feature.location().longitude()/kCoordFactor_ << std::endl;
    }
    Status status = reader->Finish();
    if (status.ok()) {
      std::cout << "ListFeatures rpc succeeded." << std::endl;
    } else {
      std::cout << "ListFeatures rpc failed." << std::endl;
    }
  }

  void RecordRoute() {
    Point point;
    RouteSummary stats;
    ClientContext context;
    const int kPoints = 10;
    unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();

    std::cout << "size is: " << feature_list_.size() << std::endl;
    // TODO assert() 在 release 模式下无效
    assert(feature_list_.size() > 0);
    if (feature_list_.size() <= 0)
    {
      std::cerr << "o(╥﹏╥)o 楼上提示了缺失数据文件，但工作不到位啊。程序崩溃…崩溃…" << std::endl;
      return;
    }
    std::default_random_engine generator(seed);
    std::uniform_int_distribution<int> feature_distribution(
        0, feature_list_.size() - 1);
    std::uniform_int_distribution<int> delay_distribution(
        500, 1500);

    std::unique_ptr<ClientWriter<Point> > writer(
        stub_->RecordRoute(&context, &stats));
    for (int i = 0; i < kPoints; i++) {
      // feature_list_.size() 为零，随机数爆炸，造成“数组”越界
      const Feature& f = feature_list_.at(feature_distribution(generator));
      std::cout << "Visiting point "
                << f.location().latitude()/kCoordFactor_ << ", "
                << f.location().longitude()/kCoordFactor_ << std::endl;
      if (!writer->Write(f.location())) {
        // Broken stream.
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(
          delay_distribution(generator)));
    }
    writer->WritesDone();
    Status status = writer->Finish();
    if (status.ok()) {
      std::cout << "Finished trip with " << stats.point_count() << " points\n"
                << "Passed " << stats.feature_count() << " features\n"
                << "Travelled " << stats.distance() << " meters\n"
                << "It took " << stats.elapsed_time() << " seconds"
                << std::endl;
    } else {
      std::cout << "RecordRoute rpc failed." << std::endl;
    }
  }

  void RouteChat() {
    ClientContext context;

    std::shared_ptr<ClientReaderWriter<RouteNote, RouteNote> > stream(
        stub_->RouteChat(&context));

    std::thread writer([stream]() {
      std::vector<RouteNote> notes{
        MakeRouteNote("First message", 0, 0),
        MakeRouteNote("Second message", 0, 1),
        MakeRouteNote("Third message", 1, 0),
        MakeRouteNote("Fourth message", 0, 0)};
      for (const RouteNote& note : notes) {
        std::cout << "Sending message " << note.message()
                  << " at " << note.location().latitude() << ", "
                  << note.location().longitude() << std::endl;
        stream->Write(note);
      }
      stream->WritesDone();
    });

    RouteNote server_note;
    while (stream->Read(&server_note)) {
      std::cout << "Got message " << server_note.message()
                << " at " << server_note.location().latitude() << ", "
                << server_note.location().longitude() << std::endl;
    }
    writer.join();
    Status status = stream->Finish();
    if (!status.ok()) {
      std::cout << "RouteChat rpc failed." << std::endl;
    }
  }

 private:

   struct AsyncClientCall {
     Feature reply;
     ClientContext context;
     Status status;
     std::unique_ptr<grpc::ClientAsyncResponseReader<Feature>> rpc;
   };

   bool GetOneFeature(const Point& point) {
     AsyncClientCall* call = new AsyncClientCall();
     call->rpc = stub_->PrepareAsyncGetFeature(&call->context, point, &cq_);
     call->rpc->StartCall();
     call->rpc->Finish(&call->reply, &call->status, call);
     return 0;
   }
public:
   void AsyncCompleteRpc()
   {
      void* got_tag;
      bool ok = false;
      while (cq_.Next(&got_tag, &ok))
      {
        auto call = static_cast<AsyncClientCall*>(got_tag);
        assert(ok);
        if (!call->status.ok()) {
          std::cerr << "GetFeature rpc failed." << std::endl;
        }
        const Feature* feature = &(call->reply);
        if (!feature->has_location()) {
          std::cout << "Server returns incomplete feature." << std::endl;
        }
        if (feature->name().empty()) {
          std::cout << "Found no feature at "
            << feature->location().latitude() / kCoordFactor_ << ", "
            << feature->location().longitude() / kCoordFactor_ << std::endl;
        }
        else {
          std::cout << "Found feature called " << feature->name() << " at "
            << feature->location().latitude() / kCoordFactor_ << ", "
            << feature->location().longitude() / kCoordFactor_ << std::endl;
        }
        delete call;
      }
  }

private:

  const float kCoordFactor_ = 10000000.0;
  std::unique_ptr<RouteGuide::Stub> stub_;
  std::vector<Feature> feature_list_;
  grpc::CompletionQueue cq_;
};

int main(int argc, char** argv) {
  // Expect only arg: --db_path=path/to/route_guide_db.json.
  std::string db = routeguide::GetDbFileContent(argc, argv);
  RouteGuideClient guide(
      grpc::CreateChannel("localhost:50051",
                          grpc::InsecureChannelCredentials()),
      db);

  std::cout << "-------------- GetFeature --------------" << std::endl;
  std::thread thread_ = std::thread(&RouteGuideClient::AsyncCompleteRpc, &guide);
  guide.GetFeature();
  /*std::cout << "-------------- ListFeatures --------------" << std::endl;
  guide.ListFeatures();
  std::cout << "-------------- RecordRoute --------------" << std::endl;
  guide.RecordRoute();
  std::cout << "-------------- RouteChat --------------" << std::endl;
  guide.RouteChat();*/

  thread_.join();
  return 0;
}
