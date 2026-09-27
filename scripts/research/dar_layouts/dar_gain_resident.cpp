// Resident capture transport for Renderer builds whose own Mach exception
// handling cannot run while LLDB owns the exception port. Installed through one
// LLDB attachment; the target then runs normally with a bounded vtable wrapper.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <mutex>
#include <pthread.h>
#include <unistd.h>
#include <vector>

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>

#include "dar_batch_bridge.cpp"

namespace {
using GainFunction = void (*)(void*, float*, int, float, float, float, float);
constexpr std::size_t k_max_channels = 128;

struct GainRecord {
    uint64_t sequence{};
    uint64_t thread{};
    uintptr_t object{};
    uintptr_t output{};
    uintptr_t room{};
    uint64_t channels{};
    int snap{};
    std::array<float, 4> arguments{};
    std::array<float, 3> position{};
    std::array<float, 3> size_mix{};
    std::array<float, k_max_channels> gains{};
    std::array<float, k_max_channels> auxiliary{};
    std::array<int, k_max_channels> channel_indices{};
    std::array<uintptr_t, 24> stack{};
    int stack_count{};
    int internal_channels{};
};

std::mutex g_records_mutex;
std::vector<GainRecord> g_records;
std::atomic<bool> g_capture{false};
std::atomic<bool> g_adm_capture{false};
std::atomic<uint64_t> g_calls{0};
GainFunction g_original{};
uintptr_t* g_slot{};
uintptr_t g_original_pointer{};
uintptr_t g_image_base{};
uintptr_t g_virtual_base{};
vm_prot_t g_slot_protection{};
std::size_t g_max_records{2048};
QString g_trace_path;
QPointer<QTimer> g_server;

template <typename Value> Value field(const void* object, std::size_t offset) {
    Value value{};
    std::memcpy(&value, static_cast<const char*>(object) + offset, sizeof(value));
    return value;
}

QJsonArray floats(const float* values, std::size_t size) {
    QJsonArray result;
    for (std::size_t i = 0; i < size; ++i)
        result.append(static_cast<double>(values[i]));
    return result;
}

QString hex_address(uintptr_t value) {
    return QStringLiteral("0x") + QString::number(value, 16);
}

#include "dar_gain_oar_capture.h"

void capture_gain(void* self, float* output, int snap, float x, float y, float z, float size) {
    // This is the method's normal call. Capture performs no other renderer
    // calls; all inspection happens after the original implementation returns.
    g_original(self, output, snap, x, y, z, size);
    if (!g_capture.load(std::memory_order_acquire))
        return;
    const auto sequence = g_calls.fetch_add(1, std::memory_order_relaxed);
    const auto channels = field<uint64_t>(self, 320);
    if (channels == 0 || channels > k_max_channels)
        return;
    const std::array<float, 4> arguments{x, y, z, size};
    std::lock_guard<std::mutex> lock(g_records_mutex);
    if (g_records.size() >= g_max_records)
        return;
    // Identical repeated control blocks need no additional snapshot.
    for (auto it = g_records.rbegin(); it != g_records.rend(); ++it) {
        if (it->object == reinterpret_cast<uintptr_t>(self)) {
            if (it->arguments == arguments && it->snap == snap)
                return;
            break;
        }
    }
    GainRecord record;
    record.sequence = sequence;
    pthread_threadid_np(nullptr, &record.thread);
    record.object = reinterpret_cast<uintptr_t>(self);
    record.output = reinterpret_cast<uintptr_t>(output);
    record.channels = channels;
    record.snap = snap;
    record.arguments = arguments;
    const auto* position = field<const float*>(self, 280);
    std::memcpy(record.position.data(), position, sizeof(record.position));
    std::memcpy(record.size_mix.data(), static_cast<const char*>(self) + 304, sizeof(record.size_mix));
    std::memcpy(record.gains.data(), output, channels * sizeof(float));
    const auto* auxiliary = field<const float*>(self, 256);
    std::memcpy(record.auxiliary.data(), auxiliary, channels * sizeof(float));
    const auto* room = field<const void*>(self, 24);
    record.room = reinterpret_cast<uintptr_t>(room);
    record.internal_channels = field<int>(room, 408);
    if (record.internal_channels > 0 && record.internal_channels <= static_cast<int>(k_max_channels)) {
        const auto* indices = field<const int*>(room, 56);
        std::memcpy(record.channel_indices.data(), indices, record.internal_channels * sizeof(int));
    }
    std::array<void*, 24> stack{};
    record.stack_count = backtrace(stack.data(), static_cast<int>(stack.size()));
    for (int i = 0; i < record.stack_count; ++i)
        record.stack[static_cast<std::size_t>(i)] = reinterpret_cast<uintptr_t>(stack[static_cast<std::size_t>(i)]);
    g_records.push_back(record);
}

bool write_slot(uintptr_t value, QString& error) {
    const auto page_size = static_cast<uintptr_t>(getpagesize());
    const auto page = reinterpret_cast<uintptr_t>(g_slot) & ~(page_size - 1);
    if (mprotect(reinterpret_cast<void*>(page), page_size, PROT_READ | PROT_WRITE) != 0) {
        error = QStringLiteral("could not make the vtable page writable");
        return false;
    }
    __atomic_store_n(g_slot, value, __ATOMIC_SEQ_CST);
    if (mprotect(reinterpret_cast<void*>(page), page_size, g_slot_protection) != 0) {
        error = QStringLiteral("could not restore the vtable page protection");
        return false;
    }
    return true;
}

bool install_hook(const QJsonObject& config, QString& error) {
    g_image_base = reinterpret_cast<uintptr_t>(_dyld_get_image_header(0));
    g_virtual_base = config.value("image_base").toString().toULongLong(nullptr, 0);
    if (config.contains("hooks"))
        return oar_capture::install(config, error);
    const auto slot = config.value("vtable_slot").toString().toULongLong(nullptr, 0);
    const auto function = config.value("gain_function").toString().toULongLong(nullptr, 0);
    g_slot = reinterpret_cast<uintptr_t*>(g_image_base + slot - g_virtual_base);
    g_original_pointer = __atomic_load_n(g_slot, __ATOMIC_SEQ_CST);
    if (g_original_pointer != g_image_base + function - g_virtual_base) {
        error = QStringLiteral("vtable method does not match the identified Renderer function");
        g_slot = nullptr;
        return false;
    }
    mach_vm_address_t region = reinterpret_cast<mach_vm_address_t>(g_slot);
    mach_vm_size_t length = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const auto status = mach_vm_region(mach_task_self(),
                                       &region,
                                       &length,
                                       VM_REGION_BASIC_INFO_64,
                                       reinterpret_cast<vm_region_info_t>(&info),
                                       &count,
                                       &object);
    if (object != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), object);
    if (status != KERN_SUCCESS) {
        error = QStringLiteral("could not read vtable page protection");
        g_slot = nullptr;
        return false;
    }
    g_slot_protection = info.protection;
    g_original = reinterpret_cast<GainFunction>(g_original_pointer);
    g_max_records = static_cast<std::size_t>(config.value("max_records").toInt(2048));
    g_records.reserve(g_max_records);
    return write_slot(reinterpret_cast<uintptr_t>(&capture_gain), error);
}

