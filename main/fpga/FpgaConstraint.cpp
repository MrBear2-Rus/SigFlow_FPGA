#include "FpgaConstraint.h"
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/datetime.h>
#include <wx/sstream.h>
#include <wx/tokenzr.h>
#include <wx/txtstrm.h>
#include <json/json.h>
#include <sstream>
#include <algorithm>
#include <memory>
#include <set>

namespace {

int FindCharacterAfter(const wxString& value, wxChar character, int startIndex)
{
    if (startIndex < 0 || static_cast<size_t>(startIndex) >= value.length()) {
        return wxNOT_FOUND;
    }

    const int offset = value.Mid(startIndex).Find(character);
    return offset == wxNOT_FOUND ? wxNOT_FOUND : startIndex + offset;
}

} // namespace

// ==================== PinBinding ====================

wxString PinBinding::GetFullPortName() const {
    if (bitIndex == 0) {
        // 检查是否为向量端口（暂时根据上下文判断；如果 bitIndex >= 0 且 portName 不含 []，则为标量）
        // 向量端口输出为 "port[N]"
        return portName;
    }
    // 如果 bitIndex 非零或我们需要标记为向量，由外部通过 portName 直接设置
    return portName;
}

// ==================== ConstraintSheet ====================

PinBinding* ConstraintSheet::FindBinding(const wxString& portName, int bitIndex) {
    for (auto& b : bindings) {
        if (b.portName == portName && b.bitIndex == bitIndex) return &b;
    }
    return nullptr;
}

const PinBinding* ConstraintSheet::FindBinding(const wxString& portName, int bitIndex) const {
    for (const auto& b : bindings) {
        if (b.portName == portName && b.bitIndex == bitIndex) return &b;
    }
    return nullptr;
}

bool ConstraintSheet::IsPortBound(const wxString& portName, int bitIndex) const {
    const auto* b = FindBinding(portName, bitIndex);
    return b && b->packagePin > 0;
}

std::vector<const PinBinding*> ConstraintSheet::GetUnboundPorts() const {
    std::vector<const PinBinding*> result;
    for (const auto& b : bindings) {
        if (b.packagePin <= 0) result.push_back(&b);
    }
    return result;
}

// ==================== ConstraintValidator ====================

ConstraintValidator::ConstraintValidator(const FpgaPinDatabase& pinDb)
    : m_pinDb(pinDb) {}

FpgaPortMap
ConstraintValidator::ParsePortsFromJsonString(const wxString& jsonStr, const wxString& topModule,
    wxString& errorMessage) const {
    FpgaPortMap ports;

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    const wxScopedCharBuffer utf8 = jsonStr.ToUTF8();
    const char* data = utf8.data();
    if (!data || utf8.length() == 0) {
        errorMessage = "Empty JSON content.";
        return ports;
    }

    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(data, data + utf8.length(), &root, &errors)) {
        errorMessage = wxString::FromUTF8(errors);
        return ports;
    }

    if (!root.isMember("modules")) {
        errorMessage = "Yosys JSON format error: missing 'modules' key.";
        return ports;
    }

    const Json::Value& modules = root["modules"];
    if (!modules.isObject()) {
        errorMessage = "Yosys JSON format error: 'modules' is not an object.";
        return ports;
    }

    // 找到顶层模块 (通常第一个或唯一的一个)
    const Json::Value* targetModule = nullptr;
    wxString firstModuleName;
    for (auto it = modules.begin(); it != modules.end(); ++it) {
        firstModuleName = wxString::FromUTF8(it.name());
        targetModule = &(*it);
        break; // 取第一个模块作为目标
    }

    const wxScopedCharBuffer topModuleUtf8 = topModule.ToUTF8();
    if (!topModuleUtf8.data() || !modules.isMember(topModuleUtf8.data())) {
        errorMessage = wxString("Top module '") + topModule +
            "' was not found in the Yosys JSON netlist.";
        return ports;
    }
    targetModule = &modules[topModuleUtf8.data()];

    if (!targetModule) {
        errorMessage = "No modules found in Yosys JSON.";
        return ports;
    }

    if (!targetModule->isMember("ports")) {
        return ports; // 无端口 — 空结果但不是错误
    }

    const Json::Value& portList = (*targetModule)["ports"];
    if (!portList.isObject()) return ports;

    for (auto portIt = portList.begin(); portIt != portList.end(); ++portIt) {
        wxString portName = wxString::FromUTF8(portIt.name());
        const Json::Value& portInfo = *portIt;

        FpgaPortDirection dir = FpgaPortDirection::Input;
        if (portInfo.isMember("direction")) {
            dir = FpgaPortDirectionFromString(
                wxString::FromUTF8(portInfo["direction"].asString()));
        }

        int width = 1;
        if (portInfo.isMember("bits") && portInfo["bits"].isArray()) {
            width = static_cast<int>(portInfo["bits"].size());
        }

        ports[portName] = {dir, width};
    }

    return ports;
}

