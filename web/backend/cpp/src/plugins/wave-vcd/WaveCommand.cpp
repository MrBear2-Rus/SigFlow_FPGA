#include "WaveCommand.h"
#include "VcdWaveformBackend.h"
#include "eda-core/JobPaths.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
namespace eda::wave {
namespace {

constexpr std::size_t kMaxVcdBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaxRequestBytes = 64 * 1024;
constexpr std::size_t kMaxResponseBytes = 900 * 1024;
constexpr std::uint64_t kMaxPageSize = 1000;

std::uint64_t Number(const std::string& text) {
    std::uint64_t number = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (text.empty() || error != std::errc() || end != text.data() + text.size())
        throw std::invalid_argument("expected decimal uint64");
    return number;
}

std::uint64_t JsonUnsigned(const Json& value, const char* field) {
    if (!value.is_number_integer())
        throw std::invalid_argument(std::string(field) + " must be a nonnegative integer");
    if (value.is_number_unsigned())
        return value.get<std::uint64_t>();
    const auto number = value.get<std::int64_t>();
    if (number < 0)
        throw std::invalid_argument(std::string(field) + " must be a nonnegative integer");
    return static_cast<std::uint64_t>(number);
}

std::uint64_t OptionalUnsigned(const Json& request, const char* field, std::uint64_t defaultValue) {
    return request.contains(field) ? JsonUnsigned(request.at(field), field) : defaultValue;
}

std::uint64_t JsonDecimalString(const Json& value, const char* field) {
    if (!value.is_string())
        throw std::invalid_argument(std::string(field) + " must be a decimal string");
    return Number(value.get<std::string>());
}

Json TimeRange(const VcdWaveformBackend& backend) {
    const auto range = backend.TimeRange();
    return range.valid
               ? Json{{"begin", std::to_string(range.begin)}, {"end", std::to_string(range.end)}}
               : Json(nullptr);
}

Json SignalsData(const VcdWaveformBackend& backend, std::uint64_t offset, std::uint64_t limit) {
    Json signals = Json::array();
    const auto& all = backend.Signals();
    const auto begin = std::min<std::uint64_t>(offset, all.size());
    const auto end = std::min<std::uint64_t>(all.size(), begin + limit);
    for (auto i = begin; i < end; ++i) {
        const auto& signal = all[static_cast<std::size_t>(i)];
        signals.push_back({{"id", signal.id},
                           {"name", signal.name},
                           {"scope", signal.scope},
                           {"full_name", signal.fullName},
                           {"width", signal.width}});
    }
    return {{"schema", "sigflow.wave.signals.v1"},
            {"timescale", backend.Timescale()},
            {"time_range", TimeRange(backend)},
            {"signals", signals},
            {"offset", offset},
            {"next_offset", end < all.size() ? Json(end) : Json(nullptr)}};
}

Json QueryData(const VcdWaveformBackend& backend, std::uint64_t signalId, std::uint64_t begin,
               std::uint64_t end, std::uint64_t offset, std::uint64_t limit) {
    if (signalId > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("invalid signal id");
    if (offset > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("offset exceeds platform limit");

    std::vector<WaveTransition> page;
    bool more = false;
    std::string error;
    if (!backend.QueryPage(static_cast<int>(signalId), begin, end, static_cast<std::size_t>(offset),
                           static_cast<std::size_t>(limit), page, more, error))
        throw std::invalid_argument(error);

    std::string value;
    if (!backend.ValueAt(static_cast<int>(signalId), begin, value, error))
        throw std::invalid_argument(error);
    Json transitions = Json::array();
    for (const auto& item : page)
        transitions.push_back({{"time", std::to_string(item.time)}, {"value", item.value}});
    return {{"schema", "sigflow.wave.query.v1"},
            {"signal_id", signalId},
            {"begin", std::to_string(begin)},
            {"end", std::to_string(end)},
            {"initial_value", value.empty() ? Json(nullptr) : Json(value)},
            {"transitions", transitions},
            {"offset", offset},
            {"next_offset", more ? Json(offset + page.size()) : Json(nullptr)}};
}

std::unique_ptr<VcdWaveformBackend> OpenBackend(const std::string& pathText) {
    fs::path path(pathText);
    if (!path.is_absolute())
        throw std::invalid_argument("VCD path must be absolute");
    path = web::CheckedPath(path.parent_path(), path, true);
    if (!fs::is_regular_file(path) || fs::file_size(path) > kMaxVcdBytes)
        throw std::invalid_argument("VCD must be a regular file no larger than 64 MiB");
    auto backend = std::make_unique<VcdWaveformBackend>();
    std::string error;
    if (!backend->Open(path.string(), error))
        throw std::runtime_error(error);
    return backend;
}

Json ErrorResponse(const Json& id, const char* code, const std::string& message) {
    return {{"id", id}, {"ok", false}, {"error", {{"code", code}, {"message", message}}}};
}

bool ValidId(const Json& id) {
    if (id.is_string()) {
        const auto& value = id.get_ref<const std::string&>();
        return !value.empty() && value.size() <= 128;
    }
    return id.is_number_integer() && (id.is_number_unsigned() || id.get<std::int64_t>() >= 0);
}

enum class LineResult { Line, TooLong, End };
LineResult ReadRequestLine(std::istream& input, std::string& line) {
    line.clear();
    bool tooLong = false;
    char character = 0;
    while (input.get(character)) {
        if (character == '\n')
            return tooLong ? LineResult::TooLong : LineResult::Line;
        if (tooLong)
            continue;
        if (line.size() == kMaxRequestBytes) {
            line.clear();
            tooLong = true;
            continue;
        }
        line.push_back(character);
    }
    if (input.bad())
        throw std::runtime_error("failed to read wave JSONL input");
    if (tooLong)
        return LineResult::TooLong;
    return line.empty() ? LineResult::End : LineResult::Line;
}

Json HandleRequest(const Json& request, std::unique_ptr<VcdWaveformBackend>& backend) {
    Json id = nullptr;
    if (!request.is_object())
        return ErrorResponse(id, "INVALID_REQUEST", "request must be a JSON object");
    if (request.contains("id") && ValidId(request.at("id")))
        id = request.at("id");
    else
        return ErrorResponse(id, "INVALID_REQUEST",
                             "id must be a nonempty string or nonnegative integer");
    if (!request.contains("op") || !request.at("op").is_string())
        return ErrorResponse(id, "INVALID_REQUEST", "op must be a string");

    const auto operation = request.at("op").get<std::string>();
    try {
        if (operation == "open") {
            if (!request.contains("vcd") || !request.at("vcd").is_string())
                throw std::invalid_argument("vcd must be an absolute path string");
            auto replacement = OpenBackend(request.at("vcd").get<std::string>());
            Json data = {{"schema", "sigflow.wave.open.v1"},
                         {"timescale", replacement->Timescale()},
                         {"time_range", TimeRange(*replacement)},
                         {"signal_count", replacement->Signals().size()}};
            backend = std::move(replacement);
            return {{"id", id}, {"ok", true}, {"data", data}};
        }
        if (operation == "close") {
            backend.reset();
            return {{"id", id}, {"ok", true}, {"data", {{"schema", "sigflow.wave.close.v1"}}}};
        }
        if (!backend)
            throw std::invalid_argument("no VCD is open; send an open request first");
        if (operation == "signals") {
            const auto offset = OptionalUnsigned(request, "offset", 0);
            const auto limit = OptionalUnsigned(request, "limit", 100);
            if (limit == 0 || limit > kMaxPageSize)
                throw std::invalid_argument("limit must be 1..1000");
            return {{"id", id}, {"ok", true}, {"data", SignalsData(*backend, offset, limit)}};
        }
        if (operation == "query") {
            for (const char* field : {"signal_id", "begin", "end"})
                if (!request.contains(field))
                    throw std::invalid_argument(std::string("missing field: ") + field);
            const auto signalId = JsonUnsigned(request.at("signal_id"), "signal_id");
            const auto begin = JsonDecimalString(request.at("begin"), "begin");
            const auto end = JsonDecimalString(request.at("end"), "end");
            const auto offset = OptionalUnsigned(request, "offset", 0);
            const auto limit = OptionalUnsigned(request, "limit", 100);
            if (limit == 0 || limit > kMaxPageSize)
                throw std::invalid_argument("limit must be 1..1000");
            return {{"id", id},
                    {"ok", true},
                    {"data", QueryData(*backend, signalId, begin, end, offset, limit)}};
        }
        return ErrorResponse(id, "UNKNOWN_OPERATION", "unsupported wave operation: " + operation);
    } catch (const std::invalid_argument& error) {
        return ErrorResponse(id, "INVALID_REQUEST", error.what());
    } catch (const std::exception& error) {
        return ErrorResponse(id, "WAVE_FAILED", error.what());
    }
}

int Serve(const std::function<void(const Json&)>& emit) {
    std::unique_ptr<VcdWaveformBackend> backend;
    std::string line;
    while (true) {
        const auto result = ReadRequestLine(std::cin, line);
        if (result == LineResult::End)
            return 0;

        Json response;
        if (result == LineResult::TooLong) {
            response = ErrorResponse(nullptr, "REQUEST_TOO_LARGE", "request line exceeds 64 KiB");
        } else {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            try {
                response = HandleRequest(Json::parse(line), backend);
            } catch (const std::exception& error) {
                response = ErrorResponse(nullptr, "INVALID_JSON", error.what());
            }
        }
        if (response.dump().size() + 1 > kMaxResponseBytes)
            response = ErrorResponse(response.value("id", Json(nullptr)), "OUTPUT_TOO_LARGE",
                                     "wave output exceeds 900 KiB; reduce page size");
        emit(response);
    }
}

}  // namespace

int WaveCommand(int argc, char** argv, const std::function<void(const Json&)>& emit) {
    try {
        if (argc < 3)
            throw std::invalid_argument("missing wave subcommand");
        const std::string action = argv[2];
        if (action == "serve") {
            if (argc != 3)
                throw std::invalid_argument("wave serve takes JSONL requests on stdin");
            return Serve(emit);
        }
        if (action != "signals" && action != "query") {
            emit(Json{{"type", "diagnostic"},
                      {"severity", "error"},
                      {"code", "NOT_IMPLEMENTED"},
                      {"message", "wave subcommand not implemented"}});
            return 3;
        }

        const std::set<std::string> allowed =
            action == "signals" ? std::set<std::string>{"--vcd", "--offset", "--limit"}
                                : std::set<std::string>{"--vcd", "--signals", "--begin",
                                                        "--end", "--offset",  "--limit"};
        std::map<std::string, std::string> options;
        for (int i = 3; i < argc; ++i) {
            std::string key = argv[i];
            if (!allowed.count(key) || options.count(key) || i + 1 == argc ||
                std::string(argv[i + 1]).empty() || std::string(argv[i + 1]).starts_with("--"))
                throw std::invalid_argument("invalid or duplicate option: " + key);
            options[key] = argv[++i];
        }
        const auto required = [&](const char* key) -> std::string {
            if (!options.count(key))
                throw std::invalid_argument(std::string("missing option: ") + key);
            return options.at(key);
        };
        const auto offset = options.count("--offset") ? Number(options.at("--offset")) : 0;
        const auto limit = options.count("--limit") ? Number(options.at("--limit")) : 100;
        if (!limit || limit > kMaxPageSize)
            throw std::invalid_argument("limit must be 1..1000");

        auto backend = OpenBackend(required("--vcd"));
        Json data;
        if (action == "signals") {
            data = SignalsData(*backend, offset, limit);
        } else {
            const auto signalId = Number(required("--signals"));
            const auto begin = Number(required("--begin"));
            const auto end = Number(required("--end"));
            data = QueryData(*backend, signalId, begin, end, offset, limit);
        }
        Json result = {{"type", "result"}, {"ok", true}, {"data", data}};
        if (result.dump().size() + 1 > kMaxResponseBytes)
            throw std::runtime_error("wave output exceeds 900 KiB; reduce page size");
        emit(result);
        return 0;
    } catch (const std::exception& error) {
        emit(Json{{"type", "diagnostic"},
                  {"severity", "error"},
                  {"code", "WAVE_FAILED"},
                  {"message", error.what()}});
        return 4;
    }
}
}  // namespace eda::wave
