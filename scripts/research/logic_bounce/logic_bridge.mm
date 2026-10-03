// Private, version-pinned research bridge for the user's installed Logic Pro.
// Requests run in the host's main run loop after the debugger detaches.
#include <array>
#include <atomic>
#include <dlfcn.h>
#include <initializer_list>
#include <pthread.h>
#include <unistd.h>

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>

struct MraadmClock {
    int64_t value;
};
@interface NSObject (MradmLogicPrivate)
- (void)preSaveNewDocument;
- (void)setDefaultAutoSaveName:(NSString*)name;
- (id)initWithFileURL:(NSURL*)url;
- (int)importADMIntoSong:(void*)song atClock:(MraadmClock)clock changeSongSettings:(BOOL)change;
- (NSInteger)srcNumFrames;
- (NSDictionary*)objectChannelsForPackFormat;
- (NSDictionary*)bedChannelsForPackFormat;
- (BOOL)isPreSaved;
- (void)removePreSaveAndDeleteProject:(BOOL)remove;
- (id)docManager;
- (void)docManagerDidBecomeActive;
@end
#import <objc/message.h>
#import <objc/runtime.h>

template <class T> static T read_at(const void* source, size_t offset) {
    T result{};
    memcpy(&result, static_cast<const unsigned char*>(source) + offset, sizeof(result));
    return result;
}
template <class T> static void write_at(void* destination, size_t offset, T value) {
    memcpy(static_cast<unsigned char*>(destination) + offset, &value, sizeof(value));
}

static uintptr_t logic_base() {
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        NSString* path = @(_dyld_get_image_name(i));
        if ([path hasSuffix:@"/Logic.framework/Versions/A/Logic"])
            return reinterpret_cast<uintptr_t>(_dyld_get_image_header(i));
    }
    return 0;
}

static uintptr_t audio_engine_base() {
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        NSString* path = @(_dyld_get_image_name(i));
        if ([path hasSuffix:@"/MAAudioEngine.framework/Versions/A/MAAudioEngine"])
            return reinterpret_cast<uintptr_t>(_dyld_get_image_header(i));
    }
    return 0;
}

