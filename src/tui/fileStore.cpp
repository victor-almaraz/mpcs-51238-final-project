#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <proj/tui/fileStore.h>
#include <proj/tui/utf8.h>

namespace fs = std::filesystem;

namespace tui {

namespace {

// The message for @p error, an errno value. std::strerror may share one
// buffer between threads; std::system_category() does not.
std::string errorText(int error) { return std::system_category().message(error); }

}  // namespace

readFileResult fileStore::read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        const int error = errno;  // before building the message can change it
        return {std::nullopt, "cannot open " + path.string() + ": " + errorText(error)};
    }
    std::ostringstream content;
    content << in.rdbuf();
    if (in.bad()) return {std::nullopt, "cannot read " + path.string()};
    std::string text = std::move(content).str();
    if (!utf8::isValid(text)) return {std::nullopt, path.string() + " is not valid UTF-8"};
    return {std::move(text), {}};
}

namespace {

// The process umask. Reading it means setting it, which would race with
// threads creating files.
fs::perms readUmask() noexcept {
    const mode_t mask = ::umask(0);
    ::umask(mask);
    return static_cast<fs::perms>(mask);
}

// Read once at static initialization, before any thread starts.
const fs::perms processUmask = readUmask();

// A temporary file beside its destination. Owns the descriptor, and
// removes the file unless commit() renamed it into place.
class temporaryFile {
public:
    explicit temporaryFile(const fs::path& destination)
        : name_((destination.has_parent_path() ? destination.parent_path() : fs::path(".")) /
                ("." + destination.filename().string() + ".XXXXXX")),
          text_(name_.string()),
          fd_(::mkstemp(text_.data())) {}  // mkstemp fills in the name

    temporaryFile(const temporaryFile&) = delete;
    temporaryFile& operator=(const temporaryFile&) = delete;

    ~temporaryFile() {
        if (fd_ >= 0) ::close(fd_);
        if (fd_ != closedAndRenamed) ::unlink(text_.c_str());
    }

    bool opened() const noexcept { return fd_ >= 0; }
    int fd() const noexcept { return fd_; }

    // Closes the descriptor; false (with errno set) if close fails.
    bool close() noexcept { return ::close(std::exchange(fd_, closed)) == 0; }

    // Renames the closed file to @p destination.
    bool commit(const fs::path& destination) noexcept {
        if (::rename(text_.c_str(), destination.c_str()) != 0) return false;
        fd_ = closedAndRenamed;
        return true;
    }

private:
    static constexpr int closed = -1;
    static constexpr int closedAndRenamed = -2;

    fs::path name_;
    std::string text_;  // name_ as the writable C string mkstemp needs
    int fd_;
};

}  // namespace

// POSIX takes file names as C strings, buffers by pointer, and stat
// results by address: the narrow unsafe use at this boundary.
std::optional<std::string> fileStore::writeAtomic(const fs::path& path, const std::string& text) {
    const auto failure = [&path](std::string_view what) {
        const int error = errno;  // before building the message can change it
        return std::string(what) + " " + path.string() + ": " + errorText(error);
    };
    temporaryFile file(path);
    if (!file.opened()) return failure("cannot create a temporary file for");

    // Keep the original file's permissions; a new file gets the default.
    // fs::perms keeps the bit operations unsigned on every platform.
    constexpr auto newFile = fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                             fs::perms::group_write | fs::perms::others_read | fs::perms::others_write;
    struct stat original {};
    const fs::perms mode = ::stat(path.c_str(), &original) == 0
                               ? static_cast<fs::perms>(original.st_mode) & fs::perms::mask
                               : newFile & ~processUmask;
    if (::fchmod(file.fd(), static_cast<mode_t>(mode)) != 0) return failure("cannot set the permissions of");

    for (std::string_view rest = text; !rest.empty();) {
        const ssize_t n = ::write(file.fd(), rest.data(), rest.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return failure("cannot write");
        }
        rest.remove_prefix(static_cast<std::size_t>(n));
    }
    if (::fsync(file.fd()) != 0) return failure("cannot flush");
    if (!file.close()) return failure("cannot close");
    if (!file.commit(path)) return failure("cannot replace");
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
