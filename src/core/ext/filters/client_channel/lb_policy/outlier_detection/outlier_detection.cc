//
// Copyright 2022 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <grpc/support/port_platform.h>

#include "src/core/ext/filters/client_channel/lb_policy/outlier_detection/outlier_detection.h"

#include <stddef.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/inlined_vector.h"
#include "absl/memory/memory.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/variant.h"

#include <grpc/impl/codegen/connectivity_state.h>
#include <grpc/impl/codegen/grpc_types.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>

#include "src/core/ext/filters/client_channel/lb_policy.h"
#include "src/core/ext/filters/client_channel/lb_policy/child_policy_handler.h"
#include "src/core/ext/filters/client_channel/lb_policy_factory.h"
#include "src/core/ext/filters/client_channel/lb_policy_registry.h"
#include "src/core/ext/filters/client_channel/subchannel_interface.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/debug/trace.h"
#include "src/core/lib/gpr/env.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gprpp/debug_location.h"
#include "src/core/lib/gprpp/orphanable.h"
#include "src/core/lib/gprpp/ref_counted.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/iomgr/closure.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/iomgr/iomgr_fwd.h"
#include "src/core/lib/iomgr/pollset_set.h"
#include "src/core/lib/iomgr/timer.h"
#include "src/core/lib/iomgr/work_serializer.h"
#include "src/core/lib/json/json.h"
#include "src/core/lib/json/json_util.h"
#include "src/core/lib/resolver/server_address.h"
#include "src/core/lib/transport/connectivity_state.h"

namespace grpc_core {

TraceFlag grpc_outlier_detection_lb_trace(false, "outlier_detection_lb");

// TODO(donnadionne): Remove once outlier detection is no longer experimental
bool XdsOutlierDetectionEnabled() {
  char* value = gpr_getenv("GRPC_EXPERIMENTAL_ENABLE_OUTLIER_DETECTION");
  bool parsed_value;
  bool parse_succeeded = gpr_parse_bool_value(value, &parsed_value);
  gpr_free(value);
  return parse_succeeded && parsed_value;
}

namespace {

constexpr char kOutlierDetection[] = "outlier_detection_experimental";

// Config for xDS Cluster Impl LB policy.
class OutlierDetectionLbConfig : public LoadBalancingPolicy::Config {
 public:
  OutlierDetectionLbConfig(
      OutlierDetectionConfig outlier_detection_config,
      RefCountedPtr<LoadBalancingPolicy::Config> child_policy)
      : outlier_detection_config_(outlier_detection_config),
        child_policy_(std::move(child_policy)) {}

  const char* name() const override { return kOutlierDetection; }

  bool CountingEnabled() const {
    return (
        outlier_detection_config_.interval != Duration::Infinity() &&
        (outlier_detection_config_.success_rate_ejection.has_value() ||
         outlier_detection_config_.failure_percentage_ejection.has_value()));
  }

  const OutlierDetectionConfig& outlier_detection_config() const {
    return outlier_detection_config_;
  }

  RefCountedPtr<LoadBalancingPolicy::Config> child_policy() const {
    return child_policy_;
  }

 private:
  OutlierDetectionConfig outlier_detection_config_;
  RefCountedPtr<LoadBalancingPolicy::Config> child_policy_;
};

// xDS Cluster Impl LB policy.
class OutlierDetectionLb : public LoadBalancingPolicy {
 public:
  explicit OutlierDetectionLb(Args args);

  const char* name() const override { return kOutlierDetection; }

  void UpdateLocked(UpdateArgs args) override;
  void ExitIdleLocked() override;
  void ResetBackoffLocked() override;

 private:
  class SubchannelState;
  class SubchannelWrapper : public DelegatingSubchannel {
   public:
    SubchannelWrapper(RefCountedPtr<SubchannelState> subchannel_state,
                      RefCountedPtr<SubchannelInterface> subchannel)
        : DelegatingSubchannel(std::move(subchannel)),
          subchannel_state_(std::move(subchannel_state)) {
      if (subchannel_state_ != nullptr) {
        subchannel_state_->AddSubchannel(this);
        if (subchannel_state_->ejection_time().has_value()) {
          ejected_ = true;
        }
      }
    }

    ~SubchannelWrapper() override {
      if (subchannel_state_ != nullptr) {
        subchannel_state_->RemoveSubchannel(this);
      }
    }

    void Eject();

    void Uneject();

    void WatchConnectivityState(
        std::unique_ptr<ConnectivityStateWatcherInterface> watcher) override;

    void CancelConnectivityStateWatch(
        ConnectivityStateWatcherInterface* watcher) override;

    RefCountedPtr<SubchannelState> subchannel_state() const {
      return subchannel_state_;
    }

