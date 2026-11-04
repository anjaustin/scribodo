#include "clay_render.h"

#include <math.h>

static inline Color to_color(Clay_Color c)
{
    return (Color){ (unsigned char)roundf(c.r), (unsigned char)roundf(c.g),
                    (unsigned char)roundf(c.b), (unsigned char)roundf(c.a) };
}

Clay_Dimensions clay_measure_text(Clay_StringSlice text, Clay_TextElementConfig *config, void *user_data)
{
    SFont *const *fonts = user_data;
    const SFont *f = fonts[config->fontId];
    float w = sfont_measure_utf8(f, text.chars, text.length);
    w += (float)config->letterSpacing * (float)text.length;
    return (Clay_Dimensions){ w, config->lineHeight ? (float)config->lineHeight : f->line_height };
}

void clay_render(Clay_RenderCommandArray commands, SFont *const *fonts)
{
    for (int32_t i = 0; i < commands.length; i++) {
        Clay_RenderCommand *cmd = Clay_RenderCommandArray_Get(&commands, i);
        Clay_BoundingBox b = cmd->boundingBox;
        Rectangle r = { roundf(b.x), roundf(b.y), roundf(b.width), roundf(b.height) };

        switch (cmd->commandType) {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
            Clay_RectangleRenderData *d = &cmd->renderData.rectangle;
            float radius = d->cornerRadius.topLeft;
            if (radius > 0) {
                float shortest = r.width < r.height ? r.width : r.height;
                DrawRectangleRounded(r, shortest > 0 ? (radius * 2.0f) / shortest : 0, 8, to_color(d->backgroundColor));
            } else {
                DrawRectangleRec(r, to_color(d->backgroundColor));
            }
        } break;

        case CLAY_RENDER_COMMAND_TYPE_BORDER: {
            /* Square borders only for now; rounded borders are not used yet. */
            Clay_BorderRenderData *d = &cmd->renderData.border;
            Color c = to_color(d->color);
            if (d->width.left)   DrawRectangle((int)r.x, (int)r.y, d->width.left, (int)r.height, c);
            if (d->width.right)  DrawRectangle((int)(r.x + r.width - d->width.right), (int)r.y, d->width.right, (int)r.height, c);
            if (d->width.top)    DrawRectangle((int)r.x, (int)r.y, (int)r.width, d->width.top, c);
            if (d->width.bottom) DrawRectangle((int)r.x, (int)(r.y + r.height - d->width.bottom), (int)r.width, d->width.bottom, c);
        } break;

        case CLAY_RENDER_COMMAND_TYPE_TEXT: {
            Clay_TextRenderData *d = &cmd->renderData.text;
            const SFont *f = fonts[d->fontId];
            float y = b.y + (b.height - f->line_height) * 0.5f;
            sfont_draw_utf8(f, d->stringContents.chars, d->stringContents.length, b.x, y, to_color(d->textColor));
        } break;

        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
            BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
            break;

        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
            EndScissorMode();
            break;

        case CLAY_RENDER_COMMAND_TYPE_IMAGE:
        case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
        default:
            break;
        }
    }
}
