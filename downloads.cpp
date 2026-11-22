// Background download monitor for the Omarchy bar.
//
// Watches known downloader processes (package managers, game launchers,
// umu/Proton, curl, git, ...) and prints one JSON snapshot per line on stdout
// whenever the picture changes. Everything is read locally from /proc and the
// file system: no network access, no root, no dependencies beyond the C++
// standard library. run.sh compiles it on first use.
//
// Usage:
//   downloads [--interval SECONDS] [--watch name1,name2] [--once] [--demo] [--self-test]

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <optional>
#include <pwd.h>
#include <set>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr int kProtocolVersion = 1;
constexpr size_t kMaxJobs = 20;
constexpr size_t kMaxText = 160;

// A job counts as downloading when its files grow this fast...
constexpr double kMinRate = 96 * 1024;
// ...or when it writes partial-download files at all.
constexpr double kMinPartialRate = 1024;
// Fallback for tools that rewrite preallocated files or open them only in
// bursts (Battle.net, some launchers): disk writes of the process tree plus
// network traffic, both at least this fast.
constexpr double kMinIoRate = 512 * 1024;
constexpr double kMinIoRx = 512 * 1024;
// Package managers whose files we cannot read: network rate at least this.
constexpr double kMinUnreadableRx = 256 * 1024;
// Linger after the last growth before hiding (seconds).
constexpr double kLinger = 4.0;
// How long after a download a pure disk-write phase (extracting) keeps showing.
constexpr double kWriteAfterDownload = 30.0;
constexpr double kEma = 0.45;

// comm name (as in /proc/<pid>/comm, max 15 chars) -> kind
const std::map<std::string, std::string> kWatched = {
    {"pacman", "pacman"}, {"yay", "aur"}, {"paru", "aur"}, {"pamac", "aur"},
    {"makepkg", "aur"}, {"flatpak", "flatpak"}, {"umu-run", "umu"}, {"lutris", "lutris"},
    {"heroic", "heroic"}, {"steam", "steam"}, {"curl", "curl"}, {"wget", "wget"},
    {"wget2", "wget"}, {"aria2c", "aria2"}, {"git", "git"}, {"git-remote-http", "git"},
    {"yt-dlp", "ytdlp"}, {"pip", "pip"}, {"pip3", "pip"}, {"uv", "pip"}, {"npm", "npm"},
    {"pnpm", "npm"}, {"bun", "npm"}, {"cargo", "cargo"}, {"rustup", "cargo"}, {"mise", "mise"},
};

const std::map<std::string, std::string> kKindTitles = {
    {"pacman", "pacman"}, {"aur", "AUR"}, {"flatpak", "Flatpak"}, {"umu", "Proton (umu)"},
    {"lutris", "Lutris"}, {"heroic", "Heroic"}, {"steam", "Steam"}, {"curl", "curl"},
    {"wget", "wget"}, {"aria2", "aria2"}, {"git", "git"}, {"ytdlp", "yt-dlp"},
    {"pip", "Python"}, {"npm", "Node.js"}, {"cargo", "Rust"}, {"mise", "mise"},
};

// umu GAMEID (without the "umu-" prefix) -> display name
const std::map<std::string, std::string> kKnownGameIds = {
    {"battlenet", "Battle.net"}, {"eaapp", "EA app"}, {"ubisoftconnect", "Ubisoft Connect"},
    {"epicgameslauncher", "Epic Games"}, {"gog", "GOG Galaxy"},
};

const std::vector<std::string> kPartialSuffixes = {
    ".part", ".parts", ".partial", ".download", ".crdownload", ".aria2", ".tmp", ".!qb", ".incomplete"};
const std::vector<std::string> kPartialMarkers = {"tmp_pack_", "/downloading/", "/download-"};
const std::vector<std::string> kIgnoredPrefixes = {"/dev/", "/proc/", "/sys/", "/run/", "/memfd:", "/tmp/.X"};
const std::vector<std::string> kIgnoredSuffixes = {
    ".log", ".lock", ".lck", "-journal", "-wal", "-shm", ".sqlite", ".db", ".reg", ".pid"};

// ------------------------------------------------------------------ helpers

using Opt = std::optional<double>;

double ema(Opt old, double value) { return old ? *old + (value - *old) * kEma : value; }

