#pragma once

/**
 * @file canvas_subsystem.hpp
 * @brief Lifecycle-owned Canvas identity, persistence, and Turn ingestion.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "canvas/protos/canvas.pb.h"
#include "canvas/protos/datastore/status.hpp"
#include "canvas/protos/presentation.pb.h"
#include "canvas/protos/turn.pb.h"
#include "state/state.hpp"

namespace puc::canvas {
class TurnPipeline;
}  // namespace puc::canvas

namespace puc::app {

/** Own the durable Canvas aggregate and its ingress-side graph nodes. */
class CanvasSubsystem final : public AppSubsystem {
 public:
  /**
   * Global lifecycle event channel used to discover Canvas namespaces.
   *
   * \channel{//canvas/channels_announce||Broadcasts OPENED and CLOSING
   * lifecycle events containing a Canvas UUID, its absolute channel root, and
   * the channel protocol version.||
   * \ref puc::app::CanvasSubsystem "CanvasSubsystem" instances.||Canvas
   * discovery clients that subscribe before querying current state.}
   */
  static constexpr std::string_view kChannelsAnnounceChannel =
      "//canvas/channels_announce";

  /**
   * Global query channel used by subscribers that missed an announcement.
   *
   * \channel{//canvas/channels_query||Requests that every running Canvas, or
   * one selected Canvas UUID, publish its current OPENED announcement.||Late
   * Canvas discovery clients after subscribing to //canvas/channels_announce.||
   * Running \ref puc::app::CanvasSubsystem "CanvasSubsystem" instances.}
   */
  static constexpr std::string_view kChannelsQueryChannel =
      "//canvas/channels_query";

  /** Protocol version carried by Canvas namespace announcements. */
  static constexpr std::uint32_t kChannelProtocolVersion = 1U;

  /**
   * Relative route accepting complete, pre-addressed Turns.
   *
   * \channel{//canvas/CANVAS_UUID/turns/submit||Carries serialized Turn
   * protobufs whose stable ID was first reserved with reply_to(); CANVAS_UUID
   * is the owning Canvas UUID rendered as 32 hexadecimal digits.||Human,
   * model, and tool input adapters.||\ref puc::app::CanvasSubsystem
   * "CanvasSubsystem", which persists and materializes each accepted Turn.}
   */
  static constexpr std::string_view kTurnSubmissionPath = "turns/submit";

  /**
   * Relative route broadcasting durable committed Turns.
   *
   * \channel{//canvas/CANVAS_UUID/turns/committed||Broadcasts each Turn after
   * it is persisted and the materialized Turn Trie is updated.||
   * \ref puc::app::CanvasSubsystem "CanvasSubsystem".||Turn Trie replicas,
   * reverse indexes, search builders, and other committed-Turn observers.}
   */
  static constexpr std::string_view kCommittedTurnPath = "turns/committed";

  /**
   * Relative route broadcasting durable Presentation commits.
   *
   * \channel{//canvas/CANVAS_UUID/presentation/committed||Broadcasts the
   * previous and new Presentation roots plus the inserted Turn ID only after
   * the Presentation commit is durable.||
   * \ref puc::app::OrchestrationSubsystem "OrchestrationSubsystem".||TUI
   * presentation consumers and Presentation-tree replicas.}
   */
  static constexpr std::string_view kCommittedPresentationPath =
      "presentation/committed";

  /** Stable graph node that persists a complete, pre-addressed Turn. */
  static constexpr std::string_view kPersistTurnNode = "canvas.persist_turn";

  /** Stable graph node that materializes a durable Turn in the Trie. */
  static constexpr std::string_view kUpdateTrieNode = "canvas.update_trie";

  /** Construct an uninitialized Canvas lifecycle adapter. */
  CanvasSubsystem();

  /** Destroy resources released by terminate(). */
  ~CanvasSubsystem() override;

  /** Restore a Canvas from initialized storage and register ingestion nodes. */
  Status initialize(AppState& app) override;

  /** Attach workers and begin receiving submitted Turns over IPC. */
  Status start(AppState& app) override;

  /** Stop IPC ingestion and detach the active worker generation. */
  Status stop(AppState& app) noexcept override;

  /** Release the graph and every Canvas-owned datastore wrapper. */
  Status terminate(AppState& app) noexcept override;

  /** Return a synchronized snapshot of the aggregate and its tree identities.
   */
  canvas::proto::Canvas canvas() const;

  /** Return the current Canvas UUID bytes. */
  const std::vector<std::uint8_t>& canvas_uuid() const noexcept;

  /**
   * Reserve one reply address and return an uncommitted Turn shell.
   *
   * Passing null selects the Canvas root. A non-null parent must identify an
   * existing Turn in this Canvas. The caller fills the remaining fields and
   * submits the returned parent-derived identity exactly once for commit.
   */
  canvas::datastore::Status reply_to(const canvas::proto::TurnId* parent,
                                     canvas::proto::Turn& started);

  /**
   * Reserve one alphabetic response-part beneath an existing Turn.
   *
   * The returned shell receives addresses such as `1.1.a`, `1.1.b`, and so
   * on from an allocator independent of the parent's numeric replies.
   */
  canvas::datastore::Status append_part(const canvas::proto::TurnId& parent,
                                        canvas::proto::Turn& started);

  /** Return the absolute namespace root for this Canvas. */
  std::string channel_root_name() const;

  /** Return the pre-addressed Turn submission channel name. */
  std::string turn_submission_channel_name() const;

  /** Return the durable committed-Turn channel name. */
  std::string committed_turn_channel_name() const;

  /** Return the committed-Presentation IPC channel name. */
  std::string committed_presentation_channel_name() const;

  /** Return the extensible Turn graph owned by this Canvas. */
  canvas::TurnPipeline* pipeline() noexcept;

  /** Update the in-memory aggregate from one durable Presentation commit. */
  void materialize_presentation(
      const canvas::proto::Presentation& presentation) noexcept;

  /** Broadcast one already-persisted Presentation to current IPC observers. */
  bool publish_committed_presentation(
      const canvas::proto::Presentation& presentation) noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Hidden lifecycle and persistence state. */
};

}  // namespace puc::app
