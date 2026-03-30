#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <box2d/box2d.h>
#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include <slug.h>

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
static const int   s_bloom_levels      = 4;
static const float s_bloom_strength    = 0.8f;

static const char s_bloom_downsample_shader[] =
  "#version 330 core\n"
  "in vec2 fragTexCoord;\n"
  "in vec4 fragColor;\n"
  "uniform sampler2D texture0;\n"
  "uniform vec4 colDiffuse;\n"
  "uniform vec2 texelSize;\n"
  "out vec4 finalColor;\n"
  "void main()\n"
  "{\n"
  "  vec3 sum = vec3(0.0);\n"
  "  for (int x = -1; x <= 1; x++)\n"
  "  {\n"
  "    for (int y = -1; y <= 1; y++)\n"
  "    {\n"
  "      sum += texture(texture0, fragTexCoord + vec2(x, y) * texelSize).rgb;\n"
  "    }\n"
  "  }\n"
  "  vec3 blurred = sum/9.0;\n"
  "  finalColor = vec4(blurred, 1.0) * colDiffuse;\n"
  "}\n";

static const char s_bloom_upsample_shader[] =
  "#version 330 core\n"
  "in vec2 fragTexCoord;\n"
  "in vec4 fragColor;\n"
  "uniform sampler2D texture0;\n"
  "uniform vec4 colDiffuse;\n"
  "out vec4 finalColor;\n"
  "void main()\n"
  "{\n"
  "  vec4 color = texture(texture0, fragTexCoord);\n"
  "  finalColor = color * colDiffuse;\n"
  "}\n";

#define FreeBuffer(buffer) \
  do {                     \
    if (buffer) {          \
      MemFree(buffer);     \
      buffer = NULL;       \
    }                      \
  } while (0)

