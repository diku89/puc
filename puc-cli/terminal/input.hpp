#pragma once

/**
 * @file input.hpp
 * @brief Trie actions and configuration for terminal input decoding.
 */

#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "puc-cli/terminal/event.hpp"
#include "puc-cli/terminal/status.hpp"
#include "utils/config/config.hpp"
#include "utils/containers/trie.hpp"

namespace puc {
namespace terminal {

/** Host operating-system families with packaged input defaults. */
enum class OperatingSystem {
  DARWIN, /**< Apple Darwin, including macOS. */
  LINUX,  /**< Linux-based systems. */
  BSD,    /**< FreeBSD, OpenBSD, NetBSD, or DragonFly BSD. */
  OTHER,  /**< A system without a packaged input-default profile. */
};

/**
 * Return the operating-system family selected by the C++ target platform.
 *
 * @return Compile-time Darwin, Linux, BSD, or unsupported classification.
 */
constexpr OperatingSystem current_operating_system() noexcept {
#if defined(__APPLE__) && defined(__MACH__)
  return OperatingSystem::DARWIN;
#elif defined(__linux__)
  return OperatingSystem::LINUX;
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) ||   \
    defined(__DragonFly__)
  return OperatingSystem::BSD;
#else
  return OperatingSystem::OTHER;
#endif
}

/**
 * Return the config-root-relative defaults file for an OS family.
 *
 * An empty result denotes an unsupported family.
 *
 * @param[in] operating_system Family whose convention path is required.
 * @return Stable relative TOML path, or an empty view for OTHER.
 */
constexpr std::string_view operating_system_defaults_path(
    OperatingSystem operating_system) noexcept {
  switch (operating_system) {
    case OperatingSystem::DARWIN:
      return "darwin-defaults.toml";
    case OperatingSystem::LINUX:
      return "linux-defaults.toml";
    case OperatingSystem::BSD:
      return "bsd-defaults.toml";
    case OperatingSystem::OTHER:
      return {};
  }
  return {};
}

/** Protocol behavior entered after an input-trie sequence is recognized. */
enum class InputProtocol {
  TEXT,   /**< Decode ordinary UTF-8; fixed controls remain trie mappings. */
  ESCAPE, /**< Resolve a standalone Escape or one Alt-modified input. */
  SS3,    /**< Capture an SS3 function-key sequence through its final byte. */
  CSI,    /**< Capture a generic control-sequence introducer sequence. */
  OSC,    /**< Capture a generic operating-system command sequence. */
  DEVICE_CONTROL_STRING, /**< Capture a DCS reply through its ST terminator. */
  APPLICATION_PROGRAM_COMMAND, /**< Capture an APC through ST. */
  PRIVACY_MESSAGE,             /**< Capture a privacy message through ST. */
  SGR_MOUSE,   /**< Capture numeric SGR mouse fields through `M` or `m`. */
  OSC52,       /**< Capture an OSC 52 clipboard response. */
  PASTE_BEGIN, /**< Enter bracketed-paste payload mode. */
  PASTE_END,   /**< Leave bracketed-paste payload mode. */
};

/** An action that immediately emits one fully constructed input event. */
struct EmitInputEvent {
  Event event; /**< Immutable event copied to a decoder's output queue. */

  /** Compare the stored event. */
  bool operator==(const EmitInputEvent&) const = default;
};

/** An action that selects protocol behavior for subsequent input bytes. */
struct EnterInputProtocol {
  InputProtocol protocol = InputProtocol::TEXT; /**< Behavior to activate. */

  /** Compare selected protocol behaviors. */
  constexpr bool operator==(const EnterInputProtocol&) const noexcept = default;
};

/**
 * Immutable value stored at an accepting input-trie node.
 *
 * The wrapper keeps `std::variant` out of the public decoding logic and gives
 * configuration loading one validated domain type. Mutable capture data is
 * held by the decoder cursor rather than by this shared trie value.
 */
class InputAction {
 public:
  /** Construct an inert action suitable for prefix-only trie nodes. */
  InputAction() noexcept = default;

  /** Construct an action that emits a fixed event. */
  explicit InputAction(Event event)
      : storage_(EmitInputEvent{std::move(event)}) {}

  /** Construct an action that enters protocol-specific capture behavior. */
  explicit constexpr InputAction(InputProtocol protocol) noexcept
      : storage_(EnterInputProtocol{protocol}) {}

  /** Return the fixed event, or `nullptr` for a non-emitting action. */
  const Event* event() const noexcept;

  /** Return the protocol action, or `nullptr` for a non-protocol action. */
  const EnterInputProtocol* protocol() const noexcept;

  /** Return whether this is the inert default action. */
  bool empty() const noexcept;