static NSDictionary* bounce_request(NSDictionary* request, bool execute, NSDocument* document_override = nil) {
    NSDocument* document = document_override ?: [NSDocumentController sharedDocumentController].currentDocument;
    if (!document && [NSApp.delegate respondsToSelector:sel_registerName("recentDocument")]) {
        id recent = ((id (*)(id, SEL)) objc_msgSend)(NSApp.delegate, sel_registerName("recentDocument"));
        if ([recent isKindOfClass:[NSDocument class]])
            document = recent;
    }
    if (!document && [NSDocumentController sharedDocumentController].documents.count == 1)
        document = [NSDocumentController sharedDocumentController].documents.firstObject;
    if (!document || ![document.displayName isEqual:request[@"expected_document"]])
        return @{@"ok": @NO, @"error": @"Current document does not match the request"};
    if (NSApp.modalWindow)
        return @{@"ok": @NO, @"error": @"Close the current modal dialog before bouncing"};
    NSString* output = request[@"audio_output"];
    if (![output isKindOfClass:[NSString class]] || !output.isAbsolutePath ||
        [NSFileManager.defaultManager fileExistsAtPath:output])
        return @{@"ok": @NO, @"error": @"Audio output must be a new absolute path"};
    uintptr_t base = logic_base();
    SEL song_selector = sel_registerName("song");
    if (!base || ![document respondsToSelector:song_selector])
        return @{@"ok": @NO, @"error": @"Logic model entry point is unavailable"};
    void* song = ((void* (*) (id, SEL)) objc_msgSend)(document, song_selector);
    auto get_outputs = reinterpret_cast<void* (*) (int)>(base + 0x2c8a10);
    void* outputs = get_outputs(1);
    if (!outputs || read_at<int16_t>(outputs, 0xf0) < 2 || !read_at<void*>(outputs, 0x50))
        return @{@"ok": @NO, @"error": @"Output 1-2 is unavailable"};
    auto* left = read_at<unsigned char*>(outputs, 0x50);
    void* right = left + 0x148;
    uint64_t start = [request[@"start_clock"] unsignedLongLongValue];
    uint64_t end = [request[@"end_clock"] unsignedLongLongValue];
    if ((!start != !end) || (start && end <= start))
        return @{@"ok": @NO, @"error": @"A nonempty clock range is required"};
    using Constructor = void (*)(void*, void*, long, void*, void*, uint64_t, uint64_t);
    using Destructor = void (*)(void*);
    using Bounce = bool (*)(void*, void*, bool);
    auto construct = reinterpret_cast<Constructor>(base + 0xfc76f8);
    auto destroy = reinterpret_cast<Destructor>(base + 0xfc8a8c);
    auto bounce = reinterpret_cast<Bounce>(base + 0xfc8d44);
    using FileConstructor = void (*)(void*, CFURLRef);
    auto file_construct = reinterpret_cast<FileConstructor>(dlsym(RTLD_DEFAULT, "_ZN8CFileRefC1EPK7__CFURL"));
    auto file_destroy = reinterpret_cast<Destructor>(dlsym(RTLD_DEFAULT, "_ZN8CFileRefD1Ev"));
    if (!file_construct || !file_destroy)
        return @{@"ok": @NO, @"error": @"CFileRef entry points are unavailable"};
    alignas(16) std::array<unsigned char, 0x6470> params{};
    alignas(16) std::array<unsigned char, 0xb0> file{};
    // The constructor reads these bounce preferences and the native export can update them.
    // The regions are pinned to Logic 12.3.1/6682 and restored even for a preparation-only request.
    std::array<unsigned char, 8> saved_general{};
    std::array<unsigned char, 22> saved_format{};
    std::array<unsigned char, 8> saved_codec{};
    auto* general_prefs = reinterpret_cast<void*>(base + 0x276db62);
    auto* format_prefs = reinterpret_cast<void*>(base + 0x276dbf2);
    auto* codec_prefs = reinterpret_cast<void*>(base + 0x25ce238);
    memcpy(saved_general.data(), general_prefs, saved_general.size());
    memcpy(saved_format.data(), format_prefs, saved_format.size());
    memcpy(saved_codec.data(), codec_prefs, saved_codec.size());
    struct PreferenceGuard {
        void* general;
        void* format;
        void* codec;
        const std::array<unsigned char, 8>& saved_general;
        const std::array<unsigned char, 22>& saved_format;
        const std::array<unsigned char, 8>& saved_codec;
        void restore() const {
            memcpy(general, saved_general.data(), saved_general.size());
            memcpy(format, saved_format.data(), saved_format.size());
            memcpy(codec, saved_codec.data(), saved_codec.size());
        }
        ~PreferenceGuard() { restore(); }
    } preference_guard{general_prefs, format_prefs, codec_prefs, saved_general, saved_format, saved_codec};
    NSURL* url = [NSURL fileURLWithPath:output];
    file_construct(file.data(), (__bridge CFURLRef) url);
    construct(params.data(), song, 4, left, right, start, end);
    // Values come from the user's 32-bit float, interleaved, surround-enabled dialog.
    write_at<uint32_t>(file.data(), 0x68, 0x57415645U); // WAVE
    write_at<uint64_t>(file.data(), 0x70, 32);
    write_at<uint8_t>(file.data(), 0x79, 0);            // normalization disabled
    write_at<uint32_t>(file.data(), 0x7c, 0x7f7fffffU); // native unused normalization sentinel
    write_at<uint8_t>(file.data(), 0x80, 2);            // automatic bounce (0 real-time, 1 offline, 2 automatic)
    write_at<uint8_t>(file.data(), 0x81, 0);            // no second cycle pass
    write_at<uint32_t>(file.data(), 0x84, 0);           // no dither
    write_at<uint16_t>(file.data(), 0x88, 1);           // PCM destination
    write_at<uint16_t>(file.data(), 0x8a, 0);           // interleaved
    write_at<uint8_t>(file.data(), 0x8c, 0);            // no audio tail
    write_at<uint8_t>(file.data(), 0xa0, 1);            // programmatic file destination
    write_at<uint8_t>(file.data(), 0xa1, 0);            // retain surround bounce
    write_at<uint8_t>(params.data(), 0x6432, 1);
    write_at<uint8_t>(params.data(), 0x645d, 1); // tempo information
    write_at<uint64_t>(params.data(), 0x6448, 48000);
    NSDictionary* prepared = @{
        @"sample_rate": @(read_at<uint64_t>(params.data(), 0x6448)),
        @"start_clock": @(read_at<uint64_t>(params.data(), 0x63e0)),
        @"end_clock": @(read_at<uint64_t>(params.data(), 0x63e8)),
        @"surround": @(params[0x6432]),
        @"configuration_tail": [[NSData dataWithBytes:params.data() + 0x6420
                                               length:0x48] base64EncodedStringWithOptions:0]
    };
    bool succeeded = !execute || bounce(params.data(), file.data(), true);
    destroy(params.data());
    file_destroy(file.data());
    preference_guard.restore();
    bool restored = memcmp(general_prefs, saved_general.data(), saved_general.size()) == 0 &&
                    memcmp(format_prefs, saved_format.data(), saved_format.size()) == 0 &&
                    memcmp(codec_prefs, saved_codec.data(), saved_codec.size()) == 0;
    return @{
        @"ok": @(succeeded),
        @"executed": @(execute),
        @"prepared": prepared,
        @"output": output,
        @"document": document.displayName ?: @"",
        @"document_edited": @(document.documentEdited),
        @"preferences_restored": @(restored)
    };
}

static NSDictionary* identity(id object) {
    if (!object)
        return @{};
    return @{
        @"class": NSStringFromClass([object class]),
        @"address": [NSString stringWithFormat:@"%p", (__bridge void*) object]
    };
}

static NSDictionary* inventory(id object) {
    if (!object)
        return @{};
    NSMutableDictionary* out = [identity(object) mutableCopy];
    NSMutableArray* ivars = [NSMutableArray array];
    NSMutableArray* methods = [NSMutableArray array];
    for (Class cls = [object class]; cls && cls != [NSObject class]; cls = class_getSuperclass(cls)) {
        if ([NSStringFromClass(cls) hasPrefix:@"NS"])
            break;
        unsigned count = 0;
        Ivar* fields = class_copyIvarList(cls, &count);
        for (unsigned i = 0; i < count; ++i) {
            const char* type = ivar_getTypeEncoding(fields[i]);
            ptrdiff_t offset = ivar_getOffset(fields[i]);
            NSMutableDictionary* field = [@{
                @"name": @(ivar_getName(fields[i])),
                @"type": @(type),
                @"offset": @(offset),
                @"owner": NSStringFromClass(cls)
            } mutableCopy];
            if (type[0] == '@') {
                id value = object_getIvar(object, fields[i]);
                field[@"value"] = identity(value);
                if ([value isKindOfClass:[NSString class]] || [value isKindOfClass:[NSNumber class]])
                    field[@"text"] = [value description];
            } else {
                NSUInteger size = 0;
                NSGetSizeAndAlignment(type, &size, nullptr);
                size = MIN(size, 2048U);
                const unsigned char* bytes = (const unsigned char*) (__bridge void*) object + offset;
                NSMutableString* hex = [NSMutableString string];
                for (NSUInteger j = 0; j < size; ++j)
                    [hex appendFormat:@"%02x", bytes[j]];
                field[@"bytes"] = hex;
            }
            [ivars addObject:field];
        }
        free(fields);
        Method* entries = class_copyMethodList(cls, &count);
        for (unsigned i = 0; i < count; ++i)
            [methods addObject:@{
                @"selector": NSStringFromSelector(method_getName(entries[i])),
                @"encoding": @(method_getTypeEncoding(entries[i])),
                @"owner": NSStringFromClass(cls)
            }];
        free(entries);
    }
    out[@"ivars"] = ivars;
    out[@"methods"] = methods;
    return out;
}

