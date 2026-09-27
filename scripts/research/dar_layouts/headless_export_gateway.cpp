// Research bridge for the Renderer gateway. Actions run on the app event loop
// without instantiating a dialog. The app owns created QObject instances.
#if defined(__aarch64__)
#include <arm_acle.h>
#endif
#include <cstdio>

#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMetaMethod>
#include <QtCore/QMetaProperty>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>

namespace {
struct FactoryContext {
    QObject* factory;
    QObject* parent;
};
FactoryContext find_factory() {
    for (auto* window : QGuiApplication::allWindows()) {
        for (auto* context = QQmlEngine::contextForObject(window); context; context = context->parentContext()) {
            auto* factory = context->contextProperty(QStringLiteral("qmlGatewayFactory")).value<QObject*>();
            if (factory && QString::fromUtf8(factory->metaObject()->className()).contains("QMLGatewayFactory")) {
                return {factory, window};
            }
        }
    }
    return {nullptr, nullptr};
}
bool write_json(const char* path, const QJsonObject& object) {
    auto* output = std::fopen(path, "wx");
    if (!output)
        return false;
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    const bool okay = std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), output) ==
                      static_cast<size_t>(bytes.size());
    std::fclose(output);
    return okay;
}
} // namespace

extern "C" __attribute__((visibility("default"))) bool
dar_headless_queue_create_export_gateway(const char* report_path) {
    const auto context = find_factory();
    if (!context.factory || !context.parent)
        return false;
    const QPointer<QObject> factory(context.factory);
    const QPointer<QObject> parent(context.parent);
    const QString path = QString::fromUtf8(report_path);
    QTimer::singleShot(0, context.parent, [factory, parent, path]() {
        QJsonObject report{{"factoryAlive", !factory.isNull()}, {"parentAlive", !parent.isNull()}};
        QObject* exporter = nullptr;
        const bool invoked = factory && parent &&
                             QMetaObject::invokeMethod(factory,
                                                       "createGateway",
                                                       Qt::DirectConnection,
                                                       Q_RETURN_ARG(QObject*, exporter),
                                                       Q_ARG(QString, QStringLiteral("RerenderExporterGateway")),
                                                       Q_ARG(QObject*, parent.data()));
        report.insert("invoked", invoked);
        report.insert("created", exporter != nullptr);
        if (exporter) {
            exporter->setObjectName(QStringLiteral("darHeadlessRerenderExporter"));
            const auto* meta = exporter->metaObject();
            report.insert("class", QString::fromUtf8(meta->className()));
            QJsonArray methods;
            for (int i = QObject::staticMetaObject.methodCount(); i < meta->methodCount(); ++i) {
                const auto method = meta->method(i);
                methods.append(QJsonObject{{"signature", QString::fromUtf8(method.methodSignature())},
                                           {"returnType", QString::fromUtf8(method.typeName())}});
            }
            report.insert("methods", methods);
            QJsonArray properties;
            for (int i = QObject::staticMetaObject.propertyCount(); i < meta->propertyCount(); ++i) {
                const auto property = meta->property(i);
                properties.append(QJsonObject{{"name", QString::fromUtf8(property.name())},
                                              {"type", QString::fromUtf8(property.typeName())},
                                              {"writable", property.isWritable()}});
            }
            report.insert("properties", properties);
        }
        write_json(path.toUtf8().constData(), report);
    });
    return true;
}

extern "C" __attribute__((visibility("default"))) bool dar_headless_queue_open_probe_adm(const char* report_path) {
    constexpr auto k_adm = "/Users/Sakuzy/code/rust/MacinDecode-AC4-Core/vectors/"
                           "probe_axes_single_object/normalized/output.wav";
    if (!QFileInfo::exists(QString::fromUtf8(k_adm)))
        return false;
    QObject* gateway = nullptr;
    QObject* parent = nullptr;
    for (auto* window : QGuiApplication::allWindows()) {
        for (auto* context = QQmlEngine::contextForObject(window); context; context = context->parentContext()) {
            auto* value = context->contextProperty(QStringLiteral("masterFileGateway")).value<QObject*>();
            if (value && QString::fromUtf8(value->metaObject()->className()).contains("MasterFileGateway")) {
                gateway = value;
                parent = window;
                break;
            }
        }
        if (gateway)
            break;
    }
    if (!gateway || !parent)
        return false;
    const QPointer<QObject> safe_gateway(gateway);
    const QPointer<QObject> safe_parent(parent);
    const QString path = QString::fromUtf8(report_path);
    QTimer::singleShot(0, parent, [safe_gateway, safe_parent, path]() {
        QJsonObject report;
        report.insert("beforeOpened", safe_gateway ? safe_gateway->property("isOpened").toBool() : false);
        const bool invoked =
            safe_gateway &&
            QMetaObject::invokeMethod(
                safe_gateway,
                "openMaster",
                Qt::DirectConnection,
                Q_ARG(QUrl,
                      QUrl::fromLocalFile(QStringLiteral("/Users/Sakuzy/code/rust/MacinDecode-AC4-Core/vectors/"
                                                         "probe_axes_single_object/normalized/output.wav"))),
                Q_ARG(QString, QString()));
        report.insert("invoked", invoked);
        if (!safe_parent) {
            write_json(path.toUtf8().constData(), report);
            return;
        }
        QTimer::singleShot(1500, safe_parent, [safe_gateway, path, report]() mutable {
            report.insert("afterOpened", safe_gateway ? safe_gateway->property("isOpened").toBool() : false);
            report.insert("path", safe_gateway ? safe_gateway->property("path").toString() : QString());
            report.insert("frameRate", safe_gateway ? safe_gateway->property("frameRate").toString() : QString());
            report.insert("hasContent", safe_gateway ? safe_gateway->property("hasContent").toBool() : false);
            write_json(path.toUtf8().constData(), report);
        });
    });
    return true;
}

