// Hash-pinned, read-only capability survey of the native OAR speaker model.
// Calls existing functions in this process; does not patch the system image.
#include <array>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <AudioToolbox/AudioToolbox.h>
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

struct SpeakerSet {
    uint64_t low{}, high{};
};
struct PositionSpan {
    int32_t* data;
    int32_t* end;
    int32_t* begin;
};
struct OarConfig {
    uint64_t groups;
    uint32_t frames, objects, unknown_one, unknown_zero;
    uint64_t sample_rate;
    int32_t lfe_index;
    uint32_t one, spread, quality;
};
static_assert(sizeof(OarConfig) == 48);

class Native {
  public:
    bool open() {
        constexpr const char* path = "/System/Library/Components/AudioCodecs.component/Contents/MacOS/AudioCodecs";
        std::ifstream input(path, std::ios::binary);
        std::vector<char> file((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        unsigned char digest[CC_SHA256_DIGEST_LENGTH]{};
        CC_SHA256(file.data(), static_cast<CC_LONG>(file.size()), digest);
        std::string hex;
        for (const auto byte : digest) {
            char text[3];
            std::snprintf(text, sizeof(text), "%02x", byte);
            hex += text;
        }
        if (hex != "826948774145d657788f3101cf36ad1103c230e9bb3712cb65bc56763fd297dd")
            return false;
        image_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!image_)
            return false;
        for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
            if (std::strcmp(_dyld_get_image_name(i), path) == 0) {
                header_ = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
                slide_ = _dyld_get_image_vmaddr_slide(i);
                break;
            }
        }
        if (!header_ || header_->magic != MH_MAGIC_64)
            return false;
        const auto* command = reinterpret_cast<const load_command*>(header_ + 1);
        uintptr_t linkedit = 0;
        const symtab_command* table = nullptr;
        for (uint32_t i = 0; i < header_->ncmds; ++i) {
            if (command->cmd == LC_SEGMENT_64) {
                const auto* segment = reinterpret_cast<const segment_command_64*>(command);
                if (!std::strcmp(segment->segname, SEG_LINKEDIT))
                    linkedit = slide_ + segment->vmaddr - segment->fileoff;
            } else if (command->cmd == LC_SYMTAB) {
                table = reinterpret_cast<const symtab_command*>(command);
            }
            command = reinterpret_cast<const load_command*>(reinterpret_cast<const char*>(command) + command->cmdsize);
        }
        if (!linkedit || !table)
            return false;
        symbols_ = reinterpret_cast<const nlist_64*>(linkedit + table->symoff);
        strings_ = reinterpret_cast<const char*>(linkedit + table->stroff);
        count_ = table->nsyms;
        string_size_ = table->strsize;
        return true;
    }

    template <class T> T get(const char* name) const {
        for (uint32_t i = 0; i < count_; ++i) {
            const auto& entry = symbols_[i];
            if (entry.n_un.n_strx < string_size_ && (entry.n_type & N_TYPE) == N_SECT &&
                !std::strcmp(strings_ + entry.n_un.n_strx, name))
                return reinterpret_cast<T>(slide_ + entry.n_value);
        }
        return nullptr;
    }

  private:
    void* image_{};
    const mach_header_64* header_{};
    intptr_t slide_{};
    const nlist_64* symbols_{};
    const char* strings_{};
    uint32_t count_{}, string_size_{};
};

int main() {
    Native native;
    if (!native.open()) {
        std::fprintf(stderr, "Native component hash or symbol table mismatch\n");
        return 1;
    }
    const auto make = native.get<SpeakerSet (*)(uint64_t, uint64_t)>("_speaker_config_init");
    const auto count = native.get<uint32_t (*)(const SpeakerSet*)>("_speaker_config_count");
    const auto identity = native.get<uint32_t (*)(const SpeakerSet*, int)>("_speaker_config_identity_get");
    const auto positions = native.get<void (*)(const SpeakerSet*, PositionSpan*)>("_speaker_config_positions_get");
    const auto query = native.get<uint64_t (*)(const OarConfig*)>("_oar_query_memory");
    const auto init = native.get<void* (*) (const OarConfig*, void*, uint64_t)>("_oar_init_safe");
    if (!make || !count || !identity || !positions || !query || !init)
        return 1;
    const auto basic = make(2591, 0);
    if (count(&basic) != 12) {
        std::fprintf(stderr, "Native speaker ABI control failed\n");
        return 1;
    }
    const std::array<uint64_t, 4> controls{1039, 2591, 3743, UINT64_MAX};
    for (const auto mask : controls) {
        const auto set = make(mask, 0);
        const auto channels = count(&set);
        std::array<int32_t, 35 * 3> coordinates{};
        PositionSpan span{coordinates.data(), coordinates.data() + coordinates.size(), coordinates.data()};
        positions(&set, &span);
        OarConfig config{mask, 1536, 32, 1, 0, 48000, -1, 1, 32, 32};
        std::printf("{\"mask\":%llu,\"speaker_set\":[%llu,%llu],\"channels\":%u,\"memory\":%llu,\"positions\":[",
                    static_cast<unsigned long long>(mask),
                    static_cast<unsigned long long>(set.low),
                    static_cast<unsigned long long>(set.high),
                    channels,
                    static_cast<unsigned long long>(query(&config)));
        for (uint32_t i = 0; i < channels; ++i) {
            const auto* p = coordinates.data() + i * 3;
            std::printf("%s{\"id\":%u,\"xyz_q15\":[%d,%d,%d]}", i ? "," : "", identity(&set, i), p[0], p[1], p[2]);
        }
        std::puts("]}");
    }
    // Find one >16-channel valid native group configuration without changing
    // positions; this is a channel-capacity control, not a 22.2 layout claim.
    for (uint64_t mask = 31; mask < (uint64_t(1) << 21); ++mask) {
        if ((mask & 31) != 31)
            continue;
        const auto set = make(mask, 0);
        if (count(&set) != 24)
            continue;
        OarConfig config{mask, 1536, 32, 1, 0, 48000, -1, 1, 32, 32};
        const auto bytes = query(&config);
        if (!bytes || bytes > 16 * 1024 * 1024)
            continue;
        std::vector<unsigned char> storage(bytes);
        void* handle = init(&config, storage.data(), bytes);
        std::array<int32_t, 35 * 3> xyz{};
        PositionSpan span{xyz.data(), xyz.data() + xyz.size(), xyz.data()};
        positions(&set, &span);
        unsigned horizontal = 0, height = 0, lower = 0, lfe = 0;
        for (unsigned i = 0; i < 24; ++i) {
            if (identity(&set, i) == 3)
                ++lfe;
            else if (xyz[i * 3 + 2] > 0)
                ++height;
            else if (xyz[i * 3 + 2] < 0)
                ++lower;
            else
                ++horizontal;
        }
        std::printf("{\"stage\":\"native_24ch_capacity\",\"mask\":%llu,\"memory\":%llu,\"initialized\":%s,"
                    "\"horizontal\":%u,\"height\":%u,\"lower\":%u,\"lfe\":%u}\n",
                    static_cast<unsigned long long>(mask),
                    static_cast<unsigned long long>(bytes),
                    handle ? "true" : "false",
                    horizontal,
                    height,
                    lower,
                    lfe);
        return handle ? 0 : 1;
    }
    return 1;
}
