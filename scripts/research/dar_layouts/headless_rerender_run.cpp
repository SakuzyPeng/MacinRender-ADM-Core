// Narrow research bridge for the existing Renderer gateway. All gateway calls
// are queued onto the application's event loop and limited to one ADM probe.
#if defined(__aarch64__)
#include <arm_acle.h>
#endif
#include <cstdio>

#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMetaObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>

namespace {
constexpr auto k_adm = "/Users/Sakuzy/code/rust/MacinDecode-AC4-Core/vectors/"
                       "probe_axes_single_object/normalized/output.wav";
constexpr auto k_output = "/Users/Sakuzy/code/cpp/MacinRender-ADM-Core/local/dar-calibration-20260925";
QUrl original_output_dir;
bool original_saved = false;

struct Context {
    QObject* exporter;
    QObject* master;
    QObject* parent;
};
Context find_context() {
    for (auto* window : QGuiApplication::allWindows()) {
        auto* exporter = window->findChild<QObject*>(QStringLiteral("darHeadlessRerenderExporter"));
        if (!exporter)
            continue;
        QObject* master = nullptr;
        for (auto* qml = QQmlEngine::contextForObject(window); qml; qml = qml->parentContext()) {
            master = qml->contextProperty(QStringLiteral("masterFileGateway")).value<QObject*>();
            if (master)
                break;
        }
        return {exporter, master, window};
    }
    return {nullptr, nullptr, nullptr};
}

bool write_report(const QString& path, const QJsonObject& report) {
    const auto utf8 = path.toUtf8();
    auto* output = std::fopen(utf8.constData(), "wx");
    if (!output)
        return false;
    const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    const bool okay = std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), output) ==
                      static_cast<size_t>(bytes.size());
    std::fclose(output);
    return okay;
}
} // namespace

extern "C" __attribute__((visibility("default"))) bool dar_queue_configure_probe_916(const char* report_path) {
    const auto context = find_context();
    if (!context.exporter || !context.master || !context.parent || !QFileInfo::exists(k_adm) ||
        !QFileInfo(k_output).isDir())
        return false;
    const QPointer<QObject> exporter(context.exporter), master(context.master);
    const QString path = QString::fromUtf8(report_path);
    QTimer::singleShot(0, context.parent, [exporter, master, path]() {
        QJsonObject report;
        report.insert("masterPath", master ? master->property("path").toString() : QString());
        report.insert("masterOpened", master ? master->property("isOpened").toBool() : false);
        if (exporter && master && master->property("path").toString() == QString::fromUtf8(k_adm)) {
            if (!original_saved) {
                original_output_dir = exporter->property("outputDir").toUrl();
                original_saved = true;
            }
            report.insert("originalOutputDir", original_output_dir.toString());
            const bool invoked =
                QMetaObject::invokeMethod(exporter,
                                          "setOutputDir",
                                          Qt::DirectConnection,
                                          Q_ARG(QUrl, QUrl::fromLocalFile(QString::fromUtf8(k_output))));
            report.insert("setOutputDirInvoked", invoked);
            report.insert("outputDir", exporter->property("outputDirString").toString());
            report.insert("outputDirError", exporter->property("outputDirError").toString());
            report.insert("isSelectionEmpty", exporter->property("isSelectionEmpty").toBool());
            report.insert("enableMultichannelOutput", exporter->property("enableMultichannelOutput").toBool());
        }
        write_report(path, report);
    });
    return true;
}

extern "C" __attribute__((visibility("default"))) bool dar_queue_start_probe_916(const char* report_path) {
    const auto context = find_context();
    if (!context.exporter || !context.master || !context.parent)
        return false;
    const QPointer<QObject> exporter(context.exporter), master(context.master);
    const QString path = QString::fromUtf8(report_path);
    QTimer::singleShot(0, context.parent, [exporter, master, path]() {
        QJsonObject report;
        const bool ready = exporter && master && master->property("isOpened").toBool() &&
                           master->property("path").toString() == QString::fromUtf8(k_adm) &&
                           exporter->property("outputDirString").toString() == QString::fromUtf8(k_output) &&
                           exporter->property("outputDirError").toString().isEmpty() &&
                           !exporter->property("isSelectionEmpty").toBool() &&
                           exporter->property("enableMultichannelOutput").toBool();
        report.insert("ready", ready);
        if (ready) {
            const bool invoked = QMetaObject::invokeMethod(
                exporter, "startExport", Qt::DirectConnection, Q_ARG(QString, QStringLiteral("probe-axes-dar-916")));
            report.insert("startExportInvoked", invoked);
            report.insert("stateAfterCall", exporter->property("state").toInt());
        }
        write_report(path, report);
    });
    return true;
}

extern "C" __attribute__((visibility("default"))) bool dar_queue_restore_export_settings(const char* report_path) {
    const auto context = find_context();
    if (!context.exporter || !context.parent || !original_saved)
        return false;
    const QPointer<QObject> exporter(context.exporter);
    const QString path = QString::fromUtf8(report_path);
    QTimer::singleShot(0, context.parent, [exporter, path]() {
        QJsonObject report{{"exporterAlive", !exporter.isNull()}};
        if (exporter) {
            report.insert("setOutputDirInvoked",
                          QMetaObject::invokeMethod(
                              exporter, "setOutputDir", Qt::DirectConnection, Q_ARG(QUrl, original_output_dir)));
            report.insert("outputDir", exporter->property("outputDirString").toString());
            report.insert("exportName", exporter->property("exportName").toString());
        }
        write_report(path, report);
    });
    return true;
}

extern "C" __attribute__((visibility("default"))) bool dar_queue_start_probe_714(const char* report_path) {
    const auto context = find_context();
    if (!context.exporter || !context.master || !context.parent)
        return false;
    const QPointer<QObject> exporter(context.exporter), master(context.master);
    const QString path = QString::fromUtf8(report_path);
    QTimer::singleShot(0, context.parent, [exporter, master, path]() {
        QJsonObject report;
        const auto rows =
            exporter ? exporter->property("rerenderRowItems").value<QList<QObject*>>() : QList<QObject*>{};
        const bool ready = exporter && master && master->property("isOpened").toBool() &&
                           master->property("path").toString() == QString::fromUtf8(k_adm) &&
                           exporter->property("outputDirString").toString() == QString::fromUtf8(k_output) &&
                           exporter->property("outputDirError").toString().isEmpty() &&
                           exporter->property("enableMultichannelOutput").toBool() && rows.size() == 1 && rows[0] &&
                           rows[0]->property("layout").toString() == QStringLiteral("7.1.4") &&
                           rows[0]->property("selected").toBool();
        report.insert("ready", ready);
        if (ready) {
            report.insert("startExportInvoked",
                          QMetaObject::invokeMethod(exporter,
                                                    "startExport",
                                                    Qt::DirectConnection,
                                                    Q_ARG(QString, QStringLiteral("probe-axes-dar-714"))));
            report.insert("stateAfterCall", exporter->property("state").toInt());
        }
        write_report(path, report);
    });
    return true;
}
