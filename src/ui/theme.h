/* The interface's colours, spacing and radii: every view takes its colours from here, by role, never as literals
 * (TECHNICAL-DEBT "Layout and shell": 76 colour literals in ten files before this header).
 *
 * Colours are raylib Colors from the palette in use (Dark, Light or Paper: theme.c); THEME_CLAY() converts one for
 * Clay. A role names what the colour is for ("muted text", "a raised surface"), so a palette changes values in
 * theme.c and nowhere else.
 *
 * Spacing is a 4 px grid (SP_1 = 4 ... SP_8 = 32); radii are RADIUS_S (controls) and RADIUS_M (cards, panels). */
#ifndef SCRIBODO_THEME_H
#define SCRIBODO_THEME_H

#include <stdbool.h>

#include "raylib.h"

/* One palette: every colour role. The three palettes and their contrast checks are in theme.c. */
typedef struct {
    const char *name;  /* in appearance.json: "dark", "light", "paper" */
    const char *label; /* in the panel */
    /* Surfaces, darkest to lightest in the dark palette. */
    Color panel;         /* toolbar, explorer, status bar */
    Color field;         /* text fields inside a raised surface */
    Color bg;            /* the document's page */
    Color card;          /* findings and recall rows */
    Color input;         /* the chat's message box */
    Color surface;       /* dialogs, menus, the review and record panels */
    Color card_hover;    /* a card under the pointer or selected; your chat message */
    Color hover;         /* a list row under the pointer */
    Color chip;          /* an attached document's chip */
    Color control;       /* a control at rest */
    Color active;        /* the selected list row; a pressed control */
    Color control_hover;
    Color highlight;     /* a passage marked in the document (a source, an edit's target) */
    Color scrim;         /* behind a dialog */
    Color shadow;        /* under a floating surface, at its darkest */
    /* Lines. */
    Color border;
    Color border_strong; /* a dialog's edge, a field's or control's outline */
    Color scrollbar;
    /* Text, strongest to weakest. */
    Color text_strong;   /* a dialog's title, a highlighted menu item */
    Color text_prose;    /* the author's words: documents, chat, findings */
    Color text;          /* interface labels */
    Color text_quote;    /* a quoted source passage */
    Color caret;
    Color text_muted;    /* secondary labels, hints */
    Color text_dim;      /* placeholders, disabled */
    /* Accent and meaning. */
    Color accent;        /* focus, the primary action */
    Color on_accent;     /* text on the accent */
    Color accent_soft;   /* the menu item under the pointer */
    Color accent_edge;   /* a panel that holds the focus: review, prompt, selected finding */
    Color link;          /* an action written as text */
    Color selection;     /* selected text */
    Color ok;            /* ready, saved */
    Color warn;          /* attention, read-only, a remote model, a document source */
    Color err;           /* failure, a possible conflict, a deletion */
    Color insert;        /* an insertion; canon */
    Color insert_bg;
    Color canon_edge;    /* a canon entry's frame */
    Color note_edge;     /* a note's frame */
    Color relation;      /* a relationship finding's label */
} ThemePalette;

enum { THEME_DARK, THEME_LIGHT, THEME_PAPER, THEME_COUNT };

/* The palette in use (dark until theme_select). Read every frame, so a change shows on the next. */
extern const ThemePalette *g_theme;
void theme_select(int which); /* out of range: dark */
int theme_current(void);
const ThemePalette *theme_palette(int which); /* NULL out of range */
int theme_by_name(const char *name);          /* -1 when unknown */
void theme_set_translucent(bool on);
bool theme_translucent(void);
Color theme_sidebar(void);
Color theme_glass_ink(Color c); /* a coloured label on the sidebar */

#define THEME_PANEL         (g_theme->panel)
/* The toolbar and Explorer: the panel colour, or, with the translucent sidebar on, the same colour partly transparent
 * over macOS's own sidebar material (ui/mac_window.h), so the desktop shows through, blurred and tinted. */
#define THEME_SIDEBAR       (theme_sidebar())
/* Text on the sidebar: on the glass each colour is one step stronger (secondary text as interface text, placeholders as
 * secondary text) and a coloured label is mixed toward the text colour, so every one keeps its own contrast level over
 * any desktop (tests/test_theme_contrast.py). Off the glass these are the ordinary roles. */
#define THEME_SIDE_MUTED    (theme_translucent() ? THEME_TEXT : THEME_TEXT_MUTED)
#define THEME_SIDE_DIM      (theme_translucent() ? theme_glass_ink(THEME_TEXT_MUTED) : THEME_TEXT_DIM)
#define THEME_FIELD         (g_theme->field)
#define THEME_BG            (g_theme->bg)
#define THEME_CARD          (g_theme->card)
#define THEME_INPUT         (g_theme->input)
#define THEME_SURFACE       (g_theme->surface)
#define THEME_CARD_HOVER    (g_theme->card_hover)
#define THEME_HOVER         (g_theme->hover)
#define THEME_CHIP          (g_theme->chip)
#define THEME_CONTROL       (g_theme->control)
#define THEME_ACTIVE        (g_theme->active)
#define THEME_CONTROL_HOVER (g_theme->control_hover)
#define THEME_HIGHLIGHT     (g_theme->highlight)
#define THEME_SCRIM         (g_theme->scrim)
#define THEME_SHADOW        (g_theme->shadow)
#define THEME_BORDER        (g_theme->border)
#define THEME_BORDER_STRONG (g_theme->border_strong)
#define THEME_SCROLLBAR     (g_theme->scrollbar)
#define THEME_TEXT_STRONG   (g_theme->text_strong)
#define THEME_TEXT_PROSE    (g_theme->text_prose)
#define THEME_TEXT          (g_theme->text)
#define THEME_TEXT_QUOTE    (g_theme->text_quote)
#define THEME_CARET         (g_theme->caret)
#define THEME_TEXT_MUTED    (g_theme->text_muted)
#define THEME_TEXT_DIM      (g_theme->text_dim)
#define THEME_ACCENT        (g_theme->accent)
#define THEME_ON_ACCENT     (g_theme->on_accent)
#define THEME_ACCENT_SOFT   (g_theme->accent_soft)
#define THEME_ACCENT_EDGE   (g_theme->accent_edge)
#define THEME_LINK          (g_theme->link)
#define THEME_SELECTION     (g_theme->selection)
#define THEME_OK            (g_theme->ok)
#define THEME_WARN          (g_theme->warn)
#define THEME_ERR           (g_theme->err)
#define THEME_INSERT        (g_theme->insert)
#define THEME_INSERT_BG     (g_theme->insert_bg)
#define THEME_CANON_EDGE    (g_theme->canon_edge)
#define THEME_NOTE_EDGE     (g_theme->note_edge)

/* A finding's kind, as its label is coloured. */
#define THEME_FINDING_ENTITY   THEME_INSERT
#define THEME_FINDING_EVENT    THEME_LINK
#define THEME_FINDING_RELATION (g_theme->relation)
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

/* A texture drawn as it is (no tint): not a colour role. */
#define THEME_NO_TINT ((Color){ 0xFF, 0xFF, 0xFF, 0xFF })

#define THEME_CLAY(c) ((Clay_Color){ (float)(c).r, (float)(c).g, (float)(c).b, (float)(c).a })

#endif