   private:
    class WatcherWrapper
        : public SubchannelInterface::ConnectivityStateWatcherInterface {
     public:
      WatcherWrapper(std::unique_ptr<
                         SubchannelInterface::ConnectivityStateWatcherInterface>
                         watcher,
                     bool ejected)
          : watcher_(std::move(watcher)), ejected_(ejected) {}

      void Eject() {
        ejected_ = true;
        if (last_seen_state_.has_value()) {
          watcher_->OnConnectivityStateChange(
              GRPC_CHANNEL_TRANSIENT_FAILURE,
              absl::UnavailableError(
                  "subchannel ejected by outlier detection"));
        }
      }

      void Uneject() {
        ejected_ = false;
        if (last_seen_state_.has_value()) {
          watcher_->OnConnectivityStateChange(*last_seen_state_,
                                              last_seen_status_);
        }
      }

      void OnConnectivityStateChange(grpc_connectivity_state new_state,
                                     absl::Status status) override {
        const bool send_update = !last_seen_state_.has_value() || !ejected_;
        last_seen_state_ = new_state;
        last_seen_status_ = status;
        if (send_update) {
          if (ejected_) {
            new_state = GRPC_CHANNEL_TRANSIENT_FAILURE;
            status = absl::UnavailableError(
                "subchannel ejected by outlier detection");
          }
          watcher_->OnConnectivityStateChange(new_state, status);
        }
      }

      grpc_pollset_set* interested_parties() override {
        return watcher_->interested_parties();
      }

     private:
      std::unique_ptr<SubchannelInterface::ConnectivityStateWatcherInterface>
          watcher_;
      absl::optional<grpc_connectivity_state> last_seen_state_;
      absl::Status last_seen_status_;
      bool ejected_;
    };

    RefCountedPtr<SubchannelState> subchannel_state_;
    bool ejected_ = false;
    std::map<SubchannelInterface::ConnectivityStateWatcherInterface*,
             WatcherWrapper*>
        watchers_;
  };

  class SubchannelState : public RefCounted<SubchannelState> {
   public:
    struct Bucket {
      std::atomic<uint64_t> successes;
      std::atomic<uint64_t> failures;
    };

    void RotateBucket() {
      backup_bucket_->successes = 0;
      backup_bucket_->failures = 0;
      current_bucket_.swap(backup_bucket_);
      active_bucket_.store(current_bucket_.get());
    }

    absl::optional<std::pair<double, uint64_t>> GetSuccessRateAndVolume() {
      uint64_t total_request =
          backup_bucket_->successes + backup_bucket_->failures;
      if (total_request == 0) {
        return absl::nullopt;
      }
      double success_rate =
          backup_bucket_->successes * 100.0 /
          (backup_bucket_->successes + backup_bucket_->failures);
      return {
          {success_rate, backup_bucket_->successes + backup_bucket_->failures}};
    }

    void AddSubchannel(SubchannelWrapper* wrapper) {
      subchannels_.insert(wrapper);
    }

    void RemoveSubchannel(SubchannelWrapper* wrapper) {
      subchannels_.erase(wrapper);
    }

    void AddSuccessCount() { active_bucket_.load()->successes.fetch_add(1); }

    void AddFailureCount() { active_bucket_.load()->failures.fetch_add(1); }

    absl::optional<Timestamp> ejection_time() const { return ejection_time_; }

    void Eject(const Timestamp& time) {
      ejection_time_ = time;
      ++multiplier_;
      for (auto& subchannel : subchannels_) {
        subchannel->Eject();
      }
    }

    void Uneject() {
      ejection_time_.reset();
      for (auto& subchannel : subchannels_) {
        subchannel->Uneject();
      }
    }

    void MaybeUneject(uint64_t base_ejection_time_in_millis,
                      uint64_t max_ejection_time_in_millis) {
      if (!ejection_time_.has_value()) {
        if (multiplier_ > 0) {
          --multiplier_;
        }
      } else {
        GPR_ASSERT(ejection_time_.has_value());
        auto change_time = ejection_time_.value() +
                           Duration::Milliseconds(std::min(
                               base_ejection_time_in_millis * multiplier_,
                               std::max(base_ejection_time_in_millis,
                                        max_ejection_time_in_millis)));
        if (change_time < ExecCtx::Get()->Now()) {
          Uneject();
        }
      }
    }

   private:
    std::unique_ptr<Bucket> current_bucket_ = absl::make_unique<Bucket>();
    std::unique_ptr<Bucket> backup_bucket_ = absl::make_unique<Bucket>();
    // The bucket used to update call counts.
    // Points to either current_bucket or active_bucket.
    std::atomic<Bucket*> active_bucket_{current_bucket_.get()};
    uint32_t multiplier_ = 0;
    absl::optional<Timestamp> ejection_time_;
    std::set<SubchannelWrapper*> subchannels_;
  };

