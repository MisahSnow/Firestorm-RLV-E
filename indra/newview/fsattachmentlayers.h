/**
 * @file fsattachmentlayers.h
 * @brief User-assigned mutual-exclusion layers for inventory attachments.
 */

#ifndef FS_ATTACHMENT_LAYERS_H
#define FS_ATTACHMENT_LAYERS_H

#include "lluuid.h"

#include <string_view>

namespace FSAttachmentLayers
{
    enum class Layer
    {
        NONE,
        TOP,
        BOTTOM,
        HAIR,
        FULL_BODY
    };

    bool isLayerAction(std::string_view action);
    Layer fromAction(std::string_view action);
    Layer getLayer(const LLUUID& item_id);
    void setLayer(const LLUUID& item_id, Layer layer);
    void setLayerRecursive(const LLUUID& folder_id, Layer layer);

    S32 setAutoAttachLinks(const LLUUID& parent_id, const uuid_vec_t& selected_ids);
    void clearAutoAttachLinks(const LLUUID& parent_id);
    uuid_vec_t getAutoAttachLinks(const LLUUID& parent_id);
    void saveBackup();
    void loadBackup();

    bool layersConflict(Layer worn_layer, Layer incoming_layer);

    // Removes worn attachments that conflict with item_id's assigned layer.
    // Returns false without removing anything when an RLV lock prevents the
    // complete layer swap.
    bool removeConflictingAttachments(const LLUUID& item_id);
}

#endif // FS_ATTACHMENT_LAYERS_H
