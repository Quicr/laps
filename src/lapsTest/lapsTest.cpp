
#include "testLogger.h"
#include "transport/transport.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <quicr/quicr_client.h>
#include <quicr/quicr_common.h>
#include <sstream>
#include <thread>

uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

class subDelegate : public quicr::SubscriberDelegate {
public:
  subDelegate(testLogger &logger) : logger(logger) {}
  ~subDelegate() override = default;

  void onSubscribeResponse(
      [[maybe_unused]] const quicr::Namespace &quicr_namespace,
      [[maybe_unused]] const quicr::SubscribeResult &result) override {

    std::stringstream log_msg;
    log_msg << "onSubscriptionResponse: name: " << quicr_namespace
            << " status: " << int(static_cast<uint8_t>(result.status));

    logger.log(qtransport::LogLevel::info, log_msg.str());
  }

  void onSubscriptionEnded(
      [[maybe_unused]] const quicr::Namespace &quicr_namespace,
      [[maybe_unused]] const quicr::SubscribeResult::SubscribeStatus &reason)
      override {

    std::stringstream log_msg;
    log_msg << "onSubscriptionEnded: name: " << quicr_namespace;

    logger.log(qtransport::LogLevel::info, log_msg.str());
  }

  void onSubscribedObject([[maybe_unused]] const quicr::Name &quicr_name,
                          [[maybe_unused]] uint8_t priority,
                          [[maybe_unused]] uint16_t expiry_age_ms,
                          [[maybe_unused]] bool use_reliable_transport,
                          [[maybe_unused]] quicr::bytes &&data) override {
    std::stringstream log_msg;

    uint64_t now = now_us();
    uint64_t sent_time = 0UL;
    std::memcpy(&sent_time, data.data(), sizeof(uint64_t));

    uint64_t delta_us = now - sent_time;

    data.erase(data.begin(), data.begin()+sizeof(uint64_t));

    log_msg << "recv object: name: " << quicr_name
            << " data sz: " << data.size();

    log_msg << " OWT us: " << delta_us;

    if (data.size())
      log_msg << " data: " << data.data();

    logger.log(qtransport::LogLevel::info, log_msg.str());
  }

  void
  onSubscribedObjectFragment([[maybe_unused]] const quicr::Name &quicr_name,
                             [[maybe_unused]] uint8_t priority,
                             [[maybe_unused]] uint16_t expiry_age_ms,
                             [[maybe_unused]] bool use_reliable_transport,
                             [[maybe_unused]] const uint64_t &offset,
                             [[maybe_unused]] bool is_last_fragment,
                             [[maybe_unused]] quicr::bytes &&data) override {}

private:
  testLogger &logger;
};

class pubDelegate : public quicr::PublisherDelegate {
public:
  std::atomic<bool> got_intent_response { false };

  ~pubDelegate() override = default;

  void onPublishIntentResponse(
      [[maybe_unused]] const quicr::Namespace &quicr_namespace,
      [[maybe_unused]] const quicr::PublishIntentResult &result) override {
    got_intent_response = true;
  }
};

