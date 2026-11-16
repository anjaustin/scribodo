/* The interface's colours, spacing and radii: every view takes its colours from here, by role, never as literals
 * (TECHNICAL-DEBT "Layout and shell": 76 colour literals in ten files before this header).
 *
 * Colours are raylib Colors; THEME_CLAY() converts one for Clay. A role names what the colour is for ("muted text",
 * "a raised surface"), so a later light or paper theme changes values here and nowhere else.
 *
 * Spacing is a 4 px grid (SP_1 = 4 ... SP_8 = 32); radii are RADIUS_S (controls) and RADIUS_M (cards, panels). */
#ifndef SCRIBODO_THEME_H
#define SCRIBODO_THEME_H

#include "raylib.h"

/* Surfaces, darkest to lightest. */
#define THEME_PANEL         ((Color){ 0x18, 0x18, 0x18, 0xFF }) /* toolbar, explorer, status bar */
#define THEME_FIELD         ((Color){ 0x1B, 0x1C, 0x1F, 0xFF }) /* text fields inside a raised surface */
#define THEME_BG            ((Color){ 0x1F, 0x1F, 0x1F, 0xFF }) /* the document's page */
#define THEME_CARD          ((Color){ 0x22, 0x24, 0x27, 0xFF }) /* findings and recall rows */
#define THEME_INPUT         ((Color){ 0x25, 0x25, 0x26, 0xFF }) /* the chat's message box */
#define THEME_SURFACE       ((Color){ 0x26, 0x27, 0x2B, 0xFF }) /* dialogs, menus, the review and record panels */
#define THEME_CARD_HOVER    ((Color){ 0x2A, 0x2D, 0x31, 0xFF }) /* a card under the pointer or selected; your chat message */
#define THEME_HOVER         ((Color){ 0x2A, 0x2D, 0x2E, 0xFF }) /* a list row under the pointer */
#define THEME_CHIP          ((Color){ 0x2C, 0x30, 0x36, 0xFF }) /* an attached document's chip */
#define THEME_CONTROL       ((Color){ 0x31, 0x31, 0x31, 0xFF }) /* a raygui button at rest */
#define THEME_ACTIVE        ((Color){ 0x37, 0x37, 0x3D, 0xFF }) /* the selected list row; a pressed control */
#define THEME_CONTROL_HOVER ((Color){ 0x3C, 0x3C, 0x3C, 0xFF })
#define THEME_HIGHLIGHT     ((Color){ 0x3A, 0x34, 0x1A, 0xFF }) /* a passage marked in the document (a source, an edit's target) */
#define THEME_SCRIM         ((Color){ 0x00, 0x00, 0x00, 0x8C }) /* behind a dialog */

/* Lines. */
#define THEME_BORDER        ((Color){ 0x2B, 0x2B, 0x2B, 0xFF })
#define THEME_BORDER_STRONG ((Color){ 0x45, 0x47, 0x4D, 0xFF }) /* a dialog's edge, a field's or control's outline */
#define THEME_SCROLLBAR     ((Color){ 0x79, 0x79, 0x79, 0x66 })

/* Text, strongest to weakest. */
#define THEME_TEXT_STRONG   ((Color){ 0xE0, 0xE0, 0xE0, 0xFF }) /* a dialog's title, a highlighted menu item */
#define THEME_TEXT_PROSE    ((Color){ 0xD4, 0xD4, 0xD4, 0xFF }) /* the author's words: documents, chat, findings */
#define THEME_TEXT          ((Color){ 0xCC, 0xCC, 0xCC, 0xFF }) /* interface labels */
#define THEME_TEXT_QUOTE    ((Color){ 0xB8, 0xB8, 0xB8, 0xFF }) /* a quoted source passage */
#define THEME_CARET         ((Color){ 0xAE, 0xAF, 0xAD, 0xFF })
#define THEME_TEXT_MUTED    ((Color){ 0x8B, 0x8B, 0x8B, 0xFF }) /* secondary labels, hints */
#define THEME_TEXT_DIM      ((Color){ 0x6B, 0x6B, 0x6B, 0xFF }) /* placeholders, disabled */

/* Accent and meaning. */
#define THEME_ACCENT        ((Color){ 0x00, 0x78, 0xD4, 0xFF }) /* focus, the primary action */
#define THEME_ON_ACCENT     ((Color){ 0xFF, 0xFF, 0xFF, 0xFF }) /* text on the accent */
#define THEME_ACCENT_SOFT   ((Color){ 0x0E, 0x63, 0x9C, 0xFF }) /* the menu item under the pointer */
#define THEME_ACCENT_EDGE   ((Color){ 0x3D, 0x6F, 0xA8, 0xFF }) /* a panel that holds the focus: review, prompt, selected finding */
#define THEME_LINK          ((Color){ 0x6C, 0xA8, 0xE0, 0xFF }) /* an action written as text */
#define THEME_SELECTION     ((Color){ 0x26, 0x4F, 0x78, 0xFF }) /* selected text */
#define THEME_OK            ((Color){ 0x4E, 0xC9, 0x8B, 0xFF }) /* ready, saved */
#define THEME_WARN          ((Color){ 0xE6, 0xB4, 0x50, 0xFF }) /* attention, read-only, a remote model, a document source */
#define THEME_ERR           ((Color){ 0xF2, 0x8B, 0x82, 0xFF }) /* failure, a possible conflict, a deletion */
#define THEME_INSERT        ((Color){ 0x8C, 0xE0, 0xA4, 0xFF }) /* an insertion; canon */
#define THEME_INSERT_BG     ((Color){ 0x1F, 0x3A, 0x2A, 0xFF })
#define THEME_CANON_EDGE    ((Color){ 0xB0, 0x8A, 0x3E, 0xFF }) /* a canon entry's frame */
#define THEME_NOTE_EDGE     ((Color){ 0x5A, 0x7F, 0x5A, 0xFF }) /* a note's frame */

/* A finding's kind, as its label is coloured. */
#define THEME_FINDING_ENTITY   THEME_INSERT
#define THEME_FINDING_EVENT    THEME_LINK
#define THEME_FINDING_RELATION ((Color){ 0xB4, 0x9C, 0xE8, 0xFF })
#define THEME_FINDING_QUESTION THEME_WARN
#define THEME_FINDING_CONFLICT THEME_ERR

/* The 4 px grid. */
#define SP_1 4.0f
#define SP_2 8.0f
#define SP_3 12.0f
#define SP_4 16.0f
#define SP_5 20.0f
#define SP_6 24.0f
#define SP_8 32.0f

#define RADIUS_S 4.0f
#define RADIUS_M 8.0f

#define THEME_CLAY(c) ((Clay_Color){ (float)(c).r, (float)(c).g, (float)(c).b, (float)(c).a })

#endif
