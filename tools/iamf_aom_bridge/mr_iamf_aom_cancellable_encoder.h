#pragma once

#include <functional>
#include <string>

#include "absl/status/status.h"
#include "iamf/cli/proto/user_metadata.pb.h"

namespace iamf_tools {

using MrIamfCancelProbe = std::function<bool()>;

absl::Status TestMainCancellable(const iamf_tools_cli_proto::UserMetadata& user_metadata,
                                 const std::string& input_wav_directory,
                                 const std::string& output_iamf_directory,
                                 const MrIamfCancelProbe& cancel_probe);

} // namespace iamf_tools