int main(int argc, char *argv[]) {
  if ((argc != 2) && (argc != 3)) {
    std::cerr << "Relay address and port set in LAPS_RELAY and LAPS_PORT env "
                 "variables."
              << std::endl;
    std::cerr << std::endl;
    std::cerr << "Usage PUB: lapsTest FF0001 pubData" << std::endl;
    std::cerr << "Usage SUB: lapsTest FF0000" << std::endl;
    exit(-1);
  }

  testLogger logger;

  char *relayName = getenv("LAPS_RELAY");
  if (!relayName) {
    static char defaultRelay[] = "127.0.0.1";
    relayName = defaultRelay;
  }

  int port = 33435;
  char *portVar = getenv("LAPS_PORT");
  if (portVar) {
    port = atoi(portVar);
  }

  char *protoVar = getenv("LAPS_PROTO");
  quicr::RelayInfo::Protocol protocol = quicr::RelayInfo::Protocol::QUIC;
  if (protoVar) {
    auto proto_str = std::string(protoVar);
    if (proto_str.compare("UDP") == 0 || proto_str.compare("udp") == 0) {
      protocol = quicr::RelayInfo::Protocol::UDP;
    }
  }


  auto name = quicr::Name(std::string(argv[1]));
  int len = 0;

  std::stringstream log_msg;

  log_msg << "Name = " << name;
  logger.log(qtransport::LogLevel::info, log_msg.str());

  std::vector<uint8_t> data;

  if (argc == 3) {
    data.insert(data.begin(), sizeof(uint64_t), 0); // Space for time
    data.insert(data.end(), (uint8_t *)(argv[2]),
                ((uint8_t *)(argv[2])) + std::strlen(argv[2]));

  }

  log_msg.str("");
  log_msg << "Connecting to " << relayName << ":" << port;
  logger.log(qtransport::LogLevel::info, log_msg.str());

  quicr::RelayInfo relay{.hostname = relayName,
                         .port = uint16_t(port),
                         .proto = protocol};

  qtransport::TransportConfig tcfg { .tls_cert_filename = NULL, .tls_key_filename = NULL};
  quicr::QuicRClient client(relay, std::move(tcfg), logger);


  client.connect();

  switch (client.status()) {
    case quicr::ClientStatus::READY:
        logger.log(qtransport::LogLevel::info, "... connected");
        break;
    case quicr::ClientStatus::TERMINATED:
        logger.log(qtransport::LogLevel::info, "... terminated");
        exit(10);
    default:
        logger.log(qtransport::LogLevel::info, "... connected status: " + std::to_string(static_cast<uint8_t>(client.status())));
        exit(11);
  }


  if (data.size() > 0) {
    auto pd = std::make_shared<pubDelegate>();
    auto nspace = quicr::Namespace(name, 96);

    logger.log(qtransport::LogLevel::info, "PublishIntent");

    client.publishIntent(pd, nspace, {}, {}, {});

    logger.log(qtransport::LogLevel::info, "... waiting for publish intent response");
    while (!pd->got_intent_response) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    logger.log(qtransport::LogLevel::info, "... got publish intent response");

    // do publish
    for (int count=0; count < 10; count++) {
        uint64_t now = now_us();
        //std::this_thread::sleep_for(std::chrono::milliseconds(1));

        std::memcpy(data.data(), &now, sizeof(uint64_t));

        auto copy = data;
        logger.log(qtransport::LogLevel::info, (std::ostringstream()
                                                << "Publish " << name
                                                << " Time us: " << now).str());

        client.publishNamedObject(name, 0, 10000, false, std::move(copy));
        name += 1;
    }

  } else {
    // do subscribe
    logger.log(qtransport::LogLevel::info, "Subscribe");
    auto sd = std::make_shared<subDelegate>(logger);
    auto nspace = quicr::Namespace(name, 96);

    log_msg.str(std::string());
    log_msg.clear();

    log_msg << "Subscribe to " << name << "/" << 96;
    logger.log(qtransport::LogLevel::info, log_msg.str());

    quicr::SubscribeIntent intent = quicr::SubscribeIntent::immediate;
    quicr::bytes empty;
    client.subscribe(sd, nspace, intent, "origin_url", false, "auth_token",
                     std::move(empty));

    logger.log(qtransport::LogLevel::info, "===> RUNNING, Press ENTER to unsubscribe <===");
    std::string line;
    std::getline(std::cin, line);

    logger.log(qtransport::LogLevel::info, "Now unsubscribing");
    client.unsubscribe(nspace, {}, {});

  }

  logger.log(qtransport::LogLevel::info,
             "===> DONE, sleeping for 100 seconds. Press ENTER to exit <=== ");

  std::string line;
  std::getline(std::cin, line);

  return 0;
}
