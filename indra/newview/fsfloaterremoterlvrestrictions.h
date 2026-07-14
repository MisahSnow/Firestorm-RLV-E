/**
 * @file fsfloaterremoterlvrestrictions.h
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

#pragma once

#include "llfloater.h"
#include "rlvremoteinventory.h"

#include <map>
#include <set>
#include <string>
#include <vector>

class LLAvatarName;
class LLButton;
class LLScrollListCtrl;
class LLTextBox;

class FSFloaterRemoteRlvRestrictions final : public LLFloater
{
    friend class LLFloaterReg;

private:
    explicit FSFloaterRemoteRlvRestrictions(const LLSD& key);
    ~FSFloaterRemoteRlvRestrictions() override;

public:
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;

private:
    struct AttachmentRecord
    {
        LLUUID      object_id;
        std::string name;
        S32         attachment_point{ 0 };
    };

    void onChooseTarget();
    void onTargetPicked(const uuid_vec_t& ids, const std::vector<LLAvatarName>& names);
    void setTarget(const LLUUID& id, const std::string& name);
    void setTargetFromId(const LLUUID& id);
    bool validateTarget(bool report_error);

    void beginRefresh();
    void onRelayProbe(U32 generation, const RlvRemoteInventory::Result& result);
    void onViewerProbe(U32 generation, const RlvRemoteInventory::Result& result);
    void requestRestrictionState(U32 generation, bool after_command);
    void onRelayStatus(U32 generation, const RlvRemoteInventory::Result& result);
    void onAllStatus(U32 generation, bool after_command, const RlvRemoteInventory::Result& result);
    void requestAttachments(U32 generation);
    void onAttachmentsResult(U32 generation, const RlvRemoteInventory::Result& result);
    void requestAttachmentPoints(U32 generation);
    void onAttachmentPointsResult(U32 generation, const RlvRemoteInventory::Result& result);

    void onRestrictionToggled(const std::string& command, bool enabled);
    void onReleaseAll();
    void sendRestrictionCommand(const std::string& command, bool enabled);

    void rebuildRestrictionList();
    void rebuildActiveList();
    void rebuildAttachmentList();
    void updateControls();
    void clearRemoteData();
    void setStatus(const std::string& message, bool error = false);
    std::string attachmentPointName(S32 attachment_point) const;

    static std::set<std::string> parseStatusReply(const std::string& payload);
    static bool                  hasRule(const std::set<std::string>& rules, const std::string& command);
    static bool                  hasExactRule(const std::set<std::string>& rules, const std::string& command);

private:
    LLTextBox*        mTargetNameText{ nullptr };
    LLTextBox*        mStatusText{ nullptr };
    LLButton*         mRefreshButton{ nullptr };
    LLButton*         mReleaseAllButton{ nullptr };
    LLScrollListCtrl* mRestrictionList{ nullptr };
    LLScrollListCtrl* mAttachmentList{ nullptr };
    LLScrollListCtrl* mActiveList{ nullptr };

    LLUUID      mTargetId;
    std::string mTargetName;
    U32         mRequestGeneration{ 0 };
    bool        mRelayReady{ false };
    bool        mCommandPending{ false };
    boost::signals2::scoped_connection mTargetChangedConnection;

    std::set<std::string>              mRelayRestrictions;
    std::set<std::string>              mAllRestrictions;
    std::map<LLUUID, AttachmentRecord> mAttachments;
    std::map<S32, std::string>         mOccupiedAttachmentPoints;
};