static NSDictionary* inspect() {
    NSMutableArray* documents = [NSMutableArray array];
    for (NSDocument* doc in [NSDocumentController sharedDocumentController].documents) {
        NSMutableDictionary* row = [inventory(doc) mutableCopy];
        row[@"file"] = doc.fileURL.path ?: @"";
        row[@"name"] = doc.displayName ?: @"";
        row[@"edited"] = @(doc.documentEdited);
        if ([doc respondsToSelector:sel_registerName("song")]) {
            void* song = ((void* (*) (id, SEL)) objc_msgSend)(doc, sel_registerName("song"));
            row[@"song_pointer"] = [NSString stringWithFormat:@"%p", song];
        }
        if ([doc respondsToSelector:sel_registerName("docManager")]) {
            id manager = ((id (*)(id, SEL)) objc_msgSend)(doc, sel_registerName("docManager"));
            row[@"doc_manager"] = inventory(manager);
        }
        [documents addObject:row];
    }
    NSMutableArray* windows = [NSMutableArray array];
    for (NSWindow* window in NSApp.windows) {
        if (!window.visible && window != NSApp.modalWindow)
            continue;
        NSMutableDictionary* entry = [@{
            @"title": window.title ?: @"",
            @"modal": @(window == NSApp.modalWindow),
            @"window": identity(window),
            @"controller": inventory(window.windowController),
            @"delegate": inventory(window.delegate)
        } mutableCopy];
        if ([NSStringFromClass([window.windowController class]) isEqual:@"BounceController"]) {
            id controller = window.windowController;
            Ivar ivar = class_getInstanceVariable([controller class], "pBounce");
            const unsigned char* record = nullptr;
            if (ivar)
                memcpy(
                    &record, (const unsigned char*) (__bridge void*) controller + ivar_getOffset(ivar), sizeof(record));
            if (record) {
                NSMutableDictionary* params = [NSMutableDictionary dictionary];
                for (NSUInteger offset : {0UL, 8UL, 0x10UL, 0x18UL, 0x63b8UL, 0x63c0UL, 0x63e0UL, 0x63e8UL, 0x6448UL}) {
                    uint64_t value;
                    memcpy(&value, record + offset, 8);
                    params[[NSString stringWithFormat:@"%lx", offset]] = [NSString stringWithFormat:@"%llx", value];
                }
                params[@"tail"] = [[NSData dataWithBytes:record + 0x6400 length:0x70] base64EncodedStringWithOptions:0];
                void* document = nullptr;
                memcpy(&document, record + 0x18, 8);
                params[@"document"] = inventory((__bridge id) document);
                entry[@"bounce_record"] = params;
            }
        }
        [windows addObject:entry];
    }
    return @{
        @"ok": @YES,
        @"documents": documents,
        @"windows": windows,
        @"application_delegate": inventory(NSApp.delegate)
    };
}

static NSDictionary* inspect_document_factory() {
    NSDocumentController* controller = NSDocumentController.sharedDocumentController;
    NSMutableArray* import_methods = [NSMutableArray array];
    Class importer = NSClassFromString(@"ADMImporter");
    unsigned count = 0;
    Method* methods = class_copyMethodList(importer, &count);
    for (unsigned i = 0; i < count; ++i)
        [import_methods addObject:@{
            @"selector": NSStringFromSelector(method_getName(methods[i])),
            @"encoding": @(method_getTypeEncoding(methods[i]))
        }];
    free(methods);
    return @{
        @"ok": @YES,
        @"controller": inventory(controller),
        @"default_type": controller.defaultType ?: @"",
        @"document_class": NSStringFromClass([controller documentClassForType:controller.defaultType]) ?: @"",
        @"import_methods": import_methods
    };
}

static const void* probe_document_key() {
    return reinterpret_cast<const void*>(sel_registerName("mradmLogicHeadlessProbeDocument"));
}

static NSArray* document_state();
static uint64_t clock_for_frames(void* song, uint64_t start, int64_t frames);
static NSDictionary* inspect_mixers();

static void* document_song(NSDocument* document) {
    return document && [document respondsToSelector:sel_registerName("song")]
               ? ((void* (*) (id, SEL)) objc_msgSend)(document, sel_registerName("song"))
               : nullptr;
}

static void* active_audio_song() {
    void* owner = read_at<void*>(reinterpret_cast<void*>(logic_base()), 0x275ff40);
    void* atomic = owner ? read_at<void*>(owner, 0) : nullptr;
    return atomic ? read_at<void*>(atomic, 0) : nullptr;
}

