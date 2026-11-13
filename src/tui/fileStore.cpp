#include <proj/tui/fileStore.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <proj/tui/utf8.h>

namespace fs = std::filesystem;

namespace tui {

readFileResult fileStore::read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {std::nullopt, "cannot open " + path.string() + ": " + std::strerror(errno)};
    std::ostringstream content;
    content << in.rdbuf();
    if (in.bad()) return {std::nullopt, "cannot read " + path.string()};
    std::string text = std::move(content).str();
    if (!utf8::isValid(text)) return {std::nullopt, path.string() + " is not valid UTF-8"};
    return {std::move(text), {}};
}

std::optional<std::string> fileStore::writeAtomic(const fs::path& path, const std::string& text) {
    const fs::path dir = path.has_parent_path() ? path.parent_path() : fs::path(".");
    std::string tmpl = (dir / ("." + path.filename().string() + ".XXXXXX")).string();
    const int fd = ::mkstemp(tmpl.data());
    if (fd < 0) return "cannot create temporary file in " + dir.string() + ": " + std::strerror(errno);

    auto failWith = [&](const std::string& what) -> std::optional<std::string> {
        const std::string message = what + ": " + std::strerror(errno);
        ::close(fd);
        ::unlink(tmpl.c_str());
        return message;
    };

    // Keep the original file's permissions.
    struct stat original {};
    if (::stat(path.c_str(), &original) == 0) {
        ::fchmod(fd, original.st_mode & 07777);
    } else {
        const mode_t mask = ::umask(0);
        ::umask(mask);
        ::fchmod(fd, 0666 & ~mask);
    }

    std::size_t written = 0;
    while (written < text.size()) {
        const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return failWith("cannot write " + path.string());
        }
        written += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) return failWith("cannot flush " + path.string());
    if (::close(fd) != 0) {
        const std::string message = "cannot close " + path.string() + ": " + std::strerror(errno);
        ::unlink(tmpl.c_str());
        return message;
    }
    if (::rename(tmpl.c_str(), path.c_str()) != 0) {
        const std::string message = "cannot replace " + path.string() + ": " + std::strerror(errno);
        ::unlink(tmpl.c_str());
        return message;
    }
    return std::nullopt;
}

std::vector<fs::path> fileStore::listProject(const fs::path& root) {
    std::vector<fs::path> files;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const bool hidden = !name.empty() && name[0] == '.';
        if (it->is_directory(ec)) {
            if (hidden || name == "build") it.disable_recursion_pending();
            continue;
        }
        if (!hidden && it->is_regular_file(ec)) files.push_back(fs::relative(it->path(), root, ec));
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace tui
