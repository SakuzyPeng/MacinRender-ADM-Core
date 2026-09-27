// Private research ABI, established from the version-locked arm64 disassembly.
// No independent engine invocation: each wrapper forwards the normal virtual call.
#pragma once

namespace oar_capture {
constexpr std::size_t max_hooks = 4;
struct Region {
    QString name;
    uintptr_t address{};
    QByteArray bytes;
};
struct Record {
    std::size_t hook{};
    uint64_t sequence{};
    uint64_t thread{};
    uintptr_t self{};
    uintptr_t argument1{};
    uintptr_t argument2{};
    int argument3{};
    uint64_t sample_start{};
    QString detail;
    std::vector<Region> regions;
    std::array<void*, 24> stack{};
    int stack_count{};
};
struct Hook {
    QString name;
    QString kind;
    uintptr_t* slot{};
    uintptr_t original{};
    uintptr_t wrapper{};
    vm_prot_t protection{};
    uint64_t calls{};
    std::size_t records{};
};
std::array<Hook, max_hooks> hooks;
std::size_t hook_count{};
std::vector<Record> records;
std::mutex mutex;
bool enabled{};
uint64_t required_output_channels{};
uint64_t record_stride{16};
struct Instance {
    uintptr_t self{};
    uint64_t frames{};
    uint64_t blocks{};
    bool audible{};
};
std::vector<Instance> instances;
uintptr_t expected_size_dispatch{};
uintptr_t offline_caller{};
uint64_t dispatch_calls{};
uint64_t dispatch_errors{};
struct DispatchContext {
    std::size_t hook{};
    uintptr_t node{};
    uintptr_t object_index{};
    uintptr_t original{};
    uint64_t sample_start{};
};
thread_local DispatchContext* current_dispatch{};

bool read(uintptr_t address, void* destination, std::size_t size) {
    if (!address || size > 65536)
        return false;
    mach_vm_size_t copied = 0;
    return mach_vm_read_overwrite(
               mach_task_self(), address, size, reinterpret_cast<mach_vm_address_t>(destination), &copied) ==
               KERN_SUCCESS &&
           copied == size;
}
template <typename T> T value(uintptr_t address, std::size_t offset = 0) {
    T result{};
    read(address + offset, &result, sizeof(result));
    return result;
}
void region(Record& record, const QString& name, uintptr_t address, std::size_t length) {
    Region item{name, address, QByteArray(static_cast<int>(length), '\0')};
    if (read(address, item.bytes.data(), length))
        record.regions.push_back(std::move(item));
}

void cpp_string(Record& record, const QString& name, uintptr_t address) {
    region(record, name + "_storage24", address, 24);
    const auto last = value<uint8_t>(address, 23);
    const bool is_long = (last & 128) != 0;
    const auto length = is_long ? value<uint64_t>(address, 8) : last;
    const auto data = is_long ? value<uintptr_t>(address) : address;
    if (length <= 1024)
        region(record, name + "_utf8", data, length);
}

void audio(Record& record, const QString& name, uintptr_t buffer) {
    // BufferOps: channels at +0, frames at +8, vector of 56-byte views at +16.
    // Each view holds float data at +8 and element count at +16.
    region(record, name + "_descriptor", buffer, 40);
    const auto channels = value<uint64_t>(buffer);
    const auto views = value<uintptr_t>(buffer, 16);
    if (channels == 0 || channels > 128 || !views)
        return;
    for (std::size_t i = 0; i < channels; ++i) {
        const auto data = value<uintptr_t>(views + i * 56, 8);
        const auto frames = value<uint64_t>(views + i * 56, 16);
        if (frames > 0 && frames <= 8192)
            region(record, name + "_channel_" + QString::number(i), data, frames * 4);
    }
}

bool audible(uintptr_t buffer) {
    const auto channels = value<uint64_t>(buffer);
    const auto views = value<uintptr_t>(buffer, 16);
    if (channels == 0 || channels > 128 || !views)
        return false;
    for (std::size_t i = 0; i < channels; ++i) {
        const auto data = value<uintptr_t>(views + i * 56, 8);
        const auto frames = value<uint64_t>(views + i * 56, 16);
        std::array<float, 8192> samples{};
        if (frames > 0 && frames <= samples.size() && read(data, samples.data(), frames * 4))
            for (std::size_t j = 0; j < frames; ++j)
                if (samples[j] != 0)
                    return true;
    }
    return false;
}

void state(Record& record, uintptr_t context, uintptr_t core, uint64_t inputs, uint64_t outputs) {
    if (inputs > 128 || outputs > 128)
        return;
    const auto pending = value<uintptr_t>(record.self, 96);
    region(record, "metadata_44_pending", value<uintptr_t>(pending, 40), inputs * 44);
    region(record, "metadata_60_effective", value<uintptr_t>(core, 744), inputs * 60);
    region(record, "metadata_60_previous", value<uintptr_t>(core, 752), inputs * 60);
    const auto rows = value<uintptr_t>(core);
    region(record, "gain_rows_48", rows, inputs * 48);
    for (uint64_t i = 0; i < inputs; ++i) {
        const auto prefix = QStringLiteral("gain_") + QString::number(i);
        region(record, prefix + "_current", value<uintptr_t>(rows + i * 48), outputs * 4);
        region(record, prefix + "_target", value<uintptr_t>(rows + i * 48, 8), outputs * 4);
        region(record, prefix + "_step", value<uintptr_t>(rows + i * 48, 16), outputs * 4);
        region(record, prefix + "_steps_left", value<uintptr_t>(rows + i * 48, 24), outputs * 4);
    }
    // The immediate consumer's input descriptor is embedded at +32. Verify
    // its adapter back-reference before following its PCM-selection container.
    const auto owner = record.argument1 - 32;
    if (value<uintptr_t>(owner, 120) == record.self) {
        region(record, "processor", owner, 136);
        const auto selection = value<uintptr_t>(owner, 8);
        region(record, "pcm_selection", selection, value<uint8_t>(owner) ? 16 : inputs * 8);
    }
}

void events72(Record& record, const QString& name, uintptr_t begin, uintptr_t end, uint64_t frames) {
    if (!begin || end < begin || (end - begin) % 72 || end - begin > 72 * 256 || frames > 8192)
        return;
    region(record, name + "_events72", begin, end - begin);
    for (std::size_t i = 0; i < (end - begin) / 72; ++i)
        region(record, name + "_pcm_" + QString::number(i), value<uintptr_t>(begin + i * 72, 8), frames * 4);
}

void omo_state(Record& record, uintptr_t engine) {
    const auto count = value<uint32_t>(engine, 20);
    const auto output_count = value<uint32_t>(engine, 32) ? 15u : 11u;
    if (count > 159)
        return;
    const auto object_states = value<uintptr_t>(engine, 48);
    region(record, "object_state_pointers", object_states, count * 8);
    for (uint32_t i = 0; i < count; ++i) {
        const auto object = value<uintptr_t>(object_states + 8 * i);
        const auto prefix = QStringLiteral("object_") + QString::number(i);
        region(record, prefix + "_state144", object, 144);
        region(record, prefix + "_size_target", value<uintptr_t>(object, 24), output_count * 4);
        region(record, prefix + "_size_previous", value<uintptr_t>(object, 32), output_count * 4);
        region(record, prefix + "_other_target", value<uintptr_t>(object, 48), output_count * 4);
        region(record, prefix + "_other_previous", value<uintptr_t>(object, 56), output_count * 4);
    }
    const auto threads = value<uint32_t>(engine);
    const auto workers = value<uintptr_t>(engine, 40);
    if (threads > 159)
        return;
    for (uint32_t i = 0; i < threads; ++i) {
        const auto worker = value<uintptr_t>(workers + 8 * i);
        const auto prefix = QStringLiteral("worker_") + QString::number(i);
        region(record, prefix, worker, 96);
        region(record, prefix + "_panner_dispatch", value<uintptr_t>(worker, 48), 40);
    }
}

void size_dispatch(
    void* dispatch, const void* metadata, const void* layout, int update, int flag, float* output, void* scratch) {
    const auto context = *current_dispatch;
    using Function = void (*)(void*, const void*, const void*, int, int, float*, void*);
    reinterpret_cast<Function>(context.original)(dispatch, metadata, layout, update, flag, output, scratch);
    std::lock_guard<std::mutex> lock(mutex);
    const auto sequence = dispatch_calls++;
    if (hooks[context.hook].records >= g_max_records)
        return;
    Record record;
    record.hook = context.hook;
    record.detail = "size_gain_callback";
    record.sequence = sequence;
    record.self = context.node;
    record.argument1 = context.object_index;
    record.argument2 = reinterpret_cast<uintptr_t>(dispatch);
    record.argument3 = flag;
    record.sample_start = context.sample_start;
    pthread_threadid_np(nullptr, &record.thread);
    region(record, "metadata60", reinterpret_cast<uintptr_t>(metadata), 60);
    region(record, "layout", reinterpret_cast<uintptr_t>(layout), 344);
    region(record, "size_gains", reinterpret_cast<uintptr_t>(output), 11 * 4);
    region(record, "dispatch", reinterpret_cast<uintptr_t>(dispatch), 40);
    if (sequence == 0) {
        const auto adapter = value<uintptr_t>(reinterpret_cast<uintptr_t>(dispatch));
        const auto panner = value<uintptr_t>(adapter);
        const auto spatial = value<uintptr_t>(panner, 8);
        const auto rooms = value<uintptr_t>(spatial);
        const auto geometry = value<uintptr_t>(rooms);
        region(record, "size_spatial_context", spatial, 72);
        region(record, "size_room_set", rooms, 24);
        region(record, "size_geometry", geometry, 120);
        const auto count = value<uint32_t>(geometry, 16);
        if (count > 0 && count <= 34) {
            region(record, "size_speaker_xyz", value<uintptr_t>(geometry), count * 12);
            region(record, "size_speaker_map", value<uintptr_t>(geometry, 8), count * 4);
            region(record, "size_wall_weights", value<uintptr_t>(geometry, 112), 1360);
            for (const auto offset : {24, 64}) {
                const auto layer_count = value<uint32_t>(geometry, static_cast<std::size_t>(offset) + 28);
                if (layer_count <= count)
                    region(record, "size_layer_" + QString::number(offset), value<uintptr_t>(geometry, offset),
                           layer_count * sizeof(uint32_t));
            }
        }
    }
    record.stack_count = backtrace(record.stack.data(), record.stack.size());
    records.push_back(std::move(record));
    ++hooks[context.hook].records;
}

template <std::size_t Index> uintptr_t omo_worker(void* self, int thread_index) {
    const auto original = reinterpret_cast<uintptr_t (*)(void*, int)>(hooks[Index].original);
    if (!g_capture.load(std::memory_order_acquire))
        return original(self, thread_index);
    std::array<void*, 24> caller_stack{};
    const auto caller_count = backtrace(caller_stack.data(), caller_stack.size());
    if (std::none_of(caller_stack.begin(), caller_stack.begin() + caller_count, [](const auto pc) {
            return reinterpret_cast<uintptr_t>(pc) == offline_caller;
        }))
        return original(self, thread_index);
    const auto node_address = reinterpret_cast<uintptr_t>(self);
    const auto object_index = value<uint64_t>(node_address, 72);
    const auto platform = value<uintptr_t>(node_address, 80);
    const auto backend = value<uintptr_t>(platform, 152);
    const auto engine = value<uintptr_t>(backend);
    const auto threads = value<uint32_t>(engine);
    const auto inputs = value<uint32_t>(engine, 20);
    const auto frames = value<uint32_t>(engine, 4);
    DispatchContext context{Index, node_address, object_index, 0, 0};
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++hooks[Index].calls;
        auto instance = std::find_if(instances.begin(), instances.end(), [self](const auto& item) {
            return item.self == reinterpret_cast<uintptr_t>(self);
        });
        if (instance == instances.end()) {
            instances.push_back(Instance{node_address});
            instance = std::prev(instances.end());
        }
        context.sample_start = instance->frames;
        instance->frames += frames;
    }
    if (thread_index < 0 || static_cast<uint32_t>(thread_index) >= threads || inputs > 159 || object_index >= inputs ||
        !frames)
        return original(self, thread_index);
    const auto worker = value<uintptr_t>(value<uintptr_t>(engine, 40) + 8 * thread_index);
    const auto dispatch = value<uintptr_t>(worker, 48);
    auto* slot = reinterpret_cast<uintptr_t*>(dispatch + 32);
    context.original = value<uintptr_t>(reinterpret_cast<uintptr_t>(slot));
    // OMO assigns one private workspace to each scheduler thread. The callback
    // table lives there; change it only for this normal worker call, and restore
    // before the scheduler can release/reuse/destroy that workspace.
    if (context.original != expected_size_dispatch || value<uint32_t>(engine, 32) != 0) {
        std::lock_guard<std::mutex> lock(mutex);
        ++dispatch_errors;
        return original(self, thread_index);
    }
    struct Restore {
        uintptr_t* slot;
        uintptr_t original;
        DispatchContext* previous;
        ~Restore() {
            __atomic_store_n(slot, original, __ATOMIC_SEQ_CST);
            current_dispatch = previous;
        }
    } restore{slot, context.original, current_dispatch};
    current_dispatch = &context;
    __atomic_store_n(slot, reinterpret_cast<uintptr_t>(&size_dispatch), __ATOMIC_SEQ_CST);
    const auto input_event_before = value<uintptr_t>(backend, 16) + object_index * 72;
    std::array<uint8_t, 72> input_before{};
    const bool selected = frames <= 8192 &&
                          (context.sample_start <= frames || (context.sample_start / frames) % record_stride == 0);
    const bool have_before = selected && read(input_event_before, input_before.data(), input_before.size());
    const auto result = original(self, thread_index);
    if (frames > 8192 || (context.sample_start > frames && (context.sample_start / frames) % record_stride))
        return result;
    const auto input_event = value<uintptr_t>(backend, 16) + object_index * 72;
    std::array<float, 8192> pcm{};
    const auto pcm_address = value<uintptr_t>(input_event, 8);
    if (!read(pcm_address, pcm.data(), frames * 4) ||
        std::none_of(pcm.begin(), pcm.begin() + frames, [](float x) { return x != 0; }))
        return result;
    std::lock_guard<std::mutex> lock(mutex);
    if (hooks[Index].records >= g_max_records)
        return result;
    Record record;
    record.hook = Index;
    record.detail = "omo_mix_inputs";
    record.self = node_address;
    record.argument1 = object_index;
    record.argument2 = worker;
    record.argument3 = thread_index;
    record.sample_start = context.sample_start;
    pthread_threadid_np(nullptr, &record.thread);
    region(record, "input_event72", input_event, 72);
    if (have_before)
        record.regions.push_back({QStringLiteral("input_event72_before"),
                                  input_event_before,
                                  QByteArray(reinterpret_cast<const char*>(input_before.data()), input_before.size())});
    region(record, "input_pcm", pcm_address, frames * 4);
    region(record, "worker", worker, 96);
    region(record, "weight_target", value<uintptr_t>(worker, 24), frames * 4);
    region(record, "weight_previous", value<uintptr_t>(worker, 40), frames * 4);
    const auto object = value<uintptr_t>(value<uintptr_t>(engine, 48) + object_index * 8);
    region(record, "object_state144", object, 144);
    for (const auto offset : {24, 32, 48, 56})
        region(record, "object_gains_" + QString::number(offset), value<uintptr_t>(object, offset), 11 * 4);
    const auto filter = value<uintptr_t>(object, 8);
    region(record, "filter", filter, 88);
    const auto levels = value<uintptr_t>(filter);
    region(record, "filter_level_state", levels, 40);
    region(record, "filter_level_detector_16", value<uintptr_t>(levels, 16), 12);
    region(record, "filter_level_detector_24", value<uintptr_t>(levels, 24), 12);
    region(record, "filter_level_prefilter", value<uintptr_t>(levels, 32), 8);
    region(record, "filter_input_delay", value<uintptr_t>(filter, 8), 24);
    region(record, "filter_coefficients", value<uintptr_t>(filter, 48), 64);
    region(record, "filter_delays", value<uintptr_t>(filter, 56), 16);
    // The identified workspace query (0x101e638d0) returns 2143 bytes
    // for this one-object call, rounded up to a 32-byte boundary by OMO.
    const auto signal_table = value<uintptr_t>(worker, 56) + 2144;
    const auto stride = (frames * 4 + 31) & ~uint64_t{31};
    for (uint64_t i = 0; i < 4; ++i) {
        const auto data = value<uintptr_t>(signal_table + i * 8);
        if (data >= signal_table + 32 && data + frames * 4 <= signal_table + 32 + 4 * stride)
            region(record, "filtered_pcm_" + QString::number(i), data, frames * 4);
    }
    const auto mode = value<uint32_t>(worker, 72) & 15;
    if (mode < 4) {
        region(
            record, "filter_channel_map", value<uintptr_t>(g_image_base - g_virtual_base + 0x103d15b68 + mode * 8), 44);
        region(record,
               "filter_channel_sign",
               value<uintptr_t>(g_image_base - g_virtual_base + 0x103d15b88 + mode * 8),
               44);
        region(record, "mix_coefficients", g_image_base - g_virtual_base + 0x1030dc320, 32);
    }
    record.stack_count = backtrace(record.stack.data(), record.stack.size());
    records.push_back(std::move(record));
    ++hooks[Index].records;
    return result;
}

