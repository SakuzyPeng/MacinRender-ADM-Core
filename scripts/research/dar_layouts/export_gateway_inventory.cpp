// Read-only Qt metaobject inventory of the running Renderer export gateways.
// Loaded into the user's existing process by LLDB; no methods or getters run.
#if defined(__aarch64__)
#include <arm_acle.h>
#endif
#include <cstdio>
#include <cstring>

#include <QtCore/QCoreApplication>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMetaMethod>
#include <QtCore/QMetaProperty>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>

namespace {
constexpr const char* k_classes[] = {
    "MasterFileGateway", "RerenderExporterGateway", "RerenderConfigGateway", "ExporterGateway", "GatewayFactory"};
constexpr const char* k_properties[] = {
    "masterFileGateway", "rerenderExporterGateway", "rerenderConfigGateway", "exporterGateway", "qmlGatewayFactory"};

bool wanted(const QObject* object) {
    if (!object)
        return false;
    const char* name = object->metaObject()->className();
    for (const auto* suffix : k_classes) {
        if (std::strstr(name, suffix))
            return true;
    }
    return false;
}

void add(QJsonArray& result, QSet<QObject*>& seen, QObject* object) {
    if (!wanted(object) || seen.contains(object))
        return;
    seen.insert(object);
    const auto* meta = object->metaObject();
    QJsonObject entry{{"class", QString::fromUtf8(meta->className())}, {"objectName", object->objectName()}};
    QJsonArray methods;
    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        const auto method = meta->method(i);
        QJsonArray parameters;
        for (const auto& type : method.parameterTypes())
            parameters.append(QString::fromUtf8(type));
        methods.append(QJsonObject{{"signature", QString::fromUtf8(method.methodSignature())},
                                   {"returnType", QString::fromUtf8(method.typeName())},
                                   {"methodType", static_cast<int>(method.methodType())},
                                   {"parameters", parameters}});
    }
    entry.insert("methods", methods);
    QJsonArray properties;
    for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
        const auto property = meta->property(i);
        properties.append(QJsonObject{{"name", QString::fromUtf8(property.name())},
                                      {"type", QString::fromUtf8(property.typeName())},
                                      {"readable", property.isReadable()},
                                      {"writable", property.isWritable()}});
    }
    entry.insert("properties", properties);
    result.append(entry);
}
} // namespace

extern "C" __attribute__((visibility("default"))) bool dar_export_gateway_inventory2(const char* report_path) {
    QJsonArray result;
    QSet<QObject*> seen;
    if (auto* app = QCoreApplication::instance()) {
        for (auto* window : QGuiApplication::allWindows()) {
            for (auto* context = QQmlEngine::contextForObject(window); context; context = context->parentContext()) {
                for (const auto* property : k_properties) {
                    add(result, seen, context->contextProperty(QString::fromUtf8(property)).value<QObject*>());
                }
            }
        }
        for (auto* object : app->findChildren<QObject*>())
            add(result, seen, object);
    }
    auto* output = std::fopen(report_path, "wx");
    if (!output)
        return false;
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Indented);
    const bool written = std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), output) ==
                         static_cast<size_t>(bytes.size());
    std::fclose(output);
    return written;
}

extern "C" __attribute__((visibility("default"))) bool dar_export_resource_inventory(const char* report_path) {
    QJsonArray result;
    QDirIterator iterator(QStringLiteral(":/"), QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const auto path = iterator.next();
        if (path.contains(QStringLiteral("RerenderExporter"), Qt::CaseInsensitive) ||
            path.endsWith(QStringLiteral("qmldir"))) {
            result.append(path);
        }
    }
    auto* output = std::fopen(report_path, "wx");
    if (!output)
        return false;
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Indented);
    const bool written = std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), output) ==
                         static_cast<size_t>(bytes.size());
    std::fclose(output);
    return written;
}

extern "C" __attribute__((visibility("default"))) bool dar_dump_resource(const char* resource_path,
                                                                         const char* report_path) {
    QFile resource(QString::fromUtf8(resource_path));
    if (!resource.open(QFile::ReadOnly))
        return false;
    const auto bytes = resource.readAll();
    auto* output = std::fopen(report_path, "wx");
    if (!output)
        return false;
    const bool written = std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), output) ==
                         static_cast<size_t>(bytes.size());
    std::fclose(output);
    return written;
}

extern "C" __attribute__((visibility("default"))) bool dar_scan_open_master_qml(const char* report_path) {
    QJsonArray result;
    QDirIterator iterator(QStringLiteral(":/"), QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const auto path = iterator.next();
        if (!path.endsWith(QStringLiteral(".qml")))
            continue;
        QFile file(path);
        if (!file.open(QFile::ReadOnly))
            continue;
        const auto lines = file.readAll().split('\n');
        for (const auto& line : lines) {
            if (line.contains("openMaster(")) {
                result.append(QJsonObject{{"path", path}, {"line", QString::fromUtf8(line.trimmed())}});
            }
        }
    }
    auto* output = std::fopen(report_path, "wx");
    if (!output)
        return false;
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Indented);
    const bool written = std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), output) ==
                         static_cast<size_t>(bytes.size());
    std::fclose(output);
    return written;
}
