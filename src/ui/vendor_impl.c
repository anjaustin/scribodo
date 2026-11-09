/* Single-header library implementations, compiled once without our warning
 * flags (see Makefile). */
#define CLAY_IMPLEMENTATION
#include "clay.h"

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

/* Clay_GetElementData() returns the last known box of an element even if it
 * was not declared in the current layout (e.g. a pane that was just closed).
 * Elements declared in the current layout carry generation == context + 1
 * (see Clay__AddHashMapItem), so this tells a live element from a stale one. */
bool clay_element_current(Clay_ElementId id)
{
    Clay_Context *context = Clay_GetCurrentContext();
    Clay_LayoutElementHashMapItem *item = Clay__GetHashMapItem(id.id);
    return item != &Clay_LayoutElementHashMapItem_DEFAULT && item->generation == context->generation + 1;
}

static bool clay_subtree_contains(Clay_Context *context, Clay_LayoutElement *element, uint32_t id, int depth)
{
    if (!element || depth > 64) return false;
    if (element->id == id) return true;
    if (Clay__ElementHasConfig(element, CLAY__ELEMENT_CONFIG_TYPE_TEXT)) return false; /* the union holds text, not children */
    for (int32_t i = 0; i < element->childrenOrTextContent.children.length; i++) {
        int32_t index = element->childrenOrTextContent.children.elements[i];
        if (index < 0 || index >= context->layoutElements.length) continue;
        if (clay_subtree_contains(context, Clay_LayoutElementArray_Get(&context->layoutElements, index), id, depth + 1)) return true;
    }
    return false;
}

/* Whether `id` was declared inside `ancestor` in the most recent layout. The
 * tree persists until the next Clay_BeginLayout, so this agrees with
 * Clay_GetElementData() between frames. */
bool clay_element_within(Clay_ElementId ancestor, Clay_ElementId id)
{
    if (!clay_element_current(ancestor) || !clay_element_current(id)) return false;
    Clay_Context *context = Clay_GetCurrentContext();
    Clay_LayoutElementHashMapItem *item = Clay__GetHashMapItem(ancestor.id);
    return item != &Clay_LayoutElementHashMapItem_DEFAULT && clay_subtree_contains(context, item->layoutElement, id.id, 0);
}
