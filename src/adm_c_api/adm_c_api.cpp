#include <memory>
#include <string>

#include "adm/c_api.h"
#include "adm/render.h"

namespace {

const char* stage_name(mradm::RenderStage stage) {
    switch (stage) {
    case mradm::RenderStage::validating:
        return "validating";
    case mradm::RenderStage::probing:
        return "probing";
    case mradm::RenderStage::importing_scene:
        return "importing_scene";
    case mradm::RenderStage::planning:
        return "planning";
    case mradm::RenderStage::rendering:
        return "rendering";
    case mradm::RenderStage::post_processing:
        return "post_processing";
    case mradm::RenderStage::finished:
        return "finished";
    }
    return "unknown";
}

adm_error_code_t map_error(mradm::ErrorCode code) {
    switch (code) {
    case mradm::ErrorCode::ok:
        return ADM_ERROR_OK;
    case mradm::ErrorCode::invalid_argument:
        return ADM_ERROR_INVALID_ARGUMENT;
    case mradm::ErrorCode::unsupported:
        return ADM_ERROR_UNSUPPORTED;
    case mradm::ErrorCode::io_error:
        return ADM_ERROR_IO;
    case mradm::ErrorCode::render_failed:
        return ADM_ERROR_RENDER_FAILED;
    case mradm::ErrorCode::cancelled:
        return ADM_ERROR_CANCELLED;
    case mradm::ErrorCode::internal_error:
        return ADM_ERROR_INTERNAL;
    }
    return ADM_ERROR_INTERNAL;
}

class CallbackProgressSink final : public mradm::ProgressSink {
  public:
    CallbackProgressSink(adm_progress_cb callback, void* user_data) : callback_(callback), user_data_(user_data) {}

    void on_progress(const mradm::ProgressEvent& event) override {
        if (callback_ == nullptr) {
            return;
        }
        const std::string message(event.message);
        callback_(event.fraction, stage_name(event.stage), message.c_str(), user_data_);
    }

  private:
    adm_progress_cb callback_{nullptr};
    void* user_data_{nullptr};
};

} // namespace

struct adm_context_t {
    mradm::RenderService service;
};

struct adm_render_result_t {
    adm_error_code_t code{ADM_ERROR_OK};
    std::string message;
};

int adm_api_version_major(void) { return ADM_API_VERSION_MAJOR; }
int adm_api_version_minor(void) { return ADM_API_VERSION_MINOR; }
int adm_api_version_patch(void) { return ADM_API_VERSION_PATCH; }

adm_context_t* adm_create_context(void) {
    return new adm_context_t{};
}

void adm_destroy_context(adm_context_t* context) {
    delete context;
}

adm_error_code_t adm_render_file(adm_context_t* context,
                                 const char* input_path,
                                 const char* output_path,
                                 adm_progress_cb progress,
                                 void* user_data,
                                 adm_render_result_t** result) {
    if (result != nullptr) {
        *result = nullptr;
    }
    if (context == nullptr || input_path == nullptr || input_path[0] == '\0') {
        return ADM_ERROR_INVALID_ARGUMENT;
    }

    mradm::RenderRequest request;
    request.input_path = input_path;
    if (output_path != nullptr && output_path[0] != '\0') {
        request.output_path = output_path;
    }

    CallbackProgressSink progress_sink(progress, user_data);
    mradm::NullLogSink log_sink;
    mradm::RenderResult cpp_result = context->service.render(request, progress_sink, log_sink);

    auto c_result = std::make_unique<adm_render_result_t>();
    c_result->code = map_error(cpp_result.error.code);
    c_result->message = cpp_result.error.message;
    const adm_error_code_t code = c_result->code;

    if (result != nullptr) {
        *result = c_result.release();
    }

    return code;
}

void adm_destroy_render_result(adm_render_result_t* result) {
    delete result;
}

adm_error_code_t adm_render_result_error_code(const adm_render_result_t* result) {
    if (result == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    return result->code;
}

const char* adm_render_result_message(const adm_render_result_t* result) {
    if (result == nullptr) {
        return "result is null";
    }
    return result->message.c_str();
}
