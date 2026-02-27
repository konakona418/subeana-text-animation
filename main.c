#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <box2d/box2d.h>
#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#ifndef MemAlloc
#define MemAlloc(size) malloc(size)
#endif

#ifndef MemFree
#define MemFree(ptr) free(ptr)
#endif

static b2WorldId world;
static Font*     default_font;

#define FONT_FILE "NotoSansJP-Regular.ttf"

// text spacing
static const int   s_spacing           = 4;
static const float s_scatter_magnitude = 300.f;
static const float gravity             = 5.f;

#define FreeBuffer(buffer)   \
    do {                     \
        if (buffer) {        \
            MemFree(buffer); \
            buffer = NULL;   \
        }                    \
    } while (0)

#define CloseFd(fd)     \
    do {                \
        if (fd) {       \
            fclose(fd); \
            fd = NULL;  \
        }               \
    } while (0)

int PrintErr(const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = vfprintf(stderr, format, args);
    va_end(args);
    return result;
}

char* ReadFileToBuffer(const char* filename, size_t* out_length) {
    char*  buffer = NULL;
    size_t length = 0;

    FILE* fd = fopen(filename, "rb");
    if (!fd) {
        PrintErr("Failed to open file: %s\n", filename);
        goto cleanup;
    }

    fseek(fd, 0, SEEK_END);
    length = ftell(fd);
    fseek(fd, 0, SEEK_SET);

    buffer = MemAlloc(length + 1);
    if (!buffer) {
        PrintErr("Failed to allocate memory for file buffer\n");
        goto cleanup;
    }

    fread(buffer, 1, length, fd);
    buffer[length] = '\0';// null-terminate

    if (out_length) {
        *out_length = length;
    }

cleanup:
    CloseFd(fd);

    return buffer;
}

int* LoadUTF8FromFile(const char* filename, size_t* out_length) {
    char*  buffer           = NULL;
    size_t length           = 0;
    int*   codepoint_buffer = NULL;
    int    utf8_length      = 0;

    buffer = ReadFileToBuffer(filename, &length);
    if (!buffer) {
        PrintErr("Failed to read file into buffer: %s\n", filename);
        goto cleanup;
    }

    codepoint_buffer = MemAlloc(sizeof(int) * (length + 1));
    if (!codepoint_buffer) {
        PrintErr("Failed to allocate memory for codepoint buffer\n");
        goto cleanup;
    }

    codepoint_buffer = LoadCodepoints(buffer, &utf8_length);
    if (!codepoint_buffer) {
        PrintErr("Failed to load codepoints from buffer\n");
        goto cleanup;
    }

    if (out_length) {
        *out_length = utf8_length;
    }

cleanup:
    FreeBuffer(buffer);

    return codepoint_buffer;
}

void FreeUTF8Buffer(int* buffer) {
    UnloadCodepoints(buffer);
}

typedef struct Utf32SV {
    const int* codepoints;
    size_t     length;
} Utf32SV;

Utf32SV CreateUtf32SV(const int* codepoints, size_t length) {
    Utf32SV sv;
    sv.codepoints = codepoints;
    sv.length     = length;
    return sv;
}

void PrintUtf32SV(Utf32SV sv) {
    for (size_t i = 0; i < sv.length; ++i) {
        printf("%c", sv.codepoints[i]);
    }
    printf("\n");
}

size_t SplitUTF8IntoLines(Utf32SV text, Utf32SV* out_lines) {
    const int* codepoints = text.codepoints;
    size_t     length     = text.length;
    size_t     line_count = 0;
    size_t     line_start = 0;

    for (size_t i = 0; i < length; ++i) {
        if (codepoints[i] == '\n') {
            if (out_lines) {
                out_lines[line_count] = CreateUtf32SV(&codepoints[line_start], (i - line_start) - 1);// exclude newline
            }
            line_start = i + 1;
            line_count++;
        }
    }

    // handle last line if it doesn't end with a newline
    if (line_start < length) {
        if (out_lines) {
            out_lines[line_count] = CreateUtf32SV(&codepoints[line_start], length - line_start);
        }
        line_count++;
    }

    return line_count;
}

void InitPhysics() {
    b2WorldDef world_def = b2DefaultWorldDef();
    world_def.gravity    = (b2Vec2){0, gravity};
    world                = b2CreateWorld(&world_def);
}

typedef struct GlyphEntity {
    int      glyph_index;
    b2BodyId body_id;
    Vector2  metrics;
} GlyphEntity;

typedef struct CompoundGlyphEntity {
    Utf32SV  text;
    b2BodyId body_id;
    Vector2  metrics;
} CompoundGlyphEntity;

b2BodyId CreateRectBody(b2WorldId world, b2BodyType type, b2Vec2 position, float width, float height) {
    b2BodyDef  body_def;
    b2BodyId   body_id;
    b2Polygon  poly;
    b2ShapeDef shape_def;

    body_def          = b2DefaultBodyDef();
    body_def.type     = type;
    body_def.position = position;
    body_id           = b2CreateBody(world, &body_def);

    poly      = b2MakeBox(width / 2, height / 2);
    shape_def = b2DefaultShapeDef();
    b2CreatePolygonShape(body_id, &shape_def, &poly);

    return body_id;
}

