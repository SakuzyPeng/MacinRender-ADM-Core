// Temporarily switch the single existing home re-render between 9.1.6 and
// 7.1.4 for a controlled ADM comparison. Calls run on the Renderer event loop.
#if defined(__aarch64__)
#include <arm_acle.h>
#endif
#include <cstdio>

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMetaMethod>
#include <QtCore/QMetaProperty>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>

namespace {
struct GatewayContext {
    QObject* gateway;
    QObject* parent;
};
GatewayContext find_gateway() {
    for (auto* window : QGuiApplication::allWindows()) {
        for (auto* context = QQmlEngine::contextForObject(window); context; context = context->parentContext()) {
            auto* gateway = context->contextProperty(QStringLiteral("rerenderConfigGateway")).value<QObject*>();
            if (gateway && QString::fromUtf8(gateway->metaObject()->className()).contains("RerenderConfigGateway")) {
                return {gateway, window};
            }
        }
    }
    return {nullptr, nullptr};
}
bool write_report(const QString& path, const QJsonObject& report) {
    const auto utf8 = path.toUtf8();
    auto* file = std::fopen(utf8.constData(), "wx");
    if (!file)
        return false;
    const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    const bool okay =
        std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), file) == static_cast<size_t>(bytes.size());
    std::fclose(file);
    return okay;
}
QList<QObject*> rerenders(QObject* gateway) {
    return gateway->property("rerenderInfos").value<QList<QObject*>>();
}
bool target_available(QObject* gateway, int index, const QString& expected) {
    const auto options = gateway->property("layoutOptions").value<QList<QObject*>>();
    return index >= 0 && index < options.size() && options[index] &&
           options[index]->property("name").toString() == expected;
}
bool queue_stage(const char* report_path, const QString& before, const QString& after, int index) {
    const auto context = find_gateway();
    if (!context.gateway || !context.parent)
        return false;
    const QPointer<QObject> gateway(context.gateway);
    const QString path = QString::fromUtf8(report_path);
    QTimer::singleShot(0, context.parent, [gateway, path, before, after, index]() {
        QJsonObject report{{"target", after}};
        if (gateway) {
            const auto rows = rerenders(gateway);
            const bool ready = rows.size() == 1 && rows[0] && rows[0]->property("layout").toString() == before &&
                               rows[0]->property("name").toString() == QStringLiteral("re-render 02") &&
                               !rows[0]->property("mapped").toBool() &&
                               !gateway->property("rerenderProcessingActive").toBool() &&
                               target_available(gateway, index, after);
            report.insert("ready", ready);
            if (ready) {
                auto* row = rows[0];
                const int property_index = row->metaObject()->indexOfProperty("newLayoutIndex");
                const bool written =
                    property_index >= 0 && row->metaObject()->property(property_index).write(row, index);
                report.insert("newLayoutIndexWritten", written);
                if (written) {
                    report.insert(
                        "updateInvoked",
                        QMetaObject::invokeMethod(gateway, "updateRerender", Qt::DirectConnection, Q_ARG(int, 0)));
                    report.insert("layoutAfterUpdate", row->property("layout").toString());
                    report.insert("newLayoutIndex", row->property("newLayoutIndex").toInt());
                }
            }
        }
        write_report(path, report);
    });
    return true;
}
bool queue_apply(const char* report_path, const QString& target, int index) {
    const auto context = find_gateway();
    if (!context.gateway || !context.parent)
        return false;
    const QPointer<QObject> gateway(context.gateway);
    const QString path = QString::fromUtf8(report_path);
    QTimer::singleShot(0, context.parent, [gateway, path, target, index]() {
        QJsonObject report{{"target", target}};
        if (gateway) {
            const auto rows = rerenders(gateway);
            const bool ready = rows.size() == 1 && rows[0] && rows[0]->property("newLayoutIndex").toInt() == index &&
                               rows[0]->property("name").toString() == QStringLiteral("re-render 02") &&
                               !gateway->property("rerenderProcessingActive").toBool();
            report.insert("ready", ready);
            if (ready) {
                report.insert("layoutBeforeApply", rows[0]->property("layout").toString());
                report.insert("applyInvoked", QMetaObject::invokeMethod(gateway, "applyChanges", Qt::DirectConnection));
                report.insert("layoutAfterApply", rows[0]->property("layout").toString());
            }
        }
        write_report(path, report);
    });
    return true;
}
} // namespace

extern "C" __attribute__((visibility("default"))) bool dar_queue_stage_714(const char* path) {
    return queue_stage(path, QStringLiteral("9.1.6"), QStringLiteral("7.1.4"), 10);
}
extern "C" __attribute__((visibility("default"))) bool dar_queue_apply_714(const char* path) {
    return queue_apply(path, QStringLiteral("7.1.4"), 10);
}
extern "C" __attribute__((visibility("default"))) bool dar_queue_stage_916(const char* path) {
    return queue_stage(path, QStringLiteral("7.1.4"), QStringLiteral("9.1.6"), 12);
}
extern "C" __attribute__((visibility("default"))) bool dar_queue_apply_916(const char* path) {
    return queue_apply(path, QStringLiteral("9.1.6"), 12);
}