bool starts_with(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

bool ends_with(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

bool contains(const std::string& s, const std::string& p) { return s.find(p) != std::string::npos; }

double now_seconds() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Printable text only, at most kMaxText code points (UTF-8 aware).
std::string clip(const std::string& in, size_t limit = kMaxText) {
  std::string text;
  size_t points = 0;
  for (unsigned char c : in) {
    if (c < 0x20 || c == 0x7f) continue;
    if ((c & 0xC0) != 0x80) points++;
    text += static_cast<char>(c);
  }
  if (points <= limit) return text;
  std::string out;
  points = 0;
  for (unsigned char c : text) {
    if ((c & 0xC0) != 0x80 && ++points == limit) break;
    out += static_cast<char>(c);
  }
  return out + "\xE2\x80\xA6";
}

std::optional<std::string> read_file(const std::string& path, size_t limit = 65536) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::string data;
  char buf[8192];
  while (data.size() < limit) {
    ssize_t n = read(fd, buf, std::min(sizeof buf, limit - data.size()));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    data.append(buf, static_cast<size_t>(n));
  }
  close(fd);
  return data;
}

std::optional<std::string> read_link(const std::string& path) {
  char buf[PATH_MAX];
  ssize_t n = readlink(path.c_str(), buf, sizeof buf - 1);
  if (n < 0) return std::nullopt;
  return std::string(buf, static_cast<size_t>(n));
}

std::vector<std::string> list_dir(const std::string& path, bool* ok = nullptr) {
  std::vector<std::string> names;
  DIR* dir = opendir(path.c_str());
  if (ok) *ok = dir != nullptr;
  if (!dir) return names;
  while (dirent* e = readdir(dir)) {
    if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
    names.emplace_back(e->d_name);
  }
  closedir(dir);
  return names;
}

bool all_digits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}

