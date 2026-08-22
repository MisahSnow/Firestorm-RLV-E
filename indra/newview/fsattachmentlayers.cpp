/**
 * @file fsattachmentlayers.cpp
 * @brief User-assigned mutual-exclusion layers for inventory attachments.
 */

#include "llviewerprecompiledheaders.h"

#include "fsattachmentlayers.h"

#include "llagent.h"
#include "llappearancemgr.h"
#include "llassetstorage.h"
#include "llfilesystem.h"
#include "llinventorymodel.h"
#include "llnotecard.h"
#include "llnotificationsutil.h"
#include "llsdserialize.h"
#include "lltrans.h"
#include "llviewerassetupload.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerobject.h"
#include "llviewerjointattachment.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "rlvcommon.h"
#include "rlvhandler.h"

#include <cctype>
#include <set>
#include <string>

namespace
{
    constexpr std::string_view SETTING_NAME = "FSAttachmentLayers";
    constexpr std::string_view AUTO_ATTACH_SETTING_NAME = "FSAutoAttachLinks";
    constexpr std::string_view BACKUP_FOLDER_NAME = "#config";
    constexpr std::string_view BACKUP_NOTECARD_NAME = "[RLV-E] Attachment Data";
    constexpr std::string_view BACKUP_HEADER = "RLV-E Attachment Data";

    const char* layerToString(FSAttachmentLayers::Layer layer)
    {
        switch (layer)
        {
            case FSAttachmentLayers::Layer::TOP:       return "top";
            case FSAttachmentLayers::Layer::BOTTOM:    return "bottom";
            case FSAttachmentLayers::Layer::HAIR:      return "hair";
            case FSAttachmentLayers::Layer::FULL_BODY: return "full_body";
            case FSAttachmentLayers::Layer::NONE:      break;
        }
        return "none";
    }

    FSAttachmentLayers::Layer layerFromString(std::string_view value)
    {
        if (value == "top")
        {
            return FSAttachmentLayers::Layer::TOP;
        }
        if (value == "bottom")
        {
            return FSAttachmentLayers::Layer::BOTTOM;
        }
        if (value == "hair")
        {
            return FSAttachmentLayers::Layer::HAIR;
        }
        if (value == "full_body")
        {
            return FSAttachmentLayers::Layer::FULL_BODY;
        }
        return FSAttachmentLayers::Layer::NONE;
    }

    LLUUID getBaseItemID(const LLUUID& item_id)
    {
        const LLUUID linked_id = gInventory.getLinkedItemID(item_id);
        return linked_id.notNull() ? linked_id : item_id;
    }

    bool isRecursiveLayerException(std::string_view name)
    {
        std::string word;
        for (const unsigned char character : name)
        {
            if (std::isalnum(character))
            {
                word.push_back(static_cast<char>(std::tolower(character)));
                continue;
            }

            if (word == "bang" || word == "bangs" || word == "hud")
            {
                return true;
            }
            word.clear();
        }
        return word == "bang" || word == "bangs" || word == "hud";
    }

    void notifyBackup(std::string_view string_name)
    {
        LLNotificationsUtil::add(
            "GenericAlert",
            LLSD().with("MESSAGE", LLTrans::getString(std::string(string_name))));
    }

    LLUUID findBackupNotecard(const LLUUID& folder_id)
    {
        LLInventoryModel::cat_array_t* categories = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(folder_id, categories, items);
        if (items)
        {
            for (const LLPointer<LLViewerInventoryItem>& item : *items)
            {
                if (item && item->getType() == LLAssetType::AT_NOTECARD && item->getName() == BACKUP_NOTECARD_NAME)
                {
                    return item->getUUID();
                }
            }
        }
        return LLUUID::null;
    }

    bool makeBackupAsset(std::string& asset_data)
    {
        LLSD backup;
        backup["header"] = std::string(BACKUP_HEADER);
        backup["version"] = 1;
        backup["attachment_layers"] = gSavedPerAccountSettings.getLLSD(SETTING_NAME);
        backup["auto_attach_links"] = gSavedPerAccountSettings.getLLSD(AUTO_ATTACH_SETTING_NAME);

        std::ostringstream xml;
        LLSDSerialize::toXML(backup, xml);
        const std::string xml_text = xml.str();
        if (xml_text.size() > LLNotecard::MAX_SIZE)
        {
            return false;
        }

        LLNotecard notecard;
        notecard.setText(xml_text);
        std::ostringstream asset;
        notecard.exportStream(asset);
        asset_data = asset.str();
        return true;
    }