FpgaPortMap
ConstraintValidator::ExtractPortsFromYosysJson(const wxString& jsonPath, const wxString& topModule,
    wxString& errorMessage) const {
    wxFile file(jsonPath, wxFile::read);
    if (!file.IsOpened()) {
        errorMessage = wxString("Cannot open Yosys JSON file: ") + jsonPath;
        return {};
    }

    wxString content;
    file.ReadAll(&content);
    file.Close();

    return ParsePortsFromJsonString(content, topModule, errorMessage);
}

std::vector<ConstraintError> ConstraintValidator::Validate(
    const ConstraintSheet& sheet,
    const FpgaPortMap& ports) const
{
    std::vector<ConstraintError> errors;

    // 1. 端口存在性检查
    for (size_t i = 0; i < sheet.bindings.size(); ++i) {
        const auto& binding = sheet.bindings[i];
        auto portIt = ports.find(binding.portName);

        if (portIt == ports.end()) {
            ConstraintError e;
            e.severity = ConstraintError::Severity::Error;
            e.message = wxString::Format("Port '%s' not found in synthesized netlist.", binding.portName);
            e.field = wxString::Format("bindings[%zu].port", i);
            e.rowIndex = static_cast<int>(i);
            errors.push_back(e);
            continue;
        }

        const auto& [dir, width] = portIt->second;

        if (binding.packagePin <= 0) {
            ConstraintError e;
            e.severity = ConstraintError::Severity::Error;
            e.message = wxString("Port '") + binding.portName + "' is not assigned to a package pin.";
            e.field = wxString::Format("bindings[%zu].package_pin", i);
            e.rowIndex = static_cast<int>(i);
            errors.push_back(e);
        }

        // 2. 向量索引范围检查
        if (binding.bitIndex >= width || binding.bitIndex < 0) {
            ConstraintError e;
            e.severity = ConstraintError::Severity::Error;
            e.message = wxString::Format("Bit index %d out of range for port '%s' (width=%d).",
                binding.bitIndex, binding.portName, width);
            e.field = wxString::Format("bindings[%zu].bit", i);
            e.rowIndex = static_cast<int>(i);
            errors.push_back(e);
        }
    }

    // 3. 引脚唯一性检查
    std::map<int, std::vector<int>> pinUsage; // pin -> [binding indices]
    for (size_t i = 0; i < sheet.bindings.size(); ++i) {
        const auto& b = sheet.bindings[i];
        if (b.packagePin > 0) {
            pinUsage[b.packagePin].push_back(static_cast<int>(i));
        }
    }
    for (const auto& [pin, indices] : pinUsage) {
        if (indices.size() > 1) {
            ConstraintError e;
            e.severity = ConstraintError::Severity::Error;
            e.message = wxString::Format("Package pin %d is assigned to %zu ports.", pin, indices.size());
            e.field = wxString::Format("bindings[%d].package_pin", indices[1]);
            e.rowIndex = indices[1];
            errors.push_back(e);
        }
    }

    // 4. 引脚有效性和可用性
    for (size_t i = 0; i < sheet.bindings.size(); ++i) {
        const auto& b = sheet.bindings[i];
        if (b.packagePin <= 0) continue;

        if (b.packagePin < 1 || b.packagePin > 88) {
            ConstraintError e;
            e.severity = ConstraintError::Severity::Error;
            e.message = wxString::Format("Pin %d is out of range (1-88) for QFN88 package.", b.packagePin);
            e.field = wxString::Format("bindings[%zu].package_pin", i);
            e.rowIndex = static_cast<int>(i);
            errors.push_back(e);
            continue;
        }

        if (!m_pinDb.IsPinAvailable(b.packagePin)) {
            const auto* pin = m_pinDb.FindPin(b.packagePin);
            wxString reservedReason("Unavailable");
            if (pin && !pin->reservedReason.IsEmpty()) {
                reservedReason = pin->reservedReason;
            }
            ConstraintError e;
            e.severity = ConstraintError::Severity::Error;
            e.message = wxString::Format("Pin %d is reserved: %s",
                b.packagePin,
                reservedReason);
            e.field = wxString::Format("bindings[%zu].package_pin", i);
            e.rowIndex = static_cast<int>(i);
            errors.push_back(e);
        }
    }

    // 5. IO_TYPE 有效性检查 (软检查)
    const auto& knownTypes = GetKnownIOTypes();
    for (size_t i = 0; i < sheet.bindings.size(); ++i) {
        const auto& b = sheet.bindings[i];
        if (!b.ioType.IsEmpty() && b.packagePin > 0) {
            auto it = std::find(knownTypes.begin(), knownTypes.end(), b.ioType);
            if (it == knownTypes.end()) {
                ConstraintError err;
                err.severity = ConstraintError::Severity::Warning;
                err.message = wxString::Format("Unknown IO_TYPE '%s' for port '%s'. Verify with device datasheet.",
                    b.ioType, b.portName);
                err.field = wxString::Format("bindings[%zu].io_type", i);
                err.rowIndex = static_cast<int>(i);
                errors.push_back(err);
            }
        }
    }

    // 6. 向量完整性: 同一向量端口所有 bit 必须全部绑定
    std::map<wxString, int, WxStringLess> boundBitCounts;
    for (const auto& b : sheet.bindings) {
        if (b.packagePin > 0) boundBitCounts[b.portName]++;
    }
    for (const auto& [portName, portInfo] : ports) {
        const int expectedWidth = portInfo.second;
        if (expectedWidth > 1) {
            int bound = boundBitCounts[portName];
            if (bound > 0 && bound < expectedWidth) {
                ConstraintError e;
                e.severity = ConstraintError::Severity::Error;
                e.message = wxString::Format("Vector port '%s' has width %d but only %d bits are bound.",
                    portName, expectedWidth, bound);
                e.field = "bindings";
                e.rowIndex = -1;
                errors.push_back(e);
            }
        }
    }

    std::sort(errors.begin(), errors.end(), [](const ConstraintError& a, const ConstraintError& b) {
        return static_cast<int>(a.severity) < static_cast<int>(b.severity);
    });
    return errors;
}

