#include "JobPaths.h"
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <stdexcept>
namespace fs = std::filesystem;
namespace eda::web {
fs::path CheckedPath(const fs::path& root, const fs::path& path, bool mustExist) {
    auto base = fs::absolute(root).lexically_normal();
    auto target = path.is_absolute() ? path : base / path;
    for (const auto& part : target)
        if (part == "..")
            throw std::invalid_argument("parent traversal is forbidden");
    target = target.lexically_normal();
    auto b = base.begin(), t = target.begin();
    for (; b != base.end(); ++b, ++t)
        if (t == target.end() || *b != *t)
            throw std::invalid_argument("path escapes authorized root");
    fs::path current;
    for (const auto& part : target) {
        current /= part;
        std::error_code ec;
        auto status = fs::symlink_status(current, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            throw fs::filesystem_error("inspect path", current, ec);
        if (fs::is_symlink(status))
            throw std::invalid_argument("symlink path is forbidden");
    }
    if (mustExist && !fs::exists(target))
        throw std::invalid_argument("input path does not exist");
    return target;
}
void AtomicJson(const fs::path& root, const fs::path& relative, const Json& value) {
    auto target = CheckedPath(root, relative, false);
    fs::create_directories(target.parent_path());
    auto temporary = CheckedPath(root, relative.string() + ".tmp", false);
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        throw std::runtime_error("cannot create atomic JSON temporary file");
    try {
        auto data = value.dump(2) + "\n";
        size_t offset = 0;
        while (offset < data.size()) {
            auto n = write(fd, data.data() + offset, data.size() - offset);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                throw std::runtime_error("JSON write failed");
            offset += n;
        }
        if (fsync(fd) != 0)
            throw std::runtime_error("JSON fsync failed");
        close(fd);
        fd = -1;
        fs::rename(temporary, target);
        int directory = open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0)
            throw std::runtime_error("cannot open JSON parent directory");
        int status = fsync(directory);
        close(directory);
        if (status != 0)
            throw std::runtime_error("JSON parent fsync failed");
    } catch (...) {
        if (fd >= 0)
            close(fd);
        std::error_code ec;
        fs::remove(temporary, ec);
        throw;
    }
}
}  // namespace eda::web