  // A simple wrapper for ref-counting a picker from the child policy.
  class RefCountedPicker : public RefCounted<RefCountedPicker> {
   public:
    explicit RefCountedPicker(std::unique_ptr<SubchannelPicker> picker)
        : picker_(std::move(picker)) {}
    PickResult Pick(PickArgs args) { return picker_->Pick(args); }

   private:
    std::unique_ptr<SubchannelPicker> picker_;
  };

  // A picker that wraps the picker from the child to perform outlier detection.
  class Picker : public SubchannelPicker {
   public:
    Picker(OutlierDetectionLb* outlier_detection_lb,
           RefCountedPtr<RefCountedPicker> picker, bool counting_enabled);

    PickResult Pick(PickArgs args) override;

   private:
    class SubchannelCallTracker;
    RefCountedPtr<RefCountedPicker> picker_;
    bool counting_enabled_;
  };

  class Helper : public ChannelControlHelper {
   public:
    explicit Helper(RefCountedPtr<OutlierDetectionLb> outlier_detection_policy)
        : outlier_detection_policy_(std::move(outlier_detection_policy)) {}

    ~Helper() override {
      outlier_detection_policy_.reset(DEBUG_LOCATION, "Helper");
    }

    RefCountedPtr<SubchannelInterface> CreateSubchannel(
        ServerAddress address, const grpc_channel_args& args) override;
    void UpdateState(grpc_connectivity_state state, const absl::Status& status,
                     std::unique_ptr<SubchannelPicker> picker) override;
    void RequestReresolution() override;
    absl::string_view GetAuthority() override;
    void AddTraceEvent(TraceSeverity severity,
                       absl::string_view message) override;

   private:
    RefCountedPtr<OutlierDetectionLb> outlier_detection_policy_;
  };

  class EjectionTimer : public InternallyRefCounted<EjectionTimer> {
   public:
    EjectionTimer(RefCountedPtr<OutlierDetectionLb> parent,
                  Timestamp start_time);

    void Orphan() override;

    Timestamp StartTime() const { return start_time_; }

   private:
    static void OnTimer(void* arg, grpc_error_handle error);
    void OnTimerLocked(grpc_error_handle);

    RefCountedPtr<OutlierDetectionLb> parent_;
    grpc_timer timer_;
    grpc_closure on_timer_;
    bool timer_pending_ = true;
    Timestamp start_time_;
    absl::BitGen bit_gen_;
  };

  ~OutlierDetectionLb() override;

  static std::string MakeKeyForAddress(const ServerAddress& address);

  void ShutdownLocked() override;

  OrphanablePtr<LoadBalancingPolicy> CreateChildPolicyLocked(
      const grpc_channel_args* args);

  void MaybeUpdatePickerLocked();

  // Current config from the resolver.
  RefCountedPtr<OutlierDetectionLbConfig> config_;

  // Internal state.
  bool shutting_down_ = false;

  OrphanablePtr<LoadBalancingPolicy> child_policy_;