QJsonObject finish_capture() {
    g_capture.store(false, std::memory_order_release);
    g_adm_capture.store(false, std::memory_order_release);
    if (oar_capture::hook_count) {
        const auto report = oar_capture::report();
        const bool written = write_response(g_trace_path, report);
        return QJsonObject{{"ok", report.value("hook_restored").toBool() && written},
                           {"hook_restored", report.value("hook_restored")},
                           {"records", report.value("records").toArray().size()},
                           {"trace_written", written},
                           {"error", report.value("error")}};
    }
    QString error;
    bool restored = true;
    if (g_slot) {
        restored = write_slot(g_original_pointer, error);
        if (restored)
            restored = __atomic_load_n(g_slot, __ATOMIC_SEQ_CST) == g_original_pointer;
    }
    QJsonArray records;
    {
        std::lock_guard<std::mutex> lock(g_records_mutex);
        for (const auto& record : g_records) {
            QJsonArray stack;
            for (int i = 0; i < record.stack_count; ++i) {
                const auto pc = record.stack[static_cast<std::size_t>(i)];
                stack.append(QJsonObject{{"runtime", hex_address(pc)},
                                         {"main_image_address", hex_address(pc - g_image_base + g_virtual_base)}});
            }
            QJsonArray indices;
            for (int i = 0; i < record.internal_channels && i < static_cast<int>(k_max_channels); ++i)
                indices.append(record.channel_indices[static_cast<std::size_t>(i)]);
            records.append(QJsonObject{{"sequence", static_cast<qint64>(record.sequence)},
                                       {"thread_id", static_cast<qint64>(record.thread)},
                                       {"object", hex_address(record.object)},
                                       {"room", hex_address(record.room)},
                                       {"output", hex_address(record.output)},
                                       {"channels", static_cast<int>(record.channels)},
                                       {"snap", record.snap},
                                       {"arguments", floats(record.arguments.data(), 4)},
                                       {"effective_position", floats(record.position.data(), 3)},
                                       {"size_and_mix", floats(record.size_mix.data(), 3)},
                                       {"gains", floats(record.gains.data(), record.channels)},
                                       {"auxiliary_gains", floats(record.auxiliary.data(), record.channels)},
                                       {"channel_indices", indices},
                                       {"stack", stack}});
        }
    }
    const QJsonObject report{{"transport", "resident_vtable_capture"},
                             {"hook_restored", restored},
                             {"error", error.isEmpty() ? QJsonValue::Null : QJsonValue(error)},
                             {"total_calls", static_cast<qint64>(g_calls.load())},
                             {"load_base", hex_address(g_image_base)},
                             {"records", records}};
    const bool written = write_response(g_trace_path, report);
    return QJsonObject{{"ok", restored && written},
                       {"hook_restored", restored},
                       {"records", records.size()},
                       {"trace_written", written},
                       {"error", error}};
}

