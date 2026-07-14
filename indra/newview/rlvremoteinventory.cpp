/**
 * @file rlvremoteinventory.cpp
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

#include "llviewerprecompiledheaders.h"

#include "rlvremoteinventory.h"

#include "fslslbridge.h"
#include "llbase64.h"
#include "llcallbacklist.h"
#include "llstring.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace
{
constexpr std::string_view BRIDGE_TAG_OPEN     = "<bridgeRemoteRlv>";
constexpr std::string_view BRIDGE_TAG_CLOSE    = "</bridgeRemoteRlv>";
constexpr std::string_view CHANNEL_PLACEHOLDER = "%CHANNEL%";

bool splitBridgeContent(std::string_view content, std::string& request_id, std::string& kind, std::string& part, std::string& total,
                        std::string& chunk)
{
    std::string* fields[] = { &request_id, &kind, &part, &total };
    size_t       offset   = 0;
    for (std::string* field : fields)
    {
        const size_t separator = content.find('|', offset);
        if (separator == std::string_view::npos)
        {
            return false;
        }
        field->assign(content.substr(offset, separator - offset));
        offset = separator + 1;
    }
    chunk.assign(content.substr(offset));
    return true;
}
} // namespace

RlvRemoteInventory::RlvRemoteInventory()
{
    mRequestTimer.stop();
    gIdleCallbacks.addFunction(onIdle, this);
}

RlvRemoteInventory::~RlvRemoteInventory()
{
    gIdleCallbacks.deleteFunction(onIdle, this);
    mCurrent.reset();
    mQueue.clear();
    mAssemblies.clear();
}

void RlvRemoteInventory::setSelectedTarget(const LLUUID& target, const std::string& name)
{
    const bool target_changed = target != mSelectedTarget;
    if (!target_changed && name == mSelectedTargetName)
    {
        return;
    }

    if (target_changed)
    {
        cancelAll();
    }
    mSelectedTarget     = target;
    mSelectedTargetName = name;
    mTargetChangedSignal(mSelectedTarget, mSelectedTargetName);
}

void RlvRemoteInventory::sendRelayCommand(const LLUUID& target, const std::string& command_template, bool expects_reply,
                                          callback_t callback)
{
    std::string error;
    if (target.isNull())
    {
        error = "Target avatar is null";
    }
    else
    {
        validateCommand(command_template, expects_reply, error);
    }

    if (!error.empty())
    {
        if (callback)
        {
            callback(Result{ EStatus::INVALID_REQUEST, target, command_template, error });
        }
        return;
    }

    enqueue(Request{ RequestKind::RELAY, nextRequestId(), target, command_template, expects_reply, std::move(callback) });
}

void RlvRemoteInventory::requestAttachments(const LLUUID& target, callback_t callback)
{
    if (target.isNull())
    {
        if (callback)
        {
            callback(Result{ EStatus::INVALID_REQUEST, target, "attachments", "Target avatar is null" });
        }
        return;
    }

    enqueue(Request{ RequestKind::ATTACHMENTS, nextRequestId(), target, "attachments", true, std::move(callback) });
}

void RlvRemoteInventory::cancelAll()
{
    std::vector<Request> cancelled;
    if (mCurrent)
    {
        FSLSLBridge::instance().viewerToLSL("RemoteRlvCancel|" + mCurrent->id);
        cancelled.emplace_back(std::move(*mCurrent));
        mCurrent.reset();
    }

    while (!mQueue.empty())
    {
        cancelled.emplace_back(std::move(mQueue.front()));
        mQueue.pop_front();
    }

    mRequestTimer.stop();
    mAssemblies.clear();

    for (Request& request : cancelled)
    {
        if (request.callback)
        {
            request.callback(Result{ EStatus::CANCELLED, request.target, request.command, "Request cancelled" });
        }
    }
}

bool RlvRemoteInventory::hasPendingRequests() const
{ return mCurrent.has_value() || !mQueue.empty(); }

const char* RlvRemoteInventory::statusToString(EStatus status)
{
    switch (status)
    {
        case EStatus::SUCCESS:
            return "success";
        case EStatus::INVALID_REQUEST:
            return "invalid_request";
        case EStatus::BRIDGE_UNAVAILABLE:
            return "bridge_unavailable";
        case EStatus::TARGET_UNAVAILABLE:
            return "target_unavailable";
        case EStatus::RELAY_REJECTED:
            return "relay_rejected";
        case EStatus::TIMEOUT:
            return "timeout";
        case EStatus::CANCELLED:
            return "cancelled";
        case EStatus::PROTOCOL_ERROR:
            return "protocol_error";
        default:
            return "unknown";
    }
}

void RlvRemoteInventory::handleBridgeMessage(std::string_view message)
{
    if (!mCurrent || message.size() < BRIDGE_TAG_OPEN.size() + BRIDGE_TAG_CLOSE.size() ||
        message.substr(0, BRIDGE_TAG_OPEN.size()) != BRIDGE_TAG_OPEN ||
        message.substr(message.size() - BRIDGE_TAG_CLOSE.size()) != BRIDGE_TAG_CLOSE)
    {
        return;
    }

    const std::string_view content =
        message.substr(BRIDGE_TAG_OPEN.size(), message.size() - BRIDGE_TAG_OPEN.size() - BRIDGE_TAG_CLOSE.size());

    std::string request_id;
    std::string kind;
    std::string part_string;
    std::string total_string;
    std::string chunk;
    if (!splitBridgeContent(content, request_id, kind, part_string, total_string, chunk))
    {
        finishCurrent(EStatus::PROTOCOL_ERROR, "Malformed bridge response");
        return;
    }

    // A delayed response from a cancelled/timed-out request is harmless.
    if (request_id != mCurrent->id)
    {
        LL_DEBUGS("RlvRemoteInventory") << "Ignoring stale bridge response " << request_id << LL_ENDL;
        return;
    }

    U32 part  = 0;
    U32 total = 0;
    if (!LLStringUtil::convertToU32(part_string, part) || !LLStringUtil::convertToU32(total_string, total) || part == 0 || total == 0 ||
        part > total || total > MAX_CHUNKS)
    {
        finishCurrent(EStatus::PROTOCOL_ERROR, "Invalid bridge chunk metadata");
        return;
    }

    if (kind != "ack" && kind != "reply" && kind != "attachments" && kind != "error")
    {
        finishCurrent(EStatus::PROTOCOL_ERROR, "Unknown bridge response type");
        return;
    }

    if (!isValidBase64(chunk))
    {
        finishCurrent(EStatus::PROTOCOL_ERROR, "Invalid base64 bridge chunk");
        return;
    }
    std::string decoded_chunk = chunk.empty() ? std::string() : LLBase64::decodeAsString(chunk);

    ChunkAssembly& assembly = mAssemblies[kind];
    if (assembly.total == 0)
    {
        assembly.total = total;
        assembly.chunks.resize(total);
    }
    else if (assembly.total != total)
    {
        finishCurrent(EStatus::PROTOCOL_ERROR, "Bridge chunk count changed");
        return;
    }

    std::string& destination = assembly.chunks[part - 1];
    if (!destination.empty())
    {
        if (destination != decoded_chunk)
        {
            finishCurrent(EStatus::PROTOCOL_ERROR, "Conflicting duplicate bridge chunk");
        }
        return;
    }

    destination = std::move(decoded_chunk);
    ++assembly.received;
    if (assembly.received != assembly.total)
    {
        return;
    }

    std::string payload;
    for (const std::string& payload_chunk : assembly.chunks)
    {
        payload += payload_chunk;
    }
    mAssemblies.erase(kind);
    processBridgePayload(kind, payload);
}

void RlvRemoteInventory::onIdle(void* userdata)
{
    RlvRemoteInventory* self = static_cast<RlvRemoteInventory*>(userdata);
    if (self && self->mCurrent && self->mRequestTimer.getStarted() && self->mRequestTimer.getElapsedTimeF32() >= REQUEST_TIMEOUT_SECONDS)
    {
        self->timeoutCurrent();
    }
}

std::string RlvRemoteInventory::nextRequestId()
{
    if (mNextRequestId == 0)
    {
        mNextRequestId = 1;
    }
    return llformat("%u", mNextRequestId++);
}

bool RlvRemoteInventory::validateCommand(const std::string& command, bool expects_reply, std::string& error)
{
    if (command.empty() || (command.front() != '@' && command.front() != '!'))
    {
        error = "Relay command must start with @ or !";
        return false;
    }
    if (command.size() > MAX_COMMAND_BYTES)
    {
        error = "Relay command is too long";
        return false;
    }

    for (const unsigned char character : command)
    {
        if (character == ',' || character == '|' || character < 0x20 || character == 0x7f)
        {
            error = "Relay command contains an unsafe delimiter or control character";
            return false;
        }
    }

    const size_t placeholder     = command.find(CHANNEL_PLACEHOLDER);
    const bool   has_placeholder = placeholder != std::string::npos;
    const bool   has_second_placeholder =
        has_placeholder && command.find(CHANNEL_PLACEHOLDER, placeholder + CHANNEL_PLACEHOLDER.size()) != std::string::npos;
    if (expects_reply != has_placeholder || has_second_placeholder)
    {
        error =
            expects_reply ? "Reply command must contain exactly one %CHANNEL% placeholder" : "Non-reply command must not contain %CHANNEL%";
        return false;
    }

    return true;
}

bool RlvRemoteInventory::isValidBase64(const std::string& encoded)
{
    if (encoded.empty())
    {
        return true;
    }
    if ((encoded.size() % 4) != 0)
    {
        return false;
    }

    bool   saw_padding   = false;
    size_t padding_count = 0;
    for (const unsigned char character : encoded)
    {
        if (character == '=')
        {
            saw_padding = true;
            ++padding_count;
            if (padding_count > 2)
            {
                return false;
            }
        }
        else if (saw_padding || !(std::isalnum(character) || character == '+' || character == '/'))
        {
            return false;
        }
    }
    return true;
}

void RlvRemoteInventory::enqueue(Request&& request)
{
    mQueue.emplace_back(std::move(request));
    pumpQueue();
}

void RlvRemoteInventory::pumpQueue()
{
    if (mCurrent || mQueue.empty())
    {
        return;
    }

    mCurrent.emplace(std::move(mQueue.front()));
    mQueue.pop_front();
    mAssemblies.clear();

    std::string bridge_command;
    if (mCurrent->kind == RequestKind::RELAY)
    {
        const std::string encoded_command =
            LLBase64::encode(reinterpret_cast<const U8*>(mCurrent->command.data()), mCurrent->command.size());
        bridge_command = "RemoteRlv|" + mCurrent->id + "|" + mCurrent->target.asString() + "|" + (mCurrent->expects_reply ? "1|" : "0|") +
                         encoded_command;
    }
    else
    {
        bridge_command = "RemoteRlvAttachments|" + mCurrent->id + "|" + mCurrent->target.asString();
    }

    if (!FSLSLBridge::instance().viewerToLSL(bridge_command))
    {
        finishCurrent(EStatus::BRIDGE_UNAVAILABLE, "Firestorm LSL Bridge is unavailable");
        return;
    }

    mRequestTimer.reset();
}

void RlvRemoteInventory::finishCurrent(EStatus status, std::string payload)
{
    if (!mCurrent)
    {
        return;
    }

    Request request = std::move(*mCurrent);
    mCurrent.reset();
    mRequestTimer.stop();
    mAssemblies.clear();

    if (request.callback)
    {
        request.callback(Result{ status, request.target, request.command, std::move(payload) });
    }

    pumpQueue();
}

void RlvRemoteInventory::processBridgePayload(const std::string& kind, const std::string& payload)
{
    if (!mCurrent)
    {
        return;
    }

    if (kind == "error")
    {
        if (payload == "target_unavailable")
        {
            finishCurrent(EStatus::TARGET_UNAVAILABLE, payload);
        }
        else if (payload.rfind("invalid_request", 0) == 0)
        {
            LL_WARNS("RlvRemoteInventory") << "The LSL bridge rejected a remote RLV request: " << payload << LL_ENDL;
            finishCurrent(EStatus::INVALID_REQUEST, payload);
        }
        else
        {
            finishCurrent(EStatus::PROTOCOL_ERROR, payload);
        }
        return;
    }

    if (kind == "attachments")
    {
        if (mCurrent->kind != RequestKind::ATTACHMENTS)
        {
            finishCurrent(EStatus::PROTOCOL_ERROR, "Unexpected attachment response");
        }
        else
        {
            finishCurrent(EStatus::SUCCESS, payload);
        }
        return;
    }

    if (mCurrent->kind != RequestKind::RELAY)
    {
        finishCurrent(EStatus::PROTOCOL_ERROR, "Unexpected relay response");
        return;
    }

    if (kind == "ack")
    {
        std::string acknowledgement = payload;
        LLStringUtil::toLower(acknowledgement);
        if (acknowledgement == "ko" || acknowledgement.rfind("ko,", 0) == 0)
        {
            finishCurrent(EStatus::RELAY_REJECTED, payload);
        }
        else if (!mCurrent->expects_reply)
        {
            finishCurrent(EStatus::SUCCESS, payload);
        }
        else
        {
            LL_DEBUGS("RlvRemoteInventory") << "Relay accepted query " << mCurrent->command << LL_ENDL;
        }
        return;
    }

    if (kind == "reply")
    {
        if (!mCurrent->expects_reply)
        {
            finishCurrent(EStatus::PROTOCOL_ERROR, "Unexpected viewer query reply");
        }
        else
        {
            finishCurrent(EStatus::SUCCESS, payload);
        }
    }
}

void RlvRemoteInventory::timeoutCurrent()
{
    if (!mCurrent)
    {
        return;
    }

    // The LSL side compares the request ID, so this remains safe even if the
    // HTTP cancel and next-start requests arrive out of order.
    FSLSLBridge::instance().viewerToLSL("RemoteRlvCancel|" + mCurrent->id);
    finishCurrent(EStatus::TIMEOUT, "Remote RLV request timed out");
}