  // Latest state and picker reported by the child policy.
  grpc_connectivity_state state_ = GRPC_CHANNEL_IDLE;
  absl::Status status_;
  RefCountedPtr<RefCountedPicker> picker_;
  std::map<std::string, RefCountedPtr<SubchannelState>> subchannel_state_map_;
  OrphanablePtr<EjectionTimer> ejection_timer_;
};

//
// OutlierDetectionLb::SubchannelWrapper
//

void OutlierDetectionLb::SubchannelWrapper::Eject() {
  ejected_ = true;
  for (auto& watcher : watchers_) {
    watcher.second->Eject();
  }
}

void OutlierDetectionLb::SubchannelWrapper::Uneject() {
  ejected_ = false;
  for (auto& watcher : watchers_) {
    watcher.second->Uneject();
  }
}

void OutlierDetectionLb::SubchannelWrapper::WatchConnectivityState(
    std::unique_ptr<ConnectivityStateWatcherInterface> watcher) {
  ConnectivityStateWatcherInterface* watcher_ptr = watcher.get();
  auto watcher_wrapper =
      absl::make_unique<WatcherWrapper>(std::move(watcher), ejected_);
  watchers_.emplace(watcher_ptr, watcher_wrapper.get());
  wrapped_subchannel()->WatchConnectivityState(std::move(watcher_wrapper));
}

void OutlierDetectionLb::SubchannelWrapper::CancelConnectivityStateWatch(
    ConnectivityStateWatcherInterface* watcher) {
  auto it = watchers_.find(watcher);
  if (it == watchers_.end()) return;
  wrapped_subchannel()->CancelConnectivityStateWatch(it->second);
  watchers_.erase(it);
}

//
// OutlierDetectionLb::Picker::SubchannelCallTracker
//

class OutlierDetectionLb::Picker::SubchannelCallTracker
    : public LoadBalancingPolicy::SubchannelCallTrackerInterface {
 public:
  SubchannelCallTracker(
      std::unique_ptr<LoadBalancingPolicy::SubchannelCallTrackerInterface>
          original_subchannel_call_tracker,
      RefCountedPtr<SubchannelState> subchannel_state)
      : original_subchannel_call_tracker_(
            std::move(original_subchannel_call_tracker)),
        subchannel_state_(std::move(subchannel_state)) {}

  ~SubchannelCallTracker() override {
    subchannel_state_.reset(DEBUG_LOCATION, "SubchannelCallTracker");
  }

  void Start() override {
    // This tracker does not care about started calls only finished calls.
    // Delegate if needed.
    if (original_subchannel_call_tracker_ != nullptr) {
      original_subchannel_call_tracker_->Start();
    }
  }

  void Finish(FinishArgs args) override {
    // Delegate if needed.
    if (original_subchannel_call_tracker_ != nullptr) {
      original_subchannel_call_tracker_->Finish(args);
    }
    // Record call completion based on status for outlier detection
    // calculations.
    if (subchannel_state_ != nullptr) {
      if (args.status.ok()) {
        subchannel_state_->AddSuccessCount();
      } else {
        subchannel_state_->AddFailureCount();
      }
    }
  }

 private:
  std::unique_ptr<LoadBalancingPolicy::SubchannelCallTrackerInterface>
      original_subchannel_call_tracker_;
  RefCountedPtr<SubchannelState> subchannel_state_;
};

//
// OutlierDetectionLb::Picker
//

OutlierDetectionLb::Picker::Picker(OutlierDetectionLb* outlier_detection_lb,
                                   RefCountedPtr<RefCountedPicker> picker,
                                   bool counting_enabled)
    : picker_(std::move(picker)), counting_enabled_(counting_enabled) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_outlier_detection_lb_trace)) {
    gpr_log(GPR_INFO,
            "[outlier_detection_lb %p] constructed new picker %p and counting "
            "is %s",
            outlier_detection_lb, this,
            (counting_enabled ? "enabled" : "disabled"));
  }
}

LoadBalancingPolicy::PickResult OutlierDetectionLb::Picker::Pick(
    LoadBalancingPolicy::PickArgs args) {
  if (picker_ == nullptr) {  // Should never happen.
    return PickResult::Fail(absl::InternalError(
        "outlier_detection picker not given any child picker"));
  }
  // Delegate to child picker
  PickResult result = picker_->Pick(args);
  auto* complete_pick = absl::get_if<PickResult::Complete>(&result.result);
  if (complete_pick != nullptr) {
    // Unwrap subchannel to pass back up the stack.
    auto* subchannel_wrapper =
        static_cast<SubchannelWrapper*>(complete_pick->subchannel.get());
    // Inject subchannel call tracker to record call completion as long as
    // not both success_rate_ejection and failure_percentage_ejection are unset.
    if (counting_enabled_) {
      complete_pick->subchannel_call_tracker =
          absl::make_unique<SubchannelCallTracker>(
              std::move(complete_pick->subchannel_call_tracker),
              subchannel_wrapper->subchannel_state());
    }
    complete_pick->subchannel = subchannel_wrapper->wrapped_subchannel();
  }
  return result;
}

//
// OutlierDetectionLb
//

OutlierDetectionLb::OutlierDetectionLb(Args args)
    : LoadBalancingPolicy(std::move(args)) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_outlier_detection_lb_trace)) {
    gpr_log(GPR_INFO, "[outlier_detection_lb %p] created", this);
  }
}

OutlierDetectionLb::~OutlierDetectionLb() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_outlier_detection_lb_trace)) {
    gpr_log(GPR_INFO,
            "[outlier_detection_lb %p] destroying outlier_detection LB policy",
            this);
  }
}

std::string OutlierDetectionLb::MakeKeyForAddress(
    const ServerAddress& address) {
  // Strip off attributes to construct the key.
  return ServerAddress(address.address(),
                       grpc_channel_args_copy(address.args()))
      .ToString();
}

void OutlierDetectionLb::ShutdownLocked() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_outlier_detection_lb_trace)) {
    gpr_log(GPR_INFO, "[outlier_detection_lb %p] shutting down", this);
  }
  ejection_timer_.reset();
  shutting_down_ = true;
  // Remove the child policy's interested_parties pollset_set from the
  // xDS policy.
  if (child_policy_ != nullptr) {
    grpc_pollset_set_del_pollset_set(child_policy_->interested_parties(),
                                     interested_parties());
    child_policy_.reset();
  }
  // Drop our ref to the child's picker, in case it's holding a ref to
  // the child.
  picker_.reset();
}