    void uploadBackupNotecard(const LLUUID& item_id, const std::string& asset_data)
    {
        const LLViewerRegion* region = gAgent.getRegion();
        const std::string url = region ? region->getCapability("UpdateNotecardAgentInventory") : std::string();
        if (url.empty())
        {
            notifyBackup("AttachmentDataBackupUnavailable");
            return;
        }

        LLResourceUploadInfo::ptr_t upload = std::make_shared<LLBufferedAssetUploadInfo>(
            item_id,
            LLAssetType::AT_NOTECARD,
            asset_data,
            [](LLUUID, LLUUID, LLUUID, LLSD)
            {
                notifyBackup("AttachmentDataBackupSaved");
            },
            nullptr);
        LLViewerAssetUpload::EnqueueInventoryUpload(url, upload);
    }

    void saveBackupToFolder(const LLUUID& folder_id, const std::string& asset_data)
    {
        if (folder_id.isNull())
        {
            notifyBackup("AttachmentDataBackupUnavailable");
            return;
        }

        const LLUUID notecard_id = findBackupNotecard(folder_id);
        if (notecard_id.notNull())
        {
            uploadBackupNotecard(notecard_id, asset_data);
            return;
        }

        LLPointer<LLInventoryCallback> callback = new LLBoostFuncInventoryCallback(
            [asset_data](const LLUUID& created_id)
            {
                if (created_id.notNull())
                {
                    uploadBackupNotecard(created_id, asset_data);
                }
                else
                {
                    notifyBackup("AttachmentDataBackupUnavailable");
                }
            });
        create_inventory_item(
            gAgent.getID(),
            gAgent.getSessionID(),
            folder_id,
            LLTransactionID::tnull,
            std::string(BACKUP_NOTECARD_NAME),
            "Viewer-managed backup of RLV-E attachment layers and auto-attach links.",
            LLAssetType::AT_NOTECARD,
            LLInventoryType::IT_NOTECARD,
            NO_INV_SUBTYPE,
            PERM_MOVE,
            callback);
    }

    void onBackupLoaded(const LLUUID& asset_id, LLAssetType::EType, void*, S32 status, LLExtStat)
    {
        if (status != 0)
        {
            notifyBackup("AttachmentDataBackupLoadFailed");
            return;
        }

        LLFileSystem file(asset_id, LLAssetType::AT_NOTECARD, LLFileSystem::READ);
        std::string buffer(file.getSize(), '\0');
        if (!buffer.empty())
        {
            file.read(reinterpret_cast<U8*>(buffer.data()), static_cast<S32>(buffer.size()));
        }

        std::istringstream asset(buffer);
        LLNotecard notecard;
        LLSD backup;
        std::istringstream xml;
        if (!notecard.importStream(asset))
        {
            notifyBackup("AttachmentDataBackupLoadFailed");
            return;
        }

        xml.str(notecard.getText());
        if (LLSDSerialize::fromXML(backup, xml) <= 0 || !backup.isMap() ||
            backup["header"].asString() != BACKUP_HEADER || backup["version"].asInteger() != 1 ||
            !backup["attachment_layers"].isMap() || !backup["auto_attach_links"].isMap())
        {
            notifyBackup("AttachmentDataBackupLoadFailed");
            return;
        }

        gSavedPerAccountSettings.setLLSD(SETTING_NAME, backup["attachment_layers"]);
        gSavedPerAccountSettings.setLLSD(AUTO_ATTACH_SETTING_NAME, backup["auto_attach_links"]);
        notifyBackup("AttachmentDataBackupLoaded");
    }
}

namespace FSAttachmentLayers
{
    bool isLayerAction(std::string_view action)
    {
        constexpr std::string_view prefix = "set_attachment_layer_";
        return action.starts_with(prefix);
    }

    Layer fromAction(std::string_view action)
    {
        constexpr std::string_view prefix = "set_attachment_layer_";
        if (!isLayerAction(action))
        {
            return Layer::NONE;
        }
        return layerFromString(action.substr(prefix.size()));
    }