#define CloseFd(fd) \
  do {              \
    if (fd) {       \
      fclose(fd);   \
      fd = NULL;    \
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

void RenderBloomPass(
  RenderTexture  render_texture,
  RenderTexture* bloom_textures,
  const int*     bloom_widths,
  const int*     bloom_heights,
  int            bloom_levels,
  Shader         downsample_shader,
  Shader         upsample_shader,
  int            downsample_texel_loc) {
  Texture source_texture = render_texture.texture;
  int     source_width   = render_texture.texture.width;
  int     source_height  = render_texture.texture.height;

  for (int i = 0; i < bloom_levels; ++i) {
    BeginTextureMode(bloom_textures[i]);
    BeginShaderMode(downsample_shader);
    {
      Vector2 texel_size = {1.0f / (float) source_width, 1.0f / (float) source_height};
      SetShaderValue(downsample_shader, downsample_texel_loc, &texel_size, SHADER_UNIFORM_VEC2);
      DrawTexturePro(
        source_texture,
        (Rectangle){0.0f, 0.0f, (float) source_width, (float) -source_height},
        (Rectangle){0.0f, 0.0f, (float) bloom_widths[i], (float) bloom_heights[i]},
        (Vector2){0.0f, 0.0f},
        0.0f,
        WHITE);
    }
    EndShaderMode();
    EndTextureMode();

    source_texture = bloom_textures[i].texture;
    source_width   = bloom_widths[i];
    source_height  = bloom_heights[i];
  }

  for (int i = bloom_levels - 2; i >= 0; --i) {
    BeginTextureMode(bloom_textures[i]);
    BeginBlendMode(BLEND_ADDITIVE);
    BeginShaderMode(upsample_shader);
    DrawTexturePro(
      bloom_textures[i + 1].texture,
      (Rectangle){0.0f, 0.0f, (float) bloom_widths[i + 1], (float) -bloom_heights[i + 1]},
      (Rectangle){0.0f, 0.0f, (float) bloom_widths[i], (float) bloom_heights[i]},
      (Vector2){0.0f, 0.0f},
      0.0f,
      WHITE);
    EndShaderMode();
    EndBlendMode();
    EndTextureMode();
  }
}

GlyphEntity** glyph_entities;

void DrawAnim(Utf32SV text) {
  const int           screen_width         = 800;
  const int           screen_height        = 600;
  Font                font                 = {0};
  PSlugFont           slug_font            = NULL;
  RenderTexture       render_texture       = {0};
  Shader              downsample_shader    = {0};
  Shader              upsample_shader      = {0};
  int                 downsample_texel_loc = -1;
  RenderTexture       bloom_textures[4]    = {0};
  int                 bloom_widths[4]      = {0};
  int                 bloom_heights[4]     = {0};
  int                 current_width;
  int                 current_height;
  Utf32SV*            lines      = NULL;
  size_t              line_count = 0;
  CompoundGlyphEntity compound_entity;
  int                 line_width;
  int                 y_offset;

  glyph_entities = NULL;

  PrintUtf32SV(text);

  line_count = SplitUTF8IntoLines(text, NULL);
  lines      = MemAlloc(sizeof(Utf32SV) * line_count);
  if (!lines) {
    PrintErr("Failed to allocate memory for %zu lines\n", line_count);
    goto cleanup;
  }
  SplitUTF8IntoLines(text, lines);

  InitWindow(screen_width, screen_height, "Subeana-style Text Animation");

  render_texture = LoadRenderTexture(screen_width, screen_height);
  SetTextureFilter(render_texture.texture, TEXTURE_FILTER_BILINEAR);

  downsample_shader    = LoadShaderFromMemory(NULL, s_bloom_downsample_shader);
  upsample_shader      = LoadShaderFromMemory(NULL, s_bloom_upsample_shader);
  downsample_texel_loc = GetShaderLocation(downsample_shader, "texelSize");

  current_width  = screen_width;
  current_height = screen_height;
  for (int i = 0; i < s_bloom_levels; ++i) {
    current_width     = (current_width > 1) ? current_width / 2 : 1;
    current_height    = (current_height > 1) ? current_height / 2 : 1;
    bloom_widths[i]   = current_width;
    bloom_heights[i]  = current_height;
    bloom_textures[i] = LoadRenderTexture(current_width, current_height);
    SetTextureFilter(bloom_textures[i].texture, TEXTURE_FILTER_BILINEAR);
  }

  font = LoadFontEx(
    FONT_FILE,
    32,
    text.codepoints,
    text.length);
  default_font = &font;

  slug_font = LoadFontSlug(FONT_FILE, text.codepoints, text.length);

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
    BeginTextureMode(render_texture);
    ClearBackground(BLACK);

    b2World_Step(world, 1 / 60.f, 4);

    b2Vec2  position = b2Body_GetPosition(compound_entity.body_id);
    Vector2 pos      = {(float) position.x, (float) position.y};
    DrawTextCodepointsSlug(
      slug_font,
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

        DrawTextCodepointSlugPro(
          slug_font,
          entity.glyph_index,
          (Vector2){pos.x, pos.y},
          (Vector2){entity.metrics.x / 2.f, entity.metrics.y / 2.f},
          angle * RAD2DEG,
          font.baseSize,
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

    EndTextureMode();

    RenderBloomPass(
      render_texture,
      bloom_textures,
      bloom_widths,
      bloom_heights,
      s_bloom_levels,
      downsample_shader,
      upsample_shader,
      downsample_texel_loc);

    BeginDrawing();
    ClearBackground(BLACK);

    DrawTextureRec(
      render_texture.texture,
      (Rectangle){0.0f, 0.0f, (float) render_texture.texture.width, (float) -render_texture.texture.height},
      (Vector2){0.0f, 0.0f},
      WHITE);

    BeginBlendMode(BLEND_ADDITIVE);
    DrawTexturePro(
      bloom_textures[0].texture,
      (Rectangle){0.0f, 0.0f, (float) bloom_widths[0], (float) -bloom_heights[0]},
      (Rectangle){0.0f, 0.0f, (float) screen_width, (float) screen_height},
      (Vector2){0.0f, 0.0f},
      0.0f,
      (Color){255, 255, 255, (unsigned char) (255.0f * s_bloom_strength)});
    EndBlendMode();

    EndDrawing();
  }

  CloseWindow();

cleanup:
  if (glyph_entities != NULL) {
    for (size_t i = 0; i < line_count - 1; ++i) {
      FreeGlyphEntities(glyph_entities[i], lines[i + 1].length);
    }
    MemFree(glyph_entities);
  }

  for (int i = 0; i < s_bloom_levels; ++i) {
    if (bloom_textures[i].id > 0) UnloadRenderTexture(bloom_textures[i]);
  }

  if (downsample_shader.id > 0) UnloadShader(downsample_shader);
  if (upsample_shader.id > 0) UnloadShader(upsample_shader);
  if (render_texture.id > 0) UnloadRenderTexture(render_texture);

  if (slug_font != NULL) UnloadFontSlug(slug_font);
  if (font.texture.id > 0) UnloadFont(font);

  FreeBuffer(lines);

  if (IsWindowReady()) CloseWindow();
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