std::vector<ConstraintError> ConstraintValidator::ValidateBinding(const PinBinding& binding) const {
    std::vector<ConstraintError> errors;
    if (binding.portName.IsEmpty()) {
        errors.push_back({ConstraintError::Severity::Error, "Port name is empty.", "port", -1});
    }
    if (binding.packagePin > 0) {
        if (binding.packagePin < 1 || binding.packagePin > 88) {
            errors.push_back({ConstraintError::Severity::Error,
                wxString::Format("Pin %d out of QFN88 range (1-88).", binding.packagePin),
                "package_pin", -1});
        } else if (!m_pinDb.IsPinAvailable(binding.packagePin)) {
            errors.push_back({ConstraintError::Severity::Error,
                wxString::Format("Pin %d is reserved.", binding.packagePin),
                "package_pin", -1});
        }
    }
    return errors;
}

// ==================== CST Generator ====================

wxString CstGenerator::ComputeBindingsHash(const std::vector<PinBinding>& bindings) {
    // 生成简单的排序后 hash (CRC-like, 用 wxString hash)
    // 按 (portName, bitIndex) 排序后连接字符串
    std::vector<std::pair<wxString, wxString>> entries;
    for (const auto& b : bindings) {
        entries.emplace_back(b.portName + "_" + wxString::Format("%d", b.bitIndex),
            wxString::Format("%d|%s|%s|%d|%d",
                b.packagePin, b.ioType, b.drive,
                b.pullUp ? 1 : 0, b.pullDown ? 1 : 0));
    }
    std::sort(entries.begin(), entries.end());

    // Simple djb2-like hash for deterministic output
    unsigned long hash = 5381;
    for (const auto& [key, val] : entries) {
        wxString combined = key + "=" + val;
        for (wxChar c : combined) {
            hash = ((hash << 5) + hash) + static_cast<unsigned long>(c);
        }
    }
    return wxString::Format("%08lx", hash);
}

