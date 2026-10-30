// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "admin_codec.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace laps {
    namespace {
        constexpr std::size_t kMaxNamespaceEntries = 32;

        /// Nested arrays and objects beyond this are rejected. The payload cap is 65536 bytes, which is
        /// enough to nest past a thread stack when every level is one recursive call.
        constexpr int kMaxJsonDepth = 32;

        std::string JsonEscape(std::string_view value);

        /// UTC `YYYY-MM-DD HH:MM:SS` for a Unix time in microseconds. Empty when `epoch_us` is 0.
        std::string FormatUtcTimestamp(std::uint64_t epoch_us)
        {
            if (epoch_us == 0) {
                return {};
            }
            const auto seconds = static_cast<std::time_t>(epoch_us / 1000000);
            std::tm utc{};
            if (gmtime_r(&seconds, &utc) == nullptr) {
                return {};
            }
            char formatted[32];
            if (std::strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M:%S", &utc) == 0) {
                return {};
            }
            return formatted;
        }

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
                            case 'u': {
                                const auto unit = ParseHex4();
                                if (unit >= 0xD800 && unit <= 0xDBFF) {
                                    if (index_ + 2 > input_.size() || input_[index_] != '\\' ||
                                        input_[index_ + 1] != 'u') {
                                        throw std::runtime_error("unpaired unicode surrogate");
                                    }
                                    index_ += 2;
                                    const auto low = ParseHex4();
                                    if (low < 0xDC00 || low > 0xDFFF) {
                                        throw std::runtime_error("unpaired unicode surrogate");
                                    }
                                    const auto code_point = 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u);
                                    AppendUtf8(value, code_point);
                                } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
                                    throw std::runtime_error("unpaired unicode surrogate");
                                } else {
                                    AppendUtf8(value, unit);
                                }
                                break;
                            }
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

            void SkipValue(int depth = 0)
            {
                if (depth > kMaxJsonDepth) {
                    throw std::runtime_error("JSON nesting is too deep");
                }
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
                    SkipObject(depth);
                    return;
                }
                if (ch == '[') {
                    SkipArray(depth);
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
                if (depth > kMaxJsonDepth) {
                    throw std::runtime_error("JSON nesting is too deep");
                }
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

            void SkipObject(int depth)
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
                    SkipValue(depth + 1);
                    if (Consume('}')) {
                        return;
                    }
                    if (!Consume(',')) {
                        throw std::runtime_error("expected ',' or '}'");
                    }
                }
            }

            void SkipArray(int depth)
            {
                if (!Consume('[')) {
                    throw std::runtime_error("expected an array");
                }
                if (Consume(']')) {
                    return;
                }
                while (true) {
                    SkipValue(depth + 1);
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

        void AppendJsonHexByte(std::string& escaped, unsigned char byte)
        {
            constexpr char kHex[] = "0123456789abcdef";
            escaped += "\\u00";
            escaped.push_back(kHex[(byte >> 4) & 0x0F]);
            escaped.push_back(kHex[byte & 0x0F]);
        }

        bool IsUtf8Continuation(unsigned char byte)
        {
            return byte >= 0x80 && byte <= 0xBF;
        }

        /// Bytes of one UTF-8 code point starting at index, or 0 when the sequence is invalid.
        std::size_t Utf8SequenceLength(std::string_view value, std::size_t index)
        {
            const auto lead = static_cast<unsigned char>(value[index]);
            std::size_t need = 0;
            if (lead >= 0xC2 && lead <= 0xDF) {
                need = 2;
            } else if (lead >= 0xE0 && lead <= 0xEF) {
                need = 3;
            } else if (lead >= 0xF0 && lead <= 0xF4) {
                need = 4;
            } else {
                return 0;
            }
            if (index + need > value.size()) {
                return 0;
            }
            for (std::size_t i = 1; i < need; ++i) {
                if (!IsUtf8Continuation(static_cast<unsigned char>(value[index + i]))) {
                    return 0;
                }
            }
            const auto second = static_cast<unsigned char>(value[index + 1]);
            if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second > 0x9F) || (lead == 0xF0 && second < 0x90) ||
                (lead == 0xF4 && second > 0x8F)) {
                return 0;
            }
            return need;
        }

        std::string JsonEscape(std::string_view value)
        {
            std::string escaped;
            escaped.reserve(value.size());
            for (std::size_t index = 0; index < value.size();) {
                const auto c = static_cast<unsigned char>(value[index]);
                switch (c) {
                    case '"':
                        escaped += "\\\"";
                        ++index;
                        break;
                    case '\\':
                        escaped += "\\\\";
                        ++index;
                        break;
                    case '\b':
                        escaped += "\\b";
                        ++index;
                        break;
                    case '\f':
                        escaped += "\\f";
                        ++index;
                        break;
                    case '\n':
                        escaped += "\\n";
                        ++index;
                        break;
                    case '\r':
                        escaped += "\\r";
                        ++index;
                        break;
                    case '\t':
                        escaped += "\\t";
                        ++index;
                        break;
                    default:
                        if (c < 0x20 || c >= 0x80) {
                            if (const auto length = c < 0x80 ? 0 : Utf8SequenceLength(value, index); length > 0) {
                                escaped.append(value.substr(index, length));
                                index += length;
                            } else {
                                AppendJsonHexByte(escaped, c);
                                ++index;
                            }
                        } else {
                            escaped.push_back(static_cast<char>(c));
                            ++index;
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

        std::uint64_t ReadUnsigned(JsonReader& reader, std::string_view field)
        {
            if (!reader.StartsNumber()) {
                throw std::runtime_error(std::string(field) + " must be an unsigned integer");
            }
            const auto text = reader.CaptureValue();
            if (text.empty() || text.front() == '-') {
                throw std::runtime_error(std::string(field) + " must be an unsigned integer");
            }
            std::size_t consumed = 0;
            const auto value = std::stoull(text, &consumed);
            if (consumed != text.size()) {
                throw std::runtime_error(std::string(field) + " must be an unsigned integer");
            }
            return value;
        }

        /// Drop a top-level `relay_id` string from an object. Other values are copied through.
        struct StrippedResult
        {
            std::string json;
            std::optional<std::string> relay_id;
        };

        StrippedResult StripTopLevelRelayId(std::string_view object_json)
        {
            StrippedResult stripped;
            stripped.json = std::string(object_json);
            JsonReader reader(object_json);
            if (!reader.Consume('{')) {
                return stripped;
            }

            std::ostringstream out;
            out << '{';
            bool first = true;
            if (!reader.Consume('}')) {
                while (true) {
                    const auto key = reader.ParseString();
                    if (!reader.Consume(':')) {
                        return stripped;
                    }
                    const auto value = reader.CaptureValue();
                    if (key == "relay_id") {
                        try {
                            JsonReader value_reader(value);
                            auto id = value_reader.ParseString();
                            if (value_reader.Exhausted()) {
                                stripped.relay_id = std::move(id);
                            }
                        } catch (const std::exception&) {
                        }
                    } else {
                        if (!first) {
                            out << ',';
                        }
                        first = false;
                        out << '"' << JsonEscape(key) << "\":" << value;
                    }
                    if (reader.Consume('}')) {
                        break;
                    }
                    if (!reader.Consume(',')) {
                        return stripped;
                    }
                }
            }
            if (!reader.Exhausted()) {
                return stripped;
            }
            out << '}';
            stripped.json = out.str();
            return stripped;
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

        if (AdminReplyOverlapsSubscribePrefix(client, relay)) {
            error = "client endpoint must not begin with the target relay endpoint; the reply namespace would be "
                    "consumed as another admin request";
            return std::nullopt;
        }

        if (layout.request.size() > kMaxNamespaceEntries || layout.reply.size() > kMaxNamespaceEntries) {
            error = "admin namespace exceeds 32 tuple entries";
            return std::nullopt;
        }

        return layout;
    }

    bool AdminReplyOverlapsSubscribePrefix(const std::vector<std::string>& client_entries,
                                           const std::vector<std::string>& relay_entries)
    {
        if (relay_entries.empty() || client_entries.size() < relay_entries.size()) {
            return false;
        }
        for (std::size_t i = 0; i < relay_entries.size(); ++i) {
            if (client_entries[i] != relay_entries[i]) {
                return false;
            }
        }
        return true;
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
                            message.id_is_number = true;
                        } else {
                            message.id = reader.ParseString();
                            message.id_is_number = false;
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
                    } else if (key == "api_version") {
                        message.api_version = ReadUnsigned(reader, "api_version");
                        message.has_api_version = true;
                    } else if (key == "relay_id") {
                        message.relay_id = reader.ParseString();
                        message.has_relay_id = true;
                    } else if (key == "node_id") {
                        message.node_id = reader.ParseString();
                        message.has_node_id = true;
                    } else if (key == "start_time_us") {
                        message.start_time_us = ReadUnsigned(reader, "start_time_us");
                        message.has_start_time_us = true;
                    } else if (key == "start_time") {
                        message.start_time = reader.ParseString();
                        message.has_start_time = true;
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

    void AppendAdminId(std::ostringstream& out, std::string_view id, bool id_is_number)
    {
        out << "\"id\":";
        if (id_is_number) {
            out << id;
        } else {
            out << '"' << JsonEscape(id) << '"';
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

    std::string EncodeAdminResult(std::string_view id,
                                  const AdminRelayIdentity& identity,
                                  std::string_view result_json,
                                  bool id_is_number)
    {
        std::ostringstream out;
        out << '{';
        AppendAdminId(out, id, id_is_number);
        out << ",\"api_version\":" << kAdminApiVersion << ",\"relay_id\":\"" << JsonEscape(identity.relay_id)
            << "\",\"node_id\":\"" << JsonEscape(identity.node_id) << "\",\"start_time_us\":" << identity.start_time_us
            << ",\"start_time\":\"" << JsonEscape(FormatUtcTimestamp(identity.start_time_us))
            << "\",\"result\":" << result_json << '}';
        return out.str();
    }

    std::string EncodeAdminError(std::string_view id,
                                 const AdminRelayIdentity& identity,
                                 std::string_view message,
                                 bool id_is_number)
    {
        std::ostringstream out;
        out << '{';
        AppendAdminId(out, id, id_is_number);
        out << ",\"api_version\":" << kAdminApiVersion << ",\"relay_id\":\"" << JsonEscape(identity.relay_id)
            << "\",\"node_id\":\"" << JsonEscape(identity.node_id) << "\",\"start_time_us\":" << identity.start_time_us
            << ",\"start_time\":\"" << JsonEscape(FormatUtcTimestamp(identity.start_time_us))
            << "\",\"error\":{\"message\":\"" << JsonEscape(message) << "\"}}";
        return out.str();
    }

    std::string EncodeAdminResult(const AdminRpcMessage& message,
                                  const AdminRelayIdentity& identity,
                                  std::string_view result_json)
    {
        return EncodeAdminResult(message.id, identity, result_json, message.id_is_number);
    }

    std::string EncodeAdminError(const AdminRpcMessage& message,
                                 const AdminRelayIdentity& identity,
                                 std::string_view error_message)
    {
        return EncodeAdminError(message.id, identity, error_message, message.id_is_number);
    }

    std::string EncodeAdminResultsDocument(const std::vector<AdminResultCall>& calls,
                                           std::string_view fallback_relay_id)
    {
        std::optional<std::uint64_t> api_version;
        std::optional<std::string> relay_id;
        std::optional<std::string> node_id;
        std::optional<std::uint64_t> start_time_us;
        std::optional<std::string> start_time;
        std::vector<std::pair<std::string, std::string>> entries;
        entries.reserve(calls.size());

        std::map<std::string, int> totals;
        for (const auto& call : calls) {
            ++totals[call.method];
        }
        std::map<std::string, int> seen;

        for (const auto& call : calls) {
            const auto message = ParseAdminMessage(call.body);
            if (!api_version.has_value() && message.has_api_version) {
                api_version = message.api_version;
            }
            if (!relay_id.has_value() && message.has_relay_id && !message.relay_id.empty()) {
                relay_id = message.relay_id;
            }
            if (!node_id.has_value() && message.has_node_id && !message.node_id.empty()) {
                node_id = message.node_id;
            }
            if (!start_time_us.has_value() && message.has_start_time_us) {
                start_time_us = message.start_time_us;
            }
            if (!start_time.has_value() && message.has_start_time) {
                start_time = message.start_time;
            }

            std::string value;
            if (!message.valid) {
                const auto reason = message.error.empty() ? "response was not a JSON object" : message.error;
                value = "{\"error\":{\"message\":\"" + JsonEscape(reason) + "\"}}";
            } else if (message.has_error || !message.has_result) {
                const auto reason = !message.error_message.empty() ? message.error_message : "response had no result";
                value = "{\"error\":{\"message\":\"" + JsonEscape(reason) + "\"}}";
            } else {
                try {
                    const auto stripped = StripTopLevelRelayId(message.result_json);
                    value = stripped.json;
                    if (!relay_id.has_value() && stripped.relay_id.has_value() && !stripped.relay_id->empty()) {
                        relay_id = stripped.relay_id;
                    }
                } catch (const std::exception&) {
                    value = std::string(message.result_json);
                }
            }

            auto& index = seen[call.method];
            ++index;
            std::string key = call.method;
            if (totals[call.method] > 1) {
                key += "#" + std::to_string(index);
            }
            entries.emplace_back(std::move(key), std::move(value));
        }

        if (!relay_id.has_value()) {
            relay_id = std::string(fallback_relay_id);
        }

        std::ostringstream out;
        out << '{';
        bool first = true;
        const auto field = [&out, &first](std::string_view key, std::string_view raw_value) {
            if (!first) {
                out << ',';
            }
            first = false;
            out << '"' << JsonEscape(key) << "\":" << raw_value;
        };
        if (api_version.has_value()) {
            field("api_version", std::to_string(*api_version));
        }
        field("relay_id", "\"" + JsonEscape(*relay_id) + "\"");
        if (node_id.has_value()) {
            field("node_id", "\"" + JsonEscape(*node_id) + "\"");
        }
        if (start_time_us.has_value()) {
            field("start_time_us", std::to_string(*start_time_us));
        }
        if (start_time.has_value()) {
            field("start_time", "\"" + JsonEscape(*start_time) + "\"");
        }

        std::ostringstream results;
        results << '{';
        bool first_result = true;
        for (const auto& [key, value] : entries) {
            if (!first_result) {
                results << ',';
            }
            first_result = false;
            results << '"' << JsonEscape(key) << "\":" << value;
        }
        results << '}';
        field("results", results.str());
        out << '}';
        return out.str();
    }

    std::string EncodeAdminStateSizes(const AdminStateSizes& sizes)
    {
        std::ostringstream out;
        bool first = true;
        out << '{';
        AppendUint(out, first, "accepted_connections", sizes.accepted_connections);
        AppendUint(out, first, "subscriptions", sizes.subscriptions);
        AppendUint(out, first, "publishes", sizes.publishes);
        AppendUint(out, first, "publish_namespaces", sizes.publish_namespaces);
        AppendUint(out, first, "subscribe_namespaces", sizes.subscribe_namespaces);
        AppendUint(out, first, "subscribe_tracks", sizes.subscribe_tracks);
        AppendUint(out, first, "requests", sizes.requests);
        AppendUint(out, first, "subscribe_active", sizes.subscribe_active);
        AppendUint(out, first, "publish_subscribes_by_request", sizes.publish_subscribes_by_request);
        AppendUint(out, first, "subscribe_alias_by_request", sizes.subscribe_alias_by_request);
        AppendUint(out, first, "total_connections", sizes.total_connections);
        AppendUint(out, first, "fanout_namespaces", sizes.fanout_namespaces);
        AppendUint(out, first, "notified_namespaces", sizes.notified_namespaces);
        AppendUint(out, first, "cache_tracks", sizes.cache_tracks);
        AppendUint(out, first, "track_rankings", sizes.track_rankings);
        AppendUint(out, first, "peering_interests", sizes.peering_interests);
        AppendCache(out, sizes.cache);
        out << '}';
        return out.str();
    }

    std::string EncodeAdminPeers(const AdminPeers& peers)
    {
        std::ostringstream out;
        out << "{\"peers\":[";
        bool first = true;
        for (const auto& peer : peers.peers) {
            if (!first) {
                out << ',';
            }
            first = false;
            out << "{\"session_id\":" << peer.session_id << ",\"role\":\"" << JsonEscape(peer.role)
                << "\",\"status\":\"" << JsonEscape(peer.status) << "\",\"host\":\"" << JsonEscape(peer.host)
                << "\",\"port\":" << peer.port << ",\"metric\":" << peer.metric
                << ",\"node_metric\":" << peer.node_metric;
            if (peer.identified) {
                out << ",\"node_id\":\"" << JsonEscape(peer.node_id) << "\",\"contact\":\"" << JsonEscape(peer.contact)
                    << "\",\"node_type\":\"" << JsonEscape(peer.node_type) << '"';
            }
            out << ",\"metrics\":{\"srtt_us\":" << peer.srtt_us << ",\"connect_time_us\":" << peer.connect_time_us
                << ",\"connect_time\":\"" << JsonEscape(FormatUtcTimestamp(peer.connect_time_us))
                << "\",\"subscribe_tracks\":" << peer.subscribe_tracks << ",\"publishes\":" << peer.publishes << "}}";
        }
        out << "]}";
        return out.str();
    }

    std::string EncodeAdminTrackList(std::string_view field, const std::vector<AdminTrackBinding>& entries)
    {
        std::ostringstream out;
        out << "{\"" << field << "\":[";
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i != 0) {
                out << ',';
            }
            const auto& entry = entries[i];
            out << "{\"connection_handle\":" << entry.connection_handle << ",\"track_namespace\":\""
                << JsonEscape(entry.track_namespace) << "\",\"track_name\":\"" << JsonEscape(entry.track_name) << "\"}";
        }
        out << "]}";
        return out.str();
    }

    std::string EncodeAdminNamespaceList(std::string_view field, const std::vector<AdminNamespaceEntry>& entries)
    {
        std::ostringstream out;
        out << "{\"" << field << "\":[";
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i != 0) {
                out << ',';
            }
            const auto& entry = entries[i];
            out << "{\"connection_handle\":" << entry.connection_handle;
            if (entry.local) {
                out << ",\"local\":true";
            }
            out << ",\"track_namespace\":\"" << JsonEscape(entry.track_namespace) << '"';
            if (entry.has_tracks) {
                out << ",\"tracks\":[";
                for (std::size_t track = 0; track < entry.tracks.size(); ++track) {
                    if (track != 0) {
                        out << ',';
                    }
                    out << "{\"track_alias\":" << entry.tracks[track].track_alias << ",\"track_name\":\""
                        << JsonEscape(entry.tracks[track].track_name) << "\"}";
                }
                out << ']';
            }
            out << '}';
        }
        out << "]}";
        return out.str();
    }

    std::string EncodeAdminMetrics(const std::vector<std::string>& connections, const std::vector<std::string>& tracks)
    {
        std::ostringstream out;
        out << "{\"connections\":[";
        for (std::size_t i = 0; i < connections.size(); ++i) {
            if (i != 0) {
                out << ',';
            }
            out << connections[i];
        }
        out << "],\"tracks\":[";
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            if (i != 0) {
                out << ',';
            }
            out << tracks[i];
        }
        out << "]}";
        return out.str();
    }

    std::vector<std::string> SplitAdminParams(std::string_view text)
    {
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (start <= text.size()) {
            const auto comma = text.find(',', start);
            const auto token =
              text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
            std::size_t left = 0;
            std::size_t right = token.size();
            while (left < right && std::isspace(static_cast<unsigned char>(token[left]))) {
                ++left;
            }
            while (right > left && std::isspace(static_cast<unsigned char>(token[right - 1]))) {
                --right;
            }
            parts.emplace_back(token.substr(left, right - left));
            if (comma == std::string_view::npos) {
                break;
            }
            start = comma + 1;
        }
        return parts;
    }

    std::optional<std::uint64_t> ParseMetricText(const std::string& text, std::string& error)
    {
        if (text.empty() || text.front() == '-') {
            error = "metric must be an unsigned integer";
            return std::nullopt;
        }
        std::size_t consumed = 0;
        unsigned long long value = 0;
        try {
            value = std::stoull(text, &consumed);
        } catch (const std::exception&) {
            error = "metric must be an unsigned integer";
            return std::nullopt;
        }
        if (consumed != text.size()) {
            error = "metric must be an unsigned integer";
            return std::nullopt;
        }
        return value;
    }

    std::optional<AdminPeerEndpoint> ParseAdminPeerEndpoint(std::string_view text,
                                                            std::string& error,
                                                            bool require_metric)
    {
        const auto trimmed = SplitAdminParams(text);
        if (trimmed.size() != 1 || trimmed.front().empty()) {
            error = require_metric ? "expected host:port:metric" : "expected host:port";
            return std::nullopt;
        }
        const auto& value = trimmed.front();

        std::string host;
        std::string port_text;
        std::optional<std::string> metric_text;
        if (!value.empty() && value.front() == '[') {
            const auto end = value.find(']');
            if (end == std::string::npos || end + 1 >= value.size() || value[end + 1] != ':') {
                error = require_metric ? "expected [ipv6]:port:metric" : "expected [ipv6]:port";
                return std::nullopt;
            }
            host = value.substr(1, end - 1);
            const auto rest = value.substr(end + 2);
            const auto colon = rest.find(':');
            if (colon == std::string::npos) {
                port_text = rest;
            } else {
                port_text = rest.substr(0, colon);
                metric_text = rest.substr(colon + 1);
            }
        } else {
            const auto colon_count = static_cast<std::size_t>(std::count(value.begin(), value.end(), ':'));
            if (colon_count == 1) {
                const auto colon = value.find(':');
                host = value.substr(0, colon);
                port_text = value.substr(colon + 1);
            } else if (colon_count == 2) {
                const auto first = value.find(':');
                const auto second = value.find(':', first + 1);
                host = value.substr(0, first);
                port_text = value.substr(first + 1, second - first - 1);
                metric_text = value.substr(second + 1);
            } else {
                error = require_metric ? "expected host:port:metric" : "expected host:port";
                return std::nullopt;
            }
        }

        if (host.empty() || port_text.empty()) {
            error = require_metric ? "expected host:port:metric" : "expected host:port";
            return std::nullopt;
        }

        std::size_t consumed = 0;
        unsigned long port = 0;
        try {
            port = std::stoul(port_text, &consumed);
        } catch (const std::exception&) {
            error = "port must be an integer from 1 to 65535";
            return std::nullopt;
        }
        if (consumed != port_text.size() || port == 0 || port > 65535) {
            error = "port must be an integer from 1 to 65535";
            return std::nullopt;
        }

        AdminPeerEndpoint endpoint;
        endpoint.host = std::move(host);
        endpoint.port = static_cast<std::uint16_t>(port);
        if (metric_text.has_value()) {
            const auto metric = ParseMetricText(*metric_text, error);
            if (!metric.has_value()) {
                return std::nullopt;
            }
            endpoint.metric = *metric;
        } else if (require_metric) {
            error = "expected host:port:metric";
            return std::nullopt;
        }
        return endpoint;
    }

    std::optional<AdminPeerEndpoint> ParseAdminPeerParams(std::string_view params_json, std::string& error)
    {
        try {
            JsonReader reader(params_json);
            if (!reader.Consume('{')) {
                error = "peer params must be an object";
                return std::nullopt;
            }

            std::optional<std::string> host;
            std::optional<std::uint16_t> port;
            std::optional<std::uint64_t> metric;
            if (!reader.Consume('}')) {
                while (true) {
                    const auto key = reader.ParseString();
                    if (!reader.Consume(':')) {
                        error = "expected ':'";
                        return std::nullopt;
                    }
                    if (key == "host") {
                        host = reader.ParseString();
                    } else if (key == "port") {
                        if (!reader.StartsNumber()) {
                            error = "port must be an integer from 1 to 65535";
                            return std::nullopt;
                        }
                        const auto text = reader.CaptureValue();
                        std::size_t consumed = 0;
                        const auto value = std::stoul(text, &consumed);
                        if (consumed != text.size() || value == 0 || value > 65535) {
                            error = "port must be an integer from 1 to 65535";
                            return std::nullopt;
                        }
                        port = static_cast<std::uint16_t>(value);
                    } else if (key == "metric") {
                        if (!reader.StartsNumber()) {
                            error = "metric must be an unsigned integer";
                            return std::nullopt;
                        }
                        const auto text = reader.CaptureValue();
                        const auto value = ParseMetricText(text, error);
                        if (!value.has_value()) {
                            return std::nullopt;
                        }
                        metric = *value;
                    } else {
                        reader.SkipValue();
                    }

                    if (reader.Consume('}')) {
                        break;
                    }
                    if (!reader.Consume(',')) {
                        error = "expected ',' or '}'";
                        return std::nullopt;
                    }
                }
            }
            if (!reader.Exhausted()) {
                error = "trailing data after peer params";
                return std::nullopt;
            }
            if (!host.has_value() || host->empty() || !port.has_value()) {
                error = "peer params require host and port";
                return std::nullopt;
            }

            AdminPeerEndpoint endpoint;
            endpoint.host = *host;
            endpoint.port = *port;
            endpoint.metric = metric;
            return endpoint;
        } catch (const std::exception& ex) {
            error = ex.what();
            return std::nullopt;
        }
    }

    std::string EncodeAdminPeerParams(const AdminPeerEndpoint& endpoint)
    {
        std::ostringstream out;
        out << "{\"host\":\"" << JsonEscape(endpoint.host) << "\",\"port\":" << endpoint.port;
        if (endpoint.metric.has_value()) {
            out << ",\"metric\":" << *endpoint.metric;
        }
        out << '}';
        return out.str();
    }

    std::string EncodeAdminPeerCommand(const AdminPeerCommand& command)
    {
        std::ostringstream out;
        out << "{\"host\":\"" << JsonEscape(command.host) << "\",\"port\":" << command.port;
        if (command.metric.has_value()) {
            out << ",\"metric\":" << *command.metric;
        }
        out << ",\"action\":\"" << JsonEscape(command.action) << "\",\"message\":\"" << JsonEscape(command.message)
            << "\"}";
        return out.str();
    }
} // namespace laps
