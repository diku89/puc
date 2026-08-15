#pragma once

/**
 * @file ipc.hpp
 * @brief Complete public interface for PUC's named IPC event system.
 *
 * @namespace puc::ipc
 * @brief Named event distribution, portable wire messages, and Unix transports.
 *
 * Include this header when an application needs several IPC components. Small
 * consumers may include the individual component header they use instead.
 */

#include "utils/ipc/channel.hpp"
#include "utils/ipc/channel_path.hpp"
#include "utils/ipc/directory.hpp"
#include "utils/ipc/filebuffer_channel.hpp"
#include "utils/ipc/metachannel.hpp"
#include "utils/ipc/msg.hpp"
#include "utils/ipc/smem_channel.hpp"
#include "utils/ipc/socket_channel.hpp"
#include "utils/ipc/status.hpp"