void OutlierDetectionLb::ExitIdleLocked() {
  if (child_policy_ != nullptr) child_policy_->ExitIdleLocked();
}

void OutlierDetectionLb::ResetBackoffLocked() {
  if (child_policy_ != nullptr) child_policy_->ResetBackoffLocked();
}

void OutlierDetectionLb::UpdateLocked(UpdateArgs args) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_outlier_detection_lb_trace)) {
    gpr_log(GPR_INFO, "[outlier_detection_lb %p] Received update", this);
  }
  auto old_config = std::move(config_);
  // Update config.
  config_ = std::move(args.config);
  // Update outlier detection timer.
  if (!config_->CountingEnabled()) {
    // No need for timer.  Cancel the current timer, if any.
    ejection_timer_.reset();
  } else if (ejection_timer_ == nullptr) {
    // No timer running.  Start it now.
    ejection_timer_ =
        MakeOrphanable<EjectionTimer>(Ref(), ExecCtx::Get()->Now());
    for (const auto& p : subchannel_state_map_) {
      p.second->RotateBucket();  // Reset call counters.
    }
  } else if (old_config->outlier_detection_config().interval !=
             config_->outlier_detection_config().interval) {
    // Timer interval changed.  Cancel the current timer and start a new one
    // with the same start time.
    // Note that if the new deadline is in the past, the timer will fire
    // immediately.
    ejection_timer_ =
        MakeOrphanable<EjectionTimer>(Ref(), ejection_timer_->StartTime());
  }
  // Create policy if needed.
  if (child_policy_ == nullptr) {
    child_policy_ = CreateChildPolicyLocked(args.args);
  }
  if (args.addresses.ok()) {
    std::set<std::string> current_addresses;
    for (const ServerAddress& address : *args.addresses) {
      std::string address_key = MakeKeyForAddress(address);
      auto& subchannel_state = subchannel_state_map_[address_key];
      if (subchannel_state == nullptr) {
        subchannel_state = MakeRefCounted<SubchannelState>();
      }
      current_addresses.emplace(address_key);
    }
    for (auto it = subchannel_state_map_.begin();
         it != subchannel_state_map_.end();) {
      if (current_addresses.find(it->first) == current_addresses.end()) {
        // remove each map entry for a subchannel address not in the updated
        // address list.
        it = subchannel_state_map_.erase(it);
      } else {
        ++it;
      }
    }
  }
  // Construct update args.
  UpdateArgs update_args;
  update_args.addresses = std::move(args.addresses);
  update_args.config = config_->child_policy();
  update_args.args = grpc_channel_args_copy(args.args);
  // Update the policy.
  if (GRPC_TRACE_FLAG_ENABLED(grpc_outlier_detection_lb_trace)) {
    gpr_log(GPR_INFO,
            "[outlier_detection_lb %p] Updating child policy handler %p", this,
            child_policy_.get());
  }
  child_policy_->UpdateLocked(std::move(update_args));
}

void OutlierDetectionLb::MaybeUpdatePickerLocked() {
  if (picker_ != nullptr) {
    auto outlier_detection_picker =
        absl::make_unique<Picker>(this, picker_, config_->CountingEnabled());
    if (GRPC_TRACE_FLAG_ENABLED(grpc_outlier_detection_lb_trace)) {
      gpr_log(GPR_INFO,
              "[outlier_detection_lb %p] updating connectivity: state=%s "
              "status=(%s) picker=%p",
              this, ConnectivityStateName(state_), status_.ToString().c_str(),
              outlier_detection_picker.get());
    }
    channel_control_helper()->UpdateState(state_, status_,
                                          std::move(outlier_detection_picker));
  }
}

OrphanablePtr<LoadBalancingPolicy> OutlierDetectionLb::CreateChildPolicyLocked(
    const grpc_channel_args* args) {
  LoadBalancingPolicy::Args lb_policy_args;
  lb_policy_args.work_serializer = work_serializer();
  lb_policy_args.args = args;
  lb_policy_args.channel_control_helper =
      absl::make_unique<Helper>(Ref(DEBUG_LOCATION, "Helper"));
  OrphanablePtr<LoadBalancingPolicy> lb_policy =
      MakeOrphanable<ChildPolicyHandler>(std::move(lb_policy_args),
                                         &grpc_outlier_detection_lb_trace);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_outlier_detection_lb_trace)) {
    gpr_log(GPR_INFO,
            "[outlier_detection_lb %p] Created new child policy handler %p",
            this, lb_policy.get());
  }
  // Add our interested_parties pollset_set to that of the newly created
  // child policy. This will make the child policy progress upon activity on
  // this policy, which in turn is tied to the application's call.
  grpc_pollset_set_add_pollset_set(lb_policy->interested_parties(),
                                   interested_parties());
  return lb_policy;
}