wxString CstGenerator::FormatCstHeader(const ConstraintSheet& sheet, const wxString& hash) {
    wxString header;
    header += "// Tang Nano 9K pin constraints — generated by SigFlow\n";
    header += wxString::Format("// Profile: %s@%s  Device: %s\n",
        sheet.targetProfileId, sheet.targetProfileVersion, sheet.device);
    header += wxString::Format("// Top module: %s  Bindings hash: %s\n",
        sheet.topModule, hash);
    header += wxString::Format("// Generated: %s\n", sheet.generatedAt);
    header += "//\n";
    header += "// Unused pins can be assigned with:\n";
    header += "//   IO_LOC <pin> port=\"__UNUSED\";\n";
    header += "\n";
    return header;
}

CstGenerationResult CstGenerator::Generate(const ConstraintSheet& sheet) const {
    CstGenerationResult result;
    result.bindingsHash = ComputeBindingsHash(sheet.bindings);

    std::ostringstream out;
    out << FormatCstHeader(sheet, result.bindingsHash).ToStdString();

    // 按 (portName, bitIndex) 排序以获得确定性输出
    std::vector<const PinBinding*> sortedBindings;
    for (const auto& b : sheet.bindings) {
        if (b.packagePin > 0) {
            sortedBindings.push_back(&b);
        }
    }
    std::sort(sortedBindings.begin(), sortedBindings.end(),
        [](const PinBinding* a, const PinBinding* b) {
            if (a->portName != b->portName) return a->portName.Cmp(b->portName) < 0;
            return a->bitIndex < b->bitIndex;
        });

    if (sortedBindings.empty()) {
        ConstraintError error;
        error.severity = ConstraintError::Severity::Error;
        error.message = "Cannot generate CST because no ports are assigned to package pins.";
        error.field = "bindings";
        result.errors.push_back(error);
        return result;
    }

    // 生成 IO_LOC 和 IO_PORT 语句
    for (const auto* b : sortedBindings) {
        // 格式化端口引用名
        wxString portRef;
        if (b->bitIndex > 0) {
            portRef = wxString::Format("%s[%d]", b->portName, b->bitIndex);
        } else {
            portRef = b->portName;
        }

        // Comment
        if (!b->comment.IsEmpty()) {
            out << "// " << b->comment.ToStdString() << "\n";
        }

        // IO_LOC
        out << "IO_LOC \"" << portRef.ToStdString() << "\" " << b->packagePin << ";\n";

        // IO_PORT
        wxString ioPortAttr;
        if (!b->ioType.IsEmpty()) {
            ioPortAttr += wxString(" IO_TYPE=") + b->ioType;
        }
        if (!b->drive.IsEmpty()) {
            ioPortAttr += wxString(" DRIVE=") + b->drive;
        }
        if (b->pullUp) {
            ioPortAttr += " PULL_MODE=UP";
        } else if (b->pullDown) {
            ioPortAttr += " PULL_MODE=DOWN";
        }

        if (!ioPortAttr.IsEmpty()) {
            out << "IO_PORT \"" << portRef.ToStdString() << "\""
                << ioPortAttr.ToStdString() << ";\n";
        }
        out << "\n";
    }

    result.cstContent = wxString::FromUTF8(out.str());
    result.success = true;
    return result;
}

