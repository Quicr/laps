
#include "testLogger.h"
#include <chrono>
#include <iostream>
#include <quicr/quicr_client.h>
#include <quicr/quicr_common.h>
#include <sstream>
#include <thread>

class subDelegate : public quicr::SubscriberDelegate {
public:
  subDelegate(testLogger &logger) : logger(logger) {}
  ~subDelegate() override = default;

  void onSubscribeResponse(
      const quicr::Namespace &quicr_namespace,
      const quicr::SubscribeResult::SubscribeStatus &result) override {}

  void onSubscriptionEnded(const quicr::Namespace &quicr_namespace,
                           const quicr::SubscribeResult &result) override {}

  void onSubscribedObject(const quicr::Name &quicr_name, uint8_t priority,
                          uint16_t expiry_age_ms, bool use_reliable_transport,
                          quicr::bytes &&data) override {
    std::stringstream log_msg;

    log_msg << "recv object: name: " << quicr_name.to_hex()
            << " data sz: " << data.size();

    if (data.size())
      log_msg << " data: " << data.data();

    logger.log(qtransport::LogLevel::info, log_msg.str());
  }

  void onSubscribedObjectFragment(const quicr::Name &quicr_name,
                                  uint8_t priority, uint16_t expiry_age_ms,
                                  bool use_reliable_transport,
                                  const uint64_t &offset, bool is_last_fragment,
                                  quicr::bytes &&data) override {}

private:
  testLogger &logger;
};

class pubDelegate : public quicr::PublisherDelegate {
public:
  ~pubDelegate() override = default;

  void
  onPublishIntentResponse(const quicr::Namespace &quicr_namespace,
                          const quicr::PublishIntentResult &result) override {}
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
    static char defaultRelay[] = "relay.us-east-2.quicr.ctgpoc.com ";
    relayName = defaultRelay;
  }

  int port = 33434;
  char *portVar = getenv("LAPS_PORT");
  if (portVar) {
    port = atoi(portVar);
  }

  auto name = quicr::Name(argv[1]);
  int len = 0;

  std::stringstream log_msg;

  log_msg << "Name = " << name.to_hex();
  logger.log(qtransport::LogLevel::info, log_msg.str());

  std::vector<uint8_t> data;
  if (argc == 3) {
    data.insert(data.end(), (uint8_t *)(argv[2]),
                ((uint8_t *)(argv[2])) + strlen(argv[2]));
  }

  log_msg.str("");
  log_msg << "Connecting to " << relayName << ":" << port;
  logger.log(qtransport::LogLevel::info, log_msg.str());

  quicr::RelayInfo relay{.hostname = relayName,
                         .port = uint16_t(port),
                         .proto = quicr::RelayInfo::Protocol::UDP};

  quicr::QuicRClient client(relay, logger);

  // TODO: Update to use status to check when ready - For now sleep to give it
  // some time
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  if (data.size() > 0) {
    // do publish
    logger.log(qtransport::LogLevel::info, "Publish");
    client.publishNamedObject(name, 0, 10000, false, std::move(data));

  } else {
    // do subscribe
    logger.log(qtransport::LogLevel::info, "Subscribe");
    auto sd = std::make_shared<subDelegate>(logger);
    auto nspace = quicr::Namespace(name, 96);

    log_msg.str(std::string());
    log_msg.clear();

    log_msg << "Subscribe to " << name.to_hex() << "/" << 96;
    logger.log(qtransport::LogLevel::info, log_msg.str());

    quicr::SubscribeIntent intent = quicr::SubscribeIntent::immediate;
    quicr::bytes empty;
    client.subscribe(sd, nspace, intent, "origin_url", false, "auth_token",
                     std::move(empty));

    logger.log(qtransport::LogLevel::info, "Sleeping for 30 seconds");
    std::this_thread::sleep_for(std::chrono::seconds(30));
  }

  std::this_thread::sleep_for(std::chrono::seconds(1));
  return 0;
}