QJsonObject resident_action(const QJsonObject& request, const Gateways& gateways) {
    const auto action = request.value("action").toString();
    if (action == "start_export")
        g_capture.store(g_original != nullptr || oar_capture::enabled, std::memory_order_release);
    if (action == "open_master")
        g_adm_capture.store(oar_capture::enabled, std::memory_order_release);
    if (action == "restore_master" || action == "resident_shutdown")
        g_adm_capture.store(false, std::memory_order_release);
    if (action == "restore_exporter")
        g_capture.store(false, std::memory_order_release);
    if (action == "gain_trace_finish")
        return finish_capture();
    if (action == "resident_shutdown") {
        g_capture.store(false, std::memory_order_release);
        QString error;
        if (!oar_capture::restore(error) || (g_slot && !write_slot(g_original_pointer, error)))
            return fail(error);
        if (g_server) {
            g_server->stop();
            g_server->deleteLater();
            g_server = nullptr;
        }
        return QJsonObject{{"ok", true}};
    }
    return perform(request, gateways);
}
} // namespace

extern "C" __attribute__((visibility("default"))) bool dar_resident_start(const char* config_path) {
    QFile input(QString::fromUtf8(config_path));
    if (!input.open(QFile::ReadOnly))
        return false;
    const auto config = QJsonDocument::fromJson(input.readAll()).object();
    const auto gateways = find_gateways();
    if (!gateways.parent || g_server)
        return false;
    QTimer::singleShot(0, gateways.parent, [config, gateways]() {
        QString error;
        bool installed = true;
        g_trace_path = config.value("trace_file").toString();
        if (config.value("capture").toBool())
            installed = install_hook(config, error);
        if (!installed) {
            QString rollback_error;
            const bool restored =
                oar_capture::restore(rollback_error) && (!g_slot || write_slot(g_original_pointer, rollback_error));
            auto result = fail(error);
            result.insert("hook_restored", restored);
            result.insert("rollback_error", rollback_error);
            write_response(config.value("ready_response").toString(), result);
            return;
        }
        auto* timer = new QTimer(gateways.parent);
        g_server = timer;
        timer->setInterval(10);
        const QString inbox = config.value("inbox").toString();
        QObject::connect(timer, &QTimer::timeout, timer, [inbox, gateways]() {
            if (!QFileInfo(QFileInfo(inbox).absolutePath()).isDir()) {
                resident_action(QJsonObject{{"action", "resident_shutdown"}}, gateways);
                return;
            }
            QFile request_file(inbox);
            if (!request_file.open(QFile::ReadOnly))
                return;
            const auto request = QJsonDocument::fromJson(request_file.readAll()).object();
            request_file.close();
            QFile::remove(inbox);
            auto result = resident_action(request, gateways);
            write_response(request.value("response").toString(), result);
        });
        timer->start();
        write_response(config.value("ready_response").toString(), QJsonObject{{"ok", true}, {"resident", true}});
    });
    return true;
}
