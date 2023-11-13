/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "host/commands/cvd/server_command/start.h"

#include <sys/types.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>

#include <android-base/parseint.h>
#include <android-base/strings.h>

#include "common/libs/fs/shared_fd.h"
#include "common/libs/utils/contains.h"
#include "common/libs/utils/files.h"
#include "common/libs/utils/flag_parser.h"
#include "common/libs/utils/result.h"
#include "cvd_server.pb.h"
#include "host/commands/cvd/command_sequence.h"
#include "host/commands/cvd/common_utils.h"
#include "host/commands/cvd/server_command/server_handler.h"
#include "host/commands/cvd/server_command/subprocess_waiter.h"
#include "host/commands/cvd/server_command/utils.h"
#include "host/commands/cvd/types.h"
#include "host/libs/config/cuttlefish_config.h"

namespace cuttlefish {
namespace {

std::optional<std::string> GetConfigPath(cvd_common::Args& args) {
  int initial_size = args.size();
  std::string config_file;
  std::vector<Flag> config_flags = {
      GflagsCompatFlag("config_file", config_file)};
  auto result = ParseFlags(config_flags, args);
  if (!result.ok() || initial_size == args.size()) {
    return std::nullopt;
  }
  return config_file;
}

RequestWithStdio CreateLoadCommand(const RequestWithStdio& request,
                                   cvd_common::Args& args,
                                   const std::string& config_file) {
  cvd::Request request_proto;
  auto& load_command = *request_proto.mutable_command_request();
  *load_command.mutable_env() = request.Message().command_request().env();
  load_command.set_working_directory(
      request.Message().command_request().working_directory());
  load_command.add_args("cvd");
  load_command.add_args("load");
  for (const auto& arg : args) {
    load_command.add_args(arg);
  }
  load_command.add_args(config_file);
  return RequestWithStdio(request.Client(), request_proto,
                          request.FileDescriptors(), request.Credentials());
}

}  // namespace

class CvdStartCommandHandler : public CvdServerHandler {
 public:
  INJECT(CvdStartCommandHandler(InstanceManager& instance_manager,
                                HostToolTargetManager& host_tool_target_manager,
                                CommandSequenceExecutor& command_executor))
      : instance_manager_(instance_manager),
        host_tool_target_manager_(host_tool_target_manager),
        // TODO: b/300476262 - Migrate to using local instances rather than
        // constructor-injected ones
        command_executor_(command_executor),
        sub_action_ended_(false) {}

  Result<bool> CanHandle(const RequestWithStdio& request) const;
  Result<cvd::Response> Handle(const RequestWithStdio& request) override;
  Result<void> Interrupt() override;
  std::vector<std::string> CmdList() const override;

 private:
  Result<void> UpdateInstanceDatabase(
      const uid_t uid, const selector::GroupCreationInfo& group_creation_info);
  Result<void> FireCommand(Command&& command, const bool wait);
  bool HasHelpOpts(const cvd_common::Args& args) const;

  Result<Command> ConstructCvdNonHelpCommand(
      const std::string& bin_file,
      const selector::GroupCreationInfo& group_info,
      const RequestWithStdio& request);

  // call this only if !is_help
  Result<selector::GroupCreationInfo> GetGroupCreationInfo(
      const std::string& start_bin, const std::string& subcmd,
      const cvd_common::Args& subcmd_args, const cvd_common::Envs& envs,
      const RequestWithStdio& request);

  Result<cvd::Response> FillOutNewInstanceInfo(
      cvd::Response&& response,
      const selector::GroupCreationInfo& group_creation_info);

  struct UpdatedArgsAndEnvs {
    cvd_common::Args args;
    cvd_common::Envs envs;
  };
  Result<UpdatedArgsAndEnvs> UpdateInstanceArgsAndEnvs(
      cvd_common::Args&& args, cvd_common::Envs&& envs,
      const std::vector<selector::PerInstanceInfo>& instances,
      const std::string& artifacts_path, const std::string& start_bin);

  Result<selector::GroupCreationInfo> UpdateArgsAndEnvs(
      selector::GroupCreationInfo&& old_group_info,
      const std::string& start_bin);