std::string basename_of(const std::string& path) {
  size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

std::string json_str(const std::string& s) { return "\"" + json_escape(s) + "\""; }

std::string json_num(double v) { return std::to_string(static_cast<long long>(std::llround(std::max(0.0, v)))); }

std::string json_opt(std::optional<uint64_t> v) { return v ? std::to_string(*v) : "null"; }

// ------------------------------------------------------------------ /proc

struct Proc {
  std::string comm;
  int ppid = 0;
  uid_t uid = 0;
};

std::unordered_map<int, Proc> process_table() {
  std::unordered_map<int, Proc> table;
  for (const auto& name : list_dir("/proc")) {
    if (!all_digits(name)) continue;
    int pid = std::atoi(name.c_str());
    auto stat_text = read_file("/proc/" + name + "/stat", 1024);
    if (!stat_text) continue;
    size_t lpar = stat_text->find('('), rpar = stat_text->rfind(')');
    if (lpar == std::string::npos || rpar == std::string::npos || rpar + 2 >= stat_text->size()) continue;
    Proc p;
    p.comm = stat_text->substr(lpar + 1, rpar - lpar - 1);
    // after ") " come: state ppid ...
    const char* rest = stat_text->c_str() + rpar + 2;
    char state = 0;
    if (sscanf(rest, "%c %d", &state, &p.ppid) != 2) continue;
    struct stat st{};
    if (stat(("/proc/" + name).c_str(), &st) != 0) continue;
    p.uid = st.st_uid;
    table.emplace(pid, std::move(p));
  }
  return table;
}

std::optional<std::string> environ_value(int pid, const std::string& key) {
  auto raw = read_file("/proc/" + std::to_string(pid) + "/environ", 262144);
  if (!raw) return std::nullopt;
  std::string prefix = key + "=";
  size_t start = 0;
  while (start < raw->size()) {
    size_t end = raw->find('\0', start);
    if (end == std::string::npos) end = raw->size();
    if (raw->compare(start, prefix.size(), prefix) == 0) return raw->substr(start + prefix.size(), end - start - prefix.size());
    start = end + 1;
  }
  return std::nullopt;
}

uint64_t net_rx_bytes() {
  auto raw = read_file("/proc/net/dev");
  if (!raw) return 0;
  uint64_t total = 0;
  size_t pos = 0;
  int line_no = 0;
  while (pos < raw->size()) {
    size_t end = raw->find('\n', pos);
    if (end == std::string::npos) end = raw->size();
    std::string line = raw->substr(pos, end - pos);
    pos = end + 1;
    if (line_no++ < 2) continue;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string iface = line.substr(0, colon);
    iface.erase(0, iface.find_first_not_of(' '));
    if (iface == "lo" || starts_with(iface, "veth") || starts_with(iface, "docker") || starts_with(iface, "virbr") ||
        starts_with(iface, "br-"))
      continue;
    total += std::strtoull(line.c_str() + colon + 1, nullptr, 10);
  }
  return total;
}

std::optional<uint64_t> write_bytes(int pid) {
  auto raw = read_file("/proc/" + std::to_string(pid) + "/io", 1024);
  if (!raw) return std::nullopt;
  size_t at = raw->find("\nwrite_bytes:");
  if (at == std::string::npos) return std::nullopt;
  return std::strtoull(raw->c_str() + at + 13, nullptr, 10);
}

bool is_partial(const std::string& path) {
  for (const auto& s : kPartialSuffixes)
    if (ends_with(path, s)) return true;
  for (const auto& m : kPartialMarkers)
    if (contains(path, m)) return true;
  return false;
}

bool interesting_path(const std::string& path) {
  if (path.empty() || path[0] != '/' || ends_with(path, " (deleted)")) return false;
  for (const auto& p : kIgnoredPrefixes)
    if (starts_with(path, p)) return false;
  for (const auto& s : kIgnoredSuffixes)
    if (ends_with(path, s)) return false;
  return true;
}

bool fd_writable(int pid, const std::string& fd) {
  auto raw = read_file("/proc/" + std::to_string(pid) + "/fdinfo/" + fd, 512);
  if (!raw) return false;
  size_t at = raw->find("flags:");
  if (at == std::string::npos) return false;
  unsigned long flags = std::strtoul(raw->c_str() + at + 6, nullptr, 8);
  unsigned long mode = flags & 3;
  return mode == 1 || mode == 2;
}

std::string strip_partial(const std::string& name) {
  for (const auto& s : kPartialSuffixes)
    if (ends_with(name, s)) return name.substr(0, name.size() - s.size());
  return name;
}

// ------------------------------------------------------------------ totals

uint64_t read_uint(const unsigned char* p, int bytes, bool big_endian) {
  uint64_t v = 0;
  for (int i = 0; i < bytes; i++) {
    int idx = big_endian ? i : bytes - 1 - i;
    v = (v << 8) | p[idx];
  }
  return v;
}

// Total length from an aria2 control file next to the download.
std::optional<uint64_t> aria2_total(const std::string& path) {
  std::string control = ends_with(path, ".aria2") ? path : path + ".aria2";
  auto head = read_file(control, 4096);
  if (!head || head->size() < 10) return std::nullopt;
  auto* p = reinterpret_cast<const unsigned char*>(head->data());
  bool big = read_uint(p, 2, true) == 1;
  uint64_t hash_len = read_uint(p + 6, 4, big);
  uint64_t offset = 10 + hash_len + 4;
  if (offset + 8 > head->size()) return std::nullopt;
  uint64_t total = read_uint(p + offset, 8, big);
  if (total == 0 || total >= (1ULL << 50)) return std::nullopt;
  return total;
}

// ------------------------------------------------------------------ Steam

// "key" "value" pairs of a Valve KeyValues file, in order.
std::vector<std::pair<std::string, std::string>> vdf_pairs(const std::string& text) {
  std::vector<std::pair<std::string, std::string>> out;
  std::vector<std::string> tokens;
  size_t i = 0;
  auto flush_line = [&]() {
    if (tokens.size() == 2) out.emplace_back(tokens[0], tokens[1]);
    tokens.clear();
  };
  while (i < text.size()) {
    char c = text[i];
    if (c == '\n' || c == '{' || c == '}') {
      flush_line();
      i++;
    } else if (c == '"') {
      size_t end = text.find('"', i + 1);
      if (end == std::string::npos) break;
      std::string tok = text.substr(i + 1, end - i - 1);
      if (tok.find('\\') != std::string::npos) tok.clear();
      tokens.push_back(tok);
      i = end + 1;
    } else {
      i++;
    }
  }
  flush_line();
  return out;
}

struct Job {
  std::string id, kind, title, detail, state;
  std::optional<uint64_t> done, total;
  double rate = 0;
  bool approx = false;
  size_t files = 0;
};

class SteamCollector {
 public:
  std::vector<std::string> libraries;
  double libraries_at = -1e9;

  std::vector<Job> collect(double now) {
    std::vector<Job> jobs;
    std::set<std::string> seen;
    for (const auto& apps : library_paths(now)) {
      for (const auto& name : list_dir(apps)) {
        if (!starts_with(name, "appmanifest_") || !ends_with(name, ".acf")) continue;
        auto text = read_file(apps + "/" + name, 1 << 16);
        if (!text) continue;
        std::map<std::string, std::string> data;
        for (auto& kv : vdf_pairs(*text)) data.emplace(kv.first, kv.second);
        uint64_t todo = std::strtoull(data["BytesToDownload"].c_str(), nullptr, 10);
        uint64_t done = std::strtoull(data["BytesDownloaded"].c_str(), nullptr, 10);
        std::string appid = data["appid"].empty() ? name.substr(12, name.size() - 16) : data["appid"];
        if (todo == 0 || done >= todo) {
          apps_.erase(appid);
          continue;
        }
        seen.insert(appid);
        auto it = apps_.find(appid);
        if (it == apps_.end()) it = apps_.emplace(appid, State{done, now, std::nullopt, 0.0}).first;
        State& st = it->second;
        double dt = now - st.t;
        if (done != st.done && dt > 0) {
          double grown = done > st.done ? static_cast<double>(done - st.done) : 0.0;
          st.rate = ema(st.rate, grown / dt);
          st.grew = now;
          st.done = done;
          st.t = now;
        } else if (dt > 15) {
          st.rate = 0.0;
        }
        if (now - st.grew > 20) continue;  // paused or queued
        Job job;
        job.id = "steam:" + clip(appid, 32);
        job.kind = "steam";
        job.title = "Steam";
        job.detail = clip(data["name"].empty() ? appid : data["name"]);
        job.state = "downloading";
        job.done = done;
        job.total = todo;
        job.rate = st.rate.value_or(0);
        jobs.push_back(std::move(job));
      }
    }
    for (auto it = apps_.begin(); it != apps_.end();) it = seen.count(it->first) ? std::next(it) : apps_.erase(it);
    return jobs;
  }

 private:
  struct State {
    uint64_t done;
    double t;
    Opt rate;
    double grew;
  };
  std::map<std::string, State> apps_;

  std::vector<std::string> library_paths(double now) {
    if (now - libraries_at < 30 && !libraries.empty()) return libraries;
    const char* home = getenv("HOME");
    if (!home) return libraries;
    std::vector<std::string> paths;
    for (const char* rel : {"/.local/share/Steam", "/.steam/steam", "/.var/app/com.valvesoftware.Steam/.local/share/Steam"}) {
      std::string root = std::string(home) + rel;
      auto text = read_file(root + "/steamapps/libraryfolders.vdf", 1 << 20);
      if (!text) continue;
      std::vector<std::string> found;
      for (auto& kv : vdf_pairs(*text))
        if (kv.first == "path") found.push_back(kv.second);
      if (found.empty()) found.push_back(root);
      for (const auto& path : found) {
        std::string apps = path + "/steamapps";
        struct stat st{};
        if (stat(apps.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
            std::find(paths.begin(), paths.end(), apps) == paths.end())
          paths.push_back(apps);
      }
    }
    libraries = paths;
    libraries_at = now;
    return libraries;
  }
};

// ------------------------------------------------------------------ monitor

struct FileInfo {
  uint64_t size = 0;
  double t = 0, grew = 0, seen = 0;
  Opt rate;
  bool partial = false;
};

struct JobState {
  std::string kind, title, state, detail;
  double grew = 0, downloaded = 0;
  Opt rate, io_rate, visible_since;
  std::map<int, uint64_t> io;
  std::optional<uint64_t> done;
  bool approx = false;
  double shown_rate = 0;
};

struct Snapshot {
  double rx = 0;
  std::vector<Job> jobs;
  std::vector<std::string> finished;
};

class Monitor {
 public:
  explicit Monitor(const std::vector<std::string>& extra) : watched_(kWatched) {
    for (auto name : extra) {
      if (name.size() > 15) name.resize(15);
      if (!name.empty()) watched_.emplace(name, "other");
    }
    uid_ = getuid();
    if (passwd* pw = getpwnam("alpm")) alpm_uid_ = pw->pw_uid;
  }

  Snapshot tick(double now) {
    std::optional<double> dt;
    if (last_t_) dt = std::max(1e-3, now - *last_t_);
    uint64_t rx = net_rx_bytes();
    if (last_rx_ && dt) rx_rate_ = ema(rx_rate_, (rx > *last_rx_ ? rx - *last_rx_ : 0) / *dt);
    last_rx_ = rx;
    last_t_ = now;

    auto table = process_table();
    auto groups = group(table);
    for (auto it = fd_cache_.begin(); it != fd_cache_.end();)
      it = table.count(it->first.first) ? std::next(it) : fd_cache_.erase(it);

    Snapshot snap;
    bool steam_running = false;
    double rx_rate = rx_rate_.value_or(0);

    for (auto& [root, members] : groups) {
      std::string kind = kind_of(table.at(root).comm);
      if (kind == "steam") {
        steam_running = true;
        continue;  // Steam is read from its app manifests instead
      }
      auto jit = jobs_.find(root);
      if (jit == jobs_.end()) {
        JobState fresh;
        fresh.kind = kind;
        fresh.title = title_for(root, kind, table);
        jit = jobs_.emplace(root, std::move(fresh)).first;
      }
      JobState& job = jit->second;

      std::vector<std::pair<std::string, FileInfo*>> files;
      uint64_t growth = 0, io_growth = 0;
      bool unreadable = false, alpm = false;
      std::map<int, uint64_t> io_now;
      for (int pid : members) {
        const Proc& p = table.at(pid);
        if (p.comm == "steam") steam_running = true;
        if (alpm_uid_ && p.uid == *alpm_uid_) alpm = true;
        if (p.uid != uid_) {
          unreadable = true;
          continue;
        }
        if (auto written = write_bytes(pid)) {
          io_now[pid] = *written;
          auto prev = job.io.find(pid);
          if (prev != job.io.end() && *written > prev->second) io_growth += *written - prev->second;
        }
        bool ok = false;
        auto paths = open_files(pid, &ok);
        if (!ok) {
          unreadable = true;
          continue;
        }
        for (const auto& path : paths) {
          uint64_t grown = 0;
          if (FileInfo* info = sample_file(path, now, &grown)) {
            files.emplace_back(path, info);
            growth += grown;
          }
        }
      }
      if (dt) {
        job.rate = ema(job.rate, growth / *dt);
        job.io_rate = ema(job.io_rate, io_growth / *dt);
      }
      job.io = std::move(io_now);
      double rate = job.rate.value_or(0);
      double io_rate = job.io_rate.value_or(0);
      bool partial_growing = std::any_of(files.begin(), files.end(), [&](const auto& f) {
        return f.second->partial && now - f.second->grew < 2.5 && f.second->rate.value_or(0) >= kMinPartialRate;
      });
      bool networked = rx_rate >= 0.3 * rate;

      std::string state;
      bool approx = false;
      if (alpm) {
        state = "downloading", approx = true, rate = rx_rate;
      } else if (rate >= kMinRate || partial_growing) {
        if (networked || partial_growing)
          state = "downloading";
        else if (now - job.downloaded < kWriteAfterDownload)
          state = "writing";
      } else if (unreadable && rx_rate >= kMinUnreadableRx && (kind == "pacman" || kind == "aur" || kind == "flatpak")) {
        state = "downloading", approx = true, rate = rx_rate;
      } else if (io_rate >= kMinIoRate && rx_rate >= kMinIoRx) {
        state = "downloading", approx = true, rate = rx_rate;
      }

      if (!state.empty()) {
        job.grew = now;
        job.state = state;
        job.approx = approx;
        job.shown_rate = rate;
        if (state == "downloading") job.downloaded = now;
      } else if (now - job.grew > kLinger) {
        finish(job, snap);
        jobs_.erase(jit);
        continue;
      }
      if (job.state.empty()) continue;
      if (!job.visible_since) job.visible_since = now;

      std::vector<std::pair<std::string, FileInfo*>> growing;
      for (auto& f : files)
        if (now - f.second->grew < kLinger) growing.push_back(f);
      std::sort(growing.begin(), growing.end(),
                [](const auto& a, const auto& b) { return a.second->rate.value_or(0) > b.second->rate.value_or(0); });
      std::optional<uint64_t> total;
      if (!growing.empty()) {
        job.detail = clip(strip_partial(basename_of(growing[0].first)));
        uint64_t sum = 0;
        for (auto& f : growing) sum += f.second->size;
        job.done = sum;
        if (growing.size() == 1) total = aria2_total(growing[0].first);
      }
      // The last file seen stays: some tools open it only in bursts.
      Job out;
      out.id = "pid:" + std::to_string(root);
      out.kind = job.kind;
      out.title = job.title;
      out.detail = job.detail;
      out.state = job.state;
      out.done = job.done;
      out.total = total;
      out.rate = job.shown_rate;
      out.approx = job.approx;
      out.files = growing.size();
      snap.jobs.push_back(std::move(out));
    }

    for (auto it = jobs_.begin(); it != jobs_.end();) {
      if (groups.count(it->first)) {
        ++it;
      } else {
        finish(it->second, snap);
        it = jobs_.erase(it);
      }
    }

    if (steam_running)
      for (auto& job : steam_.collect(now)) snap.jobs.push_back(std::move(job));

    // forget files nobody holds any more
    for (auto it = files_.begin(); it != files_.end();) it = now - it->second.seen < 10 ? std::next(it) : files_.erase(it);

    std::sort(snap.jobs.begin(), snap.jobs.end(), [](const Job& a, const Job& b) { return a.id < b.id; });
    if (snap.jobs.size() > kMaxJobs) snap.jobs.resize(kMaxJobs);
    if (snap.finished.size() > kMaxJobs) snap.finished.resize(kMaxJobs);
    snap.rx = rx_rate;
    return snap;
  }

 private:
  std::map<std::string, std::string> watched_;
  uid_t uid_;
  std::optional<uid_t> alpm_uid_;
  std::map<std::pair<int, std::string>, std::pair<std::string, bool>> fd_cache_;
  std::unordered_map<std::string, FileInfo> files_;
  std::map<int, JobState> jobs_;
  SteamCollector steam_;
  std::optional<uint64_t> last_rx_;
  Opt rx_rate_;
  std::optional<double> last_t_;

  std::string kind_of(const std::string& comm) const {
    auto it = watched_.find(comm);
    return it == watched_.end() ? "other" : it->second;
  }

  std::map<int, std::vector<int>> group(const std::unordered_map<int, Proc>& table) const {
    std::unordered_map<int, std::vector<int>> children;
    for (const auto& [pid, p] : table) children[p.ppid].push_back(pid);
    auto watched = [&](int pid) { return watched_.count(table.at(pid).comm) > 0; };

    std::set<int> roots;
    for (const auto& [pid, p] : table) {
      if (!watched(pid)) continue;
      int root = pid, cur = p.ppid;
      for (int hops = 0; table.count(cur) && hops < 32; hops++) {
        if (watched(cur)) root = cur;
        cur = table.at(cur).ppid;
      }
      roots.insert(root);
    }

    std::map<int, std::vector<int>> groups;
    for (int root : roots) {
      std::vector<int> members, stack{root};
      while (!stack.empty() && members.size() < 4096) {
        int pid = stack.back();
        stack.pop_back();
        members.push_back(pid);
        auto it = children.find(pid);
        if (it != children.end()) stack.insert(stack.end(), it->second.begin(), it->second.end());
      }
      groups.emplace(root, std::move(members));
    }
    return groups;
  }

  std::vector<std::string> open_files(int pid, bool* ok) {
    std::vector<std::string> out;
    std::string base = "/proc/" + std::to_string(pid) + "/fd";
    auto fds = list_dir(base, ok);
    if (!*ok) return out;
    for (const auto& fd : fds) {
      auto path = read_link(base + "/" + fd);
      if (!path) continue;
      auto key = std::make_pair(pid, fd);
      auto it = fd_cache_.find(key);
      if (it == fd_cache_.end() || it->second.first != *path) {
        bool writable = interesting_path(*path) && fd_writable(pid, fd);
        it = fd_cache_.insert_or_assign(key, std::make_pair(*path, writable)).first;
      }
      if (it->second.second) out.push_back(*path);
    }
    return out;
  }

  FileInfo* sample_file(const std::string& path, double now, uint64_t* growth) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return nullptr;
    uint64_t size = static_cast<uint64_t>(st.st_size);
    auto it = files_.find(path);
    if (it == files_.end()) {
      FileInfo info;
      info.size = size;
      info.t = now;
      info.seen = now;
      info.partial = is_partial(path);
      *growth = 0;
      return &files_.emplace(path, info).first->second;
    }
    FileInfo& info = it->second;
    uint64_t grown = size > info.size ? size - info.size : 0;
    double dt = std::max(1e-3, now - info.t);
    info.rate = ema(info.rate, grown / dt);
    if (grown) info.grew = now;
    info.size = size;
    info.t = now;
    info.seen = now;
    *growth = grown;
    return &info;
  }

  static void finish(const JobState& job, Snapshot& snap) {
    if (job.visible_since && job.downloaded > 0 && job.downloaded - *job.visible_since >= 3)
      snap.finished.push_back(job.title);
  }

  static std::string title_for(int root, const std::string& kind, const std::unordered_map<int, Proc>& table) {
    if (kind == "umu") {
      std::string game = environ_value(root, "GAMEID").value_or("");
      if (starts_with(game, "umu-")) game = game.substr(4);
      if (!game.empty() && !all_digits(game) && game != "default") {
        std::string lower = game;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        auto known = kKnownGameIds.find(lower);
        std::string name = known != kKnownGameIds.end() ? known->second : game;
        if (known == kKnownGameIds.end()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        return clip(name + " (Proton)");
      }
    }
    if (kind == "other") return clip(table.at(root).comm);
    auto it = kKindTitles.find(kind);
    return it == kKindTitles.end() ? kind : it->second;
  }
};

// ------------------------------------------------------------------ output

std::string to_json(const Snapshot& snap) {
  std::string out = "{\"v\":" + std::to_string(kProtocolVersion) + ",\"rx\":" + json_num(snap.rx) + ",\"jobs\":[";
  for (size_t i = 0; i < snap.jobs.size(); i++) {
    const Job& j = snap.jobs[i];
    if (i) out += ",";
    out += "{\"id\":" + json_str(j.id) + ",\"kind\":" + json_str(j.kind) + ",\"title\":" + json_str(j.title) +
           ",\"detail\":" + json_str(j.detail) + ",\"state\":" + json_str(j.state) + ",\"done\":" + json_opt(j.done) +
           ",\"total\":" + json_opt(j.total) + ",\"rate\":" + json_num(j.rate) +
           ",\"approx\":" + (j.approx ? "true" : "false") + ",\"files\":" + std::to_string(j.files) + "}";
  }
  out += "],\"finished\":[";
  for (size_t i = 0; i < snap.finished.size(); i++) out += (i ? "," : "") + json_str(snap.finished[i]);
  return out + "]}";
}

Snapshot demo_snapshot(double now) {
  double phase = std::fmod(now, 40.0);
  Snapshot s;
  s.rx = 33500000;
  Job a;
  a.id = "pid:1", a.kind = "umu", a.title = "Battle.net (Proton)", a.detail = "GE-Proton11-12-x86_64.tar.gz";
  a.state = "downloading", a.done = static_cast<uint64_t>(phase * 2.1e6), a.rate = 2100000, a.files = 1;
  Job b;
  b.id = "steam:570", b.kind = "steam", b.title = "Steam", b.detail = "Dota 2", b.state = "downloading";
  b.done = static_cast<uint64_t>(9e9 + phase * 2.5e7), b.total = 41000000000ULL, b.rate = 25000000;
  s.jobs = {a, b};
  if (phase > 20) {
    Job c;
    c.id = "pid:2", c.kind = "pacman", c.title = "pacman", c.state = "downloading", c.rate = 6400000, c.approx = true;
    s.jobs.push_back(c);
  }
  return s;
}

// Only one helper announces finished downloads. The shell runs one widget per
// monitor, so several helpers may be alive at once; the one holding this lock
// owns the "finished" events.
class NotifyLock {
 public:
  bool held() {
    if (fd_ >= 0) return true;
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    std::string dir = runtime && *runtime ? runtime : "/tmp";
    std::string path = dir + "/omarchy-downloads-monitor-" + std::to_string(getuid()) + ".lock";
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
      close(fd);
      return false;
    }
    fd_ = fd;
    return true;
  }

 private:
  int fd_ = -1;
};

// ------------------------------------------------------------------ self test

int self_test() {
  int failures = 0;
  auto check = [&](bool ok, const char* what) {
    if (!ok) {
      fprintf(stderr, "FAIL: %s\n", what);
      failures++;
    }
  };
  check(clip("a\x1b[31mb") == "a[31mb", "clip drops control characters");
  check(clip(std::string(500, 'x')) == std::string(kMaxText - 1, 'x') + "\xE2\x80\xA6", "clip bounds length");
  check(clip("Загрузка") == "Загрузка", "clip keeps UTF-8");
  check(is_partial("/home/u/.cache/x.tar.gz.part"), "partial .part");
  check(is_partial("/tmp/tmpab/GE-Proton.tar.gz.parts"), "partial .parts");
  check(is_partial("/r/.git/objects/pack/tmp_pack_AbC"), "partial git pack");
  check(!is_partial("/home/u/game/data.bin"), "regular file is not partial");
  check(!interesting_path("/dev/null"), "ignore /dev");
  check(!interesting_path("socket:[123]"), "ignore sockets");
  check(!interesting_path("/home/u/app.log"), "ignore logs");
  check(!interesting_path("/home/u/x (deleted)"), "ignore deleted files");
  check(interesting_path("/home/u/x.part"), "keep partial file");
  check(strip_partial("a.tar.gz.part") == "a.tar.gz", "strip .part");
  check(strip_partial("a.tar.gz") == "a.tar.gz", "keep plain name");
  check(json_escape("a\"b\\c\n") == "a\\\"b\\\\c\\u000a", "json escaping");
  auto pairs = vdf_pairs("\"AppState\"\n{\n\t\"appid\"\t\t\"570\"\n\t\"name\"\t\t\"Dota 2\"\n}");
  check(pairs.size() == 2 && pairs[0].second == "570" && pairs[1].second == "Dota 2", "vdf pairs");

  char dir[] = "/tmp/downloads-selftest-XXXXXX";
  if (mkdtemp(dir)) {
    std::string target = std::string(dir) + "/file.iso";
    unsigned char body[64] = {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10, 0, 0};
    uint64_t total = 123456789;
    for (int i = 0; i < 8; i++) body[14 + i] = static_cast<unsigned char>(total >> (56 - 8 * i));
    if (FILE* f = fopen((target + ".aria2").c_str(), "wb")) {
      fwrite(body, 1, sizeof body, f);
      fclose(f);
    }
    check(aria2_total(target) == std::optional<uint64_t>(total), "aria2 total");
    check(!aria2_total(std::string(dir) + "/missing"), "aria2 missing control file");
    unlink((target + ".aria2").c_str());

    std::string apps = std::string(dir) + "/steamapps";
    mkdir(apps.c_str(), 0700);
    std::string manifest = apps + "/appmanifest_570.acf";
    auto write_manifest = [&](int done) {
      if (FILE* f = fopen(manifest.c_str(), "w")) {
        fprintf(f, "\"AppState\"\n{\n\"appid\" \"570\"\n\"name\" \"Dota 2\"\n\"BytesToDownload\" \"1000\"\n"
                   "\"BytesDownloaded\" \"%d\"\n}\n", done);
        fclose(f);
      }
    };
    SteamCollector steam;
    steam.libraries = {apps};
    steam.libraries_at = 1e12;
    write_manifest(100);
    steam.collect(100.0);
    write_manifest(300);
    auto jobs = steam.collect(102.0);
    check(jobs.size() == 1 && jobs[0].detail == "Dota 2" && jobs[0].done == 300u && jobs[0].total == 1000u &&
              jobs[0].rate > 0,
          "steam progress");
    write_manifest(1000);
    check(steam.collect(104.0).empty(), "steam finished download disappears");
    unlink(manifest.c_str());
    rmdir(apps.c_str());
    rmdir(dir);
  }
  if (failures == 0) printf("self-test ok\n");
  return failures == 0 ? 0 : 1;
}

}  // namespace