static bool activate_audio_document(NSDocument* document) {
    if (!document || !document_song(document))
        return false;
    [[document docManager] docManagerDidBecomeActive];
    if (active_audio_song() != document_song(document))
        reinterpret_cast<void (*)(void*, bool, bool)>(logic_base() + 0x82154c)(document_song(document), false, true);
    return active_audio_song() == document_song(document);
}

static NSDictionary* create_probe_document() {
    if (objc_getAssociatedObject(NSApp, probe_document_key()))
        return @{@"ok": @NO, @"error": @"A probe document already exists"};
    if (NSApp.modalWindow)
        return @{@"ok": @NO, @"error": @"A modal dialog is open; no project was created"};
    if (objc_getAssociatedObject(NSApp, sel_registerName("mradmPendingRendererRestore")))
        return @{@"ok": @NO, @"error": @"Run recovery to finish restoring the previous host renderer first"};
    NSDocumentController* controller = NSDocumentController.sharedDocumentController;
    NSArray* original_state = document_state();
    NSDocument* original = controller.currentDocument;
    if (original && ![controller.documents containsObject:original])
        original = nil;
    if (!original && controller.documents.count == 1)
        original = controller.documents.firstObject;
    if (!original || !activate_audio_document(original))
        return @{
            @"ok": @NO,
            @"error": @"A single initialized host project must be available",
            @"host_name": original.displayName ?: @"",
            @"documents": original_state,
            @"host_song": [NSString stringWithFormat:@"%p", document_song(original)],
            @"active_song": [NSString stringWithFormat:@"%p", active_audio_song()]
        };
    NSDictionary* original_renderer = inspect_mixers();
    NSMutableArray* before = [NSMutableArray array];
    for (NSDocument* document in controller.documents)
        [before addObject:document.displayName ?: @""];
    NSError* error = nil;
    NSDocument* document = [controller makeUntitledDocumentOfType:controller.defaultType error:&error];
    if (!document)
        return @{@"ok": @NO, @"error": error.localizedDescription ?: @"Could not create document"};
    [document setDefaultAutoSaveName:[@"mradm-adm-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    if (![controller.documents containsObject:document])
        [controller addDocument:document];
    objc_setAssociatedObject(NSApp, probe_document_key(), document, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(
        document, sel_registerName("mradmOriginalDocumentState"), original_state, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(
        document, sel_registerName("mradmOriginalDocument"), original, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(
        document, sel_registerName("mradmOriginalRenderer"), original_renderer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    void* song = [document respondsToSelector:sel_registerName("song")]
                     ? ((void* (*) (id, SEL)) objc_msgSend)(document, sel_registerName("song"))
                     : nullptr;
    return @{
        @"ok": @YES,
        @"documents_before": before,
        @"document": identity(document),
        @"song_pointer": [NSString stringWithFormat:@"%p", song],
        @"window_controller_count": @(document.windowControllers.count),
        @"name": document.displayName ?: @"",
        @"file_url": document.fileURL.path ?: @""
    };
}

static NSDictionary* activate_probe_audio() {
    NSDocument* probe = objc_getAssociatedObject(NSApp, probe_document_key());
    bool ok = activate_audio_document(probe);
    return @{
        @"ok": @(ok),
        @"audio_graph_matches_document": @(ok),
        @"error": ok ? @"" : @"Probe audio graph activation failed",
        @"window_controller_count": @(probe.windowControllers.count)
    };
}

static NSDictionary* import_probe_adm(NSDictionary* request) {
    NSDocument* document = objc_getAssociatedObject(NSApp, probe_document_key());
    if (!document)
        return @{@"ok": @NO, @"error": @"No headless probe document exists"};
    NSString* path = request[@"adm"];
    if (![path isKindOfClass:[NSString class]] || !path.isAbsolutePath ||
        ![NSFileManager.defaultManager fileExistsAtPath:path])
        return @{@"ok": @NO, @"error": @"ADM input path is invalid"};
    [document preSaveNewDocument];
    void* song = ((void* (*) (id, SEL)) objc_msgSend)(document, sel_registerName("song"));
    id importer = [[NSClassFromString(@"ADMImporter") alloc] initWithFileURL:[NSURL fileURLWithPath:path]];
    if (!importer)
        return @{@"ok": @NO, @"error": @"ADM importer initialization failed"};
    int code = [importer importADMIntoSong:song atClock:MraadmClock{0x960000000000LL} changeSongSettings:YES];
    objc_setAssociatedObject(document,
                             sel_registerName("mradmAdmSourceFrames"),
                             @([importer srcNumFrames]),
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return @{
        @"ok": @YES,
        @"import_code": @(code),
        @"source_frames": @([importer srcNumFrames]),
        @"object_pack_count": @([importer objectChannelsForPackFormat].count),
        @"bed_pack_count": @([importer bedChannelsForPackFormat].count),
        @"name": document.displayName ?: @"",
        @"file_url": document.fileURL.path ?: @"",
        @"window_controller_count": @(document.windowControllers.count)
    };
}

static NSDictionary* inspect_mixers() {
    uintptr_t base = audio_engine_base();
    if (!base)
        return @{@"ok": @NO, @"error": @"Audio engine is unavailable"};
    auto* container = reinterpret_cast<unsigned char*>(base + 0xf7b040);
    auto* begin = read_at<unsigned char*>(container, 0);
    auto* end = read_at<unsigned char*>(container, 8);
    uintptr_t bytes = reinterpret_cast<uintptr_t>(end) - reinterpret_cast<uintptr_t>(begin);
    if (!base || bytes % 24 || bytes / 24 > 64)
        return @{@"ok": @NO, @"error": @"Unexpected sink registry layout"};
    auto find = reinterpret_cast<void* (*) (void*, const void*)>(base + 0x841cf0);
    auto string_value = reinterpret_cast<CFStringRef (*)(void*, long)>(base + 0x89e7d0);
    auto position_value = reinterpret_cast<long (*)(void*, long)>(base + 0x89e5fc);
    NSMutableArray* rows = [NSMutableArray array];
    for (uintptr_t i = 0; i < bytes / 24; ++i) {
        auto* key = begin + i * 24;
        auto* node = find(container + 24, key);
        if (!node)
            continue;
        void* mixer = read_at<void*>(node, 0x28);
        Dl_info symbol{};
        if (!mixer || !dladdr(read_at<void*>(mixer, 0), &symbol) || !symbol.dli_sname ||
            !strstr(symbol.dli_sname, "DolbyMixer"))
            continue;
        auto* update_mutex = reinterpret_cast<pthread_mutex_t*>(static_cast<unsigned char*>(mixer) + 0x2e8);
        pthread_mutex_lock(update_mutex);
        int update_state = read_at<int>(mixer, 0x2e0);
        pthread_mutex_unlock(update_mutex);
        bool long_key = key[23] & 0x80;
        const char* text = long_key ? read_at<const char*>(key, 0) : reinterpret_cast<const char*>(key);
        size_t length = long_key ? read_at<size_t>(key, 8) : key[23];
        NSString* key_string =
            length < 256 ? [[NSString alloc] initWithBytes:text length:length encoding:NSUTF8StringEncoding] : nil;
        NSMutableArray* values = [NSMutableArray array];
        for (long parameter : {1L, 2L, 3L, 4L}) {
            NSString* value = CFBridgingRelease(string_value(mixer, parameter));
            [values addObject:@{
                @"id": @(parameter),
                @"position": @(position_value(mixer, parameter)),
                @"text": value ?: @""
            }];
        }
        [rows addObject:@{
            @"key": key_string ?: @"",
            @"mixer": [NSString stringWithFormat:@"%p", mixer],
            @"update_state": @(update_state),
            @"sources_pending": @(read_at<uint8_t>(mixer, 0x328)),
            @"idle_disabled": @(read_at<uint8_t>(mixer, 0x329)),
            @"contexts_pending": @(read_at<uint8_t>(mixer, 0x32a)),
            @"has_render_engine": @(read_at<void*>(mixer, 0x350) != nullptr),
            @"parameters": values
        }];
    }
    return @{@"ok": @YES, @"registered_sinks": @(bytes / 24), @"mixers": rows};
}

static NSDictionary* inspect_audio_state() {
    NSMutableArray* documents = [NSMutableArray array];
    for (NSDocument* document in NSDocumentController.sharedDocumentController.documents)
        [documents addObject:@{
            @"name": document.displayName ?: @"",
            @"song": [NSString stringWithFormat:@"%p", document_song(document)],
            @"active": @(active_audio_song() == document_song(document)),
            @"edited": @(document.documentEdited),
            @"song_loading_guard": @(read_at<int32_t>(document_song(document), 0x678)),
            @"window_controller_count": @(document.windowControllers.count)
        }];
    return @{
        @"ok": @YES,
        @"documents": documents,
        @"active_song": [NSString stringWithFormat:@"%p", active_audio_song()],
        @"probe_exists": @(objc_getAssociatedObject(NSApp, probe_document_key()) != nil),
        @"modal_present": @(NSApp.modalWindow != nil),
        @"activation_guard": @(read_at<int32_t>(reinterpret_cast<void*>(logic_base()), 0x2633c4c)),
        @"mixers": inspect_mixers()
    };
}

static bool renderer_labels_match(NSDictionary* mixer) {
    NSArray* parameters = mixer[@"parameters"];
    NSArray* labels = @[@"Apple Renderer", @"Generic", @"false", @"Music"];
    if (parameters.count != labels.count)
        return false;
    for (NSUInteger i = 0; i < labels.count; ++i)
        if (![parameters[i][@"text"] isEqual:labels[i]])
            return false;
    return true;
}

static bool mixer_is_ready(NSDictionary* mixer) {
    return mixer && [mixer[@"update_state"] intValue] == 0 && [mixer[@"sources_pending"] intValue] == 0 &&
           [mixer[@"contexts_pending"] intValue] == 0 && [mixer[@"idle_disabled"] intValue] == 0 &&
           [mixer[@"has_render_engine"] boolValue];
}

static NSDictionary* probe_readiness() {
    NSDocument* probe = objc_getAssociatedObject(NSApp, probe_document_key());
    NSDictionary* state = inspect_audio_state();
    NSArray* mixers = state[@"mixers"][@"mixers"];
    NSDictionary* mixer = mixers.count == 1 ? mixers.firstObject : nil;
    bool ready = probe && active_audio_song() == document_song(probe) &&
                 read_at<int32_t>(document_song(probe), 0x678) == 0 && ![state[@"modal_present"] boolValue] &&
                 [state[@"activation_guard"] intValue] == 0 && mixer && [mixer[@"update_state"] intValue] == 0 &&
                 [mixer[@"sources_pending"] intValue] == 0 && [mixer[@"contexts_pending"] intValue] == 0 &&
                 [mixer[@"idle_disabled"] intValue] == 0 && [mixer[@"has_render_engine"] boolValue] &&
                 renderer_labels_match(mixer);
    return @{@"ok": @YES, @"ready": @(ready), @"state": state};
}

static NSDictionary* configure_probe_renderer() {
    NSDocument* probe = objc_getAssociatedObject(NSApp, probe_document_key());
    if (!probe || active_audio_song() != document_song(probe))
        return @{@"ok": @NO, @"error": @"Probe must be the active audio document"};
    NSArray* mixers = inspect_mixers()[@"mixers"];
    if (mixers.count != 1)
        return @{@"ok": @NO, @"error": @"Expected one Atmos mixer"};
    void* mixer = reinterpret_cast<void*>(strtoull([mixers[0][@"mixer"] UTF8String], nullptr, 16));
    auto set = reinterpret_cast<void (*)(void*, long, long)>(audio_engine_base() + 0x89f450);
    auto get = reinterpret_cast<long (*)(void*, long)>(audio_engine_base() + 0x89e5fc);
    const std::array<long, 4> wanted{1, 0, 0, 0};
    for (long i = 0; i < 4; ++i)
        if (get(mixer, i + 1) != wanted[i])
            set(mixer, i + 1, wanted[i]);
    NSDictionary* configuration = inspect_mixers();
    bool ok = [configuration[@"mixers"] count] == 1 && renderer_labels_match(configuration[@"mixers"][0]);
    return @{
        @"ok": @(ok),
        @"renderer_configuration": configuration,
        @"error": ok ? @"" : @"Renderer setting readback failed"
    };
}

static NSDictionary* bounce_probe_document(NSDictionary* request) {
    NSDocument* probe = objc_getAssociatedObject(NSApp, probe_document_key());
    if (!probe)
        return @{@"ok": @NO, @"error": @"No probe document exists"};
    NSDictionary* readiness = probe_readiness();
    if (![readiness[@"ready"] boolValue])
        return @{@"ok": @NO, @"error": @"Atmos audio graph is not ready", @"readiness": readiness};
    NSMutableDictionary* bounce = [request mutableCopy];
    bounce[@"expected_document"] = probe.displayName;
    NSNumber* source_frames = objc_getAssociatedObject(probe, sel_registerName("mradmAdmSourceFrames"));
    if (![bounce[@"start_clock"] unsignedLongLongValue]) {
        uint64_t start = 0x960000000000ULL;
        uint64_t end = clock_for_frames(document_song(probe), start, source_frames.longLongValue);
        if (!end)
            return @{@"ok": @NO, @"error": @"Could not resolve source-length bounce clocks"};
        bounce[@"start_clock"] = @(start);
        bounce[@"end_clock"] = @(end);
    }
    NSMutableDictionary* report = [bounce_request(bounce, ![request[@"prepare_only"] boolValue], probe) mutableCopy];
    report[@"renderer_configuration"] = readiness[@"state"][@"mixers"];
    report[@"readiness"] = readiness;
    report[@"window_controller_count"] = @(probe.windowControllers.count);
    report[@"source_frames"] = source_frames ?: @0;
    return report;
}

static NSDictionary* restore_original_renderer() {
    const void* key = sel_registerName("mradmPendingRendererRestore");
    NSDictionary* saved = objc_getAssociatedObject(NSApp, key);
    if (!saved)
        return @{@"ok": @YES, @"ready": @YES, @"pending": @NO};
    NSDocument* original = saved[@"document"];
    if (active_audio_song() != document_song(original))
        return @{@"ok": @NO, @"error": @"Host audio document changed during renderer restoration"};
    NSArray* original_mixers = saved[@"mixers"];
    NSArray* after_mixers = inspect_mixers()[@"mixers"];
    bool restored = original_mixers.count == after_mixers.count && after_mixers.count <= 1;
    if (restored && after_mixers.count == 1) {
        if (!mixer_is_ready(after_mixers[0]))
            return @{@"ok": @YES, @"ready": @NO, @"pending": @YES};
        void* mixer = reinterpret_cast<void*>(strtoull([after_mixers[0][@"mixer"] UTF8String], nullptr, 16));
        auto set = reinterpret_cast<void (*)(void*, long, long)>(audio_engine_base() + 0x89f450);
        auto get = reinterpret_cast<long (*)(void*, long)>(audio_engine_base() + 0x89e5fc);
        for (NSDictionary* parameter in original_mixers[0][@"parameters"]) {
            long id = [parameter[@"id"] longValue], position = [parameter[@"position"] longValue];
            if (get(mixer, id) != position)
                set(mixer, id, position);
            restored = restored && get(mixer, id) == position;
        }
        NSArray* configured = inspect_mixers()[@"mixers"];
        restored = restored && configured.count == 1 && mixer_is_ready(configured[0]);
    }
    if (restored)
        objc_setAssociatedObject(NSApp, key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return @{@"ok": @YES, @"ready": @(restored), @"pending": @(!restored)};
}

static NSDictionary* discard_probe_document() {
    NSDocument* probe = objc_getAssociatedObject(NSApp, probe_document_key());
    if (!probe)
        return @{@"ok": @YES, @"discarded": @NO};
    NSString* path = probe.fileURL.path;
    NSArray* original_state = objc_getAssociatedObject(probe, sel_registerName("mradmOriginalDocumentState"));
    NSDocument* original = objc_getAssociatedObject(probe, sel_registerName("mradmOriginalDocument"));
    NSArray* original_mixers = objc_getAssociatedObject(probe, sel_registerName("mradmOriginalRenderer"))[@"mixers"];
    bool audio_restored = original && activate_audio_document(original);
    BOOL temporary = [probe isPreSaved];
    [probe updateChangeCount:NSChangeCleared];
    [probe close];
    if ([NSDocumentController.sharedDocumentController.documents containsObject:probe])
        [NSDocumentController.sharedDocumentController removeDocument:probe];
    if (temporary && path && [NSFileManager.defaultManager fileExistsAtPath:path])
        [probe removePreSaveAndDeleteProject:YES];
    objc_setAssociatedObject(NSApp, probe_document_key(), nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    if (original && original_mixers)
        objc_setAssociatedObject(NSApp,
                                 sel_registerName("mradmPendingRendererRestore"),
                                 @{@"document": original, @"mixers": original_mixers},
                                 OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    bool settings_restored = original_mixers && [restore_original_renderer()[@"ready"] boolValue];
    NSMutableArray* remaining = [NSMutableArray array];
    for (NSDocument* document in NSDocumentController.sharedDocumentController.documents)
        [remaining addObject:@{
            @"name": document.displayName ?: @"",
            @"edited": @(document.documentEdited),
            @"path": document.fileURL.path ?: @""
        }];
    return @{
        @"ok": @YES,
        @"discarded": @YES,
        @"temporary_project": @(temporary),
        @"temporary_path": path ?: @"",
        @"temporary_path_exists": @(path && [NSFileManager.defaultManager fileExistsAtPath:path]),
        @"remaining_documents": remaining,
        @"documents_before": original_state ?: @[],
        @"original_document_state_preserved": @([original_state isEqual:remaining]),
        @"renderer_settings_restored": @(settings_restored),
        @"original_audio_graph_restored": @(audio_restored && active_audio_song() == document_song(original))
    };
}

static NSArray* document_state() {
    NSMutableArray* state = [NSMutableArray array];
    for (NSDocument* document in NSDocumentController.sharedDocumentController.documents)
        [state addObject:@{
            @"name": document.displayName ?: @"",
            @"path": document.fileURL.path ?: @"",
            @"edited": @(document.documentEdited)
        }];
    return state;
}

static uint64_t clock_for_frames(void* song, uint64_t start, int64_t frames) {
    auto sample_at = reinterpret_cast<int64_t (*)(void*, void*, uint64_t, void*)>(logic_base() + 0x7abc8c);
    int64_t base = sample_at(song, nullptr, start, nullptr);
    if (frames <= 0 || base > INT64_MAX - frames)
        return 0;
    int64_t target = base + frames;
    uint64_t origin = start >> 16, low = origin, high = origin, step = 1;
    constexpr uint64_t limit = static_cast<uint64_t>(INT64_MAX) >> 16;
    while (sample_at(song, nullptr, high << 16, nullptr) < target) {
        if (step > (limit - origin) / 2)
            return 0;
        step *= 2;
        high = origin + step;
    }
    while (low < high) {
        uint64_t middle = low + (high - low) / 2;
        if (sample_at(song, nullptr, middle << 16, nullptr) < target)
            low = middle + 1;
        else
            high = middle;
    }
    return sample_at(song, nullptr, low << 16, nullptr) == target ? low << 16 : 0;
}

static void wake_application_loop() {
    // Wake AppKit's next-event wait as well as CFRunLoop. No keyboard/mouse event or UI action is synthesized.
    NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSZeroPoint
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:event atStart:NO];
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

extern "C" bool logic_queue_request(const char* request_path);

static NSDictionary* install_server(NSDictionary* request) {
    NSString* directory = request[@"server_directory"];
    struct stat info{};
    if (![directory isKindOfClass:NSString.class] || !directory.isAbsolutePath ||
        lstat(directory.fileSystemRepresentation, &info) || !S_ISDIR(info.st_mode) || info.st_uid != getuid() ||
        (info.st_mode & 0077))
        return @{@"ok": @NO, @"error": @"Command directory must be owned by this user with mode 0700"};
    const void* key = sel_registerName("mradmLogicCommandServer");
    dispatch_source_t old = objc_getAssociatedObject(NSApp, key);
    if (old)
        dispatch_source_cancel(old);
    dispatch_queue_t queue = dispatch_queue_create("mradm.logic.commands", DISPATCH_QUEUE_SERIAL);
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    dispatch_source_set_timer(
        timer, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), 100 * NSEC_PER_MSEC, 20 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(timer, ^{
      @autoreleasepool {
          NSArray* names = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:directory error:nil]
              sortedArrayUsingSelector:@selector(compare:)];
          for (NSString* name in names) {
              if (![name.pathExtension isEqual:@"request"])
                  continue;
              NSString* path = [directory stringByAppendingPathComponent:name];
              if (logic_queue_request(path.fileSystemRepresentation))
                  [NSFileManager.defaultManager removeItemAtPath:path error:nil];
              break;
          }
      }
    });
    objc_setAssociatedObject(NSApp, key, timer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    dispatch_resume(timer);
    NSDictionary* ready = @{
        @"ok": @YES,
        @"pid": @(getpid()),
        @"server_directory": directory,
        @"protocol": @1,
        @"process_start": request[@"process_start"] ?: @""
    };
    NSString* registry = [directory.stringByDeletingLastPathComponent
        stringByAppendingPathComponent:[NSString stringWithFormat:@"logic-%d-server.json", getpid()]];
    [[NSJSONSerialization dataWithJSONObject:ready options:NSJSONWritingPrettyPrinted
                                       error:nil] writeToFile:registry atomically:YES];
    return ready;
}

extern "C" __attribute__((visibility("default"))) bool logic_queue_request(const char* request_path) {
    static std::atomic_bool busy{false};
    @autoreleasepool {
        NSString* path = [NSString stringWithUTF8String:request_path];
        NSData* data = [NSData dataWithContentsOfFile:path];
        NSDictionary* request = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        if (![request isKindOfClass:[NSDictionary class]] || ![request[@"output"] isKindOfClass:[NSString class]])
            return false;
        NSDictionary* info = NSBundle.mainBundle.infoDictionary;
        if (![info[@"CFBundleShortVersionString"] isEqual:@"12.3.1"] || ![info[@"CFBundleVersion"] isEqual:@"6682"])
            return false;
        if ([request[@"action"] isEqual:@"queue_state"]) {
            NSString* mode = CFBridgingRelease(CFRunLoopCopyCurrentMode(CFRunLoopGetMain()));
            NSArray* modes = CFBridgingRelease(CFRunLoopCopyAllModes(CFRunLoopGetMain()));
            NSDictionary* state = @{
                @"ok": @YES,
                @"mode": mode ?: @"",
                @"modes": modes ?: @[],
                @"modal_title": NSApp.modalWindow.title ?: @"",
                @"modal_controller": NSStringFromClass([NSApp.modalWindow.windowController class]) ?: @""
            };
            [[NSJSONSerialization dataWithJSONObject:state options:NSJSONWritingPrettyPrinted
                                               error:nil] writeToFile:request[@"output"] atomically:YES];
            return true;
        }
        if ([request[@"action"] isEqual:@"wake"]) {
            wake_application_loop();
            return true;
        }
        bool expected = false;
        if (!busy.compare_exchange_strong(expected, true))
            return false;
        const CFAbsoluteTime queued_at = CFAbsoluteTimeGetCurrent();
        NSArray* run_modes =
            @[(__bridge NSString*) kCFRunLoopCommonModes, NSModalPanelRunLoopMode, NSEventTrackingRunLoopMode];
        CFRunLoopPerformBlock(CFRunLoopGetMain(), (__bridge CFTypeRef) run_modes, ^{
          @autoreleasepool {
              const CFAbsoluteTime started_at = CFAbsoluteTimeGetCurrent();
              NSData* progress = [NSJSONSerialization dataWithJSONObject:@{
                  @"state": @"running",
                  @"started_at": @(started_at),
                  @"queue_wait_seconds": @(started_at - queued_at)
              }
                                                                 options:0
                                                                   error:nil];
              [progress writeToFile:[request[@"output"] stringByAppendingString:@".progress.json"] atomically:YES];
              NSDictionary* response;
              @try {
                  if (([request[@"deadline_unix"] doubleValue] > 0 &&
                       NSDate.date.timeIntervalSince1970 > [request[@"deadline_unix"] doubleValue]) ||
                      ([request[@"cancel_path"] isKindOfClass:NSString.class] &&
                       [NSFileManager.defaultManager fileExistsAtPath:request[@"cancel_path"]]))
                      response = @{
                          @"ok": @NO,
                          @"cancelled": @YES,
                          @"error": @"Request expired or was cancelled before execution"
                      };
                  else if ([request[@"action"] isEqual:@"install_server"])
                      response = install_server(request);
                  else if ([request[@"action"] isEqual:@"inspect_audio_state"])
                      response = inspect_audio_state();
                  else if ([request[@"action"] isEqual:@"probe_readiness"])
                      response = probe_readiness();
                  else if ([request[@"action"] isEqual:@"configure_probe_renderer"])
                      response = configure_probe_renderer();
                  else if ([request[@"action"] isEqual:@"inspect"])
                      response = inspect();
                  else if ([request[@"action"] isEqual:@"inspect_document_factory"])
                      response = inspect_document_factory();
                  else if ([request[@"action"] isEqual:@"create_probe_document"])
                      response = create_probe_document();
                  else if ([request[@"action"] isEqual:@"import_probe_adm"])
                      response = import_probe_adm(request);
                  else if ([request[@"action"] isEqual:@"activate_probe_audio"])
                      response = activate_probe_audio();
                  else if ([request[@"action"] isEqual:@"inspect_mixers"])
                      response = inspect_mixers();
                  else if ([request[@"action"] isEqual:@"bounce_probe_document"])
                      response = bounce_probe_document(request);
                  else if ([request[@"action"] isEqual:@"discard_probe_document"])
                      response = discard_probe_document();
                  else if ([request[@"action"] isEqual:@"restore_original_renderer"])
                      response = restore_original_renderer();
                  else if ([request[@"action"] isEqual:@"prepare_bounce"])
                      response = bounce_request(request, false);
                  else if ([request[@"action"] isEqual:@"bounce"])
                      response = bounce_request(request, true);
                  else
                      response = @{@"ok": @NO, @"error": @"Unsupported action"};
              } @catch (NSException* exception) {
                  response = @{@"ok": @NO, @"error": exception.reason ?: exception.name};
              }
              NSMutableDictionary* timed_response = [response mutableCopy];
              timed_response[@"queue_wait_seconds"] = @(started_at - queued_at);
              timed_response[@"action_seconds"] = @(CFAbsoluteTimeGetCurrent() - started_at);
              NSData* result = [NSJSONSerialization dataWithJSONObject:timed_response
                                                               options:NSJSONWritingPrettyPrinted
                                                                 error:nil];
              [result writeToFile:request[@"output"] atomically:YES];
              NSData* done = [NSJSONSerialization dataWithJSONObject:@{
                  @"state": @"completed",
                  @"ok": response[@"ok"] ?: @NO
              }
                                                             options:0
                                                               error:nil];
              [done writeToFile:[request[@"output"] stringByAppendingString:@".progress.json"] atomically:YES];
              busy.store(false);
          }
        });
        wake_application_loop();
        return true;
    }
}
