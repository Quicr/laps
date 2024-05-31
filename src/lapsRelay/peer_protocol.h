#pragma once

namespace laps {
    /* ------------------------------------------------------------------------
     * Wire messages
     * ------------------------------------------------------------------------
     */
    const uint8_t QUICR_PEERING_MSG_TYPE = 128;

    /**
     * Peering Message sub types
     */
    enum class PeeringSubType : uint8_t
    {
        CONNECT = 0,
        CONNECT_OK = 1,
        SUBSCRIBE = 2,
        UNSUBSCRIBE = 3,
        PUBLISH_INTENT = 4,
        PUBLISH_INTENT_DONE = 5,
    };

    /**
     * @brief Peer Common Header for messages
     */
    struct PeerCommonHeader
    {
        uint32_t msg_len;                       /// Length of the message
        uint8_t type{ QUICR_PEERING_MSG_TYPE }; /// Indicates this is a peering message
        PeeringSubType subtype;                 /// Message header subtype/encoding
    };

    /**
     * @brief Connect Message
     * @details A connect message MUST be the first message sent after establishing a connection. The
     *      connect message is sent by the client (one making the connection).  The server responds
     *      back with a connect ok message.
     *
     *      The connect message advertise information about the
     *
     */
    struct MsgConnect : PeerCommonHeader
    {
        MsgConnect() { subtype = PeeringSubType::CONNECT; }

      // Flags
        uint8_t     flag_reliable : 1;            /// Instructs peer to use reliable for published objects
        uint8_t     flag_reserved : 7;            /// Reserved/unused flags

        double longitude{ 0 }; /// 8 byte longitude value detailing the location of the local relay
        double latitude{ 0 };  /// 8 byte latitude value detailing the location of the local relay

        uint8_t id_len; /// Length of the ID (local) string in bytes
        /// id follows and is the size of id_len

    } __attribute__((__packed__, aligned(1)));

    /**
     * @brief Connect Ok Message
     * @details A connection ok response message. The server will send a response if the connection is allowed.
     *      The response will indicate information about the remote server. If a response is not received,
     *      then it's likely it's denied or that a connection cannot be made, such as trying to make
     *      a connection to a device that is behind NAT.
     */
    struct MsgConnectOk : PeerCommonHeader
    {
        MsgConnectOk() { subtype = PeeringSubType::CONNECT_OK; }

        double longitude{ 0 }; /// 8 byte longitude value detailing the location of the remote relay
        double latitude{ 0 };  /// 8 byte latitude value detailing the location of the remote relay

        uint8_t id_len; /// Length of the ID (remote) string in bytes
        /// variable id follows, the size of id_len

        // TODO: Add/document publish policies where a relay can only publish to specifics namespaces
    };

    /**
     * @brief Subscribe Message
     * @details Subscribe messages are sent to indicate which names the peer/relay is interested in receiving.
     */
    struct MsgSubscribe : PeerCommonHeader
    {
        MsgSubscribe() { subtype = PeeringSubType::SUBSCRIBE; }

        uint64_t remote_data_ctx_id{ 0 };         /// Remote data context ID

        uint8_t count_subscribes;                 /// Number of subscribes by namespace

        /* subscribes use a 128 bit namespace with an encoding of <uint8_t bit_len><uint8_t*>. The number of bytes
         *       for the name is calculated using `len / 8 = byte_len + (len % 8 > 0 ? 1 : 0)`. In other
         *       words, the length in bits defines the number of bytes to follow/encoded to represent the name.
         *
         *       For example, length of 96 is an even 12 bytes, so only 12 bytes follow the length byte. In the case
         *       of 98 bits, there are extra bits left over, therefore the number of bytes needed are 13.
         *       The extra bits in the last byte are zeroed.
         */
        /// variable array of <bit len><bytes>
    };

    /**
     * @brief Unsubscribe Message
     * @details Unsubscribe messages are sent to indicate which names the peer/relay is no longer interested in.
     *      Normally the unsubscribe will indicate an exact match on a subscribe, but there are cases
     *      where an unsubscribe might indicate a more specific that is not wanted.  For example, subscribe to 0/0
     *      but unsubscribe to chatty names that do not have any edge client subscribers.
     */
    struct MsgUnsubscribe : PeerCommonHeader
    {
        MsgUnsubscribe() { subtype = PeeringSubType::UNSUBSCRIBE; }

        uint8_t count_subscribes; /// Number of subscribes by namespace

