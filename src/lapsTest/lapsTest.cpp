
#include <cantina/logger.h>
#include "transport/transport.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <quicr/quicr_client.h>
#include <quicr/quicr_common.h>
#include <sstream>
#include <thread>

uint64_t
now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

class subDelegate : public quicr::SubscriberDelegate
{
  private:
    uint64_t prev_recv_time {0};

  public:
    subDelegate(const cantina::LoggerPointer& logger)
      : logger(std::make_shared<cantina::Logger>("SDEL", logger))
    {
    }
    ~subDelegate() override = default;

    void onSubscribeResponse([[maybe_unused]] const quicr::Namespace& quicr_namespace,
                             [[maybe_unused]] const quicr::SubscribeResult& result) override
    {
        logger->info << "onSubscriptionResponse: name: " << quicr_namespace
                     << " status: " << static_cast<unsigned>(result.status) << std::flush;
    }

    void onSubscriptionEnded([[maybe_unused]] const quicr::Namespace& quicr_namespace,
                             [[maybe_unused]] const quicr::SubscribeResult::SubscribeStatus& reason) override
    {
        logger->info << "onSubscriptionEnded: name: " << quicr_namespace << std::flush;
    }

    void onSubscribedObject([[maybe_unused]] const quicr::Name& quicr_name,
                            [[maybe_unused]] uint8_t priority,
                            [[maybe_unused]] uint16_t expiry_age_ms,
                            [[maybe_unused]] bool use_reliable_transport,
                            [[maybe_unused]] quicr::bytes&& data) override
    {
        static uint64_t prev_sent_time = 0UL;

        auto now = now_us();

        logger->info << "recv object: name: " << quicr_name << " data sz: " << data.size();

        uint64_t sent_time{ 0 };
        std::memcpy(&sent_time, data.data(), sizeof(uint64_t));
        data.erase(data.begin(), data.begin() + sizeof(uint64_t));


        if (prev_sent_time) {
            uint64_t pub_jitter = sent_time - prev_sent_time;;
            uint64_t sub_jitter = now - prev_recv_time;
            uint64_t e2e_latency = now - sent_time;

            logger->info << " pub_jitter: " << pub_jitter
                         << " sub_jitter: " << sub_jitter
                         << " e2e_latency: " << e2e_latency;
        }

        prev_sent_time = sent_time;
        prev_recv_time = now;

        if (data.size())
            logger->info << " data: " << data.data();

        logger->info << std::flush;
    }

    void onSubscribedObjectFragment([[maybe_unused]] const quicr::Name& quicr_name,
                                    [[maybe_unused]] uint8_t priority,
                                    [[maybe_unused]] uint16_t expiry_age_ms,
                                    [[maybe_unused]] bool use_reliable_transport,
                                    [[maybe_unused]] const uint64_t& offset,
                                    [[maybe_unused]] bool is_last_fragment,
                                    [[maybe_unused]] quicr::bytes&& data) override
    {
    }

  private:
    cantina::LoggerPointer logger;
};

class pubDelegate : public quicr::PublisherDelegate
{
  public:
    std::atomic<bool> got_intent_response{ false };

    ~pubDelegate() override = default;

    void onPublishIntentResponse([[maybe_unused]] const quicr::Namespace& quicr_namespace,
                                 [[maybe_unused]] const quicr::PublishIntentResult& result) override
    {
        got_intent_response = true;
    }
};

int
main(int argc, char* argv[])
{
    if ((argc != 2) && (argc != 3)) {
        std::cerr << "Relay address and port set in LAPS_RELAY and LAPS_PORT env "
                     "variables."
                  << std::endl;
        std::cerr << std::endl;
        std::cerr << "Usage PUB: lapsTest FF0001 pubData" << std::endl;
        std::cerr << "Usage SUB: lapsTest FF0000" << std::endl;
        exit(-1);
    }

    cantina::LoggerPointer logger = std::make_shared<cantina::Logger>("lapsTest");

    char* relayName = getenv("LAPS_RELAY");
    if (!relayName) {
        static char defaultRelay[] = "127.0.0.1";
        relayName = defaultRelay;
    }

    int port = 33435;
    char* portVar = getenv("LAPS_PORT");
    if (portVar) {
        port = atoi(portVar);
    }

    int obj_count = 10;
    char* objCountVar = getenv("LAPS_OBJ_COUNT");
    if (objCountVar) {
        obj_count = atoi(objCountVar);
    }

    char* protoVar = getenv("LAPS_PROTO");
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

    logger->info << "Name = " << name << std::flush;

    std::vector<uint8_t> data;

    if (argc == 3) {
        data.insert(data.begin(), sizeof(uint64_t), 0); // Space for time
        data.insert(data.end(), (uint8_t*)(argv[2]), ((uint8_t*)(argv[2])) + std::strlen(argv[2]));
    }

    logger->info << "Connecting to " << relayName << ":" << port << std::flush;

    quicr::RelayInfo relay{ .hostname = relayName, .port = uint16_t(port), .proto = protocol };

    qtransport::TransportConfig tcfg{ .tls_cert_filename = NULL, .tls_key_filename = NULL };
    quicr::QuicRClient client(relay, std::move(tcfg), logger);

    client.connect();

    switch (client.status()) {
        case quicr::ClientStatus::READY:
            logger->Log("... connected");
            break;
        case quicr::ClientStatus::TERMINATED:
            logger->Log("... terminated");
            exit(10);
        default:
            logger->info << "... connected status: "
                         << static_cast<unsigned>(client.status())
                         << std::flush;
            exit(11);
    }

    if (data.size() > 0) {
        auto pd = std::make_shared<pubDelegate>();
        auto nspace = quicr::Namespace(name, 96);

        logger->Log("PublishIntent");

        client.publishIntent(pd, nspace, {}, {}, {});

        logger->Log("... waiting for publish intent response");
        while (!pd->got_intent_response) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        logger->Log("... got publish intent response");

        logger->info << "Publish starting: " << name << " Time us: " << now_us()
                     << std::flush;

        // do publish
        for (int count = 0; count < obj_count; count++) {
            // std::this_thread::sleep_for(std::chrono::milliseconds(1));

            uint64_t now = now_us();

            auto copy = data;
            std::memcpy(copy.data(), &now, sizeof(uint64_t));
            logger->info << "Publish " << name << " Time us: " << now
                         << std::flush;

            client.publishNamedObject(name, 0, 1000, false, std::move(copy));
            name += 1;
        }

        logger->info << "Publish done: " << name << " Time us: " << now_us()
                     << std::flush;

        logger->Log("===> Press ENTER to exit <=== ");

        std::string line;
        std::getline(std::cin, line);

        client.publishIntentEnd(nspace, {});
        std::this_thread::sleep_for(std::chrono::seconds(1));

    } else {
        // do subscribe
        logger->Log("Subscribe");
        auto sd = std::make_shared<subDelegate>(logger);
        auto nspace = quicr::Namespace(name, 96);

        logger->info << "Subscribe to " << name << "/" << 96 << std::flush;

        quicr::SubscribeIntent intent = quicr::SubscribeIntent::immediate;
        quicr::bytes empty;
        client.subscribe(sd, nspace, intent, "origin_url", false, "auth_token", std::move(empty));

        logger->Log("===> RUNNING, Press ENTER to unsubscribe <===");
        std::string line;
        std::getline(std::cin, line);

        logger->Log("Now unsubscribing");
        client.unsubscribe(nspace, {}, {});

        logger->Log("===> Press ENTER to exit <=== ");

        std::getline(std::cin, line);
    }

    return 0;
}