extern "C" __attribute__((visibility("default"))) bool dar_headless_queue_snapshot_exporter(const char* report_path) {
    QObject* exporter = nullptr;
    QObject* parent = nullptr;
    for (auto* window : QGuiApplication::allWindows()) {
        exporter = window->findChild<QObject*>(QStringLiteral("darHeadlessRerenderExporter"));
        if (exporter) {
            parent = window;
            break;
        }
    }
    if (!exporter || !parent)
        return false;
    const QPointer<QObject> safe_exporter(exporter);
    const QString path = QString::fromUtf8(report_path);
    QTimer::singleShot(0, parent, [safe_exporter, path]() {
        QJsonObject report{{"found", !safe_exporter.isNull()}};
        if (safe_exporter) {
            for (const auto* name : {"outputDirString",
                                     "outputDirError",
                                     "masterName",
                                     "exportName",
                                     "enableTimeRange",
                                     "inTimecode",
                                     "outTimecode",
                                     "rangeLength",
                                     "timecodeError",
                                     "state",
                                     "percentCompleted",
                                     "enableMultichannelOutput",
                                     "isSelectionEmpty",
                                     "globalCheckState"}) {
                report.insert(name, QJsonValue::fromVariant(safe_exporter->property(name)));
            }
            QJsonArray rows;
            const auto objects = safe_exporter->property("rerenderRowItems").value<QList<QObject*>>();
            for (auto* object : objects) {
                if (!object)
                    continue;
                QJsonObject row{{"class", QString::fromUtf8(object->metaObject()->className())}};
                const auto* meta = object->metaObject();
                for (int i = QObject::staticMetaObject.propertyCount(); i < meta->propertyCount(); ++i) {
                    const auto property = meta->property(i);
                    const QString type = QString::fromUtf8(property.typeName());
                    if (type == "QString" || type == "bool" || type == "int" || type == "double" ||
                        type == "Qt::CheckState") {
                        row.insert(property.name(), QJsonValue::fromVariant(property.read(object)));
                    }
                }
                rows.append(row);
            }
            report.insert("rows", rows);
        }
        write_json(path.toUtf8().constData(), report);
    });
    return true;
}

extern "C" __attribute__((visibility("default"))) bool
dar_headless_queue_snapshot_rerender_config(const char* report_path) {
    QObject* gateway = nullptr;
    QObject* parent = nullptr;
    for (auto* window : QGuiApplication::allWindows()) {
        for (auto* context = QQmlEngine::contextForObject(window); context; context = context->parentContext()) {
            auto* value = context->contextProperty(QStringLiteral("rerenderConfigGateway")).value<QObject*>();
            if (value && QString::fromUtf8(value->metaObject()->className()).contains("RerenderConfigGateway")) {
                gateway = value;
                parent = window;
                break;
            }
        }
        if (gateway)
            break;
    }
    if (!gateway || !parent)
        return false;
    const QPointer<QObject> safe_gateway(gateway);
    const QString path = QString::fromUtf8(report_path);
    QTimer::singleShot(0, parent, [safe_gateway, path]() {
        QJsonObject report{{"found", !safe_gateway.isNull()}};
        if (safe_gateway) {
            for (const auto* name : {"layoutOptionsSelectedIndex", "rerenderProcessingActive"}) {
                report.insert(name, QJsonValue::fromVariant(safe_gateway->property(name)));
            }
            for (const auto* name : {"rerenderInfos", "layoutOptions", "systemTags"}) {
                QJsonArray entries;
                const auto objects = safe_gateway->property(name).value<QList<QObject*>>();
                for (auto* object : objects) {
                    if (!object)
                        continue;
                    QJsonObject item{{"class", QString::fromUtf8(object->metaObject()->className())}};
                    const auto* meta = object->metaObject();
                    for (int i = QObject::staticMetaObject.propertyCount(); i < meta->propertyCount(); ++i) {
                        const auto property = meta->property(i);
                        const QString type = QString::fromUtf8(property.typeName());
                        if (type == "QString" || type == "bool" || type == "int" || type == "double") {
                            item.insert(property.name(), QJsonValue::fromVariant(property.read(object)));
                        }
                    }
                    entries.append(item);
                }
                report.insert(name, entries);
            }
        }
        write_json(path.toUtf8().constData(), report);
    });
    return true;
}

