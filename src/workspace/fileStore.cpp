#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <espresso/workspace/fileStore.h>
#include <espresso/workspace/utf8.h>

namespace fs = std::filesystem;

namespace workspace {

namespace {

// The message for @p error, an errno value. std::strerror may share one
// buffer between threads; std::system_category() does not.
std::string errorText(int error) { return std::system_category().message(error); }

// A message for an entry that exists already, as GNOME Builder words it.
std::string existsMessage(const fs::path& path) {
    return "A file or folder named \u201c" + path.filename().string() + "\u201d already exists";
}

}  // namespace

std::optional<std::string> fileStore::modified(const fs::path& path) {
    std::error_code ec;
    const auto time = fs::last_write_time(path, ec);
    if (ec) return std::nullopt;
    // libc++'s file clock counts in a 128-bit integer; nanoseconds in 64 bits hold until 2262.
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch());
    return std::to_string(static_cast<std::int64_t>(ns.count()));
}

std::optional<std::string> fileStore::create(const fs::path& path) {
    std::error_code ec;
    if (fs::exists(fs::symlink_status(path, ec))) return existsMessage(path);
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    if (ec) return "cannot create " + path.parent_path().string() + ": " + ec.message();
    const std::ofstream out(path, std::ios::binary);
    if (!out) {
        const int error = errno;  // before building the message can change it
        return "cannot create " + path.string() + ": " + errorText(error);
    }
    return std::nullopt;
}

std::optional<std::string> fileStore::rename(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(from, ec)))
        return "cannot rename " + from.string() + ": it does not exist";
    if (fs::exists(fs::symlink_status(to, ec))) return existsMessage(to);
    if (to.has_parent_path()) fs::create_directories(to.parent_path(), ec);
    if (ec) return "cannot create " + to.parent_path().string() + ": " + ec.message();
    fs::rename(from, to, ec);
    if (ec) return "cannot rename " + from.string() + " to " + to.string() + ": " + ec.message();
    return std::nullopt;
}

readFileResult fileStore::read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        const int error = errno;  // before building the message can change it
        return {.text = std::nullopt, .error = "cannot open " + path.string() + ": " + errorText(error)};
    }
    std::ostringstream content;
    content << in.rdbuf();
    if (in.bad()) return {.text = std::nullopt, .error = "cannot read " + path.string()};
    std::string text = std::move(content).str();
    if (!utf8::isValid(text)) return {.text = std::nullopt, .error = path.string() + " is not valid UTF-8"};
    return {.text = std::move(text), .error = {}};
}

namespace {

// The process umask. Reading it means setting it, which would race with
// threads creating files.
fs::perms readUmask() noexcept {
    const auto mask = ::umask(0);
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

// The file that a save to @p path replaces: the file at the end of a
// chain of symbolic links, so that the rename replaces it and keeps the
// links. A link's target is relative to the directory holding the link;
// the result is not normalized, because `..` after a linked directory
// means its target's parent.
fs::path linkTarget(const fs::path& path) {
    constexpr int maxLinks = 40;  // the bound Linux puts on a chain of links
    fs::path target = path;
    std::error_code ec;
    for (int links = 0; links < maxLinks && fs::is_symlink(target, ec); ++links) {
        const fs::path next = fs::read_symlink(target, ec);
        if (ec) break;
        target = next.is_absolute() ? next : target.parent_path() / next;
    }
    return target;
}

}  // namespace

// POSIX takes file names as C strings, buffers by pointer, and stat
// results by address: the narrow unsafe use at this boundary.
std::optional<std::string> fileStore::writeAtomic(const fs::path& path, const std::string& text) {
    const auto failure = [&path](std::string_view what) {
        const int error = errno;  // before building the message can change it
        return std::string(what) + " " + path.string() + ": " + errorText(error);
    };
    const fs::path target = linkTarget(path);
    temporaryFile file(target);
    if (!file.opened()) return failure("cannot create a temporary file for");

    // Keep the original file's permissions; a new file gets the default.
    // fs::perms keeps the bit operations unsigned on every platform.
    constexpr auto newFile = fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                             fs::perms::group_write | fs::perms::others_read | fs::perms::others_write;
    struct stat original{};
    const fs::perms mode = ::stat(target.c_str(), &original) == 0
                               ? static_cast<fs::perms>(original.st_mode) & fs::perms::mask
                               : newFile & ~processUmask;
    if (::fchmod(file.fd(), static_cast<decltype(original.st_mode)>(mode)) != 0)
        return failure("cannot set the permissions of");

    for (std::string_view rest = text; !rest.empty();) {
        const auto n = ::write(file.fd(), rest.data(), rest.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return failure("cannot write");
        }
        rest.remove_prefix(static_cast<std::size_t>(n));
    }
    if (::fsync(file.fd()) != 0) return failure("cannot flush");
    if (!file.close()) return failure("cannot close");
    if (!file.commit(target)) return failure("cannot replace");
    return std::nullopt;
}

namespace {

// A directory on the walk from the project root to the current entry.
struct walkStep {
    fs::path resolved;  // with every link resolved: a link back to it is a cycle
    fs::path relative;  // as named in the project, links unresolved
};

}  // namespace

std::vector<fs::path> fileStore::listProject(const fs::path& root, std::size_t limit) {
    std::vector<fs::path> files;
    std::error_code ec;
    std::vector<walkStep> walk{{.resolved = fs::weakly_canonical(root, ec), .relative = {}}};
    if (ec) return files;
    // Linked directories are listed like the directories they name.
    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied | fs::directory_options::follow_directory_symlink,
        ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        // An entry at depth d is inside the root and d directories.
        walk.resize(static_cast<std::size_t>(it.depth()) + 1);
        const fs::path name = it->path().filename();
        const fs::path relative = walk.back().relative / name;
        const bool hidden = name.string().starts_with('.');
        std::error_code entryError;  // a dangling link or an unreadable entry skips only that entry
        if (it->is_directory(entryError)) {
            const fs::path resolved = fs::canonical(it->path(), entryError);
            const bool cycle =
                std::ranges::any_of(walk, [&](const walkStep& s) { return s.resolved == resolved; });
            if (hidden || name == "build" || entryError || cycle) {
                it.disable_recursion_pending();
            } else {
                walk.push_back({.resolved = resolved, .relative = relative});
            }
            continue;
        }
        if (!hidden && it->is_regular_file(entryError)) {
            files.push_back(relative);
            if (files.size() > limit) break;  // one past the limit: the caller knows there are more
        }
    }
    std::ranges::sort(files);
    return files;
}

}  // namespace workspace