  Result<std::string> FindStartBin(const std::string& android_host_out);

  static void MarkLockfiles(selector::GroupCreationInfo& group_info,
                            const InUseState state);
  static void MarkLockfilesInUse(selector::GroupCreationInfo& group_info) {
    MarkLockfiles(group_info, InUseState::kInUse);
  }

  /*
   * wait, remove the instance group if start failed, filling out the
   * response.
   */
  Result<cvd::Response> PostStartExecutionActions(
      std::optional<selector::GroupCreationInfo>& group_creation_info,
      const uid_t uid, const bool is_daemonized);
  Result<void> AcloudCompatActions(
      const selector::GroupCreationInfo& group_creation_info,
      const RequestWithStdio& request);
  Result<void> CreateSymlinks(
      const selector::GroupCreationInfo& group_creation_info);

  InstanceManager& instance_manager_;
  SubprocessWaiter subprocess_waiter_;
  HostToolTargetManager& host_tool_target_manager_;
  CommandSequenceExecutor& command_executor_;
  std::mutex interruptible_;
  bool interrupted_ = false;
  /*
   * Used by Interrupt() not to call command_executor_.Interrupt()
   *
   * If true, it is guaranteed that the command_executor_ ended the execution.
   * If false, it may or may not be after the command_executor_.Execute()
   */
  std::atomic<bool> sub_action_ended_;
  static const std::array<std::string, 2> supported_commands_;
};

Result<void> CvdStartCommandHandler::AcloudCompatActions(
    const selector::GroupCreationInfo& group_creation_info,
    const RequestWithStdio& request) {
  std::unique_lock interrupt_lock(interruptible_);
  CF_EXPECT(!interrupted_, "Interrupted");
  // rm -fr "TempDir()/acloud_cvd_temp/local-instance-<i>"
  std::string acloud_compat_home_prefix =
      TempDir() + "/acloud_cvd_temp/local-instance-";
  std::vector<std::string> acloud_compat_homes;
  acloud_compat_homes.reserve(group_creation_info.instances.size());
  for (const auto& instance : group_creation_info.instances) {
    acloud_compat_homes.push_back(
        ConcatToString(acloud_compat_home_prefix, instance.instance_id_));
  }
  for (const auto& acloud_compat_home : acloud_compat_homes) {
    bool result_deleted = true;
    std::stringstream acloud_compat_home_stream;
    if (!FileExists(acloud_compat_home)) {
      continue;
    }
    if (!Contains(group_creation_info.envs, kLaunchedByAcloud) ||
        group_creation_info.envs.at(kLaunchedByAcloud) != "true") {
      if (!DirectoryExists(acloud_compat_home, /*follow_symlinks=*/false)) {
        // cvd created a symbolic link
        result_deleted = RemoveFile(acloud_compat_home);
      } else {
        // acloud created a directory
        // rm -fr isn't supporetd by TreeHugger, so if we fork-and-exec to
        // literally run "rm -fr", the presubmit testing may fail if ever this
        // code is tested in the future.
        result_deleted = RecursivelyRemoveDirectory(acloud_compat_home);
      }
    }
    if (!result_deleted) {
      LOG(ERROR) << "Removing " << acloud_compat_home << " failed.";
      continue;
    }
  }

  // ln -f -s  [target] [symlink]
  // 1. mkdir -p home
  // 2. ln -f -s android_host_out home/host_bins
  // 3. for each i in ids,
  //     ln -f -s home /tmp/acloud_cvd_temp/local-instance-<i>
  std::vector<MakeRequestForm> request_forms;
  const cvd_common::Envs& common_envs = group_creation_info.envs;

  const std::string& home_dir = group_creation_info.home;
  const std::string client_pwd =
      request.Message().command_request().working_directory();
  request_forms.push_back(
      {.cmd_args = cvd_common::Args{"mkdir", "-p", home_dir},
       .env = common_envs,
       .selector_args = cvd_common::Args{},
       .working_dir = client_pwd});
  const std::string& android_host_out = group_creation_info.host_artifacts_path;
  request_forms.push_back(
      {.cmd_args = cvd_common::Args{"ln", "-T", "-f", "-s", android_host_out,
                                    home_dir + "/host_bins"},
       .env = common_envs,
       .selector_args = cvd_common::Args{},
       .working_dir = client_pwd});
  /* TODO(weihsu@): cvd acloud delete/list must handle multi-tenancy gracefully
   *
   * acloud delete just calls, for all instances in a group,
   *  /tmp/acloud_cvd_temp/local-instance-<i>/host_bins/stop_cvd
   *
   * That isn't necessary. Not desirable. Cvd acloud should read the instance
   * manager's in-memory data structure, and call stop_cvd once for the entire
   * group.
   *
   * Likewise, acloud list simply shows all instances in a flattened way. The
   * user has no clue about an instance group. Cvd acloud should show the
   * hierarchy.
   *
   * For now, we create the symbolic links so that it is compatible with acloud
   * in Python.
   */
  for (const auto& acloud_compat_home : acloud_compat_homes) {
    if (acloud_compat_home == home_dir) {
      LOG(ERROR) << "The \"HOME\" directory is acloud workspace, which will "
                 << "be deleted by next cvd start or acloud command with the"
                 << " same directory being \"HOME\"";
      continue;
    }
    request_forms.push_back({
        .cmd_args = cvd_common::Args{"ln", "-T", "-f", "-s", home_dir,
                                     acloud_compat_home},
        .env = common_envs,
        .selector_args = cvd_common::Args{},
        .working_dir = client_pwd,
    });
  }
  std::vector<cvd::Request> request_protos;
  for (const auto& request_form : request_forms) {
    request_protos.emplace_back(MakeRequest(request_form));
  }
  std::vector<RequestWithStdio> new_requests;
  auto dev_null = SharedFD::Open("/dev/null", O_RDWR);
  CF_EXPECT(dev_null->IsOpen(), dev_null->StrError());
  std::vector<SharedFD> dev_null_fds = {dev_null, dev_null, dev_null};
  for (auto& request_proto : request_protos) {
    new_requests.emplace_back(request.Client(), request_proto, dev_null_fds,
                              request.Credentials());
  }
  interrupt_lock.unlock();
  CF_EXPECT(command_executor_.Execute(new_requests, dev_null));
  return {};
}

void CvdStartCommandHandler::MarkLockfiles(
    selector::GroupCreationInfo& group_info, const InUseState state) {
  auto& instances = group_info.instances;
  for (auto& instance : instances) {
    if (!instance.instance_file_lock_) {
      continue;
    }
    auto result = instance.instance_file_lock_->Status(state);
    if (!result.ok()) {
      LOG(ERROR) << result.error().FormatForEnv();
    }
  }
}

Result<bool> CvdStartCommandHandler::CanHandle(
    const RequestWithStdio& request) const {
  auto invocation = ParseInvocation(request.Message());
  return Contains(supported_commands_, invocation.command);
}

Result<CvdStartCommandHandler::UpdatedArgsAndEnvs>
CvdStartCommandHandler::UpdateInstanceArgsAndEnvs(
    cvd_common::Args&& args, cvd_common::Envs&& envs,
    const std::vector<selector::PerInstanceInfo>& instances,
    const std::string& artifacts_path, const std::string& start_bin) {
  std::vector<unsigned> ids;
  ids.reserve(instances.size());
  for (const auto& instance : instances) {
    ids.emplace_back(instance.instance_id_);
  }

  cvd_common::Args new_args{std::move(args)};
  std::string old_instance_nums;
  std::string old_num_instances;
  std::string old_base_instance_num;

  std::vector<Flag> instance_id_flags{
      GflagsCompatFlag("instance_nums", old_instance_nums),
      GflagsCompatFlag("num_instances", old_num_instances),
      GflagsCompatFlag("base_instance_num", old_base_instance_num)};
  // discard old ones
  ParseFlags(instance_id_flags, new_args);

  auto check_flag = [artifacts_path, start_bin,
                     this](const std::string& flag_name) -> Result<void> {
    CF_EXPECT(
        host_tool_target_manager_.ReadFlag({.artifacts_path = artifacts_path,
                                            .op = "start",
                                            .flag_name = flag_name}));
    return {};
  };
  auto max = *(std::max_element(ids.cbegin(), ids.cend()));
  auto min = *(std::min_element(ids.cbegin(), ids.cend()));

  const bool is_consecutive = ((max - min) == (ids.size() - 1));
  const bool is_sorted = std::is_sorted(ids.begin(), ids.end());

  if (!is_consecutive || !is_sorted) {
    std::string flag_value = android::base::Join(ids, ",");
    CF_EXPECT(check_flag("instance_nums"));
    new_args.emplace_back("--instance_nums=" + flag_value);
    return UpdatedArgsAndEnvs{.args = std::move(new_args),
                              .envs = std::move(envs)};
  }

  // sorted and consecutive, so let's use old flags
  // like --num_instances and --base_instance_num
  if (ids.size() > 1) {
    CF_EXPECT(check_flag("num_instances"),
              "--num_instances is not supported but multi-tenancy requested.");
    new_args.emplace_back("--num_instances=" + std::to_string(ids.size()));
  }
  cvd_common::Envs new_envs{std::move(envs)};
  if (check_flag("base_instance_num").ok()) {
    new_args.emplace_back("--base_instance_num=" + std::to_string(min));
  }
  new_envs[kCuttlefishInstanceEnvVarName] = std::to_string(min);
  return UpdatedArgsAndEnvs{.args = std::move(new_args),
                            .envs = std::move(new_envs)};
}

Result<Command> CvdStartCommandHandler::ConstructCvdNonHelpCommand(
    const std::string& bin_file, const selector::GroupCreationInfo& group_info,
    const RequestWithStdio& request) {
  auto bin_path = group_info.host_artifacts_path;
  bin_path.append("/bin/").append(bin_file);
  CF_EXPECT(!group_info.home.empty());
  ConstructCommandParam construct_cmd_param{
      .bin_path = bin_path,
      .home = group_info.home,
      .args = group_info.args,
      .envs = group_info.envs,
      .working_dir = request.Message().command_request().working_directory(),
      .command_name = bin_file,
      .in = request.In(),
      .out = request.Out(),
      .err = request.Err()};
  Command non_help_command = CF_EXPECT(ConstructCommand(construct_cmd_param));
  return non_help_command;
}

// call this only if !is_help
Result<selector::GroupCreationInfo>
CvdStartCommandHandler::GetGroupCreationInfo(
    const std::string& start_bin, const std::string& subcmd,
    const std::vector<std::string>& subcmd_args, const cvd_common::Envs& envs,
    const RequestWithStdio& request) {
  using CreationAnalyzerParam =
      selector::CreationAnalyzer::CreationAnalyzerParam;
  const auto& selector_opts =
      request.Message().command_request().selector_opts();
  const auto selector_args = cvd_common::ConvertToArgs(selector_opts.args());
  CreationAnalyzerParam analyzer_param{
      .cmd_args = subcmd_args, .envs = envs, .selector_args = selector_args};
  auto cred = CF_EXPECT(request.Credentials());
  auto group_creation_info =
      CF_EXPECT(instance_manager_.Analyze(subcmd, analyzer_param, cred));
  auto final_group_creation_info =
      CF_EXPECT(UpdateArgsAndEnvs(std::move(group_creation_info), start_bin));
  return final_group_creation_info;
}

Result<selector::GroupCreationInfo> CvdStartCommandHandler::UpdateArgsAndEnvs(
    selector::GroupCreationInfo&& old_group_info,
    const std::string& start_bin) {
  selector::GroupCreationInfo group_creation_info = std::move(old_group_info);
  // update instance related-flags, envs
  const auto& instances = group_creation_info.instances;
  const auto& host_artifacts_path = group_creation_info.host_artifacts_path;
  auto [new_args, new_envs] = CF_EXPECT(UpdateInstanceArgsAndEnvs(
      std::move(group_creation_info.args), std::move(group_creation_info.envs),
      instances, host_artifacts_path, start_bin));
  group_creation_info.args = std::move(new_args);
  group_creation_info.envs = std::move(new_envs);

  // for backward compatibility, older cvd host tools don't accept group_id
  auto has_group_id_flag =
      host_tool_target_manager_
          .ReadFlag({.artifacts_path = group_creation_info.host_artifacts_path,
                     .op = "start",
                     .flag_name = "group_id"})
          .ok();
  if (has_group_id_flag) {
    group_creation_info.args.emplace_back("--group_id=" +
                                          group_creation_info.group_name);
  }

  group_creation_info.envs["HOME"] = group_creation_info.home;
  group_creation_info.envs[kAndroidHostOut] =
      group_creation_info.host_artifacts_path;
  group_creation_info.envs[kAndroidProductOut] =
      group_creation_info.product_out_path;
  /* b/253644566
   *
   * Old branches used kAndroidSoongHostOut instead of kAndroidHostOut
   */
  group_creation_info.envs[kAndroidSoongHostOut] =
      group_creation_info.host_artifacts_path;
  group_creation_info.envs[kCvdMarkEnv] = "true";
  return group_creation_info;
}

static std::ostream& operator<<(std::ostream& out, const cvd_common::Args& v) {
  if (v.empty()) {
    return out;
  }
  for (int i = 0; i < v.size() - 1; i++) {
    out << v.at(i) << " ";
  }
  out << v.back();
  return out;
}

static void ShowLaunchCommand(const std::string& bin,
                              const cvd_common::Args& args,
                              const cvd_common::Envs& envs) {
  std::stringstream ss;
  std::vector<std::string> interesting_env_names{"HOME",
                                                 kAndroidHostOut,
                                                 kAndroidSoongHostOut,
                                                 "ANDROID_PRODUCT_OUT",
                                                 kCuttlefishInstanceEnvVarName,
                                                 kCuttlefishConfigEnvVarName};
  for (const auto& interesting_env_name : interesting_env_names) {
    if (Contains(envs, interesting_env_name)) {
      ss << interesting_env_name << "=\"" << envs.at(interesting_env_name)
         << "\" ";
    }
  }
  ss << " " << bin << " " << args;
  LOG(ERROR) << "launcher command: " << ss.str();
}

static void ShowLaunchCommand(const std::string& bin,
                              selector::GroupCreationInfo& group_info) {
  ShowLaunchCommand(bin, group_info.args, group_info.envs);
}

Result<std::string> CvdStartCommandHandler::FindStartBin(
    const std::string& android_host_out) {
  auto start_bin = CF_EXPECT(host_tool_target_manager_.ExecBaseName({
      .artifacts_path = android_host_out,
      .op = "start",
  }));
  return start_bin;
}

Result<bool> IsDaemonModeFlag(const cvd_common::Args& args) {
  bool flag_set = false;
  bool is_daemon = true;
  Flag flag =
      Flag()
          .Alias({FlagAliasMode::kFlagPrefix, "-daemon="})
          .Alias({FlagAliasMode::kFlagPrefix, "--daemon="})
          .Alias({FlagAliasMode::kFlagExact, "-daemon"})
          .Alias({FlagAliasMode::kFlagExact, "--daemon"})
          .Alias({FlagAliasMode::kFlagExact, "-nodaemon"})
          .Alias({FlagAliasMode::kFlagExact, "--nodaemon"})
          .Setter([&is_daemon,
                   &flag_set](const FlagMatch& match) -> Result<void> {
            flag_set = true;
            if (match.key == match.value) {
              is_daemon = match.key.find("no") == std::string::npos;
              return {};
            }
            CF_EXPECTF(match.value.find(",") == std::string::npos,
                       "{} had a comma", match.value);
            static constexpr std::string_view kFalseStrings[] = {"n", "no",
                                                                 "false"};
            for (const auto& falseString : kFalseStrings) {
              if (android::base::EqualsIgnoreCase(falseString, match.value)) {
                is_daemon = false;
              }
            }
            // Allow `cvd_internal_start` to produce its own error for other
            // invalid strings.
            return {};
          });
  auto args_copy = args;
  CF_EXPECT(ParseFlags({flag}, args_copy));
  return flag_set && is_daemon;
}

// For backward compatibility, we add extra symlink in system wide home
// when HOME is NOT overridden and selector flags are NOT given.
Result<void> CvdStartCommandHandler::CreateSymlinks(
    const selector::GroupCreationInfo& group_creation_info) {
  std::string instance_home_dir = "";
  auto system_wide_home = CF_EXPECT(SystemWideUserHome());
  for (const auto& instance : group_creation_info.instances) {
    std::string legacy_path = system_wide_home + "/cuttlefish_runtime.";
    legacy_path = ConcatToString(legacy_path, instance.instance_id_);
    instance_home_dir = group_creation_info.home;
    instance_home_dir = instance_home_dir + "/cuttlefish/instances/cvd-";
    instance_home_dir = ConcatToString(instance_home_dir, instance.instance_id_);
    if (DirectoryExists(legacy_path, /* follow_symlinks */ true)) {
      CF_EXPECT(RecursivelyRemoveDirectory(legacy_path),
                "Failed to remove legacy directory " << legacy_path);
    }
    if (symlink(instance_home_dir.c_str(), legacy_path.c_str())) {
      return CF_ERRNO("symlink(\"" << instance_home_dir << "\", \""
                                   << legacy_path << "\") failed");
    }
    legacy_path = system_wide_home + "/cuttlefish_runtime";
    if (DirectoryExists(legacy_path, true)) {
      CF_EXPECT(RecursivelyRemoveDirectory(legacy_path),
                "Failed to remove legacy directory " << legacy_path);
    }
    if (symlink(instance_home_dir.c_str(), legacy_path.c_str())) {
      return CF_ERRNO("symlink(\"" << instance_home_dir << "\", \""
                                   << legacy_path << "\") failed");
    }
    std::string cuttlefish_path = group_creation_info.home + "/cuttlefish/";
    legacy_path = system_wide_home + "/cuttlefish";
    if (DirectoryExists(legacy_path,  true)) {
      CF_EXPECT(RecursivelyRemoveDirectory(legacy_path),
                "Failed to remove legacy directory " << legacy_path);
    }
    if (symlink(cuttlefish_path.c_str(), legacy_path.c_str())) {
      return CF_ERRNO("symlink(\"" << cuttlefish_path << "\", \"" << legacy_path
                                   << "\") failed");
    }
    std::string cuttlefish_assembly_path = cuttlefish_path + "assembly/";
    legacy_path = system_wide_home + "/cuttlefish_assembly";
    if (DirectoryExists(legacy_path,  true)) {
      CF_EXPECT(RecursivelyRemoveDirectory(legacy_path),
                "Failed to remove legacy directory " << legacy_path);
    }
    if (symlink(cuttlefish_assembly_path.c_str(), legacy_path.c_str())) {
      return CF_ERRNO("symlink(\"" << cuttlefish_assembly_path << "\", \""
                                   << legacy_path << "\") failed");
    }
    std::string config_path =
        cuttlefish_assembly_path + "cuttlefish_config.json";
    legacy_path = system_wide_home + "/.cuttlefish_config.json";
    if (FileExists(legacy_path,  false)) {
      CF_EXPECT(RemoveFile(legacy_path),
                "Failed to remove instance_dir symlink " << legacy_path);
    }
    if (symlink(config_path.c_str(), legacy_path.c_str())) {
      return CF_ERRNO("symlink(\"" << config_path << "\", \"" << legacy_path
                                   << "\") failed");
    }
  }
  return {};
}

Result<cvd::Response> CvdStartCommandHandler::Handle(
    const RequestWithStdio& request) {
  std::unique_lock interrupt_lock(interruptible_);
  if (interrupted_) {
    return CF_ERR("Interrupted");
  }
  CF_EXPECT(CanHandle(request));

  cvd::Response response;
  response.mutable_command_response();

  auto [subcmd, subcmd_args] = ParseInvocation(request.Message());
  std::optional<std::string> config_file = GetConfigPath(subcmd_args);
  if (config_file) {
    auto subrequest = CreateLoadCommand(request, subcmd_args, *config_file);
    interrupt_lock.unlock();
    response =
        CF_EXPECT(command_executor_.ExecuteOne(subrequest, request.Err()));
    sub_action_ended_ = true;
    return response;
  }

  auto precondition_verified = VerifyPrecondition(request);
  if (!precondition_verified.ok()) {
    response.mutable_status()->set_code(cvd::Status::FAILED_PRECONDITION);
    response.mutable_status()->set_message(
        precondition_verified.error().Message());
    return response;
  }

  const uid_t uid = request.Credentials()->uid;
  cvd_common::Envs envs =
      cvd_common::ConvertToEnvs(request.Message().command_request().env());
  if (Contains(envs, "HOME")) {
    if (envs.at("HOME").empty()) {
      envs.erase("HOME");
    } else {
      // As the end-user may override HOME, this could be a relative path
      // to client's pwd, or may include "~" which is the client's actual
      // home directory.
      auto client_pwd = request.Message().command_request().working_directory();
      const auto given_home_dir = envs.at("HOME");
      /*
       * Imagine this scenario:
       *   client$ export HOME=/tmp/new/dir
       *   client$ HOME="~/subdir" cvd start
       *
       * The value of ~ isn't sent to the server. The server can't figure that
       * out as it might be overridden before the cvd start command.
       */
      CF_EXPECT(!android::base::StartsWith(given_home_dir, "~") &&
                    !android::base::StartsWith(given_home_dir, "~/"),
                "The HOME directory should not start with ~");
      envs["HOME"] = CF_EXPECT(
          EmulateAbsolutePath({.current_working_dir = client_pwd,
                               .home_dir = CF_EXPECT(SystemWideUserHome(uid)),
                               .path_to_convert = given_home_dir,
                               .follow_symlink = false}));
    }
  }
  CF_EXPECT(Contains(envs, kAndroidHostOut));
  const auto bin = CF_EXPECT(FindStartBin(envs.at(kAndroidHostOut)));

  // update DB if not help
  // collect group creation infos
  CF_EXPECT(Contains(supported_commands_, subcmd),
            "subcmd should be start but is " << subcmd);
  const bool is_help = HasHelpOpts(subcmd_args);
  const bool is_daemon = CF_EXPECT(IsDaemonModeFlag(subcmd_args));

  std::optional<selector::GroupCreationInfo> group_creation_info;
  if (!is_help) {
    group_creation_info = CF_EXPECT(
        GetGroupCreationInfo(bin, subcmd, subcmd_args, envs, request));
    CF_EXPECT(UpdateInstanceDatabase(uid, *group_creation_info));
    response = CF_EXPECT(
        FillOutNewInstanceInfo(std::move(response), *group_creation_info));
  }

  Command command =
      is_help
          ? CF_EXPECT(ConstructCvdHelpCommand(bin, envs, subcmd_args, request))
          : CF_EXPECT(
                ConstructCvdNonHelpCommand(bin, *group_creation_info, request));

  if (!is_help) {
    CF_EXPECT(
        group_creation_info != std::nullopt,
        "group_creation_info should be nullopt only when --help is given.");
  }

  if (is_help) {
    ShowLaunchCommand(command.Executable(), subcmd_args, envs);
  } else {
    ShowLaunchCommand(command.Executable(), *group_creation_info);
    CF_EXPECT(request.Message().command_request().wait_behavior() !=
              cvd::WAIT_BEHAVIOR_START);
  }

  FireCommand(std::move(command), /*should_wait*/ true);
  interrupt_lock.unlock();

  if (is_help) {
    auto infop = CF_EXPECT(subprocess_waiter_.Wait());
    return ResponseFromSiginfo(infop);
  }

  // For backward compatibility, we add extra symlink in system wide home
  // when HOME is NOT overridden and selector flags are NOT given.
  if (group_creation_info->is_default_group) {
    CreateSymlinks(*group_creation_info);
  }

  // make acquire interrupt_lock inside.
  auto acloud_compat_action_result =
      AcloudCompatActions(*group_creation_info, request);
  sub_action_ended_ = true;
  if (!acloud_compat_action_result.ok()) {
    LOG(ERROR) << acloud_compat_action_result.error().FormatForEnv();
    LOG(ERROR) << "AcloudCompatActions() failed"
               << " but continue as they are minor errors.";
  }
  return PostStartExecutionActions(group_creation_info, uid, is_daemon);
}

Result<cvd::Response> CvdStartCommandHandler::PostStartExecutionActions(
    std::optional<selector::GroupCreationInfo>& group_creation_info,
    const uid_t uid, const bool is_daemonized) {
  auto infop = CF_EXPECT(subprocess_waiter_.Wait());
  if (infop.si_code != CLD_EXITED || infop.si_status != EXIT_SUCCESS) {
    // perhaps failed in launch
    instance_manager_.RemoveInstanceGroup(uid, group_creation_info->home);
  }
  auto final_response = ResponseFromSiginfo(infop);
  if (!final_response.has_status() ||
      final_response.status().code() != cvd::Status::OK) {
    return final_response;
  }
  if (is_daemonized) {
    // If not daemonized, reaching here means the instance group terminated.
    // Thus, it's enough to release the file lock in the destructor.
    // If daemonized, reaching here means the group started successfully
    // As the destructor will release the file lock, the instance lock
    // files must be marked as used
    MarkLockfilesInUse(*group_creation_info);
  }
  // group_creation_info is nullopt only if is_help is false
  return FillOutNewInstanceInfo(std::move(final_response),
                                *group_creation_info);
}

Result<void> CvdStartCommandHandler::Interrupt() {
  std::scoped_lock interrupt_lock(interruptible_);
  interrupted_ = true;
  if (!sub_action_ended_) {
    auto result = command_executor_.Interrupt();
    if (!result.ok()) {
      LOG(ERROR) << "Failed to interrupt CommandExecutor"
                 << result.error().FormatForEnv();
    }
  }
  CF_EXPECT(subprocess_waiter_.Interrupt());
  return {};
}

Result<cvd::Response> CvdStartCommandHandler::FillOutNewInstanceInfo(
    cvd::Response&& response,
    const selector::GroupCreationInfo& group_creation_info) {
  auto new_response = std::move(response);
  auto& command_response = *(new_response.mutable_command_response());
  auto& instance_group_info =
      *(CF_EXPECT(command_response.mutable_instance_group_info()));
  instance_group_info.set_group_name(group_creation_info.group_name);
  instance_group_info.add_home_directories(group_creation_info.home);
  for (const auto& per_instance_info : group_creation_info.instances) {
    auto* new_entry = CF_EXPECT(instance_group_info.add_instances());
    new_entry->set_name(per_instance_info.per_instance_name_);
    new_entry->set_instance_id(per_instance_info.instance_id_);
  }
  return new_response;
}

Result<void> CvdStartCommandHandler::UpdateInstanceDatabase(
    const uid_t uid, const selector::GroupCreationInfo& group_creation_info) {
  CF_EXPECT(instance_manager_.SetInstanceGroup(uid, group_creation_info),
            group_creation_info.home
                << " is already taken so can't create new instance.");
  return {};
}

Result<void> CvdStartCommandHandler::FireCommand(Command&& command,
                                                 const bool wait) {
  SubprocessOptions options;
  if (!wait) {
    options.ExitWithParent(false);
  }
  CF_EXPECT(subprocess_waiter_.Setup(command.Start(options)));
  return {};
}

bool CvdStartCommandHandler::HasHelpOpts(
    const std::vector<std::string>& args) const {
  return IsHelpSubcmd(args);
}

std::vector<std::string> CvdStartCommandHandler::CmdList() const {
  std::vector<std::string> subcmd_list;
  subcmd_list.reserve(supported_commands_.size());
  for (const auto& cmd : supported_commands_) {
    subcmd_list.emplace_back(cmd);
  }
  return subcmd_list;
}

const std::array<std::string, 2> CvdStartCommandHandler::supported_commands_{
    "start", "launch_cvd"};

fruit::Component<fruit::Required<InstanceManager, HostToolTargetManager,
                                 CommandSequenceExecutor>>
CvdStartCommandComponent() {
  return fruit::createComponent()
      .addMultibinding<CvdServerHandler, CvdStartCommandHandler>();
}

}  // namespace cuttlefish