CompoundGlyphEntity CreateCompoundGlyphEntity(b2WorldId world, Utf32SV text, b2BodyType type, float x, float y) {
    CompoundGlyphEntity entity;
    Vector2             metrics;
    char*               utf8_buffer;

    utf8_buffer    = LoadUTF8(text.codepoints, text.length);
    metrics        = MeasureTextEx(*default_font, utf8_buffer, default_font->baseSize, s_spacing);
    entity.text    = text;
    entity.body_id = CreateRectBody(world, type, (b2Vec2){x, y}, metrics.x, metrics.y);
    entity.metrics = metrics;

    UnloadUTF8(utf8_buffer);
    return entity;
}

void FreeCompoundGlyphEntity(CompoundGlyphEntity* entity) {
    if (!entity) {
        return;
    }

    b2DestroyBody(entity->body_id);
}

GlyphEntity CreateGlyphEntity(b2WorldId world, int code_point, b2BodyType type, float x, float y) {
    GlyphEntity entity;
    GlyphInfo   metrics;

    metrics            = GetGlyphInfo(*default_font, code_point);
    entity.glyph_index = code_point;
    entity.body_id     = CreateRectBody(world, type, (b2Vec2){x, y}, metrics.advanceX, default_font->baseSize);
    entity.metrics     = (Vector2){metrics.advanceX, default_font->baseSize};

    return entity;
}

GlyphEntity* CreateGlyphEntitiesForText(b2WorldId world, Utf32SV text, b2BodyType type, float x, float y) {
    GlyphEntity* entities = MemAlloc(sizeof(GlyphEntity) * text.length);
    if (!entities) {
        PrintErr("Failed to allocate memory for glyph entities\n");
        return NULL;
    }

    float offset_x = 0;
    for (size_t i = 0; i < text.length; ++i) {
        int code_point = text.codepoints[i];
        entities[i]    = CreateGlyphEntity(world, code_point, type, x + offset_x, y);
        offset_x += entities[i].metrics.x + s_spacing;
    }

    return entities;
}

void FreeGlyphEntities(GlyphEntity* entities, size_t count) {
    if (!entities) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        b2DestroyBody(entities[i].body_id);
    }
    MemFree(entities);
}

void DrawGlyphPro(Font font, int code_point, Vector2 position, float rotation, Vector2 size, Color tint) {
    rlPushMatrix();
    rlTranslatef(position.x, position.y, 0.0f);
    rlRotatef(rotation, 0.0f, 0.0f, 1.0f);
    DrawTextCodepoint(font, code_point, (Vector2){-size.x / 2.0f, -size.y / 2.0f}, font.baseSize, tint);
    rlPopMatrix();
}

void ScatterGlyphEntities(GlyphEntity* entities, size_t count, float magnitude) {
    for (size_t i = 0; i < count; ++i) {
        float  angle   = (float) rand() / RAND_MAX * 2.f * B2_PI;
        float  force   = (float) rand() / RAND_MAX * magnitude;
        b2Vec2 impulse = {cosf(angle) * force, sinf(angle) * force};
        b2Body_ApplyLinearImpulse(entities[i].body_id, impulse, (b2Vec2){0, 0}, true);
    }
}

int MeasureUtf32SVWidth(Font font, Utf32SV text) {
    int width = 0;
    for (size_t i = 0; i < text.length; ++i) {
        GlyphInfo info = GetGlyphInfo(font, text.codepoints[i]);
        width += info.advanceX + s_spacing;
    }
    return width;
}

GlyphEntity** glyph_entities;