  /** Compare complete action state. */
  bool operator==(const InputAction&) const = default;

 private:
  using Storage =
      std::variant<std::monostate, EmitInputEvent, EnterInputProtocol>;

  Storage storage_; /**< Direct, allocation-free action representation. */
};

static_assert(std::regular<InputAction>);

/**
 * Mutable builder for the immutable trie consumed by Decoder.
 *
 * Decoder::setup() is the canonical construction path. It builds a temporary
 * map in one explicit source-of-truth hierarchy and moves the finished trie
 * into the decoder. In increasing authority, that hierarchy is:
 *
 * 1. key sequences materialized from terminfo;
 * 2. an optional `terminals/<TERM>.toml` terminal profile; and
 * 3. `input_keys.toml`, merged by Config from system defaults followed by user
 *    overrides; and
 * 4. the detected host's `*-defaults.toml`, likewise merged with its same-path
 *    user override.
 *
 * Later declarations replace earlier exact paths. Generic protocol actions
 * remain accepting prefixes beneath more-specific terminfo, profile, and TOML
 * sequences; they do not erase those descendants. Consequently the longest
 * matching configured sequence wins before the decoder enters a parameterized
 * fallback parser. After overrides have resolved exact paths, setup rejects
 * every pair of active semantic command sequences for which either byte path
 * is a prefix of the other. Duplicate commands in one physical source file
 * are also rejected; exact declarations from distinct system and user files
 * retain their documented override behavior.
 */
class InputMap {
 private:
  friend class Decoder;

  /** Trie containing byte paths and directly stored decoder actions. */
  using Trie = containers::Trie<char, InputAction>;

  /** One terminfo capability associated with a terminal-independent key. */
  struct TerminfoKeyBinding {
    std::string capability; /**< Short terminfo string-capability name. */
    KeyEvent event; /**< PUC key emitted for the capability's byte sequence. */
  };

  /** One currently effective semantic command and its owned provenance. */
  struct CommandSequence {
    std::string sequence;    /**< Exact decoded Trie path. */
    std::string source;      /**< TOML file that declared the active mapping. */
    std::size_t line   = 0U; /**< One-based source line when available. */
    std::size_t column = 0U; /**< One-based source column when available. */
  };

  /** Two same-source declarations that attempted an exact-path collision. */
  struct ExactCommandConflict {
    CommandSequence first;  /**< Earlier declaration. */
    CommandSequence second; /**< Later declaration. */
  };

  /** Construct an empty map containing only the trie's sentinel root. */
  InputMap() = default;

  /** Insert or replace one exact byte-sequence action. */
  Status register_sequence(std::string_view sequence, InputAction action);

  /** Insert or replace one exact byte sequence that emits a key event. */
  Status register_key_sequence(std::string_view sequence, KeyEvent event);

  /** Remove the action for an exact byte sequence without pruning its path. */
  bool disable_sequence(std::string_view sequence);

  /** Add or replace a terminfo capability association. */
  Status register_terminfo_key(std::string_view capability, KeyEvent event);

  /** Transfer the constructed trie into a decoder. */
  Trie take_trie() noexcept { return std::move(trie_); }

  /** Find one configured exact sequence that enters a protocol behavior. */
  bool find_protocol_sequence(InputProtocol protocol,
                              std::string& sequence) const;

  /**
   * Build the sole configured map used by Decoder::setup().
   *
   * Sources are applied in the class-level hierarchy above. Mappings within
   * each TOML document are applied in declaration order. The required files
   * are read at runtime; no TOML document is compiled into the application.
   */
  static Status setup(const config::Config& configurations, InputMap& output,
                      std::string_view terminal_name, int output_fd);

  /** Materialize declared key capabilities from one terminfo entry. */
  Status load_terminfo(std::string_view terminal_name, int output_fd);

  Status validate_config(const config::Value& root) const;
  /** Apply one source's mappings in declaration order. */
  Status apply_mappings(const config::Value& root);
  Status apply_terminfo_bindings(const config::Value& root);

  /** Reject ambiguous prefix relationships between semantic commands. */
  Status validate_command_sequences() const;

  /** Update effective command provenance after one mapping declaration. */
  void track_command_sequence(std::string_view sequence,
                              const InputAction* action,
                              config::SourceLocation location);

  Trie trie_; /**< Byte-sequence paths and accepting actions. */
  std::vector<TerminfoKeyBinding>
      terminfo_keys_; /**< Capability names awaiting materialization. */
  std::vector<CommandSequence>
      command_sequences_; /**< Effective semantic command paths. */
  std::vector<ExactCommandConflict>
      exact_command_conflicts_; /**< Same-source exact-path conflicts. */
};

}  // namespace terminal
}  // namespace puc
