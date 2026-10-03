#pragma once
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <string>

namespace dp {

// Owned bytes only: no game objects or timestamps participate in input identity.
struct InputFileData {
    bool readable = false;
    std::string bytes;
    mutable std::string signature;
    uint64_t revision = 0;
};

// Read a file completely; a short read is not a cacheable input.
inline bool readFileBytesRaw(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    if (n < 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize((size_t)n);
    return n == 0 || bool(in.read(&out[0], n));
}

// A worker job pins only files its caller guarantees it will not rewrite.
class InputFiles {
    std::map<std::string, std::shared_ptr<InputFileData>> previous_, pinned_;
    uint64_t session_ = 0, nextRevision_ = 0;
    uint64_t job_ = 0;
    std::string csv_;
    bool active_ = false;
public:
    uint64_t reads = 0, hits = 0, levelHits = 0;
    uint64_t levelRevision = 0;
    // A new session cannot inherit any input identity from the preceding level.
    void begin(uint64_t session, const std::string& csv) {
        if (session != session_) { previous_.clear(); csv_.clear(); levelRevision = 0; }
        if (!levelRevision || csv_ != csv) { csv_ = csv; levelRevision = ++nextRevision_; }
        session_ = session;
        pinned_.clear();
        reads = hits = levelHits = 0;
        ++job_;
        active_ = true;
    }
    // Drop the job's pins before another attempt can publish new recordings.
    void end() { pinned_.clear(); active_ = false; }
    // Zero means an ordinary CLI call, with no immutable-input guarantee.
    uint64_t job() const { return active_ ? job_ : 0; }
    // Revalidate between jobs by bytes, never by size or modification time.
    std::shared_ptr<const InputFileData> get(const std::string& path, bool pin = false) {
        if (active_) {
            const auto it = pinned_.find(path);
            if (it != pinned_.end()) { ++hits; return it->second; }
        }
        auto data = std::make_shared<InputFileData>();
        ++reads;
        data->readable = readFileBytesRaw(path, data->bytes);
        if (!data->readable) { data->bytes.clear(); return data; }
        if (active_) {
            const auto it = previous_.find(path);
            if (it != previous_.end() && it->second->bytes == data->bytes)
                data = it->second;
            else {
                data->revision = ++nextRevision_;
                previous_[path] = data;
            }
            if (pin) pinned_[path] = data;
        }
        return data;
    }
    // Copy for existing small-file parsers; group timelines use get() without copying.
    bool read(const std::string& path, std::string& out) {
        if (active_) {
            const auto it = pinned_.find(path);
            if (it != pinned_.end()) {
                ++hits;
                out = it->second->bytes;
                return true;
            }
        }
        return readFileBytesRaw(path, out);
    }
};
inline InputFiles g_inputFiles;

// Preserve the log's size/FNV spelling, computing it once per owned byte buffer.
inline const std::string& inputSignature(const std::shared_ptr<const InputFileData>& data) {
    // The signature is metadata, not the identity used for failed-plan decisions.
    auto& sig = data->signature;
    if (sig.empty()) {
        if (!data->readable) sig = "-";
        else {
            uint64_t h = 1469598103934665603ULL;
            for (unsigned char c : data->bytes) { h ^= c; h *= 1099511628211ULL; }
            char b[48];
            std::snprintf(b, sizeof b, "%zu/%08x", data->bytes.size(), (unsigned)(h & 0xffffffffULL));
            sig = b;
        }
    }
    return sig;
}

// The ordinary CLI still rereads files on every call; only explicitly pinned paths are reused.
inline bool readFileBytes(const std::string& path, std::string& out) {
    return g_inputFiles.read(path, out);
}
}  // namespace dp