    Layer getLayer(const LLUUID& item_id)
    {
        if (item_id.isNull())
        {
            return Layer::NONE;
        }

        const LLSD assignments = gSavedPerAccountSettings.getLLSD(SETTING_NAME);
        const std::string key = getBaseItemID(item_id).asString();
        return assignments.has(key) ? layerFromString(assignments[key].asString()) : Layer::NONE;
    }

    void setLayer(const LLUUID& item_id, Layer layer)
    {
        if (item_id.isNull())
        {
            return;
        }

        LLSD assignments = gSavedPerAccountSettings.getLLSD(SETTING_NAME);
        const std::string key = getBaseItemID(item_id).asString();
        if (layer == Layer::NONE)
        {
            assignments.erase(key);
        }
        else
        {
            assignments[key] = layerToString(layer);
        }
        gSavedPerAccountSettings.setLLSD(SETTING_NAME, assignments);
    }

    void setLayerRecursive(const LLUUID& folder_id, Layer layer)
    {
        LLInventoryModel::cat_array_t categories;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(
            folder_id,
            categories,
            items,
            LLInventoryModel::EXCLUDE_TRASH);

        LLSD assignments = gSavedPerAccountSettings.getLLSD(SETTING_NAME);
        for (const LLPointer<LLViewerInventoryItem>& item : items)
        {
            if (!item || item->getType() != LLAssetType::AT_OBJECT || isRecursiveLayerException(item->getName()))
            {
                continue;
            }

            const std::string key = getBaseItemID(item->getUUID()).asString();
            if (layer == Layer::NONE)
            {
                assignments.erase(key);
            }
            else
            {
                assignments[key] = layerToString(layer);
            }
        }
        gSavedPerAccountSettings.setLLSD(SETTING_NAME, assignments);
    }

    S32 setAutoAttachLinks(const LLUUID& parent_id, const uuid_vec_t& selected_ids)
    {
        const LLUUID base_parent_id = getBaseItemID(parent_id);
        if (base_parent_id.isNull())
        {
            return 0;
        }

        std::set<LLUUID> linked_ids;
        for (const LLUUID& selected_id : selected_ids)
        {
            const LLViewerInventoryItem* item = gInventory.getItem(selected_id);
            if (!item || item->getType() != LLAssetType::AT_OBJECT)
            {
                continue;
            }

            const LLUUID base_selected_id = getBaseItemID(selected_id);
            if (base_selected_id.notNull() && base_selected_id != base_parent_id)
            {
                linked_ids.insert(base_selected_id);
            }
        }

        LLSD links = gSavedPerAccountSettings.getLLSD(AUTO_ATTACH_SETTING_NAME);
        const std::string parent_key = base_parent_id.asString();
        if (linked_ids.empty())
        {
            links.erase(parent_key);
        }
        else
        {
            LLSD children = LLSD::emptyArray();
            for (const LLUUID& linked_id : linked_ids)
            {
                children.append(linked_id);
            }
            links[parent_key] = children;
        }
        gSavedPerAccountSettings.setLLSD(AUTO_ATTACH_SETTING_NAME, links);
        return static_cast<S32>(linked_ids.size());
    }

    void clearAutoAttachLinks(const LLUUID& parent_id)
    {
        const LLUUID base_parent_id = getBaseItemID(parent_id);
        if (base_parent_id.isNull())
        {
            return;
        }

        LLSD links = gSavedPerAccountSettings.getLLSD(AUTO_ATTACH_SETTING_NAME);
        links.erase(base_parent_id.asString());
        gSavedPerAccountSettings.setLLSD(AUTO_ATTACH_SETTING_NAME, links);
    }

    uuid_vec_t getAutoAttachLinks(const LLUUID& parent_id)
    {
        uuid_vec_t result;
        const LLUUID base_parent_id = getBaseItemID(parent_id);
        if (base_parent_id.isNull())
        {
            return result;
        }

        const LLSD links = gSavedPerAccountSettings.getLLSD(AUTO_ATTACH_SETTING_NAME);
        const LLSD& children = links[base_parent_id.asString()];
        if (!children.isArray())
        {
            return result;
        }

        std::set<LLUUID> unique_ids;
        for (const LLSD& child : llsd::inArray(children))
        {
            const LLUUID child_id = child.asUUID();
            if (child_id.notNull() && child_id != base_parent_id && gInventory.getItem(child_id))
            {
                unique_ids.insert(child_id);
            }
        }
        result.assign(unique_ids.begin(), unique_ids.end());
        return result;
    }