        /* subscribes use a 128 bit namespace with an encoding of <uint8_t bit_len><uint8_t*>. The number of bytes
         *       for the name is calculated using `len / 8 = byte_len + (len % 8 > 0 ? 1 : 0)`. In other
         *       words, the length in bits defines the number of bytes to follow/encoded to represent the name.
         *
         *       For example, length of 96 is an even 12 bytes, so only 12 bytes follow the length byte. In the case
         *       of 98 bits, there are extra bits left over, therefore the number of bytes needed are 13.
         *       The extra bits in the last byte are zeroed.
         */
        /// variable array of <bit len><bytes>
    };

    /**
     * @brief Publish Intent Message
     * @details Publish intent messages are sent to indicate peer publishers
     */
    struct MsgPublishIntent : PeerCommonHeader
    {
        MsgPublishIntent() { subtype = PeeringSubType::PUBLISH_INTENT; }

        uint64_t remote_data_ctx_id{ 0 };         /// Remote data context ID

        uint8_t origin_id_len;                    /// Length of the origin ID string in bytes
        uint8_t count_publish_intents;            /// Number of publish intent namespace

        /// variable origin id follows using the size of origin_id_len

        /* use a 128 bit namespace with an encoding of <uint8_t bit_len><uint8_t*>. The number of bytes
         *       for the name is calculated using `len / 8 = byte_len + (len % 8 > 0 ? 1 : 0)`. In other
         *       words, the length in bits defines the number of bytes to follow/encoded to represent the name.
         *
         *       For example, length of 96 is an even 12 bytes, so only 12 bytes follow the length byte. In the case
         *       of 98 bits, there are extra bits left over, therefore the number of bytes needed are 13.
         *       The extra bits in the last byte are zeroed.
         */
        /// variable array of <bit len><bytes>
    };

    /**
     * @brief Publish Intent Done Message
     * @details Publish intent done messages are sent to indicate peer publishers are done
     */
    struct MsgPublishIntentDone : PeerCommonHeader
    {
        MsgPublishIntentDone() { subtype = PeeringSubType::PUBLISH_INTENT_DONE; }

        uint8_t origin_id_len; /// Length of the origin ID string in bytes
        uint8_t count_publish_intents; /// Number of publish intent namespace

        /// variable origin id follows using the size of origin_id_len

        /* use a 128 bit namespace with an encoding of <uint8_t bit_len><uint8_t*>. The number of bytes
         *       for the name is calculated using `len / 8 = byte_len + (len % 8 > 0 ? 1 : 0)`. In other
         *       words, the length in bits defines the number of bytes to follow/encoded to represent the name.
         *
         *       For example, length of 96 is an even 12 bytes, so only 12 bytes follow the length byte. In the case
         *       of 98 bits, there are extra bits left over, therefore the number of bytes needed are 13.
         *       The extra bits in the last byte are zeroed.
         */
        /// variable array of <bit len><bytes>
    };

    /**
     * @brief Decode the namespaces from message
     *
     * @param [in]  encoded_data    Vector array of the encoded data to parse
     * @param[out]  ns_list         Vector of namespaces, will be updated from decode
     */
    void inline decodeNamespaces(const std::vector<uint8_t> &encoded_data, std::vector<Namespace> &ns_list) {

        for (size_t i=0; i < encoded_data.size(); i++) {
            uint8_t byte_count = encoded_data[i] / 8;
            byte_count += encoded_data[i] % 8 > 0 ? 1 : 0;

            // Make name from bytes encoded
            Name n(encoded_data.data() + (i+1), byte_count);
            n = messages::swap_bytes(n);

            ns_list.emplace_back(n, encoded_data[i]);

            i += byte_count;
        }
    }

    /**
     * @brief Encode namespaces to message encoding
     *
     * @param[out] encoded_data     Vector that will be updated with the encoded namespaces
     * @param[in]  ns_list          Vector of namespaces, will be updated from decode
     */
    void inline encodeNamespaces(std::vector<uint8_t> &encoded_data, const std::vector<Namespace> &ns_list) {

        for (const auto &ns: ns_list) {
            uint8_t byte_count = ns.length() / 8;
            byte_count += ns.length() % 8 > 0 ? 1 : 0;

            encoded_data.push_back(ns.length());

            // TODO: Update name to pull out just used bytes for the name based on length
            for (uint8_t i=sizeof(Name)-1; i >= sizeof(Name) - byte_count; i--) {
                encoded_data.push_back(ns.name()[i]);
            }
        }
    }


} // namespace laps