//
// OutlierDetectionLb::Helper
//

RefCountedPtr<SubchannelInterface> OutlierDetectionLb::Helper::CreateSubchannel(
    ServerAddress address, const grpc_channel_args& args) {
  if (outlier_detection_policy_->shutting_down_) return nullptr;
  std::string key = MakeKeyForAddress(address);
  RefCountedPtr<SubchannelState> subchannel_state;
  auto it = outlier_detection_policy_->subchannel_state_map_.find(key);
  if (it != outlier_detection_policy_->subchannel_state_map_.end()) {
    subchannel_state = it->second->Ref();
  }
  auto subchannel = MakeRefCounted<SubchannelWrapper>(
      subchannel_state,
      outlier_detection_policy_->channel_control_helper()->CreateSubchannel(
          std::move(address), args));
  if (subchannel_state != nullptr) {
    subchannel_state->AddSubchannel(subchannel.get());
  }
  return subchannel;
}

void OutlierDetectionLb::Helper::UpdateState(
    grpc_connectivity_state state, const absl::Status& status,
    std::unique_ptr<SubchannelPicker> picker) {
  if (outlier_detection_policy_->shutting_down_) return;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_outlier_detection_lb_trace)) {
    gpr_log(GPR_INFO,
            "[outlier_detection_lb %p] child connectivity state update: "
            "state=%s (%s) "
            "picker=%p",
            outlier_detection_policy_.get(), ConnectivityStateName(state),
            status.ToString().c_str(), picker.get());
  }
  // Save the state and picker.
  outlier_detection_policy_->state_ = state;
  outlier_detection_policy_->status_ = status;
  outlier_detection_policy_->picker_ =
      MakeRefCounted<RefCountedPicker>(std::move(picker));
  // Wrap the picker and return it to the channel.
  outlier_detection_policy_->MaybeUpdatePickerLocked();
}

void OutlierDetectionLb::Helper::RequestReresolution() {
  if (outlier_detection_policy_->shutting_down_) return;
  outlier_detection_policy_->channel_control_helper()->RequestReresolution();
}

absl::string_view OutlierDetectionLb::Helper::GetAuthority() {
  return outlier_detection_policy_->channel_control_helper()->GetAuthority();
}

void OutlierDetectionLb::Helper::AddTraceEvent(TraceSeverity severity,
                                               absl::string_view message) {
  if (outlier_detection_policy_->shutting_down_) return;
  outlier_detection_policy_->channel_control_helper()->AddTraceEvent(severity,
                                                                     message);
}

//
// OutlierDetectionLb::EjectionTimer
//

OutlierDetectionLb::EjectionTimer::EjectionTimer(
    RefCountedPtr<OutlierDetectionLb> parent, Timestamp start_time)
    : parent_(std::move(parent)), start_time_(start_time) {
  GRPC_CLOSURE_INIT(&on_timer_, OnTimer, this, nullptr);
  Ref().release();
  grpc_timer_init(
      &timer_,
      start_time_ + parent_->config_->outlier_detection_config().interval,
      &on_timer_);
}

void OutlierDetectionLb::EjectionTimer::Orphan() {
  if (timer_pending_) {
    timer_pending_ = false;
    grpc_timer_cancel(&timer_);
  }
  Unref();
}

void OutlierDetectionLb::EjectionTimer::OnTimer(void* arg,
                                                grpc_error_handle error) {
  auto* self = static_cast<EjectionTimer*>(arg);
  (void)GRPC_ERROR_REF(error);  // ref owned by lambda
  self->parent_->work_serializer()->Run(
      [self, error]() { self->OnTimerLocked(error); }, DEBUG_LOCATION);
}

