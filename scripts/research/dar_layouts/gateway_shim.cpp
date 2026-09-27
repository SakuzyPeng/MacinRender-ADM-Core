// Local diagnostic bridge for the user's running Dolby Atmos Renderer.
// First call only inspects Qt metaobjects; the second calls the gateway's
// existing public Qt-invokable method on the GUI thread. No code is patched.
#if defined(__aarch64__)
#include <arm_acle.h>
#endif
#include <QtCore/QCoreApplication>
#include <QtCore/QMetaMethod>
#include <QtCore/QObject>
#include <QtCore/QVariant>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtCore/QUrl>

#include <cstdio>
#include <cstring>
#include <string>

namespace {
QObject* find_gateway(unsigned* windows, unsigned* contexts) {
    *windows = 0;
    *contexts = 0;
    auto* app = QCoreApplication::instance();
    if (!app)
        return nullptr;
    for (auto* window : QGuiApplication::allWindows()) {
        ++*windows;
        for (auto* context = QQmlEngine::contextForObject(window); context; context = context->parentContext()) {
            ++*contexts;
            const QVariant value = context->contextProperty(QStringLiteral("importAtmosConfigGateway"));
            if (!value.canConvert<QObject*>())
                continue;
            auto* object = value.value<QObject*>();
            if (object && std::strstr(object->metaObject()->className(), "ImportAtmosConfigGateway"))
                return object;
        }
    }
    for (auto* object : app->findChildren<QObject*>()) {
        if (std::strstr(object->metaObject()->className(), "ImportAtmosConfigGateway"))
            return object;
    }
    return nullptr;
}

void report(FILE* file, QObject* gateway, unsigned windows, unsigned contexts) {
    std::fprintf(file, "{\"windows\":%u,\"contexts\":%u,\"found\":%s,\"methods\":[",
                 windows, contexts, gateway ? "true" : "false");
    if (gateway) {
        const auto* meta = gateway->metaObject();
        bool first = true;
        for (int i = 0; i < meta->methodCount(); ++i) {
            const QMetaMethod method = meta->method(i);
            if (!method.name().contains("AtmosConfig"))
                continue;
            if (!first)
                std::fputc(',', file);
            first = false;
            std::fprintf(file, "\"%s\"", method.methodSignature().constData());
        }
    }
    std::fputs("]}\n", file);
}
} // namespace

extern "C" __attribute__((visibility("default"))) bool dar_gateway_inspect(const char* path) {
    unsigned windows = 0, contexts = 0;
    auto* gateway = find_gateway(&windows, &contexts);
    FILE* output = std::fopen(path, "wx");
    if (output) {
        report(output, gateway, windows, contexts);
        std::fclose(output);
    }
    return gateway != nullptr;
}

extern "C" __attribute__((visibility("default"))) bool dar_gateway_open_config(const char* dac_path,
                                                                                 const char* report_path) {
    unsigned windows = 0, contexts = 0;
    auto* gateway = find_gateway(&windows, &contexts);
    const QUrl url = QUrl::fromLocalFile(QString::fromUtf8(dac_path));
    const bool invoked = gateway && QMetaObject::invokeMethod(gateway, "openAtmosConfig", Qt::DirectConnection,
                                                               Q_ARG(QUrl, url));
    FILE* output = std::fopen(report_path, "wx");
    if (output) {
        std::fprintf(output, "{\"found\":%s,\"invoked\":%s}\n",
                     gateway ? "true" : "false", invoked ? "true" : "false");
        std::fclose(output);
    }
    return invoked;
}