std::vector<PinBinding> CstGenerator::ImportCst(
    const wxString& cstContent,
    std::vector<wxString>& unmanagedLines,
    wxString& errorMessage) const
{
    std::vector<PinBinding> bindings;
    unmanagedLines.clear();

    wxStringInputStream stream(cstContent);
    wxTextInputStream textStream(stream);

    while (!stream.Eof()) {
        wxString line = textStream.ReadLine();
        wxString trimmed = line;
        trimmed.Trim(true).Trim(false);

        // 跳过空行和注释行
        if (trimmed.IsEmpty() || trimmed.StartsWith("//") || trimmed.StartsWith("#")) {
            continue;
        }

        // IO_LOC "name" <pin>;
        if (trimmed.StartsWith("IO_LOC")) {
            wxString rest = trimmed.Mid(6); // 去掉 "IO_LOC"
            rest.Trim(false);

            // 提取引号内的端口名
            int q1 = rest.Find('"');
            int q2 = FindCharacterAfter(rest, '"', q1 + 1);
            if (q1 == wxNOT_FOUND || q2 == wxNOT_FOUND) {
                unmanagedLines.push_back(line);
                continue;
            }
            wxString portRef = rest.Mid(q1 + 1, q2 - q1 - 1);

            // 提取引脚号
            wxString pinStr = rest.Mid(q2 + 1);
            pinStr = pinStr.BeforeFirst(';');
            pinStr.Trim(true).Trim(false);

            long pinNum = 0;
            if (!pinStr.ToLong(&pinNum)) {
                unmanagedLines.push_back(line);
                continue;
            }

            // 解析端口名和位索引
            PinBinding binding;
            binding.source = "import";
            binding.packagePin = static_cast<int>(pinNum);

            int bracketPos = portRef.Find('[');
            if (bracketPos != wxNOT_FOUND) {
                int bracketEnd = FindCharacterAfter(portRef, ']', bracketPos + 1);
                if (bracketEnd != wxNOT_FOUND) {
                    binding.portName = portRef.Left(bracketPos);
                    wxString bitStr = portRef.Mid(bracketPos + 1, bracketEnd - bracketPos - 1);
                    long bitVal = 0;
                    if (bitStr.ToLong(&bitVal)) {
                        binding.bitIndex = static_cast<int>(bitVal);
                    }
                }
            } else {
                binding.portName = portRef;
                binding.bitIndex = 0;
            }

            bindings.push_back(binding);
            continue;
        }

        // IO_PORT "name" attr=value;
        if (trimmed.StartsWith("IO_PORT")) {
            wxString rest = trimmed.Mid(7);
            rest.Trim(false);

            int q1 = rest.Find('"');
            int q2 = FindCharacterAfter(rest, '"', q1 + 1);
            if (q1 == wxNOT_FOUND || q2 == wxNOT_FOUND) {
                unmanagedLines.push_back(line);
                continue;
            }
            wxString portRef = rest.Mid(q1 + 1, q2 - q1 - 1);

            // 解析属性和值
            wxString attrs = rest.Mid(q2 + 1);
            attrs = attrs.BeforeFirst(';');

            // 查找匹配的 binding 并更新属性
            wxString portName = portRef;
            int bitIndex = 0;
            int bracketPos = portRef.Find('[');
            if (bracketPos != wxNOT_FOUND) {
                int bracketEnd = FindCharacterAfter(portRef, ']', bracketPos + 1);
                if (bracketEnd != wxNOT_FOUND) {
                    portName = portRef.Left(bracketPos);
                    wxString bitStr = portRef.Mid(bracketPos + 1, bracketEnd - bracketPos - 1);
                    long bitVal = 0;
                    if (bitStr.ToLong(&bitVal)) bitIndex = static_cast<int>(bitVal);
                }
            }

            // 查找并更新已有 binding
            for (auto& b : bindings) {
                if (b.portName == portName && b.bitIndex == bitIndex) {
                    wxStringTokenizer tokenizer(attrs, " ");
                    while (tokenizer.HasMoreTokens()) {
                        wxString token = tokenizer.GetNextToken();
                        int eqPos = token.Find('=');
                        if (eqPos != wxNOT_FOUND) {
                            wxString key = token.Left(eqPos).Upper();
                            wxString value = token.Mid(eqPos + 1);
                            if (key == "IO_TYPE") b.ioType = value;
                            else if (key == "DRIVE") b.drive = value;
                            else if (key == "PULL_MODE") {
                                if (value.Upper() == "UP") { b.pullUp = true; b.pullDown = false; }
                                else if (value.Upper() == "DOWN") { b.pullDown = true; b.pullUp = false; }
                            }
                        }
                    }
                    break;
                }
            }
            continue;
        }

        // 非托管行
        unmanagedLines.push_back(line);
    }

    return bindings;
}

// ==================== 串行化 ====================

