/**
 * @file fsattachmentlayers.cpp
 * @brief User-assigned mutual-exclusion layers for inventory attachments.
 */

#include "llviewerprecompiledheaders.h"

#include "fsattachmentlayers.h"

#include "llappearancemgr.h"
#include "llinventorymodel.h"
#include "llnotificationsutil.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerjointattachment.h"
#include "llvoavatarself.h"
#include "rlvcommon.h"
#include "rlvhandler.h"

#include <set>
#include <string>

namespace
{
    constexpr std::string_view SETTING_NAME = "FSAttachmentLayers";

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
            if (!item || item->getType() != LLAssetType::AT_OBJECT)
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
