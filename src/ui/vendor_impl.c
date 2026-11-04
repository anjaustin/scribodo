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