extern "C" __attribute__((visibility("default"))) bool
dar_headless_queue_refresh_export_gateway(const char* report_path) {
    const auto context = find_factory();
    if (!context.factory || !context.parent)
        return false;
    auto* old = context.parent->findChild<QObject*>(QStringLiteral("darHeadlessRerenderExporter"));
    if (!old)
        return false;
    const QPointer<QObject> factory(context.factory), parent(context.parent);
    const QString path = QString::fromUtf8(report_path);
    QTimer::singleShot(0, context.parent, [factory, parent, old, path]() {
        old->deleteLater();
        QTimer::singleShot(250, parent, [factory, parent, path]() {
            QJsonObject report{{"factoryAlive", !factory.isNull()}, {"parentAlive", !parent.isNull()}};
            QObject* exporter = nullptr;
            const bool invoked = factory && parent &&
                                 QMetaObject::invokeMethod(factory,
                                                           "createGateway",
                                                           Qt::DirectConnection,
                                                           Q_RETURN_ARG(QObject*, exporter),
                                                           Q_ARG(QString, QStringLiteral("RerenderExporterGateway")),
                                                           Q_ARG(QObject*, parent.data()));
            report.insert("invoked", invoked);
            report.insert("created", exporter != nullptr);
            if (exporter) {
                exporter->setObjectName(QStringLiteral("darHeadlessRerenderExporter"));
                QJsonArray rows;
                const auto objects = exporter->property("rerenderRowItems").value<QList<QObject*>>();
                for (auto* item : objects) {
                    if (item)
                        rows.append(QJsonObject{{"name", item->property("name").toString()},
                                                {"layout", item->property("layout").toString()},
                                                {"selected", item->property("selected").toBool()}});
                }
                report.insert("rows", rows);
            }
            write_json(path.toUtf8().constData(), report);
        });
    });
    return true;
}

extern "C" __attribute__((visibility("default"))) bool
dar_headless_queue_release_export_gateway(const char* report_path) {
    QObject* exporter = nullptr;
    QObject* parent = nullptr;
    for (auto* window : QGuiApplication::allWindows()) {
        exporter = window->findChild<QObject*>(QStringLiteral("darHeadlessRerenderExporter"));
        if (exporter) {
            parent = window;
            break;
        }
    }
    if (!exporter || !parent)
        return false;
    const QPointer<QObject> safe_exporter(exporter);
    const QString path = QString::fromUtf8(report_path);
    const QString original_name = QString::fromUtf8("ADMCUT - RADWIMPS - 前前前世_SSDX");
    QTimer::singleShot(0, parent, [safe_exporter, parent, path, original_name]() {
        QJsonObject report{{"found", !safe_exporter.isNull()}};
        if (safe_exporter) {
            report.insert("nameBefore", safe_exporter->property("exportName").toString());
            report.insert("outputDir", safe_exporter->property("outputDirString").toString());
            const auto* meta = safe_exporter->metaObject();
            const int index = meta->indexOfProperty("exportName");
            report.insert("nameWriteSucceeded",
                          index >= 0 && meta->property(index).write(safe_exporter, original_name));
        }
        QTimer::singleShot(150, parent, [safe_exporter, parent, path, original_name, report]() mutable {
            report.insert("nameAfter", safe_exporter ? safe_exporter->property("exportName").toString() : QString());
            report.insert("nameRestored",
                          safe_exporter && safe_exporter->property("exportName").toString() == original_name);
            if (safe_exporter)
                safe_exporter->deleteLater();
            QTimer::singleShot(150, parent, [safe_exporter, path, report]() mutable {
                report.insert("destroyed", safe_exporter.isNull());
                write_json(path.toUtf8().constData(), report);
            });
        });
    });
    return true;
}