template <std::size_t Index> uintptr_t node(void* self) {
    const auto original = reinterpret_cast<uintptr_t (*)(void*)>(hooks[Index].original);
    const auto result = original(self);
    if (!g_capture.load(std::memory_order_acquire) ||
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)) != offline_caller)
        return result;
    std::lock_guard<std::mutex> lock(mutex);
    auto& hook = hooks[Index];
    const auto sequence = hook.calls++;
    Record record;
    record.hook = Index;
    record.sequence = sequence;
    pthread_threadid_np(nullptr, &record.thread);
    record.self = reinterpret_cast<uintptr_t>(self);
    const auto platform = value<uintptr_t>(record.self, 72);
    region(record, "node", record.self, 80);
    region(record, "platform", platform, hook.kind == "omo_node" ? 200 : 152);
    const auto backend = value<uintptr_t>(platform, hook.kind == "omo_node" ? 152 : 120);
    const auto engine = value<uintptr_t>(backend);
    const auto frames = value<uint32_t>(engine, 4);
    auto instance = std::find_if(instances.begin(), instances.end(), [self](const auto& item) {
        return item.self == reinterpret_cast<uintptr_t>(self);
    });
    if (instance == instances.end()) {
        instances.push_back(Instance{reinterpret_cast<uintptr_t>(self)});
        instance = std::prev(instances.end());
    }
    record.sample_start = instance->frames;
    instance->frames += frames;
    const auto block = instance->blocks++;
    if (hook.records >= g_max_records || (block > 1 && block % record_stride))
        return result;
    region(record, "backend", backend, 144);
    region(record, "engine", engine, 512);
    if (hook.kind == "omo_node") {
        omo_state(record, engine);
        events72(record, "input", value<uintptr_t>(backend, 16), value<uintptr_t>(backend, 24), frames);
        events72(record, "direct", value<uintptr_t>(backend, 40), value<uintptr_t>(backend, 48), frames);
        events72(record, "branches", value<uintptr_t>(backend, 64), value<uintptr_t>(backend, 72), frames);
    }
    record.stack_count = backtrace(record.stack.data(), record.stack.size());
    records.push_back(std::move(record));
    ++hook.records;
    return result;
}

