// In-process bridge for the installed Renderer 5.5 Qt gateways. The caller
// queues one JSON action, detaches LLDB, then reads its JSON response.
#if defined(__aarch64__)
#include <arm_acle.h>
#endif
#include <cstdio>

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMetaProperty>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSaveFile>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>

namespace {
constexpr auto k_exporter_name = "darBatchRerenderExporter";

struct Gateways {
    QObject* parent{};
    QObject* factory{};
    QObject* master{};
    QObject* config{};
};

Gateways find_gateways() {
    for (auto* window : QGuiApplication::allWindows()) {
        Gateways value{window};
        for (auto* context = QQmlEngine::contextForObject(window); context; context = context->parentContext()) {
            if (!value.factory)
                value.factory = context->contextProperty(QStringLiteral("qmlGatewayFactory")).value<QObject*>();
            if (!value.master)
                value.master = context->contextProperty(QStringLiteral("masterFileGateway")).value<QObject*>();
            if (!value.config)
                value.config = context->contextProperty(QStringLiteral("rerenderConfigGateway")).value<QObject*>();
        }
        if (value.factory && value.master && value.config)
            return value;
    }
    return {};
}

QObject* find_exporter(QObject* parent) {
    return parent ? parent->findChild<QObject*>(QString::fromUtf8(k_exporter_name)) : nullptr;
}

QList<QObject*> objects_property(QObject* object, const char* name) {
    return object ? object->property(name).value<QList<QObject*>>() : QList<QObject*>{};
}

bool write_property(QObject* object, const char* name, const QVariant& value) {
    if (!object)
        return false;
    const auto* meta = object->metaObject();
    const int index = meta->indexOfProperty(name);
    return index >= 0 && meta->property(index).write(object, value);
}

bool call_void(QObject* object, const char* method) {
    return object && QMetaObject::invokeMethod(object, method, Qt::DirectConnection);
}

QJsonObject snapshot(const Gateways& gateways) {
    QJsonObject result;
    result.insert("master",
                  QJsonObject{
                      {"opened", gateways.master->property("isOpened").toBool()},
                      {"path", gateways.master->property("path").toString()},
                      {"hasContent", gateways.master->property("hasContent").toBool()},
                      {"frameRate", gateways.master->property("frameRate").toString()},
                  });
    QJsonArray rows;
    for (auto* item : objects_property(gateways.config, "rerenderInfos")) {
        if (!item)
            continue;
        rows.append(QJsonObject{{"name", item->property("name").toString()},
                                {"layout", item->property("layout").toString()},
                                {"groups", item->property("groupsResume").toString()},
                                {"hasInvalidGroups", item->property("hasInvalidGroups").toBool()},
                                {"mapped", item->property("mapped").toBool()},
                                {"newLayoutIndex", item->property("newLayoutIndex").toInt()}});
    }
    QJsonArray layouts;
    for (auto* item : objects_property(gateways.config, "layoutOptions")) {
        layouts.append(item ? item->property("name").toString() : QString());
    }
    result.insert("config",
                  QJsonObject{{"rows", rows},
                              {"layoutOptions", layouts},
                              {"processing", gateways.config->property("rerenderProcessingActive").toBool()}});
    if (auto* exporter = find_exporter(gateways.parent)) {
        QJsonArray export_rows;
        for (auto* item : objects_property(exporter, "rerenderRowItems")) {
            if (!item)
                continue;
            export_rows.append(QJsonObject{{"name", item->property("name").toString()},
                                           {"layout", item->property("layout").toString()},
                                           {"selected", item->property("selected").toBool()}});
        }
        result.insert("exporter",
                      QJsonObject{
                          {"outputDir", exporter->property("outputDir").toUrl().toString()},
                          {"outputDirString", exporter->property("outputDirString").toString()},
                          {"outputDirError", exporter->property("outputDirError").toString()},
                          {"exportName", exporter->property("exportName").toString()},
                          {"enableMultichannelOutput", exporter->property("enableMultichannelOutput").toBool()},
                          {"enableNumberedMonoFiles", exporter->property("enableNumberedMonoFiles").toBool()},
                          {"enableTimeRange", exporter->property("enableTimeRange").toBool()},
                          {"isSelectionEmpty", exporter->property("isSelectionEmpty").toBool()},
                          {"state", exporter->property("state").toInt()},
                          {"percentCompleted", exporter->property("percentCompleted").toInt()},
                          {"statusInfo", exporter->property("statusInfo").toString()},
                          {"rows", export_rows},
                      });
    } else {
        result.insert("exporter", QJsonValue::Null);
    }
    bool other_exporter = false;
    for (auto* object : gateways.parent->findChildren<QObject*>()) {
        if (QString::fromUtf8(object->metaObject()->className()).contains("RerenderExporterGateway") &&
            object != find_exporter(gateways.parent)) {
            other_exporter = true;
            break;
        }
    }
    result.insert("otherExporterPresent", other_exporter);
    return result;
}

QJsonObject fail(const QString& message) {
    return QJsonObject{{"ok", false}, {"error", message}};
}

QJsonObject perform(const QJsonObject& request, const Gateways& gateways) {
    const QString action = request.value("action").toString();
    if (action == "inspect") {
        auto result = snapshot(gateways);
        result.insert("ok", true);
        return result;
    }
    if (action == "open_master") {
        const QString path = request.value("path").toString();
        if (!QFileInfo(path).isAbsolute() || !QFileInfo::exists(path))
            return fail("ADM path is absent");
        const bool invoked = QMetaObject::invokeMethod(gateways.master,
                                                       "openMaster",
                                                       Qt::DirectConnection,
                                                       Q_ARG(QUrl, QUrl::fromLocalFile(path)),
                                                       Q_ARG(QString, QString()));
        return QJsonObject{{"ok", invoked}, {"invoked", invoked}};
    }
    if (action == "restore_master") {
        const QString path = request.value("path").toString();
        const bool opened = request.value("opened").toBool();
        if (!opened) {
            const bool invoked = call_void(gateways.master, "close");
            return QJsonObject{{"ok", invoked}, {"invoked", invoked}};
        }
        if (!QFileInfo(path).isAbsolute() || !QFileInfo::exists(path))
            return fail("original master path is unavailable");
        const bool invoked = QMetaObject::invokeMethod(gateways.master,
                                                       "openMaster",
                                                       Qt::DirectConnection,
                                                       Q_ARG(QUrl, QUrl::fromLocalFile(path)),
                                                       Q_ARG(QString, QString()));
        return QJsonObject{{"ok", invoked}, {"invoked", invoked}};
    }
    if (action == "stage_layout") {
        const QString layout = request.value("layout").toString();
        const auto rows = objects_property(gateways.config, "rerenderInfos");
        const auto options = objects_property(gateways.config, "layoutOptions");
        if (rows.size() != 1 || !rows[0] || rows[0]->property("mapped").toBool() ||
            gateways.config->property("rerenderProcessingActive").toBool())
            return fail("requires one unmapped idle re-render row");
        int index = -1;
        for (int i = 0; i < options.size(); ++i) {
            if (options[i] && options[i]->property("name").toString() == layout)
                index = i;
        }
        if (index < 0 || (layout != "7.1.4" && layout != "9.1.6"))
            return fail("unsupported target layout");
        const bool written = write_property(rows[0], "newLayoutIndex", index);
        const bool invoked = written && QMetaObject::invokeMethod(
                                            gateways.config, "updateRerender", Qt::DirectConnection, Q_ARG(int, 0));
        if (request.value("applyImmediately").toBool()) {
            const auto updated = objects_property(gateways.config, "rerenderInfos");
            if (!invoked || updated.size() != 1 || !updated[0] || updated[0]->property("layout").toString() != layout)
                return fail("layout staging did not complete synchronously");
            const bool applied = call_void(gateways.config, "applyChanges");
            return QJsonObject{{"ok", applied}, {"index", index}, {"updateInvoked", invoked}, {"applyInvoked", applied}};
        }
        return QJsonObject{{"ok", written && invoked}, {"index", index}, {"updateInvoked", invoked}};
    }
    if (action == "apply_layout") {
        const QString layout = request.value("layout").toString();
        const auto rows = objects_property(gateways.config, "rerenderInfos");
        if (rows.size() != 1 || !rows[0] || rows[0]->property("layout").toString() != layout)
            return fail("staged layout has not appeared");
        const bool invoked = call_void(gateways.config, "applyChanges");
        return QJsonObject{{"ok", invoked}, {"applyInvoked", invoked}};
    }
    if (action == "cancel_layout") {
        const bool invoked = call_void(gateways.config, "cancelChanges");
        return QJsonObject{{"ok", invoked}, {"cancelInvoked", invoked}};
    }
    if (action == "create_exporter") {
        if (find_exporter(gateways.parent) || snapshot(gateways).value("otherExporterPresent").toBool())
            return fail("an exporter gateway already exists");
        QObject* exporter = nullptr;
        const bool invoked = QMetaObject::invokeMethod(gateways.factory,
                                                       "createGateway",
                                                       Qt::DirectConnection,
                                                       Q_RETURN_ARG(QObject*, exporter),
                                                       Q_ARG(QString, QStringLiteral("RerenderExporterGateway")),
                                                       Q_ARG(QObject*, gateways.parent));
        if (exporter)
            exporter->setObjectName(QString::fromUtf8(k_exporter_name));
        return QJsonObject{{"ok", invoked && exporter}, {"created", exporter != nullptr}};
    }
    if (action == "configure_exporter") {
        auto* exporter = find_exporter(gateways.parent);
        const QString path = request.value("outputDir").toString();
        if (!exporter || !QFileInfo(path).isDir() || !QFileInfo(path).isAbsolute())
            return fail("export gateway or output directory is unavailable");
        const auto rows = objects_property(exporter, "rerenderRowItems");
        if (rows.size() != 1 || !rows[0])
            return fail("requires one export row");
        const QJsonObject original{
            {"outputDir", exporter->property("outputDir").toUrl().toString()},
            {"exportName", exporter->property("exportName").toString()},
            {"enableMultichannelOutput", exporter->property("enableMultichannelOutput").toBool()},
            {"enableNumberedMonoFiles", exporter->property("enableNumberedMonoFiles").toBool()},
            {"enableTimeRange", exporter->property("enableTimeRange").toBool()},
            {"selected", rows[0]->property("selected").toBool()}};
        bool okay = QMetaObject::invokeMethod(
            exporter, "setOutputDir", Qt::DirectConnection, Q_ARG(QUrl, QUrl::fromLocalFile(path)));
        const bool multichannel = request.value("multichannel").toBool(true);
        if (exporter->property("enableMultichannelOutput").toBool() != multichannel)
            okay &= QMetaObject::invokeMethod(
                exporter, "setEnableMultichannelOutput", Qt::DirectConnection, Q_ARG(bool, multichannel));
        if (!multichannel && !exporter->property("enableNumberedMonoFiles").toBool())
            okay &= QMetaObject::invokeMethod(
                exporter, "setEnableNumberedMonoFiles", Qt::DirectConnection, Q_ARG(bool, true));
        if (exporter->property("enableTimeRange").toBool())
            okay &= QMetaObject::invokeMethod(exporter, "setEnableTimeRange", Qt::DirectConnection, Q_ARG(bool, false));
        if (!rows[0]->property("selected").toBool())
            okay &= QMetaObject::invokeMethod(exporter, "toggleSelection", Qt::DirectConnection, Q_ARG(int, 0));
        return QJsonObject{{"ok", okay}, {"original", original}};
    }
    if (action == "start_export") {
        auto* exporter = find_exporter(gateways.parent);
        const QString layout = request.value("layout").toString();
        const QString output_dir = request.value("outputDir").toString();
        const QString name = request.value("name").toString();
        const auto rows = objects_property(exporter, "rerenderRowItems");
        const bool ready =
            exporter && gateways.master->property("isOpened").toBool() &&
            gateways.master->property("path").toString() == request.value("adm").toString() &&
            exporter->property("outputDirString").toString() == output_dir &&
            exporter->property("outputDirError").toString().isEmpty() &&
            exporter->property("enableMultichannelOutput").toBool() == request.value("multichannel").toBool(true) &&
            !exporter->property("enableTimeRange").toBool() && rows.size() == 1 && rows[0] &&
            rows[0]->property("layout").toString() == layout && rows[0]->property("selected").toBool();
        if (!ready || name.isEmpty())
            return fail("export preconditions failed");
        const bool invoked =
            QMetaObject::invokeMethod(exporter, "startExport", Qt::DirectConnection, Q_ARG(QString, name));
        return QJsonObject{{"ok", invoked}, {"startInvoked", invoked}};
    }
    if (action == "cancel_export") {
        auto* exporter = find_exporter(gateways.parent);
        if (!exporter)
            return fail("exporter has already been released");
        const bool invoked = call_void(exporter, "cancelExport");
        return QJsonObject{{"ok", invoked}, {"cancelInvoked", invoked}};
    }
    if (action == "restore_exporter") {
        auto* exporter = find_exporter(gateways.parent);
        if (!exporter)
            return QJsonObject{{"ok", true}, {"alreadyReleased", true}};
        const QUrl original = QUrl(request.value("outputDir").toString());
        const QString name = request.value("exportName").toString();
        if (!original.isLocalFile())
            return fail("original output directory is not a file URL");
        bool okay = QMetaObject::invokeMethod(exporter, "setOutputDir", Qt::DirectConnection, Q_ARG(QUrl, original));
        okay &= write_property(exporter, "exportName", name);
        const bool multichannel = request.value("enableMultichannelOutput").toBool();
        if (exporter->property("enableMultichannelOutput").toBool() != multichannel)
            okay &= QMetaObject::invokeMethod(
                exporter, "setEnableMultichannelOutput", Qt::DirectConnection, Q_ARG(bool, multichannel));
        const bool numbered = request.value("enableNumberedMonoFiles").toBool();
        if (exporter->property("enableNumberedMonoFiles").toBool() != numbered)
            okay &= QMetaObject::invokeMethod(
                exporter, "setEnableNumberedMonoFiles", Qt::DirectConnection, Q_ARG(bool, numbered));
        const bool time_range = request.value("enableTimeRange").toBool();
        if (exporter->property("enableTimeRange").toBool() != time_range)
            okay &= QMetaObject::invokeMethod(
                exporter, "setEnableTimeRange", Qt::DirectConnection, Q_ARG(bool, time_range));
        const auto rows = objects_property(exporter, "rerenderRowItems");
        if (rows.size() == 1 && rows[0] && rows[0]->property("selected").toBool() != request.value("selected").toBool())
            okay &= QMetaObject::invokeMethod(exporter, "toggleSelection", Qt::DirectConnection, Q_ARG(int, 0));
        return QJsonObject{{"ok", okay}, {"restoreInvoked", okay}};
    }
    if (action == "release_exporter") {
        auto* exporter = find_exporter(gateways.parent);
        if (!exporter)
            return QJsonObject{{"ok", true}, {"alreadyReleased", true}};
        exporter->deleteLater();
        return QJsonObject{{"ok", true}, {"deleteQueued", true}};
    }
    return fail("unknown action");
}

bool write_response(const QString& path, const QJsonObject& response) {
    if (QFileInfo::exists(path))
        return false;
    // Publish only complete JSON. The out-of-process poller must never observe
    // the interval between creating a file and flushing its first bytes.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const auto bytes = QJsonDocument(response).toJson(QJsonDocument::Indented);
    return file.write(bytes) == bytes.size() && file.commit();
}
} // namespace

extern "C" __attribute__((visibility("default"))) bool dar_queue_batch_request(const char* request_path) {
    QFile file(QString::fromUtf8(request_path));
    if (!file.open(QFile::ReadOnly))
        return false;
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const auto request = document.object();
    const QString response_path = request.value("response").toString();
    if (response_path.isEmpty() || !QFileInfo(response_path).isAbsolute())
        return false;
    const auto gateways = find_gateways();
    if (!gateways.parent)
        return false;
    const QPointer<QObject> parent(gateways.parent);
    const QPointer<QObject> factory(gateways.factory);
    const QPointer<QObject> master(gateways.master);
    const QPointer<QObject> config(gateways.config);
    QTimer::singleShot(0, gateways.parent, [parent, factory, master, config, request, response_path]() {
        if (!parent || !factory || !master || !config) {
            write_response(response_path, fail("gateway was destroyed before request ran"));
            return;
        }
        write_response(response_path, perform(request, {parent, factory, master, config}));
    });
    return true;
}
