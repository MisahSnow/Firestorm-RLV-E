/**
 * @file fsfloaterremoterlvinventory.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsfloaterremoterlvinventory.h"

#include "fslslbridge.h"
#include "llagent.h"
#include "llavatarnamecache.h"
#include "llbase64.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llfloateravatarpicker.h"
#include "llfolderview.h"
#include "llfolderviewitem.h"
#include "llfolderviewmodel.h"
#include "lldbstrings.h"
#include "llnotificationsutil.h"
#include "llpanel.h"
#include "llscrollcontainer.h"
#include "llscrolllistctrl.h"
#include "lltextbox.h"
#include "llui.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "rlvhelper.h"
#include "rlvlocks.h"

#include <boost/algorithm/string.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace
{
constexpr F64 REMOTE_RLV_REPLY_RANGE = 100.0;
constexpr F32 FORCE_VERIFY_DELAY     = 3.0f;

bool hasUnsafePathCharacters(const std::string& path)
{ return path.find_first_of(",|=\r\n") != std::string::npos; }

bool hasUnsafeFolderNameCharacters(const std::string& name)
{
    // Slash is valid between RLV path components, but not inside one folder
    // name because the viewer cannot address that folder unambiguously.
    return name.find_first_of(",|=/\r\n") != std::string::npos;
}

std::string wornStateLabel(S32 state)
{
    switch (state)
    {
        case 2:
            return "Partial";
        case 3:
            return "Worn";
        default:
            return "";
    }
}

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
            return "The request timed out. Their relay may be off, approval may have been declined, RLV may be disabled, or they may be "
                   "out of reply range.";
        case RemoteStatus::INVALID_REQUEST:
            if (result.payload == "invalid_request:empty")
            {
                return "The LSL Bridge received an empty encoded relay command.";
            }
            if (result.payload == "invalid_request:overlong")
            {
                return "The LSL Bridge decoded an overlong relay command.";
            }
            if (result.payload == "invalid_request:bad_prefix")
            {
                return "The LSL Bridge decoded a relay command with a corrupted first character.";
            }
            if (result.payload == "invalid_request:comma" || result.payload == "invalid_request:pipe" ||
                result.payload == "invalid_request:newline" || result.payload == "invalid_request:carriage_return")
            {
                return "The LSL Bridge decoded an unsafe relay delimiter (" + result.payload.substr(16) + ").";
            }
            if (result.payload == "invalid_request:placeholder_mismatch")
            {
                return "The LSL Bridge did not receive the expected reply-channel placeholder.";
            }
            if (result.payload == "invalid_request:multiple_placeholders")
            {
                return "The LSL Bridge decoded more than one reply-channel placeholder.";
            }
            if (!result.payload.empty() && result.payload != "invalid_request")
            {
                return result.payload;
            }
            return "The selected folder contains characters that cannot be represented safely by the RLV relay protocol.";
        case RemoteStatus::PROTOCOL_ERROR:
            return "The relay or bridge returned a malformed response.";
        case RemoteStatus::CANCELLED:
            return "The request was cancelled.";
        case RemoteStatus::SUCCESS:
            break;
    }
    return result.payload.empty() ? "The request failed." : result.payload;
}
} // namespace

// ---------------------------------------------------------------------------
// Lightweight arbitrary folder model. Remote RLV paths are not LLInventory
// objects and must never be inserted into gInventory.

class FSRemoteRlvFolderItem final : public LLFolderViewModelItemCommon
{
public:
    FSRemoteRlvFolderItem(FSFloaterRemoteRlvInventory* floater, std::string name, std::string path, bool synthetic_root,
                          LLFolderViewModelInterface& root_model) :
        LLFolderViewModelItemCommon(root_model),
        mFloater(floater),
        mName(std::move(name)),
        mPath(std::move(path)),
        mSyntheticRoot(synthetic_root)
    {
    }

    const std::string& getName() const override { return mName; }
    const std::string& getDisplayName() const override;
    const std::string& getSearchableName() const override { return mName; }
    std::string        getSearchableDescription() const override { return mPath; }
    std::string        getSearchableCreatorName() const override { return {}; }
    std::string        getSearchableUUIDString() const override { return {}; }
    std::string        getSearchableAll() const override { return mName + " " + mPath; }

    LLPointer<LLUIImage> getIcon() const override { return LLUI::getUIImage("Inv_FolderClosed"); }
    LLPointer<LLUIImage> getIconOpen() const override { return LLUI::getUIImage("Inv_FolderOpen"); }
    LLPointer<LLUIImage> getIconOverlay() const override { return nullptr; }
    LLFontGL::StyleFlags getLabelStyle() const override { return LLFontGL::NORMAL; }
    std::string          getLabelSuffix() const override { return {}; }

    void openItem() override;
    void closeItem() override {}
    void selectItem() override {}
    void navigateToFolder(bool new_window = false, bool change_mode = false) override {}
    bool isFavorite() const override { return false; }
    bool isItemWearable() const override { return false; }
    bool isItemRenameable() const override { return false; }
    bool renameItem(const std::string&) override { return false; }
    bool isItemMovable() const override { return false; }
    void move(LLFolderViewModelItem*) override {}
    bool isItemRemovable(bool check_worn = true) const override { return false; }
    bool isItemInTrash() const override { return false; }
    bool removeItem() override { return false; }
    void removeBatch(std::vector<LLFolderViewModelItem*>&) override {}
    bool isItemCopyable(bool can_copy_as_link = true) const override { return false; }
    bool copyToClipboard() const override { return false; }
    bool cutToClipboard() override { return false; }
    bool isCutToClipboard() override { return false; }
    bool isClipboardPasteable() const override { return false; }
    void pasteFromClipboard() override {}
    void pasteLinkFromClipboard() override {}
    bool isAgentInventory() const override { return false; }
    bool isAgentInventoryRoot() const override { return false; }
    void buildContextMenu(LLMenuGL&, U32) override {}
    bool potentiallyVisible() override { return true; }
    bool hasChildren() const override;
    bool dragOrDrop(MASK, bool, EDragAndDropType, void*, std::string&) override { return false; }
    bool filter(LLFolderViewFilter& filter) override;

    const std::string& getPath() const { return mPath; }

private:
    bool filterChild(LLFolderViewModelItem* item, LLFolderViewFilter& filter);

    FSFloaterRemoteRlvInventory* mFloater;
    std::string                  mName;
    std::string                  mPath;
    mutable std::string          mDisplayName;
    bool                         mSyntheticRoot;
};

class FSRemoteRlvSort
{
public:
    bool operator()(const FSRemoteRlvFolderItem* lhs, const FSRemoteRlvFolderItem* rhs) const
    { return LLStringUtil::compareDict(lhs->getName(), rhs->getName()) < 0; }
};

class FSRemoteRlvFilter final : public LLFolderViewFilter
{
public:
    bool                   check(const LLFolderViewModelItem*) override { return true; }
    bool                   checkFolder(const LLFolderViewModelItem*) const override { return true; }
    void                   setEmptyLookupMessage(const std::string&) override {}
    std::string            getEmptyLookupMessage(bool is_empty_folder = false) const override { return {}; }
    bool                   showAllResults() const override { return true; }
    std::string::size_type getStringMatchOffset(LLFolderViewModelItem*) const override { return std::string::npos; }
    std::string::size_type getFilterStringSize() const override { return 0; }
    bool                   isActive() const override { return false; }
    bool                   isModified() const override { return false; }
    void                   clearModified() override {}
    const std::string&     getName() const override { return mEmpty; }
    const std::string&     getFilterText() override { return mEmpty; }
    void                   setModified(EFilterModified behavior = FILTER_RESTART) override {}
    void                   resetTime(S32 timeout) override {}
    bool                   isTimedOut() override { return false; }
    bool                   isDefault() const override { return true; }
    bool                   isNotDefault() const override { return false; }
    void                   markDefault() override {}
    void                   resetDefault() override {}
    S32                    getCurrentGeneration() const override { return 0; }
    S32                    getFirstSuccessGeneration() const override { return 0; }
    S32                    getFirstRequiredGeneration() const override { return 0; }

private:
    std::string mEmpty;
};

class FSRemoteRlvViewModel final
    : public LLFolderViewModel<FSRemoteRlvSort, FSRemoteRlvFolderItem, FSRemoteRlvFolderItem, FSRemoteRlvFilter>
{
public:
    using base_t = LLFolderViewModel<FSRemoteRlvSort, FSRemoteRlvFolderItem, FSRemoteRlvFolderItem, FSRemoteRlvFilter>;
    FSRemoteRlvViewModel() : base_t(new FSRemoteRlvSort(), new FSRemoteRlvFilter()) {}
    bool startDrag(std::vector<LLFolderViewModelItem*>&) override { return false; }
};

const std::string& FSRemoteRlvFolderItem::getDisplayName() const
{
    mDisplayName = (mSyntheticRoot || !mFloater) ? mName : mFloater->folderDisplayName(mPath);
    return mDisplayName;
}

void FSRemoteRlvFolderItem::openItem()
{
    if (!mSyntheticRoot && mFloater)
    {
        mFloater->requestFolder(mPath, false);
    }
}

bool FSRemoteRlvFolderItem::hasChildren() const
{ return mSyntheticRoot || (mFloater && mFloater->folderHasChildren(mPath)); }

bool FSRemoteRlvFolderItem::filterChild(LLFolderViewModelItem* item, LLFolderViewFilter& filter)
{
    const S32 generation = filter.getCurrentGeneration();
    if (item && item->getLastFilterGeneration() < generation)
    {
        item->filter(filter);
    }
    if (item && item->passedFilter())
    {
        FSRemoteRlvFolderItem* model = this;
        while (model && model->mMostFilteredDescendantGeneration < generation)
        {
            model->mMostFilteredDescendantGeneration = generation;
            model                                    = static_cast<FSRemoteRlvFolderItem*>(model->mParent);
        }
    }
    return !filter.isTimedOut();
}

bool FSRemoteRlvFolderItem::filter(LLFolderViewFilter& filter)
{
    const S32 generation = filter.getCurrentGeneration();
    setPassedFolderFilter(true, generation);
    for (child_list_t::iterator it = mChildren.begin(); it != mChildren.end(); ++it)
    {
        if (!filterChild(*it, filter))
        {
            return false;
        }
    }
    setPassedFilter(true, generation);
    return true;
}

// ---------------------------------------------------------------------------
// Floater lifecycle and folder tree

FSFloaterRemoteRlvInventory::FSFloaterRemoteRlvInventory(const LLSD& key) :
    LLFloater(key),
    mFolderViewModel(std::make_unique<FSRemoteRlvViewModel>())
{
    mCommitCallbackRegistrar.add("RemoteRlv.AddFolder", [this](LLUICtrl*, const LLSD&) { onAddFolder(); });
    mCommitCallbackRegistrar.add("RemoteRlv.DetachFolder", [this](LLUICtrl*, const LLSD&) { onDetachFolder(); });
    mCommitCallbackRegistrar.add("RemoteRlv.RefreshFolder", [this](LLUICtrl*, const LLSD&) { onRefreshFolder(); });
}

FSFloaterRemoteRlvInventory::~FSFloaterRemoteRlvInventory()
{
    ++mRequestGeneration;
    RlvRemoteInventory::instance().cancelAll();
    if (mIdleRegistered)
    {
        gIdleCallbacks.deleteFunction(onIdle, this);
    }
}

bool FSFloaterRemoteRlvInventory::postBuild()
{
    mTargetNameText       = getChild<LLTextBox>("target_name");
    mStatusText           = getChild<LLTextBox>("status_text");
    mRefreshButton        = getChild<LLButton>("refresh_btn");
    mAddFolderButton      = getChild<LLButton>("add_folder_btn");
    mDetachFolderButton   = getChild<LLButton>("detach_folder_btn");
    mRefreshFolderButton  = getChild<LLButton>("refresh_folder_btn");
    mDetachAttachedButton = getChild<LLButton>("detach_attached_btn");
    mAttachedList         = getChild<LLScrollListCtrl>("attached_list");
    mTreeHost             = getChild<LLPanel>("folder_tree_host");

    getChild<LLButton>("choose_target_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChooseTarget(); });
    mRefreshButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { beginRefresh(); });
    mAddFolderButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAddFolder(); });
    mDetachFolderButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDetachFolder(); });
    mRefreshFolderButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRefreshFolder(); });
    mDetachAttachedButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDetachAttached(); });
    getChild<LLButton>("refresh_attached_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { beginRefresh(); });
    mAttachedList->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateControls(); });

    initFolderRoot();
    clearRemoteData();
    updateControls();
    return true;
}

void FSFloaterRemoteRlvInventory::onOpen(const LLSD& key)
{
    if (!mIdleRegistered)
    {
        gIdleCallbacks.addFunction(onIdle, this);
        mIdleRegistered = true;
    }

    LLUUID target;
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
    }
}

void FSFloaterRemoteRlvInventory::onClose(bool app_quitting)
{
    ++mRequestGeneration;
    RlvRemoteInventory::instance().cancelAll();
    mRelayReady          = false;
    mVerificationPending = false;
    if (mIdleRegistered)
    {
        gIdleCallbacks.deleteFunction(onIdle, this);
        mIdleRegistered = false;
    }
    LLFloater::onClose(app_quitting);
}

void FSFloaterRemoteRlvInventory::initFolderRoot()
{
    LLRect                    scroller_rect = mTreeHost->getLocalRect();
    LLScrollContainer::Params scroll_params(LLUICtrlFactory::getDefaultParams<LLFolderViewScrollContainer>());
    scroll_params.name("remote rlv inventory scroller");
    scroll_params.rect(scroller_rect);
    scroll_params.follows.flags(FOLLOWS_ALL);
    scroll_params.tab_stop(true);
    mTreeScroller = LLUICtrlFactory::create<LLFolderViewScrollContainer>(scroll_params);
    mTreeHost->addChild(mTreeScroller);

    FSRemoteRlvFolderItem* root_model = new FSRemoteRlvFolderItem(this, "Remote RLV root", "", true, *mFolderViewModel);
    LLFolderView::Params   root_params(LLUICtrlFactory::getDefaultParams<LLFolderView>());
    root_params.name         = "remote rlv inventory";
    root_params.title        = "remote rlv inventory";
    root_params.rect         = LLRect(0, 0, scroller_rect.getWidth(), 0);
    root_params.parent_panel = mTreeHost;
    root_params.listener     = root_model;
    root_params.view_model   = mFolderViewModel.get();
    root_params.root         = nullptr;
    root_params.use_ellipses = true;
    root_params.options_menu = "menu_fs_remote_rlv_inventory.xml";
    mFolderRoot              = LLUICtrlFactory::create<LLFolderView>(root_params);
    mFolderRoot->setCallbackRegistrar(&mCommitCallbackRegistrar);
    mFolderRoot->setEnableRegistrar(&mEnableCallbackRegistrar);
    mTreeScroller->addChild(mFolderRoot);
    mFolderRoot->setScrollContainer(mTreeScroller);
    mFolderRoot->setFollowsAll();
    mFolderRoot->setOpen(true);
    mFolderRoot->setSelectCallback([this](const std::deque<LLFolderViewItem*>& items, bool user_action)
                                   { onFolderSelectionChanged(items, user_action); });
}

void FSFloaterRemoteRlvInventory::clearRemoteData()
{
    if (mFolderRoot)
    {
        while (mFolderRoot->getFoldersBegin() != mFolderRoot->getFoldersEnd())
        {
            (*mFolderRoot->getFoldersBegin())->destroyView();
        }
    }

    mFolders.clear();
    mFolderViews.clear();
    mSelectedFolderPath.clear();
    mAttachments.clear();
    mOccupiedAttachmentPoints.clear();
    if (mAttachedList)
    {
        mAttachedList->clearRows();
    }
    createSharedRoot();
}

void FSFloaterRemoteRlvInventory::createSharedRoot()
{
    if (!mFolderRoot)
    {
        return;
    }

    FolderState state;
    state.name = "#RLV";
    state.path.clear();
    mFolders.emplace(state.path, state);
    LLFolderViewFolder* view = createFolderView(mFolders.at(""), mFolderRoot);
    view->setVisible(true);
    mFolderRoot->setChildrenInited(true);
    mFolderViewModel->requestSortAll();
    mFolderRoot->arrangeAll();
    mFolderRoot->update();
}

LLFolderViewFolder* FSFloaterRemoteRlvInventory::createFolderView(const FolderState& state, LLFolderViewFolder* parent)
{
    FSRemoteRlvFolderItem*     model = new FSRemoteRlvFolderItem(this, state.name, state.path, false, *mFolderViewModel);
    LLFolderViewFolder::Params params(LLUICtrlFactory::getDefaultParams<LLFolderViewFolder>());
    params.name                 = state.name;
    params.root                 = mFolderRoot;
    params.listener             = model;
    params.tool_tip             = state.path.empty() ? "#RLV" : "#RLV/" + state.path;
    const LLUIColor item_color  = LLUIColorTable::instance().getColor("MenuItemEnabledColor");
    params.font_color           = item_color;
    params.font_highlight_color = item_color;
    LLFolderViewFolder* view    = LLUICtrlFactory::create<LLFolderViewFolder>(params);
    view->addToFolder(parent);
    view->setVisible(true);
    view->setChildrenInited(state.loaded);
    mFolderViews[state.path] = view;
    return view;
}

void FSFloaterRemoteRlvInventory::removeFolderDescendants(const std::string& path)
{
    auto view_it = mFolderViews.find(path);
    if (view_it == mFolderViews.end())
    {
        return;
    }

    LLFolderViewFolder* view = view_it->second;
    while (view->getFoldersBegin() != view->getFoldersEnd())
    {
        (*view->getFoldersBegin())->destroyView();
    }

    const std::string prefix = path.empty() ? std::string() : path + "/";
    for (auto it = mFolders.begin(); it != mFolders.end();)
    {
        const bool descendant = (it->first != path) && (path.empty() || boost::starts_with(it->first, prefix));
        if (descendant)
        {
            mFolderViews.erase(it->first);
            it = mFolders.erase(it);
        }
        else
        {
            ++it;
        }
    }

    FolderState& state = mFolders.at(path);
    state.children.clear();
    state.loaded  = false;
    state.loading = false;
    view->setChildrenInited(false);
    view->requestArrange();
}

void FSFloaterRemoteRlvInventory::onFolderSelectionChanged(const std::deque<LLFolderViewItem*>& items, bool user_action)
{
    mSelectedFolderPath.clear();
    if (!items.empty())
    {
        if (auto* model = dynamic_cast<FSRemoteRlvFolderItem*>(items.front()->getViewModelItem()))
        {
            mSelectedFolderPath = model->getPath();
        }
    }
    updateControls();
}

bool FSFloaterRemoteRlvInventory::folderHasChildren(const std::string& path) const
{
    auto it = mFolders.find(path);
    return it != mFolders.end() && (!it->second.loaded || !it->second.children.empty());
}

std::string FSFloaterRemoteRlvInventory::folderDisplayName(const std::string& path) const
{
    auto it = mFolders.find(path);
    if (it == mFolders.end())
    {
        return path;
    }

    const FolderState& state   = it->second;
    std::string        display = state.name;
    if (state.loading)
    {
        display += " (loading...)";
    }
    else if (state.worn_direct == 3)
    {
        display += " (worn)";
    }
    else if (state.worn_direct == 2)
    {
        display += " (partly worn)";
    }
    else if (state.worn_descendants >= 2)
    {
        display += " (contains worn items)";
    }
    return display;
}

// ---------------------------------------------------------------------------
// Target and relay session

void FSFloaterRemoteRlvInventory::onChooseTarget()
{
    LLFloaterAvatarPicker* picker = LLFloaterAvatarPicker::show(
        [handle = getDerivedHandle<FSFloaterRemoteRlvInventory>()](const uuid_vec_t& ids, const std::vector<LLAvatarName>& names)
        {
            if (FSFloaterRemoteRlvInventory* self = handle.get())
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

void FSFloaterRemoteRlvInventory::onTargetPicked(const uuid_vec_t& ids, const std::vector<LLAvatarName>& names)
{
    if (ids.empty())
    {
        return;
    }
    const std::string name = !names.empty() ? names.front().getCompleteName() : ids.front().asString();
    setTarget(ids.front(), name);
}

void FSFloaterRemoteRlvInventory::setTarget(const LLUUID& id, const std::string& name)
{
    ++mRequestGeneration;
    RlvRemoteInventory::instance().cancelAll();
    mTargetId            = id;
    mTargetName          = name;
    mRelayReady          = false;
    mVerificationPending = false;
    mTargetNameText->setValue(name.empty() ? id.asString() : name);
    clearRemoteData();
    updateControls();
    beginRefresh();
}

void FSFloaterRemoteRlvInventory::setTargetFromId(const LLUUID& id)
{
    if (id.isNull())
    {
        return;
    }

    LLAvatarName cached_name;
    setTarget(id, LLAvatarNameCache::get(id, &cached_name) ? cached_name.getCompleteName() : id.asString());
    LLAvatarNameCache::get(id,
                           [handle = getDerivedHandle<FSFloaterRemoteRlvInventory>(), id](const LLUUID&, const LLAvatarName& avatar_name)
                           {
                               if (FSFloaterRemoteRlvInventory* self = handle.get(); self && self->mTargetId == id)
                               {
                                   self->mTargetName = avatar_name.getCompleteName();
                                   self->mTargetNameText->setValue(self->mTargetName);
                               }
                           });
}

bool FSFloaterRemoteRlvInventory::validateTarget(bool report_error)
{
    std::string error;
    if (mTargetId.isNull())
    {
        error = "Choose a person first.";
    }
    else if (mTargetId == gAgentID)
    {
        error = "Choose another person. Your own #RLV folders are already available in Inventory.";
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
                error = llformat("Move within 100 m of %s to receive their RLV inventory replies (currently %.0f m away).",
                                 mTargetName.c_str(), distance);
            }
        }
    }

    if (!error.empty() && report_error)
    {
        setStatus(error, true);
    }
    return error.empty();
}

void FSFloaterRemoteRlvInventory::beginRefresh()
{
    if (mTargetId.isNull())
    {
        setStatus("Choose someone in the same region who is wearing an enabled RLV relay.");
        updateControls();
        return;
    }

    ++mRequestGeneration;
    const U32 generation = mRequestGeneration;
    RlvRemoteInventory::instance().cancelAll();
    mRelayReady          = false;
    mVerificationPending = false;
    clearRemoteData();
    updateControls();

    if (!validateTarget(true))
    {
        return;
    }

    setStatus("Looking for the target's RLV relay...");
    RlvRemoteInventory::instance().sendRelayCommand(
        mTargetId, "!version", false,
        [handle = getDerivedHandle<FSFloaterRemoteRlvInventory>(), generation](const RlvRemoteInventory::Result& result)
        {
            if (FSFloaterRemoteRlvInventory* self = handle.get())
            {
                self->onRelayProbe(generation, result);
            }
        });
}

void FSFloaterRemoteRlvInventory::onRelayProbe(U32 generation, const RlvRemoteInventory::Result& result)
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

    setStatus("Relay found. Waiting for relay approval and a response from the target's RLV viewer...");
    RlvRemoteInventory::instance().sendRelayCommand(
        mTargetId, "@versionnum=%CHANNEL%", true,
        [handle = getDerivedHandle<FSFloaterRemoteRlvInventory>(), generation](const RlvRemoteInventory::Result& viewer_result)
        {
            if (FSFloaterRemoteRlvInventory* self = handle.get())
            {
                self->onViewerProbe(generation, viewer_result);
            }
        });
}

void FSFloaterRemoteRlvInventory::onViewerProbe(U32 generation, const RlvRemoteInventory::Result& result)
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
    setStatus(llformat("Connected to the relay and RLV API %ld. Loading shared folders and attachments...", version));
    updateControls();

    auto root_view = mFolderViews.find("");
    if (root_view != mFolderViews.end())
    {
        root_view->second->setOpen(true);
    }
    requestFolder("", true);
    requestAttachments(generation);
    requestAttachmentPoints(generation);
}

void FSFloaterRemoteRlvInventory::requestFolder(const std::string& path, bool scan_worn, bool force_reload)
{
    if (!mRelayReady || mTargetId.isNull())
    {
        return;
    }

    auto state_it = mFolders.find(path);
    if (state_it == mFolders.end())
    {
        return;
    }
    if (hasUnsafePathCharacters(path))
    {
        setStatus("This folder name contains a comma, pipe, equals sign, or line break and cannot be queried safely through RLV.", true);
        return;
    }

    if (force_reload)
    {
        removeFolderDescendants(path);
        state_it = mFolders.find(path);
    }

    FolderState& state = state_it->second;
    state.scan_worn    = state.scan_worn || scan_worn;
    if (state.loading)
    {
        return;
    }
    if (state.loaded)
    {
        if (state.scan_worn)
        {
            for (const std::string& child_path : state.children)
            {
                auto child = mFolders.find(child_path);
                if (child != mFolders.end() && child->second.worn_descendants >= 2)
                {
                    requestFolder(child_path, true);
                }
            }
        }
        return;
    }

    state.loading = true;
    if (auto view = mFolderViews.find(path); view != mFolderViews.end())
    {
        view->second->refresh();
    }

    const std::string command    = path.empty() ? "@getinvworn=%CHANNEL%" : "@getinvworn:" + path + "=%CHANNEL%";
    const U32         generation = mRequestGeneration;
    RlvRemoteInventory::instance().sendRelayCommand(
        mTargetId, command, true,
        [handle = getDerivedHandle<FSFloaterRemoteRlvInventory>(), path, scan_worn, generation](const RlvRemoteInventory::Result& result)
        {
            if (FSFloaterRemoteRlvInventory* self = handle.get())
            {
                self->onFolderResult(path, scan_worn, generation, result);
            }
        });
}

void FSFloaterRemoteRlvInventory::onFolderResult(const std::string& path, bool scan_worn, U32 generation,
                                                 const RlvRemoteInventory::Result& result)
{
    if (generation != mRequestGeneration)
    {
        return;
    }
    auto state_it = mFolders.find(path);
    if (state_it == mFolders.end())
    {
        return;
    }
    state_it->second.loading = false;
    if (!result.succeeded())
    {
        if (auto view = mFolderViews.find(path); view != mFolderViews.end())
        {
            view->second->refresh();
        }
        setStatus(requestFailureMessage(result), result.status != RlvRemoteInventory::EStatus::CANCELLED);
        return;
    }

    WornReplyEntry              root_entry;
    std::vector<WornReplyEntry> child_entries;
    if (!parseWornReply(result.payload, root_entry, child_entries))
    {
        setStatus("The target returned an ambiguous or truncated #RLV folder response. One of its folder names cannot be represented "
                  "safely by the RLV path protocol.",
                  true);
        return;
    }

    if (state_it->second.loaded)
    {
        removeFolderDescendants(path);
        state_it = mFolders.find(path);
    }
    FolderState& state     = state_it->second;
    state.worn_direct      = root_entry.worn_direct;
    state.worn_descendants = root_entry.worn_descendants;
    state.loaded           = true;
    state.loading          = false;
    state.scan_worn        = state.scan_worn || scan_worn;

    std::sort(child_entries.begin(), child_entries.end(),
              [](const WornReplyEntry& lhs, const WornReplyEntry& rhs) { return LLStringUtil::compareDict(lhs.name, rhs.name) < 0; });

    LLFolderViewFolder*   parent_view = mFolderViews.at(path);
    std::set<std::string> seen_names;
    for (const WornReplyEntry& entry : child_entries)
    {
        if (entry.name.empty() || !seen_names.insert(entry.name).second)
        {
            continue;
        }
        const std::string child_path = path.empty() ? entry.name : path + "/" + entry.name;
        FolderState       child;
        child.name             = entry.name;
        child.path             = child_path;
        child.worn_direct      = entry.worn_direct;
        child.worn_descendants = entry.worn_descendants;
        mFolders[child_path]   = child;
        state.children.push_back(child_path);
        createFolderView(mFolders.at(child_path), parent_view);
    }
    parent_view->setChildrenInited(true);
    parent_view->refresh();
    mFolderViewModel->requestSortAll();
    mFolderRoot->arrangeAll();
    mFolderRoot->update();

    if (path.empty() && child_entries.empty())
    {
        setStatus("The target returned no shared #RLV folders. Their #RLV folder may be empty, missing, or unavailable to the viewer.");
    }
    else
    {
        setStatus("Remote #RLV wardrobe loaded. Relay acceptance does not override attachment locks or nostrip items.");
    }

    refreshAttachedList();
    if (state.scan_worn)
    {
        const std::vector<std::string> children = state.children;
        for (const std::string& child_path : children)
        {
            auto child = mFolders.find(child_path);
            if (child != mFolders.end() && child->second.worn_descendants >= 2)
            {
                requestFolder(child_path, true);
            }
        }
    }
}

bool FSFloaterRemoteRlvInventory::parseWornReply(const std::string& payload, WornReplyEntry& root,
                                                 std::vector<WornReplyEntry>& children) const
{
    root = WornReplyEntry();
    children.clear();
    if (payload.size() >= MAX_MSG_STR_LEN)
    {
        // RLV query replies are sent as one shout and the viewer truncates at
        // this boundary. Do not present a potentially incomplete tree.
        return false;
    }
    if (payload.empty())
    {
        return true;
    }

    std::vector<std::string> tokens;
    boost::split(tokens, payload, boost::is_any_of(","), boost::token_compress_off);
    for (size_t index = 0; index < tokens.size(); ++index)
    {
        const std::string&           token     = tokens[index];
        const std::string::size_type separator = token.rfind('|');
        if (separator == std::string::npos || separator + 3 != token.size() ||
            !std::isdigit(static_cast<unsigned char>(token[separator + 1])) ||
            !std::isdigit(static_cast<unsigned char>(token[separator + 2])))
        {
            return false;
        }

        WornReplyEntry entry;
        entry.name             = token.substr(0, separator);
        entry.worn_direct      = token[separator + 1] - '0';
        entry.worn_descendants = token[separator + 2] - '0';
        if (entry.worn_direct > 3 || entry.worn_descendants > 3)
        {
            return false;
        }
        if (index == 0)
        {
            if (!entry.name.empty())
            {
                return false;
            }
            root = entry;
        }
        else
        {
            if (hasUnsafeFolderNameCharacters(entry.name))
            {
                return false;
            }
            children.push_back(entry);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Attached tab

void FSFloaterRemoteRlvInventory::requestAttachments(U32 generation)
{
    RlvRemoteInventory::instance().requestAttachments(
        mTargetId,
        [handle = getDerivedHandle<FSFloaterRemoteRlvInventory>(), generation](const RlvRemoteInventory::Result& result)
        {
            if (FSFloaterRemoteRlvInventory* self = handle.get())
            {
                self->onAttachmentsResult(generation, result);
            }
        });
}

void FSFloaterRemoteRlvInventory::onAttachmentsResult(U32 generation, const RlvRemoteInventory::Result& result)
{
    if (generation != mRequestGeneration)
    {
        return;
    }
    if (!result.succeeded())
    {
        if (result.status != RlvRemoteInventory::EStatus::CANCELLED)
        {
            setStatus(requestFailureMessage(result), true);
        }
        return;
    }

    mAttachments.clear();
    if (!result.payload.empty())
    {
        std::vector<std::string> rows;
        boost::split(rows, result.payload, boost::is_any_of("|"), boost::token_compress_off);
        for (const std::string& encoded_row : rows)
        {
            std::vector<std::string> fields;
            boost::split(fields, encoded_row, boost::is_any_of(","), boost::token_compress_off);
            if (fields.size() != 3)
            {
                continue;
            }

            LLUUID object_id;
            S32    attachment_point = 0;
            if (!object_id.set(fields[0], false) || !LLStringUtil::convertToS32(fields[2], attachment_point))
            {
                continue;
            }

            AttachmentRecord record;
            record.object_id        = object_id;
            record.name             = fields[1].empty() ? std::string() : LLBase64::decodeAsString(fields[1]);
            record.attachment_point = attachment_point;
            if (record.name.empty())
            {
                record.name = object_id.asString();
            }
            mAttachments[object_id] = std::move(record);
        }
    }
    refreshAttachedList();
}

void FSFloaterRemoteRlvInventory::requestAttachmentPoints(U32 generation)
{
    RlvRemoteInventory::instance().sendRelayCommand(
        mTargetId, "@getattach=%CHANNEL%", true,
        [handle = getDerivedHandle<FSFloaterRemoteRlvInventory>(), generation](const RlvRemoteInventory::Result& result)
        {
            if (FSFloaterRemoteRlvInventory* self = handle.get())
            {
                self->onAttachmentPointsResult(generation, result);
            }
        });
}

void FSFloaterRemoteRlvInventory::onAttachmentPointsResult(U32 generation, const RlvRemoteInventory::Result& result)
{
    if (generation != mRequestGeneration)
    {
        return;
    }
    if (!result.succeeded())
    {
        if (result.status != RlvRemoteInventory::EStatus::CANCELLED)
        {
            setStatus(requestFailureMessage(result), true);
        }
        return;
    }

    mOccupiedAttachmentPoints.clear();
    // @getattach prepends the unused zero index; each following character is
    // the occupancy bit for the attachment point with that numeric index.
    for (S32 index = 1; index < static_cast<S32>(result.payload.size()); ++index)
    {
        if (result.payload[index] == '1')
        {
            mOccupiedAttachmentPoints[index] = attachmentPointName(index);
        }
    }
    refreshAttachedList();
}

std::string FSFloaterRemoteRlvInventory::attachmentPointName(S32 attachment_point) const
{
    if (LLViewerJointAttachment* point = RlvAttachPtLookup::getAttachPoint(attachment_point))
    {
        return point->getName();
    }
    return llformat("Point %d", attachment_point);
}

void FSFloaterRemoteRlvInventory::refreshAttachedList()
{
    if (!mAttachedList)
    {
        return;
    }

    mAttachedList->clearRows();
    std::set<S32> visible_points;
    for (const auto& [object_id, record] : mAttachments)
    {
        visible_points.insert(record.attachment_point);
        LLSD value;
        value["kind"]  = "object";
        value["uuid"]  = object_id;
        value["point"] = record.attachment_point;

        LLSD row;
        row["value"]                = value;
        row["columns"][0]["column"] = "type";
        row["columns"][0]["value"]  = "Attachment";
        row["columns"][1]["column"] = "name";
        row["columns"][1]["value"]  = record.name;
        row["columns"][2]["column"] = "point";
        row["columns"][2]["value"]  = attachmentPointName(record.attachment_point);
        row["columns"][3]["column"] = "state";
        row["columns"][3]["value"]  = "Visible";
        mAttachedList->addElement(row, ADD_BOTTOM);
    }

    for (const auto& [path, state] : mFolders)
    {
        if (path.empty() || state.worn_direct < 2)
        {
            continue;
        }
        LLSD value;
        value["kind"] = "folder";
        value["path"] = path;

        LLSD row;
        row["value"]                = value;
        row["columns"][0]["column"] = "type";
        row["columns"][0]["value"]  = "RLV folder";
        row["columns"][1]["column"] = "name";
        row["columns"][1]["value"]  = "#RLV/" + path;
        row["columns"][2]["column"] = "point";
        row["columns"][2]["value"]  = "";
        row["columns"][3]["column"] = "state";
        row["columns"][3]["value"]  = wornStateLabel(state.worn_direct);
        mAttachedList->addElement(row, ADD_BOTTOM);
    }

    for (const auto& [point_index, point_name] : mOccupiedAttachmentPoints)
    {
        if (visible_points.count(point_index))
        {
            continue;
        }
        LLSD value;
        value["kind"]       = "point";
        value["point_name"] = point_name;

        LLSD row;
        row["value"]                = value;
        row["columns"][0]["column"] = "type";
        row["columns"][0]["value"]  = "Hidden point";
        row["columns"][1]["column"] = "name";
        row["columns"][1]["value"]  = "Object name unavailable (HUD or unloaded attachment)";
        row["columns"][2]["column"] = "point";
        row["columns"][2]["value"]  = point_name;
        row["columns"][3]["column"] = "state";
        row["columns"][3]["value"]  = "Occupied";
        mAttachedList->addElement(row, ADD_BOTTOM);
    }
    updateControls();
}

// ---------------------------------------------------------------------------
// User actions

void FSFloaterRemoteRlvInventory::onAddFolder()
{
    if (!mRelayReady || mSelectedFolderPath.empty() || hasUnsafePathCharacters(mSelectedFolderPath))
    {
        return;
    }
    sendForceCommand("@attachallover:" + mSelectedFolderPath + "=force", "The relay accepted the Add Folder command.");
}

void FSFloaterRemoteRlvInventory::onDetachFolder()
{
    if (!mRelayReady || mSelectedFolderPath.empty() || hasUnsafePathCharacters(mSelectedFolderPath))
    {
        return;
    }
    sendForceCommand("@detachall:" + mSelectedFolderPath + "=force", "The relay accepted the Detach Folder command.");
}

void FSFloaterRemoteRlvInventory::onRefreshFolder()
{
    if (!mRelayReady || !mFolderRoot || !mFolderRoot->getCurSelectedItem())
    {
        return;
    }
    if (mSelectedFolderPath.empty())
    {
        beginRefresh();
        return;
    }

    ++mRequestGeneration;
    RlvRemoteInventory::instance().cancelAll();
    requestFolder(mSelectedFolderPath, false, true);
}

void FSFloaterRemoteRlvInventory::onDetachAttached()
{
    if (!mRelayReady || !mAttachedList || mAttachedList->getNumSelected() == 0)
    {
        return;
    }

    const LLSD        value = mAttachedList->getSelectedValue();
    const std::string kind  = value["kind"].asString();
    if (kind == "object")
    {
        const LLUUID object_id = value["uuid"].asUUID();
        if (object_id.notNull())
        {
            sendForceCommand("@remattach:" + object_id.asString() + "=force", "The relay accepted the exact attachment detach command.");
        }
    }
    else if (kind == "folder")
    {
        const std::string path = value["path"].asString();
        if (!path.empty() && !hasUnsafePathCharacters(path))
        {
            sendForceCommand("@detachall:" + path + "=force", "The relay accepted the RLV folder detach command.");
        }
    }
    else if (kind == "point")
    {
        confirmDetachPoint(value["point_name"].asString());
    }
}

void FSFloaterRemoteRlvInventory::sendForceCommand(const std::string& command, const std::string& accepted_message)
{
    if (!validateTarget(true))
    {
        return;
    }

    ++mRequestGeneration;
    const U32 generation = mRequestGeneration;
    RlvRemoteInventory::instance().cancelAll();
    setStatus("Sending command through the target's relay...");
    RlvRemoteInventory::instance().sendRelayCommand(
        mTargetId, command, false,
        [handle = getDerivedHandle<FSFloaterRemoteRlvInventory>(), generation, accepted_message](const RlvRemoteInventory::Result& result)
        {
            FSFloaterRemoteRlvInventory* self = handle.get();
            if (!self || generation != self->mRequestGeneration)
            {
                return;
            }
            if (!result.succeeded())
            {
                self->setStatus(requestFailureMessage(result), result.status != RlvRemoteInventory::EStatus::CANCELLED);
                return;
            }
            self->setStatus(accepted_message + " Refreshing shortly to verify what actually changed.");
            self->scheduleVerificationRefresh();
        });
}

void FSFloaterRemoteRlvInventory::confirmDetachPoint(const std::string& point_name)
{
    if (point_name.empty() || hasUnsafePathCharacters(point_name))
    {
        return;
    }
    LLSD args;
    args["MESSAGE"] = "Second Life does not reveal HUD item names to scripts. Detach every removable attachment at '" + point_name +
                      "'? This can remove more than one object.";
    LLNotificationsUtil::add(
        "GenericAlertYesCancel", args, LLSD(),
        [handle = getDerivedHandle<FSFloaterRemoteRlvInventory>(), point_name](const LLSD& notification, const LLSD& response)
        {
            if (FSFloaterRemoteRlvInventory* self = handle.get())
            {
                return self->onDetachPointConfirmation(notification, response, point_name);
            }
            return false;
        });
}

bool FSFloaterRemoteRlvInventory::onDetachPointConfirmation(const LLSD& notification, const LLSD& response, const std::string& point_name)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
    {
        sendForceCommand("@remattach:" + point_name + "=force", "The relay accepted the attachment-point detach command.");
    }
    return false;
}

void FSFloaterRemoteRlvInventory::scheduleVerificationRefresh()
{
    mVerificationTimer.reset();
    mVerificationPending = true;
}

void FSFloaterRemoteRlvInventory::updateControls()
{
    const bool has_target = mTargetId.notNull();
    if (mRefreshButton)
    {
        mRefreshButton->setEnabled(has_target);
    }

    bool folder_selected = false;
    if (mFolderRoot && mFolderRoot->getCurSelectedItem())
    {
        folder_selected = dynamic_cast<FSRemoteRlvFolderItem*>(mFolderRoot->getCurSelectedItem()->getViewModelItem()) != nullptr;
    }
    const bool folder_action =
        mRelayReady && folder_selected && !mSelectedFolderPath.empty() && !hasUnsafePathCharacters(mSelectedFolderPath);
    if (mAddFolderButton)
    {
        mAddFolderButton->setEnabled(folder_action);
        mDetachFolderButton->setEnabled(folder_action);
        mRefreshFolderButton->setEnabled(mRelayReady && folder_selected);
    }
    if (mDetachAttachedButton)
    {
        mDetachAttachedButton->setEnabled(mRelayReady && mAttachedList && mAttachedList->getNumSelected() > 0);
    }
}

void FSFloaterRemoteRlvInventory::setStatus(const std::string& message, bool error)
{
    if (!mStatusText)
    {
        return;
    }
    mStatusText->setValue(message);
    mStatusText->setColor(LLUIColorTable::instance().getColor(error ? "AlertCautionTextColor" : "LabelTextColor"));
}

void FSFloaterRemoteRlvInventory::onIdle(void* userdata)
{
    FSFloaterRemoteRlvInventory* self = static_cast<FSFloaterRemoteRlvInventory*>(userdata);
    if (!self)
    {
        return;
    }
    if (self->mFolderRoot)
    {
        self->mFolderRoot->update();
    }
    if (self->mVerificationPending && self->mVerificationTimer.getElapsedTimeF32() >= FORCE_VERIFY_DELAY)
    {
        self->mVerificationPending = false;
        self->beginRefresh();
    }
}