template <std::size_t Index> void metadata(void* self, uint64_t index, const void* event, int flag) {
    const auto original = reinterpret_cast<void (*)(void*, uint64_t, const void*, int)>(hooks[Index].original);
    original(self, index, event, flag);
    if (!g_capture.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> lock(mutex);
    auto& hook = hooks[Index];
    const auto sequence = hook.calls++;
    if (required_output_channels && value<uint64_t>(reinterpret_cast<uintptr_t>(self), 136) != required_output_channels)
        return;
    // Metadata from silent/background instances is recovered together with
    // their corresponding process record; avoid spending the cap on padding.
    if (hook.records >= g_max_records || record_stride > 1)
        return;
    std::array<char, 44> bytes{};
    if (!read(reinterpret_cast<uintptr_t>(event), bytes.data(), bytes.size()))
        return;
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        if (it->hook == Index && it->self == reinterpret_cast<uintptr_t>(self) && it->argument1 == index) {
            if (!it->regions.empty() && it->regions[0].bytes == QByteArray(bytes.data(), bytes.size()))
                return;
            break;
        }
    }
    Record record;
    record.hook = Index;
    record.sequence = sequence;
    pthread_threadid_np(nullptr, &record.thread);
    record.self = reinterpret_cast<uintptr_t>(self);
    record.argument1 = index;
    record.argument2 = reinterpret_cast<uintptr_t>(event);
    record.argument3 = flag;
    region(record, "metadata_44", record.argument2, 44);
    region(record, "adapter", record.self, 184);
    record.stack_count = backtrace(record.stack.data(), record.stack.size());
    records.push_back(std::move(record));
    ++hook.records;
}

