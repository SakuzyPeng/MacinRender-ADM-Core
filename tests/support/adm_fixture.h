// Test-only XML authoring helpers. This is not an ADM parser or production model.
// Values are intentionally emitted independently of the Rust metadata implementation.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace fixture {
inline std::string escaped(const std::string& value) {
    std::string out;
    for (char c : value) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out += c;
        }
    }
    return out;
}
template <class T> std::string value_string(const T& value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(17) << value;
    return out.str();
}
struct Time {
    int64_t ns{0};
    template <class Rep, class Period>
    explicit Time(std::chrono::duration<Rep, Period> value)
        : ns(std::chrono::duration_cast<std::chrono::nanoseconds>(value).count()) {}
    std::string str() const {
        std::ostringstream out;
        auto seconds = ns / 1000000000;
        out << std::setfill('0') << std::setw(2) << seconds / 3600 << ":" << std::setw(2) << (seconds / 60) % 60 << ":"
            << std::setw(2) << seconds % 60 << "." << std::setw(9) << ns % 1000000000;
        return out.str();
    }
};
struct Param {
    std::string name, text;
    bool attribute{false};
    std::map<std::string, std::string> attrs;
    std::vector<Param> children;
    Param() = default;
    Param(std::string key, std::string value, bool attr = false)
        : name(std::move(key)), text(std::move(value)), attribute(attr) {}
    void set_parameter(const Param& value) {
        if (value.name == "#text") {
            text = value.text;
            return;
        }
        if (value.attribute) {
            attrs[value.name] = value.text;
            return;
        }
        auto it = std::find_if(children.begin(), children.end(), [&](const Param& p) {
            return p.name == value.name && p.attrs.find("coordinate") == p.attrs.end();
        });
        if (it == children.end()) {
            children.push_back(value);
        } else {
            *it = value;
        }
    }
    template <class... T> void set_all(const T&... values) { (set_parameter(values), ...); }
};
inline void xml(std::ostream& out, const Param& p, std::string name = {}) {
    if (name.empty()) {
        name = p.name;
    }
    out << "<" << name;
    for (const auto& [key, value] : p.attrs) {
        out << " " << key << "=\"" << escaped(value) << "\"";
    }
    out << ">" << escaped(p.text);
    for (const auto& child : p.children) {
        xml(out, child);
    }
    out << "</" << name << ">";
}
struct AudioProgrammeName : Param {
    template <class T> explicit AudioProgrammeName(T value) : Param("audioProgrammeName", value_string(value), true) {}
};
struct AudioContentName : Param {
    template <class T> explicit AudioContentName(T value) : Param("audioContentName", value_string(value), true) {}
};
struct AudioObjectName : Param {
    template <class T> explicit AudioObjectName(T value) : Param("audioObjectName", value_string(value), true) {}
};
struct AudioPackFormatName : Param {
    template <class T>
    explicit AudioPackFormatName(T value) : Param("audioPackFormatName", value_string(value), true) {}
};
struct AudioChannelFormatName : Param {
    template <class T>
    explicit AudioChannelFormatName(T value) : Param("audioChannelFormatName", value_string(value), true) {}
};
struct AudioStreamFormatName : Param {
    template <class T>
    explicit AudioStreamFormatName(T value) : Param("audioStreamFormatName", value_string(value), true) {}
};
struct AudioTrackFormatName : Param {
    template <class T>
    explicit AudioTrackFormatName(T value) : Param("audioTrackFormatName", value_string(value), true) {}
};
struct AudioProgrammeLanguage : Param {
    template <class T>
    explicit AudioProgrammeLanguage(T value) : Param("audioProgrammeLanguage", value_string(value), true) {}
};
struct AudioContentLanguage : Param {
    template <class T>
    explicit AudioContentLanguage(T value) : Param("audioContentLanguage", value_string(value), true) {}
};
struct Importance : Param {
    template <class T> explicit Importance(T value) : Param("importance", value_string(value), true) {}
};
struct DialogueId : Param {
    template <class T> explicit DialogueId(T value) : Param("dialogue", value_string(value), true) {}
};
struct LabelLanguage : Param {
    template <class T> explicit LabelLanguage(T value) : Param("language", value_string(value), true) {}
};
struct LoudnessMethod : Param {
    template <class T> explicit LoudnessMethod(T value) : Param("loudnessMethod", value_string(value), true) {}
};
struct AzimuthRange : Param {
    template <class T> explicit AzimuthRange(T value) : Param("azimuthRange", value_string(value), true) {}
};
struct PositionRange : Param {
    template <class T> explicit PositionRange(T value) : Param("positionRange", value_string(value), true) {}
};
struct MaxDistance : Param {
    template <class T> explicit MaxDistance(T value) : Param("maxDistance", value_string(value), true) {}
};
struct Width : Param {
    template <class T> explicit Width(T value) : Param("width", value_string(value), false) {}
};
struct Height : Param {
    template <class T> explicit Height(T value) : Param("height", value_string(value), false) {}
};
struct Depth : Param {
    template <class T> explicit Depth(T value) : Param("depth", value_string(value), false) {}
};
struct Diffuse : Param {
    template <class T> explicit Diffuse(T value) : Param("diffuse", value_string(value), false) {}
};
struct HeadLocked : Param {
    template <class T> explicit HeadLocked(T value) : Param("headLocked", value_string(value), false) {}
};
struct Mute : Param {
    template <class T> explicit Mute(T value) : Param("mute", value_string(value), false) {}
};
struct ScreenRef : Param {
    template <class T> explicit ScreenRef(T value) : Param("screenRef", value_string(value), false) {}
};
struct Cartesian : Param {
    template <class T> explicit Cartesian(T value) : Param("cartesian", value_string(value), false) {}
};
struct Order : Param {
    template <class T> explicit Order(T value) : Param("order", value_string(value), false) {}
};
struct Degree : Param {
    template <class T> explicit Degree(T value) : Param("degree", value_string(value), false) {}
};
struct SpeakerLabel : Param {
    template <class T> explicit SpeakerLabel(T value) : Param("speakerLabel", value_string(value), false) {}
};
struct IntegratedLoudness : Param {
    template <class T> explicit IntegratedLoudness(T value) : Param("integratedLoudness", value_string(value), false) {}
};
struct MaxTruePeak : Param {
    template <class T> explicit MaxTruePeak(T value) : Param("maxTruePeak", value_string(value), false) {}
};
struct LoudnessRange : Param {
    template <class T> explicit LoudnessRange(T value) : Param("loudnessRange", value_string(value), false) {}
};
struct MaxMomentary : Param {
    template <class T> explicit MaxMomentary(T value) : Param("maxMomentary", value_string(value), false) {}
};
struct MaxShortTerm : Param {
    template <class T> explicit MaxShortTerm(T value) : Param("maxShortTerm", value_string(value), false) {}
};
struct DialogueLoudness : Param {
    template <class T> explicit DialogueLoudness(T value) : Param("dialogueLoudness", value_string(value), false) {}
};
struct Normalization : Param {
    template <class T> explicit Normalization(T value) : Param("normalization", value_string(value), false) {}
};
struct NfcRefDist : Param {
    template <class T> explicit NfcRefDist(T value) : Param("nfcRefDist", value_string(value), false) {}
};
struct ChannelLockFlag : Param {
    template <class T> explicit ChannelLockFlag(T value) : Param("#text", value_string(value), false) {}
};
struct JumpPositionFlag : Param {
    template <class T> explicit JumpPositionFlag(T value) : Param("#text", value_string(value), false) {}
};
struct Divergence : Param {
    template <class T> explicit Divergence(T value) : Param("#text", value_string(value), false) {}
};
struct LabelValue : Param {
    template <class T> explicit LabelValue(T value) : Param("#text", value_string(value), false) {}
};
struct Azimuth : Param {
    template <class T> explicit Azimuth(T value) : Param("azimuth", value_string(value), false) {}
};
struct Elevation : Param {
    template <class T> explicit Elevation(T value) : Param("elevation", value_string(value), false) {}
};
struct Distance : Param {
    template <class T> explicit Distance(T value) : Param("distance", value_string(value), false) {}
};
struct X : Param {
    template <class T> explicit X(T value) : Param("X", value_string(value), false) {}
};
struct Y : Param {
    template <class T> explicit Y(T value) : Param("Y", value_string(value), false) {}
};
struct Z : Param {
    template <class T> explicit Z(T value) : Param("Z", value_string(value), false) {}
};
struct AzimuthOffset : Param {
    template <class T> explicit AzimuthOffset(T value) : Param("azimuth", value_string(value), false) {}
};
struct ElevationOffset : Param {
    template <class T> explicit ElevationOffset(T value) : Param("elevation", value_string(value), false) {}
};
struct DistanceOffset : Param {
    template <class T> explicit DistanceOffset(T value) : Param("distance", value_string(value), false) {}
};
struct Rtime : Param {
    explicit Rtime(Time value) : Param("rtime", value.str(), true) {}
};
struct Duration : Param {
    explicit Duration(Time value) : Param("duration", value.str(), true) {}
};
struct Start : Param {
    explicit Start(Time value) : Param("start", value.str(), true) {}
};
struct End : Param {
    explicit End(Time value) : Param("end", value.str(), true) {}
};
struct Gain : Param {
    explicit Gain(double value) : Param("gain", value_string(value)) {}
    static Gain from_db(double value) {
        Gain p(value);
        p.attrs["gainUnit"] = "dB";
        return p;
    }
    static Gain from_linear(double value) { return Gain(value); }
};
struct InterpolationLength : Param {
    template <class Rep, class Period>
    explicit InterpolationLength(std::chrono::duration<Rep, Period> value)
        : Param("interpolationLength", value_string(std::chrono::duration<double>(value).count()), true) {}
};
struct LowPass : Param {
    explicit LowPass(float value) : Param("frequency", value_string(value)) { attrs["typeDefinition"] = "lowPass"; }
};
struct Frequency : Param {
    explicit Frequency(const LowPass& p) : Param(p) {}
};
struct ChannelLock : Param {
    void set(const Param& value) { set_parameter(value); }
    template <class... T> explicit ChannelLock(const T&... values) {
        name = "channelLock";
        set_all(values...);
    }
};
struct ObjectDivergence : Param {
    void set(const Param& value) { set_parameter(value); }
    template <class... T> explicit ObjectDivergence(const T&... values) {
        name = "objectDivergence";
        set_all(values...);
    }
};
struct JumpPosition : Param {
    void set(const Param& value) { set_parameter(value); }
    template <class... T> explicit JumpPosition(const T&... values) {
        name = "jumpPosition";
        set_all(values...);
    }
};
struct Label : Param {
    void set(const Param& value) { set_parameter(value); }
    template <class... T> explicit Label(const T&... values) {
        name = "label";
        set_all(values...);
    }
};
struct LoudnessMetadata : Param {
    void set(const Param& value) { set_parameter(value); }
    template <class... T> explicit LoudnessMetadata(const T&... values) {
        name = "loudnessMetadata";
        set_all(values...);
    }
};
struct LoudnessMetadatas : Param {
    template <class... T> explicit LoudnessMetadatas(const T&... values) {
        name = "#list";
        children = {values...};
    }
};
struct Position : Param {
    Position(bool cartesian, bool offset) {
        name = offset ? "#offset" : "#position";
        if (!offset) {
            if (cartesian) {
                set(X{0});
                set(Y{0});
                set(Z{0});
            } else {
                set(Azimuth{0});
                set(Elevation{0});
                set(Distance{1});
            }
        }
    }
    void set(const Param& value) {
        Param p{name == "#offset" ? "positionOffset" : "position", value.text};
        p.attrs["coordinate"] = value.name;
        auto it = std::find_if(
            children.begin(), children.end(), [&](const Param& q) { return q.attrs.at("coordinate") == value.name; });
        if (it == children.end()) {
            children.push_back(std::move(p));
        } else {
            *it = std::move(p);
        }
    }
};
struct SphericalPosition : Position {
    template <class... T> explicit SphericalPosition(const T&... values) : Position(false, false) {
        (set(values), ...);
    }
};
struct SphericalSpeakerPosition : Position {
    template <class... T> explicit SphericalSpeakerPosition(const T&... values) : Position(false, false) {
        (set(values), ...);
    }
};
struct CartesianPosition : Position {
    template <class... T> explicit CartesianPosition(const T&... values) : Position(true, false) { (set(values), ...); }
};
struct CartesianSpeakerPosition : Position {
    template <class... T> explicit CartesianSpeakerPosition(const T&... values) : Position(true, false) {
        (set(values), ...);
    }
};
struct SphericalPositionOffset : Position {
    template <class... T> explicit SphericalPositionOffset(const T&... values) : Position(false, true) {
        (set(values), ...);
    }
};
struct CartesianPositionOffset : Position {
    template <class... T> explicit CartesianPositionOffset(const T&... values) : Position(true, true) {
        (set(values), ...);
    }
};
struct PositionOffset : Param {
    explicit PositionOffset(const Position& p) : Param(p) {}
};
enum class TypeDefinition { direct_speakers = 1, matrix = 2, objects = 3, hoa = 4, binaural = 5 };
using TypeDescriptor = TypeDefinition;
enum class FormatDefinition { pcm };
enum class DialogueContent { voiceover };
inline std::string hex(uint32_t value, int width) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(width) << value;
    return out.str();
}
struct AudioTrackUidIdValue {
    uint32_t value;
    explicit AudioTrackUidIdValue(uint32_t v) : value(v) {}
};
struct AudioTrackUidId {
    std::string value;
    explicit AudioTrackUidId(std::string v) : value(std::move(v)) {}
    explicit AudioTrackUidId(AudioTrackUidIdValue v) : value("ATU_" + hex(v.value, 8)) {}
};
inline std::string format_id(const AudioTrackUidId& value) {
    return value.value;
}
struct Properties {
    std::map<std::string, std::string> attributes;
    std::vector<Param> parameters;
    void set(const Param& p) {
        if (p.attribute) {
            attributes[p.name] = p.text;
            return;
        }
        if (p.name == "#position" || p.name == "#offset") {
            const auto key = p.name == "#position" ? "position" : "positionOffset";
            std::erase_if(parameters, [&](const auto& v) { return v.name == key; });
            parameters.insert(parameters.end(), p.children.begin(), p.children.end());
            return;
        }
        if (p.name == "#list") {
            parameters.insert(parameters.end(), p.children.begin(), p.children.end());
            return;
        }
        auto it = std::find_if(parameters.begin(), parameters.end(), [&](const auto& v) { return v.name == p.name; });
        if (it == parameters.end()) {
            parameters.push_back(p);
        } else {
            *it = p;
        }
    }
    void add(const Param& p) { parameters.push_back(p); }
    void set(DialogueContent) {
        Param p{"dialogue", "1"};
        p.attrs["dialogueContentKind"] = "2";
        set(p);
    }
};
struct AudioBlockFormatObjects : Properties {
    AudioBlockFormatObjects() { set(SphericalPosition{}); }
    template <class... T> explicit AudioBlockFormatObjects(const T&... values) : AudioBlockFormatObjects() {
        (set(values), ...);
    }
};
struct AudioBlockFormatDirectSpeakers : Properties {
    AudioBlockFormatDirectSpeakers() { set(SphericalSpeakerPosition{}); }
    template <class... T>
    explicit AudioBlockFormatDirectSpeakers(const T&... values) : AudioBlockFormatDirectSpeakers() {
        (set(values), ...);
    }
};
struct AudioBlockFormatHoa : Properties {
    template <class... T> explicit AudioBlockFormatHoa(const T&... values) { (set(values), ...); }
};
class Document;
struct Entity : Properties, std::enable_shared_from_this<Entity> {
    struct Link {
        std::weak_ptr<Entity> weak;
        std::shared_ptr<Entity> retained;
    };
    std::string tag, id;
    TypeDefinition type{TypeDefinition::objects};
    std::vector<Link> refs;
    std::weak_ptr<Document> owner;
    std::vector<AudioBlockFormatObjects> objects;
    std::vector<AudioBlockFormatDirectSpeakers> direct;
    std::vector<AudioBlockFormatHoa> hoa;
    explicit Entity(std::string name) : tag(std::move(name)) {}
    virtual ~Entity() = default;
    using Properties::add;
    using Properties::set;
    void set(TypeDefinition t) { type = t; }
    void set(FormatDefinition) {
        attributes["formatLabel"] = "0001";
        attributes["formatDefinition"] = "pcm";
    }
    void set(const AudioTrackUidId& value) { id = value.value; }
    template <class T> T get() const {
        if constexpr (std::is_same_v<T, AudioTrackUidId>) {
            return AudioTrackUidId{id};
        } else {
            return type;
        }
    }
    template <class T> std::vector<std::shared_ptr<T>> get_references() const {
        std::vector<std::shared_ptr<T>> result;
        for (const auto& reference : refs) {
            if (auto base = reference.weak.lock()) {
                if (auto p = std::dynamic_pointer_cast<T>(base)) {
                    result.push_back(std::move(p));
                }
            }
        }
        return result;
    }
    void add_reference(const std::shared_ptr<Entity>& value);
    void set_reference(const std::shared_ptr<Entity>& value) { add_reference(value); }
    void add(const AudioBlockFormatObjects& v) { objects.push_back(v); }
    void add(const AudioBlockFormatDirectSpeakers& v) { direct.push_back(v); }
    void add(const AudioBlockFormatHoa& v) { hoa.push_back(v); }
    template <class T> std::vector<T>& get_elements() {
        if constexpr (std::is_same_v<T, AudioBlockFormatObjects>) {
            return objects;
        } else if constexpr (std::is_same_v<T, AudioBlockFormatDirectSpeakers>) {
            return direct;
        } else {
            return hoa;
        }
    }
};
struct AudioProgramme : Entity {
    template <class... T> explicit AudioProgramme(const T&... values) : Entity("audioProgramme") { (set(values), ...); }
    template <class... T> static std::shared_ptr<AudioProgramme> create(const T&... values) {
        return std::make_shared<AudioProgramme>(values...);
    }
};
struct AudioContent : Entity {
    template <class... T> explicit AudioContent(const T&... values) : Entity("audioContent") { (set(values), ...); }
    template <class... T> static std::shared_ptr<AudioContent> create(const T&... values) {
        return std::make_shared<AudioContent>(values...);
    }
};
struct AudioObject : Entity {
    template <class... T> explicit AudioObject(const T&... values) : Entity("audioObject") { (set(values), ...); }
    template <class... T> static std::shared_ptr<AudioObject> create(const T&... values) {
        return std::make_shared<AudioObject>(values...);
    }
};
struct AudioPackFormat : Entity {
    template <class... T> explicit AudioPackFormat(const T&... values) : Entity("audioPackFormat") {
        (set(values), ...);
    }
    template <class... T> static std::shared_ptr<AudioPackFormat> create(const T&... values) {
        return std::make_shared<AudioPackFormat>(values...);
    }
};
struct AudioChannelFormat : Entity {
    template <class... T> explicit AudioChannelFormat(const T&... values) : Entity("audioChannelFormat") {
        (set(values), ...);
    }
    template <class... T> static std::shared_ptr<AudioChannelFormat> create(const T&... values) {
        return std::make_shared<AudioChannelFormat>(values...);
    }
};
struct AudioStreamFormat : Entity {
    template <class... T> explicit AudioStreamFormat(const T&... values) : Entity("audioStreamFormat") {
        (set(values), ...);
    }
    template <class... T> static std::shared_ptr<AudioStreamFormat> create(const T&... values) {
        return std::make_shared<AudioStreamFormat>(values...);
    }
};
struct AudioTrackFormat : Entity {
    template <class... T> explicit AudioTrackFormat(const T&... values) : Entity("audioTrackFormat") {
        (set(values), ...);
    }
    template <class... T> static std::shared_ptr<AudioTrackFormat> create(const T&... values) {
        return std::make_shared<AudioTrackFormat>(values...);
    }
};
struct AudioTrackUid : Entity {
    template <class... T> explicit AudioTrackUid(const T&... values) : Entity("audioTrackUID") { (set(values), ...); }
    template <class... T> static std::shared_ptr<AudioTrackUid> create(const T&... values) {
        return std::make_shared<AudioTrackUid>(values...);
    }
};
struct AudioPackFormatHoa : AudioPackFormat {
    template <class... T> explicit AudioPackFormatHoa(const T&... values) : AudioPackFormat(values...) {
        type = TypeDefinition::hoa;
    }
    template <class... T> static std::shared_ptr<AudioPackFormatHoa> create(const T&... values) {
        return std::make_shared<AudioPackFormatHoa>(values...);
    }
};
class Document : public std::enable_shared_from_this<Document> {
  public:
    std::vector<std::shared_ptr<Entity>> elements;
    static std::shared_ptr<Document> create() { return std::make_shared<Document>(); }
    void add(const std::shared_ptr<Entity>& e) {
        if (std::find(elements.begin(), elements.end(), e) != elements.end()) {
            return;
        }
        elements.push_back(e);
        e->owner = shared_from_this();
        for (auto& ref : e->refs) {
            if (auto next = ref.weak.lock()) {
                add(next);
            }
            ref.retained.reset();
        }
    }
    template <class T> std::vector<std::shared_ptr<T>> get_elements() const {
        std::vector<std::shared_ptr<T>> out;
        for (const auto& e : elements) {
            if (auto p = std::dynamic_pointer_cast<T>(e)) {
                out.push_back(std::move(p));
            }
        }
        return out;
    }
};
inline void Entity::add_reference(const std::shared_ptr<Entity>& value) {
    if (auto doc = owner.lock()) {
        doc->add(value);
        refs.push_back({value, {}});
    } else {
        refs.push_back({value, value});
    }
}
inline void reassign_ids(const std::shared_ptr<Document>& doc) {
    std::map<std::string, uint32_t> counters;
    for (const auto& e : doc->elements) {
        std::string prefix;
        if (e->tag == "audioProgramme") {
            prefix = "APR";
        } else if (e->tag == "audioContent") {
            prefix = "ACO";
        } else if (e->tag == "audioObject") {
            prefix = "AO";
        }
        if (!prefix.empty()) {
            e->id = prefix + "_" + hex(0x1000U + (++counters[prefix]), 4);
        } else if (e->tag == "audioTrackUID") {
            e->id = "ATU_" + hex(++counters["ATU"], 8);
        } else if (e->tag == "audioChannelFormat" || e->tag == "audioPackFormat") {
            prefix = e->tag == "audioChannelFormat" ? "AC" : "AP";
            auto type = hex(static_cast<uint32_t>(e->type), 4);
            e->id = prefix + "_" + type + hex(0x1000U + (++counters[prefix + type]), 4);
        }
    }
    for (const auto& e : doc->elements) {
        if (e->tag == "audioStreamFormat") {
            for (const auto& r : e->refs) {
                if (auto next = r.weak.lock(); next && next->tag == "audioChannelFormat") {
                    e->type = next->type;
                }
            }
            auto type = hex(static_cast<uint32_t>(e->type), 4);
            e->id = "AS_" + type + hex(0x1000U + (++counters["AS" + type]), 4);
        }
    }
    for (const auto& e : doc->elements) {
        if (e->tag == "audioTrackFormat") {
            for (const auto& r : e->refs) {
                if (auto next = r.weak.lock(); next && next->tag == "audioStreamFormat") {
                    e->type = next->type;
                }
            }
            auto type = hex(static_cast<uint32_t>(e->type), 4);
            e->id = "AT_" + type + hex(0x1000U + (++counters["AT" + type]), 4) + "_01";
        }
    }
}
inline void properties_xml(std::ostream& out, const Properties& p, const std::string& label_name) {
    for (const auto& parameter : p.parameters) {
        xml(out, parameter, parameter.name == "label" ? label_name : parameter.name);
    }
}
inline void write_xml(std::ostream& out, const std::shared_ptr<Document>& doc) {
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?><ebuCoreMain><coreMetadata><format><audioFormatExtended>";
    for (const auto& e : doc->elements) {
        const auto id_name = e->tag == "audioTrackUID" ? "UID" : e->tag + "ID";
        out << "<" << e->tag << " " << id_name << "=\"" << e->id << "\"";
        for (const auto& [key, value] : e->attributes) {
            out << " " << key << "=\"" << escaped(value) << "\"";
        }
        if (e->tag == "audioChannelFormat" || e->tag == "audioPackFormat") {
            out << " typeLabel=\"" << hex(static_cast<uint32_t>(e->type), 4) << "\"";
        }
        out << ">";
        properties_xml(out, *e, e->tag + "Label");
        uint32_t block_id = 0;
        const auto blocks = [&](const auto& values) {
            for (const auto& block : values) {
                out << "<audioBlockFormat audioBlockFormatID=\"AB_" << e->id.substr(3) << "_" << hex(++block_id, 8)
                    << "\"";
                for (const auto& [key, value] : block.attributes) {
                    out << " " << key << "=\"" << escaped(value) << "\"";
                }
                out << ">";
                properties_xml(out, block, "label");
                out << "</audioBlockFormat>";
            }
        };
        blocks(e->objects);
        blocks(e->direct);
        blocks(e->hoa);
        for (const auto& ref : e->refs) {
            if (auto next = ref.weak.lock()) {
                const auto tag = next->tag == "audioTrackUID" ? "audioTrackUIDRef" : next->tag + "IDRef";
                out << "<" << tag << ">" << next->id << "</" << tag << ">";
            }
        }
        out << "</" << e->tag << ">";
    }
    out << "</audioFormatExtended></format></coreMetadata></ebuCoreMain>";
}
} // namespace fixture