    void saveBackup()
    {
        std::string asset_data;
        if (!makeBackupAsset(asset_data))
        {
            notifyBackup("AttachmentDataBackupTooLarge");
            return;
        }

        const LLUUID folder_id = gInventory.findCategoryByName(BACKUP_FOLDER_NAME);
        if (folder_id.notNull())
        {
            saveBackupToFolder(folder_id, asset_data);
        }
        else
        {
            gInventory.createNewCategory(
                gInventory.getRootFolderID(),
                LLFolderType::FT_NONE,
                std::string(BACKUP_FOLDER_NAME),
                [asset_data](const LLUUID& created_id)
                {
                    saveBackupToFolder(created_id, asset_data);
                });
        }
    }

    void loadBackup()
    {
        const LLUUID folder_id = gInventory.findCategoryByName(BACKUP_FOLDER_NAME);
        const LLUUID notecard_id = folder_id.notNull() ? findBackupNotecard(folder_id) : LLUUID::null;
        const LLViewerInventoryItem* notecard = gInventory.getItem(notecard_id);
        if (!notecard || notecard->getAssetUUID().isNull() || !gAssetStorage)
        {
            notifyBackup("AttachmentDataBackupMissing");
            return;
        }

        gAssetStorage->getInvItemAsset(
            LLHost(),
            gAgent.getID(),
            gAgent.getSessionID(),
            notecard->getPermissions().getOwner(),
            LLUUID::null,
            notecard->getUUID(),
            notecard->getAssetUUID(),
            LLAssetType::AT_NOTECARD,
            onBackupLoaded,
            nullptr,
            true);
    }

    bool layersConflict(Layer worn_layer, Layer incoming_layer)
    {
        if (worn_layer == Layer::NONE || incoming_layer == Layer::NONE)
        {
            return false;
        }
        if ((worn_layer == Layer::FULL_BODY && incoming_layer == Layer::HAIR) ||
            (worn_layer == Layer::HAIR && incoming_layer == Layer::FULL_BODY))
        {
            return false;
        }
        if (worn_layer == Layer::FULL_BODY || incoming_layer == Layer::FULL_BODY)
        {
            return true;
        }
        return worn_layer == incoming_layer;
    }

    bool removeConflictingAttachments(const LLUUID& item_id)
    {
        const LLUUID incoming_id = getBaseItemID(item_id);
        const Layer incoming_layer = getLayer(incoming_id);
        if (incoming_layer == Layer::NONE || !isAgentAvatarValid())
        {
            return true;
        }

        std::set<LLUUID> conflict_ids;
        for (const auto& [attach_point_id, attachment] : gAgentAvatarp->mAttachmentPoints)
        {
            (void)attach_point_id;
            for (const LLPointer<LLViewerObject>& object : attachment->mAttachedObjects)
            {
                if (!object)
                {
                    continue;
                }

                const LLUUID worn_id = getBaseItemID(object->getAttachmentItemID());
                if (worn_id.notNull() && worn_id != incoming_id && layersConflict(getLayer(worn_id), incoming_layer))
                {
                    conflict_ids.insert(worn_id);
                }
            }
        }

        if (conflict_ids.empty())
        {
            return true;
        }

        if (rlv_handler_t::isEnabled())
        {
            for (const LLUUID& conflict_id : conflict_ids)
            {
                if (!rlvPredCanRemoveItem(conflict_id))
                {
                    LLNotificationsUtil::add(
                        "GenericAlert",
                        LLSD().with("MESSAGE", LLTrans::getString("AttachmentLayerConflictLocked")));
                    return false;
                }
            }
        }

        uuid_vec_t conflicts(conflict_ids.begin(), conflict_ids.end());
        LLAppearanceMgr::instance().removeItemsFromAvatar(conflicts);
        return true;
    }
}