// Confirmed SAX start-element ABI for the version-locked ADM object/block
// parsers. Read the normal parser state only; never query the engine from here.
template <std::size_t Index> void adm_element(void* self, const void* element) {
    const auto original = reinterpret_cast<void (*)(void*, const void*)>(hooks[Index].original);
    original(self, element);
    if (!g_adm_capture.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> lock(mutex);
    auto& hook = hooks[Index];
    const auto sequence = hook.calls++;
    if (hook.records >= g_max_records)
        return;
    const auto pointer = reinterpret_cast<uintptr_t>(self);
    const auto depth = value<uint32_t>(pointer, 24);
    if (depth > 2)
        return;
    Record record;
    record.hook = Index;
    record.sequence = sequence;
    record.self = pointer;
    record.argument1 = reinterpret_cast<uintptr_t>(element);
    record.argument3 = static_cast<int>(depth);
    record.detail = hook.kind;
    pthread_threadid_np(nullptr, &record.thread);
    region(record, "parser_header", pointer, 40);
    // XML token name accessor 0x102399c2c returns element +16. Capture the
    // bytes directly; calling that accessor inside a callback is unnecessary.
    cpp_string(record, "element_name", reinterpret_cast<uintptr_t>(element) + 16);
    const auto capture_string = [&](std::size_t offset, const QString& name) {
        region(record, name + "_storage24", pointer + offset, 24);
        const auto last = value<uint8_t>(pointer + offset, 23);
        const bool is_long = (last & 128) != 0;
        const auto length = is_long ? value<uint64_t>(pointer + offset, 8) : last;
        const auto data = is_long ? value<uintptr_t>(pointer + offset) : pointer + offset;
        if (length <= 1024)
            region(record, name + "_utf8", data, length);
    };
    if (hook.kind == "adm_object_element") {
        capture_string(40, "object_id");
        capture_string(64, "object_name");
        capture_string(88, "object_start");
        capture_string(112, "object_duration");
    } else {
        capture_string(72, "block_id");
        const auto consumer = value<uintptr_t>(pointer, 40);
        region(record, "block_consumer_vtable", consumer, 8);
        region(record, "block_consumer_state56", consumer, 56);
        const auto delegated = value<uintptr_t>(pointer, 16);
        region(record, "active_child_header", delegated, 40);
        if (delegated && value<uintptr_t>(delegated) == g_image_base - g_virtual_base + 0x103d36970)
            region(record, "active_value_converter", value<uintptr_t>(delegated, 64), 24);
    }
    record.stack_count = backtrace(record.stack.data(), record.stack.size());
    records.push_back(std::move(record));
    ++hook.records;
}

template <std::size_t Index> uintptr_t adm_diffuse(void* self, uint8_t flag) {
    const auto original = reinterpret_cast<uintptr_t (*)(void*, uint8_t)>(hooks[Index].original);
    const auto result = original(self, flag);
    if (!g_adm_capture.load(std::memory_order_acquire))
        return result;
    std::lock_guard<std::mutex> lock(mutex);
    auto& hook = hooks[Index];
    const auto sequence = hook.calls++;
    if (hook.records >= g_max_records)
        return result;
    Record record;
    record.hook = Index;
    record.sequence = sequence;
    record.self = reinterpret_cast<uintptr_t>(self);
    record.argument3 = flag;
    record.detail = "adm_diffuse_boolean";
    pthread_threadid_np(nullptr, &record.thread);
    region(record, "block_consumer_state56", record.self, 56);
    // 0x102290bf4 dispatches the completed block (self +8) through this
    // callback at self +96. Keep pointer/slot evidence without guessing types.
    region(record, "consumer_callback96", value<uintptr_t>(record.self, 96), 8);
    region(record, "consumer_callback96_vtable", value<uintptr_t>(value<uintptr_t>(record.self, 96)), 56);
    record.stack_count = backtrace(record.stack.data(), record.stack.size());
    records.push_back(std::move(record));
    ++hook.records;
    return result;
}

template <std::size_t Index> uintptr_t process(void* self, void* input, void* output) {
    const auto original = reinterpret_cast<uintptr_t (*)(void*, void*, void*)>(hooks[Index].original);
    const auto result = original(self, input, output);
    if (!g_capture.load(std::memory_order_acquire))
        return result;
    std::lock_guard<std::mutex> lock(mutex);
    auto& hook = hooks[Index];
    const auto sequence = hook.calls++;
    if (required_output_channels && value<uint64_t>(reinterpret_cast<uintptr_t>(output)) != required_output_channels)
        return result;
    std::array<void*, 24> call_stack{};
    const auto stack_count = backtrace(call_stack.data(), call_stack.size());
    if (std::none_of(call_stack.begin(), call_stack.begin() + stack_count, [](const auto pc) {
            return reinterpret_cast<uintptr_t>(pc) == offline_caller;
        }))
        return result;
    const auto self_address = reinterpret_cast<uintptr_t>(self);
    auto instance = std::find_if(
        instances.begin(), instances.end(), [self_address](const auto& item) { return item.self == self_address; });
    if (instance == instances.end()) {
        instances.push_back(Instance{self_address});
        instance = std::prev(instances.end());
    }
    const auto frames = value<uint64_t>(reinterpret_cast<uintptr_t>(input), 8);
    const auto sample_start = instance->frames;
    instance->frames += frames;
    const auto block = instance->blocks++;
    instance->audible = instance->audible || audible(reinterpret_cast<uintptr_t>(input));
    if (!instance->audible || (block > 1 && block % record_stride != 0))
        return result;
    if (hook.records >= g_max_records)
        return result;
    Record record;
    record.hook = Index;
    record.sequence = sequence;
    pthread_threadid_np(nullptr, &record.thread);
    record.self = reinterpret_cast<uintptr_t>(self);
    record.argument1 = reinterpret_cast<uintptr_t>(input);
    record.argument2 = reinterpret_cast<uintptr_t>(output);
    record.sample_start = sample_start;
    region(record, "adapter", record.self, 184);
    const auto context = value<uintptr_t>(record.self, 112);
    region(record, "context", context, 280);
    const auto core = value<uintptr_t>(context, 208);
    region(record, "core", core, 1152);
    state(record, context, core, value<uint64_t>(record.argument1), value<uint64_t>(record.argument2));
    audio(record, "input", record.argument1);
    audio(record, "output", record.argument2);
    record.stack = call_stack;
    record.stack_count = stack_count;
    records.push_back(std::move(record));
    ++hook.records;
    return result;
}

bool write(Hook& hook, uintptr_t pointer, QString& error) {
    const auto size = static_cast<uintptr_t>(getpagesize());
    const auto page = reinterpret_cast<uintptr_t>(hook.slot) & ~(size - 1);
    if (mprotect(reinterpret_cast<void*>(page), size, PROT_READ | PROT_WRITE) != 0) {
        error = "could not make OAR vtable writable";
        return false;
    }
    __atomic_store_n(hook.slot, pointer, __ATOMIC_SEQ_CST);
    if (mprotect(reinterpret_cast<void*>(page), size, hook.protection) != 0) {
        error = "could not restore OAR vtable protection";
        return false;
    }
    return true;
}

bool restore(QString& error) {
    bool ok = true;
    for (std::size_t i = 0; i < hook_count; ++i) {
        if (hooks[i].slot) {
            const bool written = write(hooks[i], hooks[i].original, error);
            ok = written && value<uintptr_t>(reinterpret_cast<uintptr_t>(hooks[i].slot)) == hooks[i].original && ok;
        }
    }
    return ok;
}

bool install(const QJsonObject& config, QString& error) {
    const auto requests = config.value("hooks").toArray();
    if (requests.isEmpty() || requests.size() > max_hooks) {
        error = "OAR capture requires 1 to 4 identified hooks";
        return false;
    }
    const std::array<uintptr_t, max_hooks> metadata_wrappers{reinterpret_cast<uintptr_t>(&metadata<0>),
                                                             reinterpret_cast<uintptr_t>(&metadata<1>),
                                                             reinterpret_cast<uintptr_t>(&metadata<2>),
                                                             reinterpret_cast<uintptr_t>(&metadata<3>)};
    const std::array<uintptr_t, max_hooks> process_wrappers{reinterpret_cast<uintptr_t>(&process<0>),
                                                            reinterpret_cast<uintptr_t>(&process<1>),
                                                            reinterpret_cast<uintptr_t>(&process<2>),
                                                            reinterpret_cast<uintptr_t>(&process<3>)};
    const std::array<uintptr_t, max_hooks> node_wrappers{reinterpret_cast<uintptr_t>(&node<0>),
                                                         reinterpret_cast<uintptr_t>(&node<1>),
                                                         reinterpret_cast<uintptr_t>(&node<2>),
                                                         reinterpret_cast<uintptr_t>(&node<3>)};
    const std::array<uintptr_t, max_hooks> worker_wrappers{reinterpret_cast<uintptr_t>(&omo_worker<0>),
                                                           reinterpret_cast<uintptr_t>(&omo_worker<1>),
                                                           reinterpret_cast<uintptr_t>(&omo_worker<2>),
                                                           reinterpret_cast<uintptr_t>(&omo_worker<3>)};
    const std::array<uintptr_t, max_hooks> adm_wrappers{reinterpret_cast<uintptr_t>(&adm_element<0>),
                                                      reinterpret_cast<uintptr_t>(&adm_element<1>),
                                                      reinterpret_cast<uintptr_t>(&adm_element<2>),
                                                      reinterpret_cast<uintptr_t>(&adm_element<3>)};
    const std::array<uintptr_t, max_hooks> diffuse_wrappers{reinterpret_cast<uintptr_t>(&adm_diffuse<0>),
                                                          reinterpret_cast<uintptr_t>(&adm_diffuse<1>),
                                                          reinterpret_cast<uintptr_t>(&adm_diffuse<2>),
                                                          reinterpret_cast<uintptr_t>(&adm_diffuse<3>)};
    g_max_records = static_cast<std::size_t>(std::clamp(config.value("max_records").toInt(64), 1, 2048));
    required_output_channels = config.value("output_channels").toInt();
    record_stride = std::clamp(config.value("record_stride").toInt(16), 1, 1024);
    expected_size_dispatch =
        config.contains("size_dispatch")
            ? g_image_base - g_virtual_base + config.value("size_dispatch").toString().toULongLong(nullptr, 0)
            : 0;
    offline_caller = g_image_base - g_virtual_base + 0x101ed38f8;
    instances.reserve(128);
    records.reserve(g_max_records * requests.size());
    for (const auto& request : requests) {
        const auto item = request.toObject();
        auto& hook = hooks[hook_count];
        hook.name = item.value("name").toString();
        hook.kind = item.value("kind").toString();
        if (hook.kind != "oar_metadata" && hook.kind != "oar_process" && hook.kind != "omo_node" &&
            hook.kind != "spatial_node" && hook.kind != "omo_worker" && hook.kind != "adm_object_element" &&
            hook.kind != "adm_block_element" && hook.kind != "adm_diffuse_value") {
            error = "unidentified OAR wrapper ABI";
            return false;
        }
        hook.slot = reinterpret_cast<uintptr_t*>(g_image_base - g_virtual_base +
                                                 item.value("vtable_slot").toString().toULongLong(nullptr, 0));
        const auto expected = g_image_base - g_virtual_base + item.value("function").toString().toULongLong(nullptr, 0);
        hook.original = value<uintptr_t>(reinterpret_cast<uintptr_t>(hook.slot));
        if (hook.original != expected) {
            error = "OAR vtable method differs from the identified function";
            hook.slot = nullptr;
            return false;
        }
        mach_vm_address_t address = reinterpret_cast<mach_vm_address_t>(hook.slot);
        mach_vm_size_t length = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        const auto status = mach_vm_region(mach_task_self(),
                                           &address,
                                           &length,
                                           VM_REGION_BASIC_INFO_64,
                                           reinterpret_cast<vm_region_info_t>(&info),
                                           &count,
                                           &object);
        if (object != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), object);
        if (status != KERN_SUCCESS) {
            error = "could not query OAR vtable protection";
            hook.slot = nullptr;
            return false;
        }
        hook.protection = info.protection;
        hook.wrapper = hook.kind == "adm_diffuse_value" ? diffuse_wrappers[hook_count]
                       : hook.kind.startsWith("adm_") ? adm_wrappers[hook_count]
                       : hook.kind == "oar_metadata"  ? metadata_wrappers[hook_count]
                       : hook.kind == "oar_process" ? process_wrappers[hook_count]
                       : hook.kind == "omo_worker"  ? worker_wrappers[hook_count]
                                                    : node_wrappers[hook_count];
        ++hook_count;
        if (!write(hook, hook.wrapper, error))
            return false;
    }
    enabled = true;
    return true;
}