// ------------------------------------------------------------------ main

int main(int argc, char** argv) {
  double interval = 1.0;
  bool once = false, demo = false;
  std::vector<std::string> extra;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--interval" && i + 1 < argc) {
      interval = std::atof(argv[++i]);
    } else if (arg == "--watch" && i + 1 < argc) {
      std::string list = argv[++i];
      size_t start = 0;
      while (start <= list.size()) {
        size_t comma = list.find(',', start);
        if (comma == std::string::npos) comma = list.size();
        std::string name = list.substr(start, comma - start);
        if (!name.empty()) extra.push_back(name);
        start = comma + 1;
      }
    } else if (arg == "--once") {
      once = true;
    } else if (arg == "--demo") {
      demo = true;
    } else if (arg == "--self-test") {
      return self_test();
    } else if (arg == "--help" || arg == "-h") {
      printf("usage: downloads [--interval SECONDS] [--watch a,b] [--once] [--demo] [--self-test]\n");
      return 0;
    }
  }
  interval = std::min(10.0, std::max(0.5, interval));
  signal(SIGPIPE, SIG_IGN);

  Monitor monitor(extra);
  NotifyLock lock;
  pid_t parent = getppid();
  std::string last;
  int samples = 0;
  timespec pause{static_cast<time_t>(interval), static_cast<long>((interval - static_cast<long>(interval)) * 1e9)};

  while (true) {
    double now = now_seconds();
    Snapshot snap = demo ? demo_snapshot(now) : monitor.tick(now);
    if (!snap.finished.empty() && !lock.held()) snap.finished.clear();
    std::string line = to_json(snap);
    samples++;
    if (once && samples < 2) {
      nanosleep(&pause, nullptr);
      continue;
    }
    if (line != last || !snap.jobs.empty()) {
      line += "\n";
      if (fputs(line.c_str(), stdout) < 0 || fflush(stdout) != 0) return 0;
      line.pop_back();
      last = line;
    }
    if (once) return 0;
    if (getppid() != parent) return 0;  // the shell went away
    nanosleep(&pause, nullptr);
  }
}
