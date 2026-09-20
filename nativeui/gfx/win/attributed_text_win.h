// Copyright 2020 Cheng Zhao. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

#ifndef NATIVEUI_GFX_WIN_ATTRIBUTED_TEXT_WIN_H_
#define NATIVEUI_GFX_WIN_ATTRIBUTED_TEXT_WIN_H_

#include <memory>
#include <vector>

#include "nativeui/gfx/font.h"
#include "nativeui/gfx/win/gdiplus.h"

namespace nu {

// One ranged attribute in insertion order; later runs win where overlapping.
// `font`/`brush` are optional independently depending on which API set it.
struct AttributedTextRun {
  int start = 0;  // [start, end) in UTF-16 code units, end < 0 means to end
  int end = -1;
  scoped_refptr<Font> font;
  std::unique_ptr<Gdiplus::SolidBrush> brush;
};

struct AttributedTextImpl {
  AttributedTextImpl();
  ~AttributedTextImpl();

  std::wstring text;
  scoped_refptr<Font> font;
  std::unique_ptr<Gdiplus::SolidBrush> brush;
  Gdiplus::StringFormat format;
  std::vector<AttributedTextRun> runs;
};

// One styled segment on a laid-out line (pixel coordinates relative to the
// line box origin).
struct GdiTextSegment {
  const Gdiplus::Font* font;
  Gdiplus::SolidBrush* brush;
  float x;
  int start;
  int end;
};

// One laid-out line: styled segments plus geometry.
struct GdiTextLine {
  std::vector<GdiTextSegment> segments;
  float x;      // horizontal offset produced by alignment
  float y;      // offset from top of the box
  float width;
  float height;
};

// Split the text into styled lines honoring explicit '\n', the wrap flag and
// alignment. `box` is the destination rect in pixel units. Returns the laid
// out lines and the natural size (max line width / total height) so both
// measuring and drawing go through the same layout.
void LayoutAttributedText(const AttributedTextImpl& impl,
                          Gdiplus::Graphics& g,
                          const Gdiplus::RectF& box,
                          std::vector<GdiTextLine>* lines,
                          Gdiplus::SizeF* natural);

}  // namespace nu

#endif  // NATIVEUI_GFX_WIN_ATTRIBUTED_TEXT_WIN_H_