QJsonObject report() {
    QString error;
    const bool restored = restore(error);
    std::lock_guard<std::mutex> lock(mutex);
    QJsonArray result;
    QJsonArray counts;
    uint64_t calls = 0;
    for (std::size_t i = 0; i < hook_count; ++i) {
        counts.append(QJsonObject{{"name", hooks[i].name},
                                  {"calls", static_cast<qint64>(hooks[i].calls)},
                                  {"records", static_cast<int>(hooks[i].records)}});
        calls += hooks[i].calls;
    }
    for (const auto& record : records) {
        QJsonArray regions;
        for (const auto& item : record.regions)
            regions.append(QJsonObject{{"name", item.name},
                                       {"address", hex_address(item.address)},
                                       {"bytes", item.bytes.size()},
                                       {"hex", QString::fromLatin1(item.bytes.toHex())}});
        QJsonArray stack;
        for (int i = 0; i < record.stack_count; ++i) {
            const auto pc = reinterpret_cast<uintptr_t>(record.stack[i]);
            Dl_info info{};
            dladdr(reinterpret_cast<void*>(pc), &info);
            const auto base = reinterpret_cast<uintptr_t>(info.dli_fbase);
            stack.append(QJsonObject{
                {"runtime", hex_address(pc)},
                {"module", info.dli_fname ? QString::fromUtf8(info.dli_fname) : QString()},
                {"module_offset", hex_address(pc - base)},
                {"main_image_address",
                 base == g_image_base ? QJsonValue(hex_address(pc - base + g_virtual_base)) : QJsonValue::Null}});
        }
        result.append(QJsonObject{{"name", hooks[record.hook].name},
                                  {"kind", hooks[record.hook].kind},
                                  {"detail", record.detail},
                                  {"sequence", static_cast<qint64>(record.sequence)},
                                  {"thread_id", static_cast<qint64>(record.thread)},
                                  {"self", hex_address(record.self)},
                                  {"argument1", hex_address(record.argument1)},
                                  {"sample_start", static_cast<qint64>(record.sample_start)},
                                  {"argument2", hex_address(record.argument2)},
                                  {"argument3", record.argument3},
                                  {"regions", regions},
                                  {"stack", stack}});
    }
    return QJsonObject{{"transport", "resident_vtable_capture"},
                       {"hook_restored", restored},
                       {"size_dispatch_calls", static_cast<qint64>(dispatch_calls)},
                       {"size_dispatch_errors", static_cast<qint64>(dispatch_errors)},
                       {"error", error.isEmpty() ? QJsonValue::Null : QJsonValue(error)},
                       {"total_calls", static_cast<qint64>(calls)},
                       {"hooks", counts},
                       {"load_base", hex_address(g_image_base)},
                       {"records", result}};
}
} // namespace oar_capture
