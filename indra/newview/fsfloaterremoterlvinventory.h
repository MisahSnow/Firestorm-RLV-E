/**
 * @file fsfloaterremoterlvinventory.h
 * @brief Inventory-style controller for another avatar's RLV shared folders.
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
#include "llframetimer.h"
#include "rlvremoteinventory.h"

#include <map>
#include <memory>
#include <deque>
#include <set>
#include <string>
#include <vector>

class LLAvatarName;
class LLButton;
class LLFolderView;
class LLFolderViewFolder;
class LLFolderViewItem;
class LLPanel;
class LLScrollContainer;
class LLScrollListCtrl;
class LLTextBox;
class FSRemoteRlvFolderItem;
class FSRemoteRlvViewModel;

class FSFloaterRemoteRlvInventory final : public LLFloater
{
    friend class LLFloaterReg;
    friend class FSRemoteRlvFolderItem;

private:
    explicit FSFloaterRemoteRlvInventory(const LLSD& key);
    ~FSFloaterRemoteRlvInventory() override;

public:
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;

private:
    struct FolderState
    {
        std::string              name;
        std::string              path;
        S32                      worn_direct{ 0 };
        S32                      worn_descendants{ 0 };
        bool                     loaded{ false };
        bool                     loading{ false };
        bool                     scan_worn{ false };
        std::vector<std::string> children;
    };

    struct WornReplyEntry
    {
        std::string name;
        S32         worn_direct{ 0 };
        S32         worn_descendants{ 0 };
    };

    struct AttachmentRecord
    {
        LLUUID      object_id;
        std::string name;
        S32         attachment_point{ 0 };
    };

    // Folder-view setup and model callbacks
    void                initFolderRoot();
    void                clearRemoteData();
    void                createSharedRoot();
    LLFolderViewFolder* createFolderView(const FolderState& state, LLFolderViewFolder* parent);
    void                removeFolderDescendants(const std::string& path);
    void                onFolderSelectionChanged(const std::deque<LLFolderViewItem*>& items, bool user_action);
    bool                folderHasChildren(const std::string& path) const;
    std::string         folderDisplayName(const std::string& path) const;
    void                requestFolder(const std::string& path, bool scan_worn, bool force_reload = false);
    void                onFolderResult(const std::string& path, bool scan_worn, U32 generation, const RlvRemoteInventory::Result& result);
    bool                parseWornReply(const std::string& payload, WornReplyEntry& root, std::vector<WornReplyEntry>& children) const;

    // Target/session lifecycle
    void onChooseTarget();
    void onTargetPicked(const uuid_vec_t& ids, const std::vector<LLAvatarName>& names);
    void setTarget(const LLUUID& id, const std::string& name);
    void setTargetFromId(const LLUUID& id);
    bool validateTarget(bool report_error);
    void beginRefresh();
    void onRelayProbe(U32 generation, const RlvRemoteInventory::Result& result);
    void onViewerProbe(U32 generation, const RlvRemoteInventory::Result& result);

    // Attached tab
    void        requestAttachments(U32 generation);
    void        onAttachmentsResult(U32 generation, const RlvRemoteInventory::Result& result);
    void        requestAttachmentPoints(U32 generation);
    void        onAttachmentPointsResult(U32 generation, const RlvRemoteInventory::Result& result);
    void        refreshAttachedList();
    std::string attachmentPointName(S32 attachment_point) const;

    // Commands and controls
    void onAddFolder();
    void onDetachFolder();
    void onRefreshFolder();
    void onDetachAttached();
    void sendForceCommand(const std::string& command, const std::string& accepted_message);
    void confirmDetachPoint(const std::string& point_name);
    bool onDetachPointConfirmation(const LLSD& notification, const LLSD& response, const std::string& point_name);
    void scheduleVerificationRefresh();
    void updateControls();
    void setStatus(const std::string& message, bool error = false);

    static void onIdle(void* userdata);

private:
    std::unique_ptr<FSRemoteRlvViewModel>      mFolderViewModel;
    LLPanel*                                   mTreeHost{ nullptr };
    LLScrollContainer*                         mTreeScroller{ nullptr };
    LLFolderView*                              mFolderRoot{ nullptr };
    std::map<std::string, FolderState>         mFolders;
    std::map<std::string, LLFolderViewFolder*> mFolderViews;
    std::string                                mSelectedFolderPath;

    LLTextBox*        mTargetNameText{ nullptr };
    LLTextBox*        mStatusText{ nullptr };
    LLButton*         mRefreshButton{ nullptr };
    LLButton*         mAddFolderButton{ nullptr };
    LLButton*         mDetachFolderButton{ nullptr };
    LLButton*         mRefreshFolderButton{ nullptr };
    LLButton*         mDetachAttachedButton{ nullptr };
    LLScrollListCtrl* mAttachedList{ nullptr };

    LLUUID       mTargetId;
    std::string  mTargetName;
    U32          mRequestGeneration{ 0 };
    bool         mRelayReady{ false };
    bool         mIdleRegistered{ false };
    bool         mVerificationPending{ false };
    LLFrameTimer mVerificationTimer;
    boost::signals2::scoped_connection mTargetChangedConnection;

    std::map<LLUUID, AttachmentRecord> mAttachments;
    std::map<S32, std::string>         mOccupiedAttachmentPoints;
};