void DrawAnim(Utf32SV text) {
    const int           screen_width  = 800;
    const int           screen_height = 600;
    Font                font;
    Utf32SV*            lines;
    size_t              line_count;
    CompoundGlyphEntity compound_entity;
    int                 line_width;
    int                 y_offset;

    PrintUtf32SV(text);

    line_count = SplitUTF8IntoLines(text, NULL);
    lines      = MemAlloc(sizeof(Utf32SV) * line_count);
    if (!lines) {
        PrintErr("Failed to allocate memory for %zu lines\n", line_count);
        goto cleanup;
    }
    SplitUTF8IntoLines(text, lines);

    InitWindow(screen_width, screen_height, "Subeana-style Text Animation");

    font = LoadFontEx(
            FONT_FILE,
            32,
            text.codepoints,
            text.length);
    default_font = &font;

    compound_entity = CreateCompoundGlyphEntity(
            world, lines[0], b2_staticBody,
            screen_width / 2.f, screen_height / 2.f);

    glyph_entities = MemAlloc(sizeof(GlyphEntity*) * (line_count - 1));
    if (!glyph_entities) {
        PrintErr("Failed to allocate memory for glyph entities array\n");
        goto cleanup;
    }

    y_offset = -((line_count - 1) * font.baseSize + screen_height / 2.f);
    printf("y_offset: %d\n", y_offset);

    for (size_t i = 0; i < line_count - 1; ++i) {
        line_width        = MeasureUtf32SVWidth(font, lines[i + 1]);
        glyph_entities[i] = CreateGlyphEntitiesForText(
                world, lines[i + 1], b2_dynamicBody,
                screen_width / 2.f - line_width / 2.f, y_offset + i * font.baseSize * 1.2f);
        if (!glyph_entities[i]) {
            PrintErr("Failed to create glyph entities for line %zu\n", i);
            goto cleanup;
        }
        ScatterGlyphEntities(glyph_entities[i], lines[i + 1].length, s_scatter_magnitude);
    }

    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground(BLACK);

        b2World_Step(world, 1 / 60.f, 4);

        b2Vec2  position = b2Body_GetPosition(compound_entity.body_id);
        Vector2 pos      = {(float) position.x, (float) position.y};
        DrawTextCodepoints(
                font,
                compound_entity.text.codepoints,
                compound_entity.text.length,
                Vector2Subtract(pos, (Vector2){compound_entity.metrics.x / 2.f, compound_entity.metrics.y / 2.f}),
                font.baseSize,
                s_spacing,
                WHITE);

#ifdef DEBUG
        DrawRectangleLinesEx(
                (Rectangle){
                        .x      = pos.x - compound_entity.metrics.x / 2.f,
                        .y      = pos.y - compound_entity.metrics.y / 2.f,
                        .width  = compound_entity.metrics.x,
                        .height = compound_entity.metrics.y,
                },
                2,
                RED);
#endif

        for (size_t i = 0; i < line_count - 1; ++i) {
            for (size_t j = 0; j < lines[i + 1].length; ++j) {
                GlyphEntity entity = glyph_entities[i][j];
                b2Vec2      pos    = b2Body_GetPosition(entity.body_id);
                b2Rot       rot    = b2Body_GetRotation(entity.body_id);
                float       angle  = atan2f(rot.s, rot.c);

                DrawGlyphPro(
                        font,
                        entity.glyph_index,
                        (Vector2){pos.x, pos.y},
                        angle * RAD2DEG,
                        entity.metrics,
                        WHITE);

#ifdef DEBUG
                rlPushMatrix();
                rlTranslatef(pos.x, pos.y, 0.0f);
                rlRotatef(angle * RAD2DEG, 0.0f, 0.0f, 1.0f);
                DrawRectangleLinesEx(
                        (Rectangle){
                                .x      = -entity.metrics.x / 2.f,
                                .y      = -entity.metrics.y / 2.f,
                                .width  = entity.metrics.x,
                                .height = entity.metrics.y,
                        },
                        1, BLUE);
                rlPopMatrix();
#endif
            }
        }

        if (IsKeyPressed(KEY_SPACE)) {
            for (size_t i = 0; i < line_count - 1; ++i) {
                FreeGlyphEntities(glyph_entities[i], lines[i + 1].length);
            }
            MemFree(glyph_entities);

            glyph_entities = MemAlloc(sizeof(GlyphEntity*) * (line_count - 1));
            if (!glyph_entities) {
                PrintErr("Failed to allocate memory for glyph entities array\n");
                goto cleanup;
            }

            for (size_t i = 0; i < line_count - 1; ++i) {
                line_width        = MeasureUtf32SVWidth(font, lines[i + 1]);
                glyph_entities[i] = CreateGlyphEntitiesForText(
                        world, lines[i + 1], b2_dynamicBody,
                        screen_width / 2.f - line_width / 2.f, y_offset + i * font.baseSize * 1.2f);
                if (!glyph_entities[i]) {
                    PrintErr("Failed to create glyph entities for line %zu\n", i);
                    goto cleanup;
                }
                ScatterGlyphEntities(glyph_entities[i], lines[i + 1].length, s_scatter_magnitude);
            }
        }

        EndDrawing();
    }

    CloseWindow();

cleanup:
    for (size_t i = 0; i < line_count - 1; ++i) {
        FreeGlyphEntities(glyph_entities[i], lines[i + 1].length);
    }
    MemFree(glyph_entities);

    UnloadFont(font);
    FreeBuffer(lines);
}

int main(int argc, char** argv) {
    int         result;
    const char* text_file;
    size_t      text_length;
    int*        codepoints;

    result = EXIT_SUCCESS;

    if (argc < 2) {
        PrintErr("Usage: %s <text_file>\n", argv[0]);
        result = EXIT_FAILURE;
        goto cleanup;
    }

    text_file  = argv[1];
    codepoints = LoadUTF8FromFile(text_file, &text_length);

    if (!codepoints) {
        PrintErr("Failed to load UTF-8 text from file: %s\n", text_file);
        result = EXIT_FAILURE;
        goto cleanup;
    }

    InitPhysics();

    DrawAnim(CreateUtf32SV(codepoints, text_length));

cleanup:
    FreeUTF8Buffer(codepoints);

    return result;
}