void OutlierDetectionLb::EjectionTimer::OnTimerLocked(grpc_error_handle error) {
  if (error == GRPC_ERROR_NONE && timer_pending_) {
    std::map<SubchannelState*, double> success_rate_ejection_candidates;
    std::map<SubchannelState*, double> failure_percentage_ejection_candidates;
    size_t ejected_host_count = 0;
    double success_rate_sum = 0;
    auto time_now = ExecCtx::Get()->Now();
    auto& config = parent_->config_->outlier_detection_config();
    for (auto& state : parent_->subchannel_state_map_) {
      auto* subchannel_state = state.second.get();
      // For each address, swap the call counter's buckets in that address's
      // map entry.
      subchannel_state->RotateBucket();
      // Gather data to run success rate algorithm or failure percentage
      // algorithm.
      if (subchannel_state->ejection_time().has_value()) {
        ++ejected_host_count;
      }
      absl::optional<std::pair<double, uint64_t>> host_success_rate_and_volume =
          subchannel_state->GetSuccessRateAndVolume();
      if (!host_success_rate_and_volume.has_value()) {
        continue;
      }
      double success_rate = host_success_rate_and_volume->first;
      uint64_t request_volume = host_success_rate_and_volume->second;
      if (config.success_rate_ejection.has_value()) {
        if (request_volume >= config.success_rate_ejection->request_volume) {
          success_rate_ejection_candidates[subchannel_state] = success_rate;
          success_rate_sum += success_rate;
        }
      }
      if (config.failure_percentage_ejection.has_value()) {
        if (request_volume >=
            config.failure_percentage_ejection->request_volume) {
          failure_percentage_ejection_candidates[subchannel_state] =
              success_rate;
        }
      }
    }
    // success rate algorithm
    if (!success_rate_ejection_candidates.empty() &&
        success_rate_ejection_candidates.size() >=
            config.success_rate_ejection->minimum_hosts) {
      // calculate ejection threshold: (mean - stdev *
      // (success_rate_ejection.stdev_factor / 1000))
      double mean = success_rate_sum / success_rate_ejection_candidates.size();
      double variance = 0;
      std::for_each(success_rate_ejection_candidates.begin(),
                    success_rate_ejection_candidates.end(),
                    [&variance, mean](std::pair<SubchannelState*, double> v) {
                      variance += std::pow(v.second - mean, 2);
                    });
      variance /= success_rate_ejection_candidates.size();
      double stdev = std::sqrt(variance);
      const double success_rate_stdev_factor =
          static_cast<double>(config.success_rate_ejection->stdev_factor) /
          1000;
      double ejection_threshold = mean - stdev * success_rate_stdev_factor;
      for (auto& candidate : success_rate_ejection_candidates) {
        if (candidate.second < ejection_threshold) {
          uint32_t random_key = absl::Uniform(bit_gen_, 1, 100);
          double current_percent = 100.0 * ejected_host_count /
                                   parent_->subchannel_state_map_.size();
          if (random_key <
                  config.success_rate_ejection->enforcement_percentage &&
              (ejected_host_count == 0 ||
               (current_percent < config.max_ejection_percent))) {
            // Eject and record the timestamp for use when ejecting addresses in
            // this iteration.
            candidate.first->Eject(time_now);
            ++ejected_host_count;
          }
        }
      }
    }
    // failure percentage algorithm
    if (!failure_percentage_ejection_candidates.empty() &&
        failure_percentage_ejection_candidates.size() >=
            config.failure_percentage_ejection->minimum_hosts) {
      for (auto& candidate : failure_percentage_ejection_candidates) {
        // Extra check to make sure success rate algorithm didn't already
        // eject this backend.
        if (candidate.first->ejection_time().has_value()) continue;
        if ((100.0 - candidate.second) >
            config.failure_percentage_ejection->threshold) {
          uint32_t random_key = absl::Uniform(bit_gen_, 1, 100);
          double current_percent = 100.0 * ejected_host_count /
                                   parent_->subchannel_state_map_.size();
          if (random_key <
                  config.failure_percentage_ejection->enforcement_percentage &&
              (ejected_host_count == 0 ||
               (current_percent < config.max_ejection_percent))) {
            // Eject and record the timestamp for use when ejecting addresses in
            // this iteration.
            candidate.first->Eject(time_now);
            ++ejected_host_count;
          }
        }
      }
    }
    // For each address in the map:
    //   If the address is not ejected and the multiplier is greater than 0,
    //   decrease the multiplier by 1. If the address is ejected, and the
    //   current time is after ejection_timestamp + min(base_ejection_time *
    //   multiplier, max(base_ejection_time, max_ejection_time)), un-eject the
    //   address.
    for (auto& state : parent_->subchannel_state_map_) {
      auto* subchannel_state = state.second.get();
      subchannel_state->MaybeUneject(config.base_ejection_time.millis(),
                                     config.max_ejection_time.millis());
    }
    timer_pending_ = false;
    parent_->ejection_timer_ =
        MakeOrphanable<EjectionTimer>(parent_, ExecCtx::Get()->Now());
  }
  Unref(DEBUG_LOCATION, "Timer");
  GRPC_ERROR_UNREF(error);
}

//
// factory
//

class OutlierDetectionLbFactory : public LoadBalancingPolicyFactory {
 public:
  OrphanablePtr<LoadBalancingPolicy> CreateLoadBalancingPolicy(
      LoadBalancingPolicy::Args args) const override {
    return MakeOrphanable<OutlierDetectionLb>(std::move(args));
  }

  const char* name() const override { return kOutlierDetection; }

