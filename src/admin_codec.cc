// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "admin_codec.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace laps {
    namespace {
        constexpr std::size_t kMaxNamespaceEntries = 32;

        std::string JsonEscape(std::string_view value);

        class JsonReader
        {
          public:
            explicit JsonReader(std::string_view input)
              : input_(input)
            {
            }

            void SkipWs()
            {
                while (index_ < input_.size()) {
                    const auto ch = static_cast<unsigned char>(input_[index_]);
                    if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
                        break;
                    }
                    ++index_;
                }
            }

            bool Consume(char expected)
            {
                SkipWs();
                if (index_ < input_.size() && input_[index_] == expected) {
                    ++index_;
                    return true;
                }
                return false;
            }

            bool Exhausted()
            {
                SkipWs();
                return index_ >= input_.size();
            }

            std::string ParseString()
            {
                SkipWs();
                if (index_ >= input_.size() || input_[index_] != '"') {
                    throw std::runtime_error("expected a string");
                }
                ++index_;

                std::string value;
                while (index_ < input_.size()) {
                    const auto ch = input_[index_++];
                    if (ch == '"') {
                        return value;
                    }
                    if (ch == '\\') {
                        if (index_ >= input_.size()) {
                            throw std::runtime_error("truncated string escape");
                        }
                        const auto escaped = input_[index_++];
                        switch (escaped) {
                            case '"':
                            case '\\':
                            case '/':
                                value.push_back(escaped);
                                break;
                            case 'b':
                                value.push_back('\b');
                                break;
                            case 'f':
                                value.push_back('\f');
                                break;
                            case 'n':
                                value.push_back('\n');
                                break;
                            case 'r':
                                value.push_back('\r');
                                break;
                            case 't':
                                value.push_back('\t');
                                break;
                            case 'u':
                                AppendUtf8(value, ParseHex4());
                                break;
                            default:
                                throw std::runtime_error("invalid string escape");
                        }
                        continue;
                    }
                    if (static_cast<unsigned char>(ch) < 0x20) {
                        throw std::runtime_error("unescaped control character in string");
                    }
                    value.push_back(ch);
                }

                throw std::runtime_error("unterminated string");
            }

            std::string CaptureValue()
            {
                SkipWs();
                const auto start = index_;
                SkipValue();
                return std::string(input_.substr(start, index_ - start));
            }

            void SkipRawValue() { SkipValue(); }

            void SkipValue()
            {
                SkipWs();
                if (index_ >= input_.size()) {
                    throw std::runtime_error("expected a JSON value");
                }

                const auto ch = input_[index_];
                if (ch == '"') {
                    (void)ParseString();
                    return;
                }
                if (ch == '{') {
                    SkipObject();
                    return;
                }
                if (ch == '[') {
                    SkipArray();
                    return;
                }
                if (ch == 't') {
                    ExpectLiteral("true");
                    return;
                }
                if (ch == 'f') {
                    ExpectLiteral("false");
                    return;
                }
                if (ch == 'n') {
                    ExpectLiteral("null");
                    return;
                }
                if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)) != 0) {
                    SkipNumber();
                    return;
                }

                throw std::runtime_error("expected a JSON value");
            }

            bool StartsNumber()
            {
                SkipWs();
                if (index_ >= input_.size()) {
                    return false;
                }
                const auto ch = input_[index_];
                return ch == '-' || std::isdigit(static_cast<unsigned char>(ch)) != 0;
            }

            /// One JSON value, indented by depth * 2 spaces. Objects and arrays break across lines.
            std::string PrettyValue(int depth)
            {
                SkipWs();
                if (index_ >= input_.size()) {
                    throw std::runtime_error("expected a JSON value");
                }

                const auto ch = input_[index_];
                if (ch == '"') {
                    return "\"" + JsonEscape(ParseString()) + "\"";
                }
                if (ch == '{') {
                    return PrettyObject(depth);
                }
                if (ch == '[') {
                    return PrettyArray(depth);
                }

                const auto start = index_;
                SkipValue();
                return std::string(input_.substr(start, index_ - start));
            }

          private:
            static std::string Indent(int depth) { return std::string(static_cast<std::size_t>(depth) * 2, ' '); }

            std::string PrettyObject(int depth)
            {
                if (!Consume('{')) {
                    throw std::runtime_error("expected an object");
                }
                if (Consume('}')) {
                    return "{}";
                }

                std::string out = "{";
                const auto pad = Indent(depth + 1);
                while (true) {
                    const auto key = ParseString();
                    if (!Consume(':')) {
                        throw std::runtime_error("expected ':'");
                    }
                    out += '\n';
                    out += pad;
                    out += '"';
                    out += JsonEscape(key);
                    out += "\": ";
                    out += PrettyValue(depth + 1);
                    if (Consume('}')) {
                        out += '\n';
                        out += Indent(depth);
                        out += '}';
                        return out;
                    }
                    if (!Consume(',')) {
                        throw std::runtime_error("expected ',' or '}'");
                    }
                    out += ',';
                }
            }

            std::string PrettyArray(int depth)
            {
                if (!Consume('[')) {
                    throw std::runtime_error("expected an array");
                }
                if (Consume(']')) {
                    return "[]";
                }

                std::string out = "[";
                const auto pad = Indent(depth + 1);
                while (true) {
                    out += '\n';
                    out += pad;
                    out += PrettyValue(depth + 1);
                    if (Consume(']')) {
                        out += '\n';
                        out += Indent(depth);
                        out += ']';
                        return out;
                    }
                    if (!Consume(',')) {
                        throw std::runtime_error("expected ',' or ']'");
                    }
                    out += ',';
                }
            }

          private:
            void ExpectLiteral(std::string_view literal)
            {
                if (input_.substr(index_, literal.size()) != literal) {
                    throw std::runtime_error("invalid JSON literal");
                }
                index_ += literal.size();
            }

            void SkipNumber()
            {
                if (index_ < input_.size() && input_[index_] == '-') {
                    ++index_;
                }
                if (index_ >= input_.size() || std::isdigit(static_cast<unsigned char>(input_[index_])) == 0) {
                    throw std::runtime_error("invalid number");
                }
                if (input_[index_] == '0') {
                    ++index_;
                } else {
                    while (index_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[index_])) != 0) {
                        ++index_;
                    }
                }
                if (index_ < input_.size() && input_[index_] == '.') {
                    ++index_;
                    if (index_ >= input_.size() || std::isdigit(static_cast<unsigned char>(input_[index_])) == 0) {
                        throw std::runtime_error("invalid number");
                    }
                    while (index_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[index_])) != 0) {
                        ++index_;
                    }
                }
                if (index_ < input_.size() && (input_[index_] == 'e' || input_[index_] == 'E')) {
                    ++index_;
                    if (index_ < input_.size() && (input_[index_] == '+' || input_[index_] == '-')) {
                        ++index_;
                    }
                    if (index_ >= input_.size() || std::isdigit(static_cast<unsigned char>(input_[index_])) == 0) {
                        throw std::runtime_error("invalid number");
                    }
                    while (index_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[index_])) != 0) {
                        ++index_;
                    }
                }
            }

            void SkipObject()
            {
                if (!Consume('{')) {
                    throw std::runtime_error("expected an object");
                }
                if (Consume('}')) {
                    return;
                }
                while (true) {
                    (void)ParseString();
                    if (!Consume(':')) {
                        throw std::runtime_error("expected ':'");
                    }
                    SkipValue();
                    if (Consume('}')) {
                        return;
                    }
                    if (!Consume(',')) {
                        throw std::runtime_error("expected ',' or '}'");
                    }
                }
            }

            void SkipArray()
            {
                if (!Consume('[')) {
                    throw std::runtime_error("expected an array");
                }
                if (Consume(']')) {
                    return;
                }
                while (true) {
                    SkipValue();
                    if (Consume(']')) {
                        return;
                    }
                    if (!Consume(',')) {
                        throw std::runtime_error("expected ',' or ']'");
                    }
                }
            }

            std::uint32_t ParseHex4()
            {
                if (index_ + 4 > input_.size()) {
                    throw std::runtime_error("truncated unicode escape");
                }
                std::uint32_t code_point = 0;
                for (int nibble = 0; nibble < 4; ++nibble) {
                    const auto ch = static_cast<unsigned char>(input_[index_++]);
                    code_point <<= 4;
                    if (ch >= '0' && ch <= '9') {
                        code_point += static_cast<std::uint32_t>(ch - '0');
                    } else if (ch >= 'a' && ch <= 'f') {
                        code_point += static_cast<std::uint32_t>(ch - 'a' + 10);
                    } else if (ch >= 'A' && ch <= 'F') {
                        code_point += static_cast<std::uint32_t>(ch - 'A' + 10);
                    } else {
                        throw std::runtime_error("invalid unicode escape");
                    }
                }
                if (code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
                    throw std::runtime_error("invalid unicode code point");
                }
                return code_point;
            }

            static void AppendUtf8(std::string& out, std::uint32_t code_point)
            {
                if (code_point < 0x80) {
                    out.push_back(static_cast<char>(code_point));
                } else if (code_point < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
                    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
                } else if (code_point < 0x10000) {
                    out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
                }
            }

            std::string_view input_;
            std::size_t index_{ 0 };
        };

        std::string ReadErrorMessage(JsonReader& reader)
        {
            const auto raw = reader.CaptureValue();
            if (raw.empty()) {
                throw std::runtime_error("expected an error value");
            }
            if (raw.front() == '"') {
                JsonReader string_reader(raw);
                return string_reader.ParseString();
            }
            if (raw.front() != '{') {
                return raw;
            }

            std::string extracted;
            JsonReader object_reader(raw);
            if (!object_reader.Consume('{')) {
                throw std::runtime_error("expected an error object");
            }
            if (!object_reader.Consume('}')) {
                while (true) {
                    const auto error_key = object_reader.ParseString();
                    if (!object_reader.Consume(':')) {
                        throw std::runtime_error("expected ':'");
                    }
                    if (error_key == "message") {
                        extracted = object_reader.ParseString();
                    } else {
                        object_reader.SkipRawValue();
                    }
                    if (object_reader.Consume('}')) {
                        break;
                    }
                    if (!object_reader.Consume(',')) {
                        throw std::runtime_error("expected ',' or '}'");
                    }
                }
            }
            if (!object_reader.Exhausted()) {
                throw std::runtime_error("trailing data in error");
            }
            return extracted.empty() ? raw : extracted;
        }

        std::string JsonEscape(std::string_view value)
        {
            std::string escaped;
            escaped.reserve(value.size());
            constexpr char kHex[] = "0123456789abcdef";
            for (const auto ch : value) {
                const auto c = static_cast<unsigned char>(ch);
                switch (c) {
                    case '"':
                        escaped += "\\\"";
                        break;
                    case '\\':
                        escaped += "\\\\";
                        break;
                    case '\b':
                        escaped += "\\b";
                        break;
                    case '\f':
                        escaped += "\\f";
                        break;
                    case '\n':
                        escaped += "\\n";
                        break;
                    case '\r':
                        escaped += "\\r";
                        break;
                    case '\t':
                        escaped += "\\t";
                        break;
                    default:
                        if (c < 0x20) {
                            escaped += "\\u00";
                            escaped.push_back(kHex[(c >> 4) & 0x0F]);
                            escaped.push_back(kHex[c & 0x0F]);
                        } else {
                            escaped.push_back(static_cast<char>(c));
                        }
                        break;
                }
            }
            return escaped;
        }

        void AppendUint(std::ostringstream& out, bool& first, std::string_view key, std::uint64_t value)
        {
            if (!first) {
                out << ',';
            }
            first = false;
            out << '"' << key << "\":" << value;
        }

        void AppendCache(std::ostringstream& out, const AdminCacheMetrics& cache)
        {
            out << ",\"cache\":{\"enabled\":" << (cache.enabled ? "true" : "false")
                << ",\"duration_ms\":" << cache.duration_ms << ",\"interval_ms\":" << cache.interval_ms
                << ",\"tracks\":" << cache.tracks << ",\"groups\":" << cache.groups << ",\"objects\":" << cache.objects
                << ",\"bytes\":" << cache.bytes << ",\"entries\":[";

            bool first_entry = true;
            for (const auto& entry : cache.entries) {
                if (!first_entry) {
                    out << ',';
                }
                first_entry = false;
                out << "{\"track_alias\":" << entry.track_alias << ",\"track_namespace\":\""
                    << JsonEscape(entry.track_namespace) << "\",\"track_name\":\"" << JsonEscape(entry.track_name)
                    << "\",\"groups\":" << entry.groups << ",\"objects\":" << entry.objects
                    << ",\"bytes\":" << entry.bytes;
                if (entry.has_groups) {
                    out << ",\"oldest_group\":" << entry.oldest_group << ",\"newest_group\":" << entry.newest_group;
                }
                out << '}';
            }
            out << "]}";
        }

        bool EntryEquals(std::span<const std::uint8_t> entry, std::string_view expected)
        {
            if (entry.size() != expected.size()) {
                return false;
            }
            return std::equal(entry.begin(), entry.end(), reinterpret_cast<const std::uint8_t*>(expected.data()));
        }

        std::string EntryString(std::span<const std::uint8_t> entry)
        {
            return std::string(reinterpret_cast<const char*>(entry.data()), entry.size());
        }
    } // namespace

    std::vector<std::string> SplitTrackNamespace(std::string_view value)
    {
        std::vector<std::string> entries;
        std::size_t start = 0;
        while (start <= value.size()) {
            const auto slash = value.find('/', start);
            const auto end = slash == std::string_view::npos ? value.size() : slash;
            if (end > start) {
                entries.emplace_back(value.substr(start, end - start));
            }
            if (slash == std::string_view::npos) {
                break;
            }
            start = slash + 1;
        }
        return entries;
    }

    std::string JoinTrackNamespace(const std::vector<std::string>& entries)
    {
        std::string joined;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i != 0) {
                joined.push_back('/');
            }
            joined += entries[i];
        }
        return joined;
    }

    std::optional<AdminTrackLayout> MakeAdminTrackLayout(std::string_view base_prefix,
                                                         std::string_view relay_endpoint,
                                                         std::string_view client_endpoint,
                                                         std::string& error)
    {
        AdminTrackLayout layout;
        const auto base = SplitTrackNamespace(base_prefix);
        const auto relay = SplitTrackNamespace(relay_endpoint);
        const auto client = SplitTrackNamespace(client_endpoint);
        if (base.empty() || relay.empty() || client.empty()) {
            error = "admin prefix, relay endpoint, and client endpoint must be non-empty";
            return std::nullopt;
        }

        layout.subscribe_prefix = base;
        layout.subscribe_prefix.insert(layout.subscribe_prefix.end(), relay.begin(), relay.end());
        layout.request = layout.subscribe_prefix;
        layout.request.insert(layout.request.end(), client.begin(), client.end());
        layout.reply = base;
        layout.reply.insert(layout.reply.end(), client.begin(), client.end());
        layout.reply.insert(layout.reply.end(), relay.begin(), relay.end());

        if (layout.request.size() > kMaxNamespaceEntries || layout.reply.size() > kMaxNamespaceEntries) {
            error = "admin namespace exceeds 32 tuple entries";
            return std::nullopt;
        }

        return layout;
    }

    std::optional<std::vector<std::string>> AdminClientSuffix(const quicr::TrackNamespace& track_namespace,
                                                              const std::vector<std::string>& subscribe_prefix)
    {
        const auto& entries = track_namespace.GetEntries();
        if (subscribe_prefix.empty() || entries.size() <= subscribe_prefix.size()) {
            return std::nullopt;
        }

        for (std::size_t i = 0; i < subscribe_prefix.size(); ++i) {
            if (!EntryEquals(entries[i], subscribe_prefix[i])) {
                return std::nullopt;
            }
        }

        std::vector<std::string> suffix;
        suffix.reserve(entries.size() - subscribe_prefix.size());
        for (std::size_t i = subscribe_prefix.size(); i < entries.size(); ++i) {
            suffix.push_back(EntryString(entries[i]));
        }
        return suffix;
    }

    quicr::FullTrackName AdminFullTrackName(const std::vector<std::string>& name_space)
    {
        quicr::FullTrackName full_track_name;
        full_track_name.name_space = quicr::TrackNamespace(name_space);
        full_track_name.name.assign(kAdminRpcTrackName.begin(), kAdminRpcTrackName.end());
        return full_track_name;
    }

    AdminRpcMessage ParseAdminMessage(std::string_view json)
    {
        AdminRpcMessage message;
        try {
            JsonReader reader(json);
            if (!reader.Consume('{')) {
                throw std::runtime_error("expected an object");
            }
            if (!reader.Consume('}')) {
                while (true) {
                    const auto key = reader.ParseString();
                    if (!reader.Consume(':')) {
                        throw std::runtime_error("expected ':'");
                    }

                    if (key == "id") {
                        if (reader.StartsNumber()) {
                            message.id = reader.CaptureValue();
                        } else {
                            message.id = reader.ParseString();
                        }
                    } else if (key == "method") {
                        message.method = reader.ParseString();
                        message.has_method = true;
                    } else if (key == "params") {
                        message.params_json = reader.CaptureValue();
                        message.has_params = true;
                    } else if (key == "result") {
                        message.result_json = reader.CaptureValue();
                        message.has_result = true;
                    } else if (key == "error") {
                        message.has_error = true;
                        message.error_message = ReadErrorMessage(reader);
                    } else {
                        reader.SkipValue();
                    }

                    if (reader.Consume('}')) {
                        break;
                    }
                    if (!reader.Consume(',')) {
                        throw std::runtime_error("expected ',' or '}'");
                    }
                }
            }

            if (!reader.Exhausted()) {
                throw std::runtime_error("trailing data after JSON object");
            }
            message.valid = true;
        } catch (const std::exception& ex) {
            message.valid = false;
            message.error = ex.what();
        }
        return message;
    }

    bool IsJsonValue(std::string_view json, std::string& error)
    {
        try {
            JsonReader reader(json);
            reader.SkipRawValue();
            if (!reader.Exhausted()) {
                throw std::runtime_error("trailing data after JSON value");
            }
        } catch (const std::exception& ex) {
            error = ex.what();
            return false;
        }
        return true;
    }

    std::optional<std::string> PrettyPrintJson(std::string_view json)
    {
        try {
            JsonReader reader(json);
            auto pretty = reader.PrettyValue(0);
            if (!reader.Exhausted()) {
                return std::nullopt;
            }
            return pretty;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    std::string EncodeAdminRequest(std::string_view id,
                                   std::string_view method,
                                   std::optional<std::string_view> params_json)
    {
        std::ostringstream out;
        out << "{\"id\":\"" << JsonEscape(id) << "\",\"method\":\"" << JsonEscape(method) << '"';
        if (params_json.has_value()) {
            out << ",\"params\":" << *params_json;
        }
        out << '}';
        return out.str();
    }

    std::string EncodeAdminResult(std::string_view id, std::string_view result_json)
    {
        std::ostringstream out;
        out << "{\"id\":\"" << JsonEscape(id) << "\",\"result\":" << result_json << '}';
        return out.str();
    }

    std::string EncodeAdminError(std::string_view id, std::string_view message)
    {
        std::ostringstream out;
        out << "{\"id\":\"" << JsonEscape(id) << "\",\"error\":{\"message\":\"" << JsonEscape(message) << "\"}}";
        return out.str();
    }

    std::string EncodeAdminStateSizes(const AdminStateSizes& sizes)
    {
        std::ostringstream out;
        bool first = false;
        out << "{\"relay_id\":\"" << JsonEscape(sizes.relay_id) << '"';
        AppendUint(out, first, "connections", sizes.connections);
        AppendUint(out, first, "subscriptions", sizes.subscriptions);
        AppendUint(out, first, "publishes", sizes.publishes);
        AppendUint(out, first, "publish_namespaces", sizes.publish_namespaces);
        AppendUint(out, first, "subscribe_namespaces", sizes.subscribe_namespaces);
        AppendUint(out, first, "subscribe_tracks", sizes.subscribe_tracks);
        AppendUint(out, first, "requests", sizes.requests);
        AppendUint(out, first, "subscribe_active", sizes.subscribe_active);
        AppendUint(out, first, "publish_subscribes_by_request", sizes.publish_subscribes_by_request);
        AppendUint(out, first, "subscribe_alias_by_request", sizes.subscribe_alias_by_request);
        AppendUint(out, first, "connection_metrics", sizes.connection_metrics);
        AppendUint(out, first, "fanout_namespaces", sizes.fanout_namespaces);
        AppendUint(out, first, "notified_namespaces", sizes.notified_namespaces);
        AppendUint(out, first, "cache_tracks", sizes.cache_tracks);
        AppendUint(out, first, "track_rankings", sizes.track_rankings);
        AppendUint(out, first, "peering_interests", sizes.peering_interests);
        AppendCache(out, sizes.cache);
        out << '}';
        return out.str();
    }
} // namespace laps
