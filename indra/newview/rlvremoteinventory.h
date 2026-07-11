/**
 * @file rlvremoteinventory.h
 * @brief Asynchronous controller for remote RLV inventory requests.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026 The Phoenix Firestorm Project, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * $/LicenseInfo$
 */

#ifndef RLV_REMOTE_INVENTORY_H
#define RLV_REMOTE_INVENTORY_H

#include "llframetimer.h"
#include "llsingleton.h"
#include "lluuid.h"

#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * Queues remote RLV requests and routes them through the user's Firestorm LSL
 * Bridge.  RLV viewer query replies are script chat, so the bridge must act as
 * the relay controller and listener rather than the viewer sending chat itself.
 */
class RlvRemoteInventory final : public LLSingleton<RlvRemoteInventory>
{
    LLSINGLETON(RlvRemoteInventory);
    ~RlvRemoteInventory();

public:
    enum class EStatus
    {
        SUCCESS,
        INVALID_REQUEST,
        BRIDGE_UNAVAILABLE,
        TARGET_UNAVAILABLE,
        RELAY_REJECTED,
        TIMEOUT,
        CANCELLED,
        PROTOCOL_ERROR
    };

    struct Result
    {
        EStatus     status{ EStatus::PROTOCOL_ERROR };
        LLUUID      target;
        std::string command;
        std::string payload;

        bool succeeded() const { return status == EStatus::SUCCESS; }
    };

    using callback_t = std::function<void(const Result&)>;

    /**
     * Queue one relay command.  Query commands must contain exactly one
     * "%CHANNEL%" placeholder and set expects_reply=true.  Non-query commands
     * must not contain the placeholder.
     */
    void sendRelayCommand(const LLUUID& target, const std::string& command_template, bool expects_reply, callback_t callback = {});

    /**
     * Ask the bridge for the target's viewer-visible attachment roots.  The
     * successful payload contains pipe-separated UUID,base64-name,point rows.
     * HUD attachments are not exposed by llGetAttachedList().
     */
    void requestAttachments(const LLUUID& target, callback_t callback = {});

    /** Cancel all requests that were queued at the time of this call. */
    void cancelAll();

    bool               hasPendingRequests() const;
    static const char* statusToString(EStatus status);

    /** Called by FSLSLBridge for authenticated <bridgeRemoteRlv> messages. */
    void handleBridgeMessage(std::string_view message);

private:
    enum class RequestKind
    {
        RELAY,
        ATTACHMENTS
    };

    struct Request
    {
        RequestKind kind{ RequestKind::RELAY };
        std::string id;
        LLUUID      target;
        std::string command;
        bool        expects_reply{ false };
        callback_t  callback;
    };

    struct ChunkAssembly
    {
        U32                      total{ 0 };
        U32                      received{ 0 };
        std::vector<std::string> chunks;
    };

    static constexpr F32    REQUEST_TIMEOUT_SECONDS = 65.f;
    static constexpr size_t MAX_COMMAND_BYTES       = 512;
    static constexpr U32    MAX_CHUNKS              = 64;

    static void onIdle(void* userdata);

    std::string nextRequestId();
    static bool validateCommand(const std::string& command, bool expects_reply, std::string& error);
    static bool isValidBase64(const std::string& encoded);

    void enqueue(Request&& request);
    void pumpQueue();
    void finishCurrent(EStatus status, std::string payload = {});
    void processBridgePayload(const std::string& kind, const std::string& payload);
    void timeoutCurrent();

    U32                                  mNextRequestId{ 1 };
    std::deque<Request>                  mQueue;
    std::optional<Request>               mCurrent;
    std::map<std::string, ChunkAssembly> mAssemblies;
    LLFrameTimer                         mRequestTimer;
};

#endif // RLV_REMOTE_INVENTORY_H