  RefCountedPtr<LoadBalancingPolicy::Config> ParseLoadBalancingConfig(
      const Json& json, grpc_error_handle* error) const override {
    GPR_DEBUG_ASSERT(error != nullptr && *error == GRPC_ERROR_NONE);
    if (json.type() == Json::Type::JSON_NULL) {
      // This policy was configured in the deprecated loadBalancingPolicy
      // field or in the client API.
      *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "field:loadBalancingPolicy error:outlier_detection policy requires "
          "configuration. Please use loadBalancingConfig field of service "
          "config instead.");
      return nullptr;
    }
    std::vector<grpc_error_handle> error_list;
    // Outlier detection config
    OutlierDetectionConfig outlier_detection_config;
    auto it = json.object_value().find("successRateEjection");
    if (it != json.object_value().end()) {
      if (it->second.type() != Json::Type::OBJECT) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "field:successRateEjection error:type must be object"));
      } else {
        OutlierDetectionConfig::SuccessRateEjection success_config;
        const Json::Object& object = it->second.object_value();
        ParseJsonObjectField(object, "stdevFactor",
                             &success_config.stdev_factor, &error_list,
                             /*required=*/false);
        ParseJsonObjectField(object, "enforcementPercentage",
                             &success_config.enforcement_percentage,
                             &error_list, /*required=*/false);
        ParseJsonObjectField(object, "minimumHosts",
                             &success_config.minimum_hosts, &error_list,
                             /*required=*/false);
        ParseJsonObjectField(object, "requestVolume",
                             &success_config.request_volume, &error_list,
                             /*required=*/false);
        outlier_detection_config.success_rate_ejection = success_config;
      }
    }
    it = json.object_value().find("failurePercentageEjection");
    if (it != json.object_value().end()) {
      if (it->second.type() != Json::Type::OBJECT) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "field:successRateEjection error:type must be object"));
      } else {
        OutlierDetectionConfig::FailurePercentageEjection failure_config;
        const Json::Object& object = it->second.object_value();
        ParseJsonObjectField(object, "threshold", &failure_config.threshold,
                             &error_list, /*required=*/false);
        ParseJsonObjectField(object, "enforcementPercentage",
                             &failure_config.enforcement_percentage,
                             &error_list, /*required=*/false);
        ParseJsonObjectField(object, "minimumHosts",
                             &failure_config.minimum_hosts, &error_list,
                             /*required=*/false);
        ParseJsonObjectField(object, "requestVolume",
                             &failure_config.request_volume, &error_list,
                             /*required=*/false);
        outlier_detection_config.failure_percentage_ejection = failure_config;
      }
    }
    ParseJsonObjectFieldAsDuration(json.object_value(), "interval",
                                   &outlier_detection_config.interval,
                                   &error_list);
    ParseJsonObjectFieldAsDuration(json.object_value(), "baseEjectionTime",
                                   &outlier_detection_config.base_ejection_time,
                                   &error_list, /*required=*/false);
    if (!ParseJsonObjectFieldAsDuration(
            json.object_value(), "maxEjectionTime",
            &outlier_detection_config.max_ejection_time, &error_list,
            /*required=*/false)) {
      outlier_detection_config.max_ejection_time = std::max(
          outlier_detection_config.base_ejection_time, Duration::Seconds(300));
    }
    ParseJsonObjectField(json.object_value(), "maxEjectionPercent",
                         &outlier_detection_config.max_ejection_percent,
                         &error_list, /*required=*/false);
    RefCountedPtr<LoadBalancingPolicy::Config> child_policy;
    it = json.object_value().find("childPolicy");
    if (it == json.object_value().end()) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "field:childPolicy error:required field missing"));
    } else {
      grpc_error_handle parse_error = GRPC_ERROR_NONE;
      child_policy = LoadBalancingPolicyRegistry::ParseLoadBalancingConfig(
          it->second, &parse_error);
      if (child_policy == nullptr) {
        GPR_DEBUG_ASSERT(parse_error != GRPC_ERROR_NONE);
        std::vector<grpc_error_handle> child_errors;
        child_errors.push_back(parse_error);
        error_list.push_back(
            GRPC_ERROR_CREATE_FROM_VECTOR("field:childPolicy", &child_errors));
      }
    }
    if (!error_list.empty()) {
      *error = GRPC_ERROR_CREATE_FROM_VECTOR(
          "outlier_detection_experimental LB policy config", &error_list);
      return nullptr;
    }
    return MakeRefCounted<OutlierDetectionLbConfig>(outlier_detection_config,
                                                    std::move(child_policy));
  }
};

}  // namespace

}  // namespace grpc_core

//
// Plugin registration
//

void grpc_lb_policy_outlier_detection_init() {
  if (grpc_core::XdsOutlierDetectionEnabled()) {
    grpc_core::LoadBalancingPolicyRegistry::Builder::
        RegisterLoadBalancingPolicyFactory(
            absl::make_unique<grpc_core::OutlierDetectionLbFactory>());
  }
}

void grpc_lb_policy_outlier_detection_shutdown() {}
