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
    };

    /**
     * @brief Peer Common Header for messages
     */
    struct PeerCommonHeader
    {
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

} // namespace laps
