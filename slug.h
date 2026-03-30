#pragma once

#include <raylib.h>
#include <raymath.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SlugFont* PSlugFont;

PSlugFont LoadFontSlug(const char* fontFileName, const int* codepoints, int nCodepoints);
void      UnloadFontSlug(PSlugFont slugFont);

void DrawTextCodepointsSlug(PSlugFont slugFont, const int* codepoints, int nCodepoints, Vector2 position, float fontSize, float spacing, Color tint);
void DrawTextCodepointSlug(PSlugFont slugFont, int codepoint, Vector2 position, float fontSize, Color tint);
void DrawTextCodepointSlugPro(
  PSlugFont slugFont,
  int       codepoint,
  Vector2   position,
  Vector2   origin,
  float     rotation,
  float     fontSize,
  Color     tint);

#ifdef __cplusplus
}
#endif