bool LoadConstraintSheet(const wxString& filePath, ConstraintSheet& sheet, wxString& errorMessage) {
    wxFile file(filePath, wxFile::read);
    if (!file.IsOpened()) {
        errorMessage = wxString("Cannot open pin-bindings.json: ") + filePath;
        return false;
    }

    wxString content;
    file.ReadAll(&content);
    file.Close();

    const wxScopedCharBuffer utf8 = content.ToUTF8();
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    const char* data = utf8.data();
    if (!data || !reader->parse(data, data + utf8.length(), &root, &errors)) {
        errorMessage = wxString("JSON parse error: ") + wxString::FromUTF8(errors);
        return false;
    }

    sheet.targetProfileId = wxString::FromUTF8(root.get("target_profile", "").asString());
    sheet.targetProfileVersion = wxString::FromUTF8(root.get("profile_version", "").asString());
    sheet.topModule = wxString::FromUTF8(root.get("top_module", "").asString());
    sheet.device = wxString::FromUTF8(root.get("device", "").asString());
    sheet.generatedAt = wxString::FromUTF8(root.get("generated", "").asString());

    sheet.bindings.clear();
    const Json::Value& bindingsArr = root["bindings"];
    if (bindingsArr.isArray()) {
        for (const auto& elem : bindingsArr) {
            PinBinding b;
            b.portName = wxString::FromUTF8(elem.get("port", "").asString());
            b.bitIndex = elem.get("bit", 0).asInt();
            b.direction = FpgaPortDirectionFromString(
                wxString::FromUTF8(elem.get("direction", "input").asString()));
            b.packagePin = elem.get("package_pin", -1).asInt();
            b.ioType = wxString::FromUTF8(elem.get("io_type", "LVCMOS33").asString());
            b.drive = wxString::FromUTF8(elem.get("drive", "").asString());
            b.pullUp = elem.get("pull_up", false).asBool();
            b.pullDown = elem.get("pull_down", false).asBool();
            b.comment = wxString::FromUTF8(elem.get("comment", "").asString());
            b.source = wxString::FromUTF8(elem.get("source", "gui").asString());
            sheet.bindings.push_back(b);
        }
    }

    return true;
}

bool SaveConstraintSheet(const wxString& filePath, const ConstraintSheet& sheet, wxString& errorMessage) {
    // 确保目录存在
    wxFileName fn(filePath);
    if (!fn.DirExists()) {
        fn.Mkdir(wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    }

    Json::Value root;
    root["schema_version"] = "1.0";
    root["target_profile"] = sheet.targetProfileId.ToStdString();
    root["profile_version"] = sheet.targetProfileVersion.ToStdString();
    root["top_module"] = sheet.topModule.ToStdString();
    root["device"] = sheet.device.ToStdString();
    root["generated"] = sheet.generatedAt.IsEmpty()
        ? wxDateTime::Now().FormatISOCombined().ToStdString()
        : sheet.generatedAt.ToStdString();

    Json::Value bindingsArr(Json::arrayValue);
    for (const auto& b : sheet.bindings) {
        Json::Value elem;
        elem["port"] = b.portName.ToStdString();
        elem["bit"] = b.bitIndex;
        elem["direction"] = FpgaPortDirectionToString(b.direction).ToStdString();
        elem["package_pin"] = b.packagePin;
        elem["io_type"] = b.ioType.ToStdString();
        elem["drive"] = b.drive.ToStdString();
        elem["pull_up"] = b.pullUp;
        elem["pull_down"] = b.pullDown;
        elem["comment"] = b.comment.ToStdString();
        elem["source"] = b.source.ToStdString();
        bindingsArr.append(elem);
    }
    root["bindings"] = bindingsArr;

    Json::StreamWriterBuilder writerBuilder;
    writerBuilder["indentation"] = "  ";
    std::string jsonStr = Json::writeString(writerBuilder, root);

    wxFile file(filePath, wxFile::write);
    if (!file.IsOpened()) {
        errorMessage = wxString("Cannot write to: ") + filePath;
        return false;
    }
    const char* jsonData = jsonStr.c_str();
    size_t len = jsonStr.length();
    bool ok = file.Write(jsonData, len) == static_cast<wxFileOffset>(len);
    file.Close();
    if (!ok) {
        errorMessage = wxString("Write failed for: ") + filePath;
    }
    return ok;
}

wxString FindYosysJsonPath(const wxString& projectRoot, const wxString& topModule) {
    wxFileName jsonPath(projectRoot + "\\yosys", topModule + ".json");
    jsonPath.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
    if (jsonPath.FileExists()) {
        return jsonPath.GetFullPath();
    }
    return wxString();
}
