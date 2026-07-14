/**
 * @file fsfloaterremoterlvrestrictions.cpp
 * @brief Remote RLV restriction controls and status for another avatar.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026 The Phoenix Firestorm Project, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fsfloaterremoterlvrestrictions.h"

#include "fslslbridge.h"
#include "llagent.h"
#include "llavatarnamecache.h"
#include "llbase64.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llfloateravatarpicker.h"
#include "llscrolllistcell.h"
#include "llscrolllistctrl.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "rlvhelper.h"
#include "rlvlocks.h"

#include <boost/algorithm/string.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <vector>

namespace
{
constexpr F64 REMOTE_RLV_REPLY_RANGE = 100.0;

struct RestrictionDefinition
{
    const char* command;
    const char* category;
    const char* label;
    const char* description;
};

constexpr RestrictionDefinition RESTRICTIONS[] = {
    { "sendchat", "Chat", "Block public chat", "Prevents normal public chat; emotes may still be allowed." },
    { "recvchat", "Chat", "Block nearby chat", "Prevents receiving nearby public chat." },
    { "sendim", "Chat", "Block outgoing IMs", "Prevents sending instant messages except allowed exceptions." },
    { "recvim", "Chat", "Block incoming IMs", "Prevents receiving instant messages except allowed exceptions." },
    { "fly", "Movement", "Prevent flying", "Prevents starting to fly and forces flight to stop." },
    { "jump", "Movement", "Prevent jumping", "Prevents jumping while the restriction is active." },
    { "sit", "Movement", "Prevent sitting", "Prevents sitting on objects or the ground." },
    { "unsit", "Movement", "Prevent standing", "Prevents standing from the current seat." },
    { "sittp", "Movement", "Limit sit teleport", "Prevents sitting beyond the viewer's allowed sit distance." },
    { "tploc", "Teleport", "Block location teleport", "Prevents teleporting directly to a location." },
    { "tplm", "Teleport", "Block landmark teleport", "Prevents teleporting through landmarks." },
    { "tplure", "Teleport", "Block teleport offers", "Prevents accepting teleport offers." },
    { "touchworld", "Touch", "Block world touches", "Prevents touching objects rezzed in the world." },
    { "touchattachother", "Touch", "Block others' attachments", "Prevents touching attachments worn by other avatars." },
    { "edit", "Interaction", "Block editing", "Prevents editing objects." },
    { "rez", "Interaction", "Block rezzing", "Prevents rezzing inventory objects into the world." },
    { "showloc", "Privacy", "Hide location", "Hides region and parcel location information." },
    { "shownames", "Privacy", "Hide avatar names", "Anonymizes avatar names where supported by the viewer." },
    { "showinv", "Privacy", "Hide inventory", "Prevents opening or browsing inventory." },
    { "showminimap", "Privacy", "Hide minimap", "Prevents opening the minimap." },
    { "showworldmap", "Privacy", "Hide world map", "Prevents opening the world map." },
    { "setcam_unlock", "Camera", "Lock camera to avatar", "Prevents unlocking the camera focus from the avatar." },
    { "addattach", "Wardrobe", "Block adding attachments", "Prevents adding attachments." },
    { "remattach", "Wardrobe", "Lock attachment removal", "Prevents removing attachments." },
    { "addoutfit", "Wardrobe", "Block adding clothing", "Prevents adding clothing layers." },
    { "remoutfit", "Wardrobe", "Lock clothing removal", "Prevents removing clothing layers." },
};

std::string requestFailureMessage(const RlvRemoteInventory::Result& result)
{
    using RemoteStatus = RlvRemoteInventory::EStatus;
    switch (result.status)
    {
        case RemoteStatus::BRIDGE_UNAVAILABLE:
            return "The Firestorm LSL Bridge is unavailable. Enable or recreate it, then refresh.";
        case RemoteStatus::TARGET_UNAVAILABLE:
            return "The selected person is no longer available in this region.";
        case RemoteStatus::RELAY_REJECTED:
            return "The target's relay rejected this request.";
        case RemoteStatus::TIMEOUT:
            return "The request timed out. Their relay may be off, approval may have been declined, or RLV may be disabled.";
        case RemoteStatus::INVALID_REQUEST:
            return result.payload.empty() ? "The bridge rejected an invalid remote RLV request." : result.payload;
        case RemoteStatus::PROTOCOL_ERROR:
            return "The relay or bridge returned a malformed response.";
        case RemoteStatus::CANCELLED:
            return "The request was cancelled.";
        case RemoteStatus::SUCCESS:
            break;
    }
    return result.payload.empty() ? "The request failed." : result.payload;
}

const RestrictionDefinition* findRestriction(const std::string& command)
{
    const std::string::size_type separator = command.find(':');
    const std::string            base      = command.substr(0, separator);
    for (const RestrictionDefinition& restriction : RESTRICTIONS)
    {
        if (base == restriction.command)
        {
            return &restriction;
        }
    }
    return nullptr;
}
} // namespace

FSFloaterRemoteRlvRestrictions::FSFloaterRemoteRlvRestrictions(const LLSD& key) :
    LLFloater(key)
{
}

FSFloaterRemoteRlvRestrictions::~FSFloaterRemoteRlvRestrictions()
{
    ++mRequestGeneration;
}

bool FSFloaterRemoteRlvRestrictions::postBuild()
{
    mTargetNameText   = getChild<LLTextBox>("target_name");
    mStatusText       = getChild<LLTextBox>("status_text");
    mRefreshButton    = getChild<LLButton>("refresh_btn");
    mReleaseAllButton = getChild<LLButton>("release_all_btn");
    mRestrictionList  = getChild<LLScrollListCtrl>("restriction_list");
    mAttachmentList   = getChild<LLScrollListCtrl>("attachment_list");
    mActiveList       = getChild<LLScrollListCtrl>("active_list");

    getChild<LLButton>("choose_target_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChooseTarget(); });
    mRefreshButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { beginRefresh(); });
    mReleaseAllButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onReleaseAll(); });
    mTargetChangedConnection = RlvRemoteInventory::instance().setTargetChangedCallback(
        [this](const LLUUID& id, const std::string& name)
        {
            if (id != mTargetId)
            {
                setTarget(id, name);
            }
            else if (!name.empty() && name != mTargetName)
            {
                mTargetName = name;
                mTargetNameText->setValue(name);
            }
        });

    clearRemoteData();
    updateControls();
    return true;
}

void FSFloaterRemoteRlvRestrictions::onOpen(const LLSD& key)
{
    LLUUID target;
    bool   target_applied = false;
    if (key.isMap() && key.has("avatar_id"))
    {
        target = key["avatar_id"].asUUID();
    }
    else if (key.isUUID() || key.isString())
    {
        target = key.asUUID();
    }
    if (target.notNull() && target != mTargetId)
    {
        setTargetFromId(target);
        target_applied = true;
    }
    else if (target.isNull() && RlvRemoteInventory::instance().getSelectedTarget().notNull() &&
             RlvRemoteInventory::instance().getSelectedTarget() != mTargetId)
    {
        setTarget(RlvRemoteInventory::instance().getSelectedTarget(), RlvRemoteInventory::instance().getSelectedTargetName());
        target_applied = true;
    }
    if (!target_applied && mTargetId.notNull() && !mRelayReady)
    {
        beginRefresh();
    }
}

void FSFloaterRemoteRlvRestrictions::onClose(bool app_quitting)
{
    ++mRequestGeneration;
    mRelayReady     = false;
    mCommandPending = false;
    LLFloater::onClose(app_quitting);
}

void FSFloaterRemoteRlvRestrictions::onChooseTarget()
{
    LLFloaterAvatarPicker* picker = LLFloaterAvatarPicker::show(
        [handle = getDerivedHandle<FSFloaterRemoteRlvRestrictions>()](const uuid_vec_t& ids, const std::vector<LLAvatarName>& names)
        {
            if (FSFloaterRemoteRlvRestrictions* self = handle.get())
            {
                self->onTargetPicked(ids, names);
            }
        },
        false, true, true, getName(), getChild<LLButton>("choose_target_btn"));
    if (picker)
    {
        addDependentFloater(picker);
    }
}

void FSFloaterRemoteRlvRestrictions::onTargetPicked(const uuid_vec_t& ids, const std::vector<LLAvatarName>& names)
{
    if (!ids.empty())
    {
        RlvRemoteInventory::instance().setSelectedTarget(
            ids.front(), !names.empty() ? names.front().getCompleteName() : ids.front().asString());
    }
}

void FSFloaterRemoteRlvRestrictions::setTarget(const LLUUID& id, const std::string& name)
{
    ++mRequestGeneration;
    mTargetId       = id;
    mTargetName     = name;
    mRelayReady     = false;
    mCommandPending = false;
    mTargetNameText->setValue(name.empty() ? id.asString() : name);
    clearRemoteData();
    updateControls();
    if (getVisible())
    {
        beginRefresh();
    }
    else
    {
        setStatus("Target synchronized. Open this floater or refresh to load their current restrictions.");
    }
}

void FSFloaterRemoteRlvRestrictions::setTargetFromId(const LLUUID& id)
{
    if (id.isNull())
    {
        return;
    }

    LLAvatarName cached_name;
    RlvRemoteInventory::instance().setSelectedTarget(
        id, LLAvatarNameCache::get(id, &cached_name) ? cached_name.getCompleteName() : id.asString());
    if (id != mTargetId)
    {
        setTarget(id, RlvRemoteInventory::instance().getSelectedTargetName());
    }
    LLAvatarNameCache::get(id,
                           [id](const LLUUID&, const LLAvatarName& avatar_name)
                           {
                               if (RlvRemoteInventory::instance().getSelectedTarget() == id)
                               {
                                   RlvRemoteInventory::instance().setSelectedTarget(id, avatar_name.getCompleteName());
                               }
                           });
}

bool FSFloaterRemoteRlvRestrictions::validateTarget(bool report_error)
{
    std::string error;
    if (mTargetId.isNull())
    {
        error = "Choose a person first.";
    }
    else if (mTargetId == gAgentID)
    {
        error = "Choose another person. Firestorm's local RLV Restrictions and Locks floaters show your own state.";
    }
    else if (!FSLSLBridge::instance().canUseBridge())
    {
        error = "The Firestorm LSL Bridge is unavailable. Enable or recreate it, then refresh.";
    }
    else
    {
        LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(gObjectList.findObject(mTargetId));
        if (!avatar || avatar->getRegion() != gAgent.getRegion())
        {
            error = "The selected person must be visible in the same region.";
        }
        else
        {
            const F64 distance = (avatar->getPositionGlobal() - gAgent.getPositionGlobal()).magVec();
            if (distance > REMOTE_RLV_REPLY_RANGE)
            {
                error = llformat("Move within 100 m of %s to receive their RLV replies (currently %.0f m away).", mTargetName.c_str(),
                                 distance);
            }
        }
    }

    if (!error.empty() && report_error)
    {
        setStatus(error, true);
    }
    return error.empty();
}

void FSFloaterRemoteRlvRestrictions::beginRefresh()
{
    if (mTargetId.isNull())
    {
        setStatus("Choose someone in the same region who is wearing an enabled RLV relay.");
        updateControls();
        return;
    }

    ++mRequestGeneration;
    const U32 generation = mRequestGeneration;
    mRelayReady          = false;
    mCommandPending      = false;
    clearRemoteData();
    updateControls();
    if (!validateTarget(true))
    {
        return;
    }

    setStatus("Looking for the target's RLV relay...");
    RlvRemoteInventory::instance().sendRelayCommand(
        mTargetId, "!version", false,
        [handle = getDerivedHandle<FSFloaterRemoteRlvRestrictions>(), generation](const RlvRemoteInventory::Result& result)
        {
            if (FSFloaterRemoteRlvRestrictions* self = handle.get())
            {
                self->onRelayProbe(generation, result);
            }
        });
}

void FSFloaterRemoteRlvRestrictions::onRelayProbe(U32 generation, const RlvRemoteInventory::Result& result)
{
    if (generation != mRequestGeneration)
    {
        return;
    }
    if (!result.succeeded())
    {
        setStatus(requestFailureMessage(result), result.status != RlvRemoteInventory::EStatus::CANCELLED);
        updateControls();
        return;
    }

    setStatus("Relay found. Waiting for approval and the target's RLV viewer...");
    RlvRemoteInventory::instance().sendRelayCommand(
        mTargetId, "@versionnum=%CHANNEL%", true,
        [handle = getDerivedHandle<FSFloaterRemoteRlvRestrictions>(), generation](const RlvRemoteInventory::Result& viewer_result)
        {
            if (FSFloaterRemoteRlvRestrictions* self = handle.get())
            {
                self->onViewerProbe(generation, viewer_result);
            }
        });
}

void FSFloaterRemoteRlvRestrictions::onViewerProbe(U32 generation, const RlvRemoteInventory::Result& result)
{
    if (generation != mRequestGeneration)
    {
        return;
    }
    if (!result.succeeded())
    {
        setStatus(requestFailureMessage(result), result.status != RlvRemoteInventory::EStatus::CANCELLED);
        updateControls();
        return;
    }

    char*      end     = nullptr;
    const long version = std::strtol(result.payload.c_str(), &end, 10);
    while (end && std::isspace(static_cast<unsigned char>(*end)))
    {
        ++end;
    }
    if (version <= 0 || end == result.payload.c_str() || (end && *end != '\0'))
    {
        setStatus("The relay answered, but the target's viewer did not return a valid RLV version.", true);
        return;
    }

    mRelayReady = true;
    setStatus(llformat("Connected to RLV API %ld. Loading restrictions and attachment state...", version));
    updateControls();
    requestRestrictionState(generation, false);
    requestAttachments(generation);
    requestAttachmentPoints(generation);
}

void FSFloaterRemoteRlvRestrictions::requestRestrictionState(U32 generation, bool after_command)
{
    RlvRemoteInventory::instance().sendRelayCommand(
        mTargetId, "@getstatus=%CHANNEL%", true,
        [handle = getDerivedHandle<FSFloaterRemoteRlvRestrictions>(), generation](const RlvRemoteInventory::Result& result)
        {
            if (FSFloaterRemoteRlvRestrictions* self = handle.get())
            {
                self->onRelayStatus(generation, result);
            }
        });
    RlvRemoteInventory::instance().sendRelayCommand(
        mTargetId, "@getstatusall=%CHANNEL%", true,
        [handle = getDerivedHandle<FSFloaterRemoteRlvRestrictions>(), generation, after_command](const RlvRemoteInventory::Result& result)
        {
            if (FSFloaterRemoteRlvRestrictions* self = handle.get())
            {
                self->onAllStatus(generation, after_command, result);
            }
        });
}

void FSFloaterRemoteRlvRestrictions::onRelayStatus(U32 generation, const RlvRemoteInventory::Result& result)
{
    if (generation != mRequestGeneration)
    {
        return;
    }
    if (result.succeeded())
    {
        mRelayRestrictions = parseStatusReply(result.payload);
        rebuildRestrictionList();
        rebuildActiveList();
    }
    else if (result.status != RlvRemoteInventory::EStatus::CANCELLED)
    {
        setStatus(requestFailureMessage(result), true);
    }
}

void FSFloaterRemoteRlvRestrictions::onAllStatus(U32 generation, bool after_command, const RlvRemoteInventory::Result& result)
{
    if (generation != mRequestGeneration)
    {
        return;
    }
    mCommandPending = false;
    if (result.succeeded())
    {
        mAllRestrictions = parseStatusReply(result.payload);
        rebuildRestrictionList();
        rebuildActiveList();
        rebuildAttachmentList();
        setStatus(after_command ? "Restriction command verified. Active rules are shown below."
                                : "Restrictions loaded. Checked rules are owned by the target's relay; other active rules are read-only.");
    }
    else if (result.status != RlvRemoteInventory::EStatus::CANCELLED)
    {
        setStatus(requestFailureMessage(result), true);
    }
    updateControls();
}

void FSFloaterRemoteRlvRestrictions::requestAttachments(U32 generation)
{
    RlvRemoteInventory::instance().requestAttachments(
        mTargetId,
        [handle = getDerivedHandle<FSFloaterRemoteRlvRestrictions>(), generation](const RlvRemoteInventory::Result& result)
        {
            if (FSFloaterRemoteRlvRestrictions* self = handle.get())
            {
                self->onAttachmentsResult(generation, result);
            }
        });
}

void FSFloaterRemoteRlvRestrictions::onAttachmentsResult(U32 generation, const RlvRemoteInventory::Result& result)
{
    if (generation != mRequestGeneration || !result.succeeded())
    {
        return;
    }

    mAttachments.clear();
    std::vector<std::string> rows;
    boost::split(rows, result.payload, boost::is_any_of("|"), boost::token_compress_off);
    for (const std::string& encoded_row : rows)
    {
        if (encoded_row.empty())
        {
            continue;
        }
        std::vector<std::string> fields;
        boost::split(fields, encoded_row, boost::is_any_of(","), boost::token_compress_off);
        LLUUID object_id;
        S32    attachment_point = 0;
        if (fields.size() != 3 || !object_id.set(fields[0], false) || !LLStringUtil::convertToS32(fields[2], attachment_point))
        {
            continue;
        }

        AttachmentRecord record;
        record.object_id        = object_id;
        record.name             = fields[1].empty() ? object_id.asString() : LLBase64::decodeAsString(fields[1]);
        record.attachment_point = attachment_point;
        mAttachments[object_id] = std::move(record);
    }
    rebuildAttachmentList();
}

void FSFloaterRemoteRlvRestrictions::requestAttachmentPoints(U32 generation)
{
    RlvRemoteInventory::instance().sendRelayCommand(
        mTargetId, "@getattach=%CHANNEL%", true,
        [handle = getDerivedHandle<FSFloaterRemoteRlvRestrictions>(), generation](const RlvRemoteInventory::Result& result)
        {
            if (FSFloaterRemoteRlvRestrictions* self = handle.get())
            {
                self->onAttachmentPointsResult(generation, result);
            }
        });
}

void FSFloaterRemoteRlvRestrictions::onAttachmentPointsResult(U32 generation, const RlvRemoteInventory::Result& result)
{
    if (generation != mRequestGeneration || !result.succeeded())
    {
        return;
    }

    mOccupiedAttachmentPoints.clear();
    for (S32 index = 1; index < static_cast<S32>(result.payload.size()); ++index)
    {
        if (result.payload[index] == '1')
        {
            mOccupiedAttachmentPoints[index] = attachmentPointName(index);
        }
    }
    rebuildAttachmentList();
}

void FSFloaterRemoteRlvRestrictions::onRestrictionToggled(const std::string& command, bool enabled)
{
    if (!mRelayReady || mCommandPending)
    {
        rebuildRestrictionList();
        return;
    }
    sendRestrictionCommand(command, enabled);
}

void FSFloaterRemoteRlvRestrictions::sendRestrictionCommand(const std::string& command, bool enabled)
{
    if (!validateTarget(true))
    {
        rebuildRestrictionList();
        return;
    }

    ++mRequestGeneration;
    const U32 generation = mRequestGeneration;
    mCommandPending      = true;
    rebuildRestrictionList();
    updateControls();
    setStatus(std::string(enabled ? "Applying " : "Releasing ") + "@" + command + " through the target's relay...");
    RlvRemoteInventory::instance().sendRelayCommand(
        mTargetId, "@" + command + (enabled ? "=n" : "=y"), false,
        [handle = getDerivedHandle<FSFloaterRemoteRlvRestrictions>(), generation](const RlvRemoteInventory::Result& result)
        {
            FSFloaterRemoteRlvRestrictions* self = handle.get();
            if (!self || generation != self->mRequestGeneration)
            {
                return;
            }
            if (!result.succeeded())
            {
                self->mCommandPending = false;
                self->setStatus(requestFailureMessage(result), result.status != RlvRemoteInventory::EStatus::CANCELLED);
                self->rebuildRestrictionList();
                self->updateControls();
                return;
            }
            self->setStatus("Relay accepted the command. Verifying the target's current restrictions...");
            self->requestRestrictionState(generation, true);
        });
}

void FSFloaterRemoteRlvRestrictions::onReleaseAll()
{
    if (!mRelayReady || mCommandPending)
    {
        return;
    }

    std::vector<std::string> commands;
    for (const RestrictionDefinition& restriction : RESTRICTIONS)
    {
        if (hasExactRule(mRelayRestrictions, restriction.command))
        {
            commands.emplace_back(restriction.command);
        }
    }
    if (commands.empty())
    {
        setStatus("The target's relay does not own any of the restrictions managed by this floater.");
        return;
    }

    ++mRequestGeneration;
    const U32 generation = mRequestGeneration;
    mCommandPending      = true;
    rebuildRestrictionList();
    updateControls();
    setStatus("Releasing all restrictions managed by the target's relay...");

    auto remaining = std::make_shared<size_t>(commands.size());
    auto failed    = std::make_shared<bool>(false);
    for (const std::string& command : commands)
    {
        RlvRemoteInventory::instance().sendRelayCommand(
            mTargetId, "@" + command + "=y", false,
            [handle = getDerivedHandle<FSFloaterRemoteRlvRestrictions>(), generation, remaining,
             failed](const RlvRemoteInventory::Result& result)
            {
                FSFloaterRemoteRlvRestrictions* self = handle.get();
                if (!self || generation != self->mRequestGeneration)
                {
                    return;
                }
                *failed = *failed || !result.succeeded();
                if (--(*remaining) == 0)
                {
                    self->setStatus(*failed ? "Some release commands failed. Refreshing the resulting state..."
                                            : "Release commands accepted. Verifying the resulting state...",
                                    *failed);
                    self->requestRestrictionState(generation, true);
                }
            });
    }
}

void FSFloaterRemoteRlvRestrictions::rebuildRestrictionList()
{
    if (!mRestrictionList)
    {
        return;
    }
    mRestrictionList->clearRows();
    for (const RestrictionDefinition& restriction : RESTRICTIONS)
    {
        const bool owned  = hasExactRule(mRelayRestrictions, restriction.command);
        const bool active = hasExactRule(mAllRestrictions, restriction.command);

        LLSD row;
        row["value"]                        = restriction.command;
        row["columns"][0]["column"]        = "managed";
        row["columns"][0]["type"]          = "checkbox";
        row["columns"][0]["value"]         = owned;
        row["columns"][1]["column"]        = "category";
        row["columns"][1]["value"]         = restriction.category;
        row["columns"][2]["column"]        = "restriction";
        row["columns"][2]["value"]         = restriction.label;
        row["columns"][3]["column"]        = "state";
        row["columns"][3]["value"]         = owned ? "Relay on" : (active ? "Active elsewhere" : "Off");
        row["columns"][4]["column"]        = "description";
        row["columns"][4]["value"]         = restriction.description;
        LLScrollListItem* item = mRestrictionList->addElement(row, ADD_BOTTOM);
        if (item)
        {
            if (auto* check_cell = dynamic_cast<LLScrollListCheck*>(item->getColumn(0)))
            {
                LLCheckBoxCtrl* checkbox = check_cell->getCheckBox();
                checkbox->setEnabled(mRelayReady && !mCommandPending);
                checkbox->setToolTip(std::string("Toggle @") + restriction.command + " for the target's relay.");
                checkbox->setCommitCallback(
                    [this, command = std::string(restriction.command)](LLUICtrl* control, const LLSD&)
                    {
                        LLCheckBoxCtrl* toggle = dynamic_cast<LLCheckBoxCtrl*>(control);
                        onRestrictionToggled(command, toggle && toggle->get());
                    });
            }
        }
    }
}

void FSFloaterRemoteRlvRestrictions::rebuildActiveList()
{
    if (!mActiveList)
    {
        return;
    }
    mActiveList->clearRows();
    for (const std::string& rule : mAllRestrictions)
    {
        const RestrictionDefinition* definition = findRestriction(rule);
        LLSD                        row;
        row["value"]                           = rule;
        row["columns"][0]["column"]           = "rule";
        row["columns"][0]["value"]            = "@" + rule;
        row["columns"][1]["column"]           = "category";
        row["columns"][1]["value"]            = definition ? definition->category : "Other";
        row["columns"][2]["column"]           = "source";
        row["columns"][2]["value"]            = hasExactRule(mRelayRestrictions, rule) ? "Target relay" : "Other or mixed";
        mActiveList->addElement(row, ADD_BOTTOM);
    }
}

void FSFloaterRemoteRlvRestrictions::rebuildAttachmentList()
{
    if (!mAttachmentList)
    {
        return;
    }
    mAttachmentList->clearRows();

    const bool global_remove_lock = hasExactRule(mAllRestrictions, "remattach");
    const bool self_lock_present  = hasRule(mAllRestrictions, "detach");
    std::set<S32> visible_points;
    for (const auto& [object_id, attachment] : mAttachments)
    {
        visible_points.insert(attachment.attachment_point);
        const std::string point = attachmentPointName(attachment.attachment_point);
        std::string       normalized_point = point;
        LLStringUtil::toLower(normalized_point);
        const bool point_remove_lock = hasExactRule(mAllRestrictions, "remattach:" + normalized_point);
        const bool point_add_lock    = hasExactRule(mAllRestrictions, "addattach:" + normalized_point);

        std::string lock_state;
        if (global_remove_lock || point_remove_lock)
        {
            lock_state = point_add_lock ? "Add and removal locked" : "Removal locked";
        }
        else if (point_add_lock)
        {
            lock_state = "Adding locked";
        }
        else if (self_lock_present)
        {
            lock_state = "Self-lock active; exact object private";
        }
        else
        {
            lock_state = "No aggregate lock reported";
        }

        LLSD row;
        row["value"]                  = object_id;
        row["columns"][0]["column"]  = "name";
        row["columns"][0]["value"]   = attachment.name;
        row["columns"][1]["column"]  = "point";
        row["columns"][1]["value"]   = point;
        row["columns"][2]["column"]  = "visibility";
        row["columns"][2]["value"]   = "Visible";
        row["columns"][3]["column"]  = "lock";
        row["columns"][3]["value"]   = lock_state;
        mAttachmentList->addElement(row, ADD_BOTTOM);
    }

    for (const auto& [point_index, point] : mOccupiedAttachmentPoints)
    {
        if (visible_points.count(point_index))
        {
            continue;
        }
        std::string normalized_point = point;
        LLStringUtil::toLower(normalized_point);
        const bool remove_lock = global_remove_lock || hasExactRule(mAllRestrictions, "remattach:" + normalized_point);

        LLSD row;
        row["value"]                  = point_index;
        row["columns"][0]["column"]  = "name";
        row["columns"][0]["value"]   = "Name unavailable (HUD or unloaded attachment)";
        row["columns"][1]["column"]  = "point";
        row["columns"][1]["value"]   = point;
        row["columns"][2]["column"]  = "visibility";
        row["columns"][2]["value"]   = "Occupied";
        row["columns"][3]["column"]  = "lock";
        row["columns"][3]["value"]   = remove_lock ? "Removal locked" : (self_lock_present ? "Self-lock may apply" : "Unknown");
        mAttachmentList->addElement(row, ADD_BOTTOM);
    }
}

void FSFloaterRemoteRlvRestrictions::updateControls()
{
    if (mRefreshButton)
    {
        mRefreshButton->setEnabled(mTargetId.notNull() && !mCommandPending);
    }
    if (mReleaseAllButton)
    {
        bool owns_managed_rule = false;
        for (const RestrictionDefinition& restriction : RESTRICTIONS)
        {
            owns_managed_rule = owns_managed_rule || hasExactRule(mRelayRestrictions, restriction.command);
        }
        mReleaseAllButton->setEnabled(mRelayReady && !mCommandPending && owns_managed_rule);
    }
}

void FSFloaterRemoteRlvRestrictions::clearRemoteData()
{
    mRelayRestrictions.clear();
    mAllRestrictions.clear();
    mAttachments.clear();
    mOccupiedAttachmentPoints.clear();
    rebuildRestrictionList();
    rebuildActiveList();
    rebuildAttachmentList();
}

void FSFloaterRemoteRlvRestrictions::setStatus(const std::string& message, bool error)
{
    if (mStatusText)
    {
        mStatusText->setValue(message);
        mStatusText->setColor(LLUIColorTable::instance().getColor(error ? "AlertCautionTextColor" : "LabelTextColor"));
    }
}

std::string FSFloaterRemoteRlvRestrictions::attachmentPointName(S32 attachment_point) const
{
    if (LLViewerJointAttachment* point = RlvAttachPtLookup::getAttachPoint(attachment_point))
    {
        return point->getName();
    }
    return llformat("Point %d", attachment_point);
}

std::set<std::string> FSFloaterRemoteRlvRestrictions::parseStatusReply(const std::string& payload)
{
    std::set<std::string> rules;
    std::vector<std::string> tokens;
    boost::split(tokens, payload, boost::is_any_of("/"), boost::token_compress_off);
    for (std::string token : tokens)
    {
        boost::trim(token);
        LLStringUtil::toLower(token);
        if (!token.empty())
        {
            rules.emplace(std::move(token));
        }
    }
    return rules;
}

bool FSFloaterRemoteRlvRestrictions::hasRule(const std::set<std::string>& rules, const std::string& command)
{
    return std::any_of(rules.begin(), rules.end(), [&command](const std::string& rule)
                       { return rule == command || boost::starts_with(rule, command + ":"); });
}

bool FSFloaterRemoteRlvRestrictions::hasExactRule(const std::set<std::string>& rules, const std::string& command)
{
    return rules.count(command) != 0;
}
