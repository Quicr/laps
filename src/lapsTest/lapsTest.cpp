#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <transport/transport.h>

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
    subDelegate(std::shared_ptr<spdlog::logger> logger)
      : logger(std::move(logger))
    {
    }
    ~subDelegate() override = default;

    void onSubscribeResponse([[maybe_unused]] const quicr::Namespace& quicr_namespace,
                             [[maybe_unused]] const quicr::SubscribeResult& result) override
    {
        SPDLOG_LOGGER_INFO(logger,
                           "onSubscriptionResponse: name: {0} status: {1}",
                           std::string(quicr_namespace),
                           static_cast<unsigned>(result.status));
    }

    void onSubscriptionEnded([[maybe_unused]] const quicr::Namespace& quicr_namespace,
                             [[maybe_unused]] const quicr::SubscribeResult::SubscribeStatus& reason) override
    {
        SPDLOG_LOGGER_INFO(logger, "onSubscriptionEnded: name: {0}", std::string(quicr_namespace));
    }

    void onSubscribedObject([[maybe_unused]] const quicr::Name& quicr_name,
                            [[maybe_unused]] uint8_t priority,
                            [[maybe_unused]] quicr::bytes&& data) override
    {
        static uint64_t prev_sent_time = 0UL;

        auto now = now_us();

        std::ostringstream log_msg;
        log_msg << "recv object: name: " << quicr_name << " data sz: " << data.size();

        uint64_t sent_time{ 0 };
        std::memcpy(&sent_time, data.data(), sizeof(uint64_t));
        data.erase(data.begin(), data.begin() + sizeof(uint64_t));


        if (prev_sent_time) {
            uint64_t pub_jitter = sent_time - prev_sent_time;;
            uint64_t sub_jitter = now - prev_recv_time;
            uint64_t e2e_latency = now - sent_time;

            log_msg << " pub_jitter: " << pub_jitter
                         << " sub_jitter: " << sub_jitter
                         << " e2e_latency: " << e2e_latency;
        }

        prev_sent_time = sent_time;
        prev_recv_time = now;

        if (data.size())
            log_msg << " data: " << data.data();

        SPDLOG_LOGGER_INFO(logger, log_msg.str());
    }

    void onSubscribedObjectFragment([[maybe_unused]] const quicr::Name& quicr_name,
                                    [[maybe_unused]] uint8_t priority,
                                    [[maybe_unused]] const uint64_t& offset,
                                    [[maybe_unused]] bool is_last_fragment,
                                    [[maybe_unused]] quicr::bytes&& data) override
    {
    }

  private:
    std::shared_ptr<spdlog::logger> logger;
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

    auto logger = spdlog::stderr_color_mt("lapsTest");

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

    SPDLOG_LOGGER_INFO(logger, "Name = {0}", std::string(name));

    std::vector<uint8_t> data;

    if (argc == 3) {
        data.insert(data.begin(), sizeof(uint64_t), 0); // Space for time
        data.insert(data.end(), (uint8_t*)(argv[2]), ((uint8_t*)(argv[2])) + std::strlen(argv[2]));
    }

    SPDLOG_LOGGER_INFO(logger, "Connecting to {0}:{1}", relayName, port);

    quicr::RelayInfo relay{ .hostname = relayName, .port = uint16_t(port), .proto = protocol };

    qtransport::TransportConfig tcfg{ .tls_cert_filename = "", .tls_key_filename = "" };
    quicr::Client client(relay, "lapsTest@cisco.com", 0, std::move(tcfg), logger);

    client.connect();

    if (data.size() > 0) {
        auto pd = std::make_shared<pubDelegate>();
        auto nspace = quicr::Namespace(name, 96);

        SPDLOG_LOGGER_INFO(logger, "PublishIntent");

        client.publishIntent(pd, nspace, {}, {}, {}, quicr::TransportMode::ReliablePerGroup);

        SPDLOG_LOGGER_INFO(logger, "... waiting for publish intent response");
        while (!pd->got_intent_response) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        SPDLOG_LOGGER_INFO(logger, "... got publish intent response");
        SPDLOG_LOGGER_INFO(logger, "Publish starting: {0} Time us: {1}", std::string(name), now_us());

        // do publish
        for (int count = 0; count < obj_count; count++) {
            uint64_t now = now_us();

            auto copy = data;
            std::memcpy(copy.data(), &now, sizeof(uint64_t));
            SPDLOG_LOGGER_INFO(logger, "Publish : {0} Time us: {1}", std::string(name), now);

            std::vector<qtransport::MethodTraceItem> trace;
            const auto start_time = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now());

            trace.push_back({"client:publish", start_time});

            client.publishNamedObject(name++, 0, 1000, std::move(copy), std::move(trace));
        }

        SPDLOG_LOGGER_INFO(logger, "Publish done: {0} Time us: {1}", std::string(name), now_us());

        SPDLOG_LOGGER_INFO(logger, "===> Press ENTER to exit <=== ");

        std::string line;
        std::getline(std::cin, line);

        client.publishIntentEnd(nspace, {});
        std::this_thread::sleep_for(std::chrono::seconds(1));

    } else {
        // do subscribe
        SPDLOG_LOGGER_INFO(logger, "Subscribe");
        auto sd = std::make_shared<subDelegate>(logger);
        auto nspace = quicr::Namespace(name, 96);

        SPDLOG_LOGGER_INFO(logger, "Subscribe to {0}/{1}", std::string(name), 96);

        quicr::SubscribeIntent intent = quicr::SubscribeIntent::immediate;
        quicr::bytes empty;

        client.subscribe(sd, nspace, intent, quicr::TransportMode::UsePublisher, "origin_url", "auth_token", std::move(empty));

        SPDLOG_LOGGER_INFO(logger, "===> RUNNING, Press ENTER to unsubscribe <===");
        std::string line;
        std::getline(std::cin, line);

        SPDLOG_LOGGER_INFO(logger, "Now unsubscribing");
        client.unsubscribe(nspace, {}, {});

        SPDLOG_LOGGER_INFO(logger, "===> Press ENTER to exit <=== ");

        std::getline(std::cin, line);
    }

    return 0;
}
