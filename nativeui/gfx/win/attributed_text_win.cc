// Copyright 2020 Cheng Zhao. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

#include "nativeui/gfx/win/attributed_text_win.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "base/win/scoped_hdc.h"
#include "nativeui/gfx/attributed_text.h"
#include "nativeui/gfx/geometry/size_conversions.h"
#include "nativeui/win/screen_win.h"

namespace nu {

namespace {

// A styled segment after applying ranged attributes on top of global ones.
// `font`/`brush` fall back to the global ones when no run covers the segment.
struct ResolvedSegment {
  int start;
  int end;
  const Gdiplus::Font* font;
  Gdiplus::SolidBrush* brush;
};

// Measure the width of [start, end) of `text` with `font`.
float MeasureSubString(const std::wstring& text,
                       int start, int end,
                       const Gdiplus::Font* font,
                       const Gdiplus::StringFormat& format,
                       Gdiplus::Graphics& g) {
  int len = end - start;
  if (len <= 0)
    return 0.f;
  Gdiplus::RectF rect;
  g.MeasureString(text.data() + start, len, font, Gdiplus::PointF(0.f, 0.f),
                  &format, &rect);
  return rect.Width;
}

// Split [line_start, line_end) into segments by run boundaries and resolve
// each segment's attributes: later runs win, global font/brush as fallback.
void ResolveSegments(const AttributedTextImpl& impl,
                     int line_start, int line_end,
                     std::vector<ResolvedSegment>* out) {
  // Collect cut points inside the line.
  std::vector<int> cuts;
  cuts.push_back(line_start);
  cuts.push_back(line_end);
  for (const AttributedTextRun& run : impl.runs) {
    if (run.start > line_start && run.start < line_end)
      cuts.push_back(run.start);
    if (run.end > line_start && run.end < line_end)
      cuts.push_back(run.end);
  }
  std::sort(cuts.begin(), cuts.end());
  cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
  // Resolve each atomic segment.
  for (size_t i = 0; i + 1 < cuts.size(); ++i) {
    int a = cuts[i];
    int b = cuts[i + 1];
    if (a >= b)
      continue;
    const Gdiplus::Font* font = impl.font->GetNative();
    Gdiplus::SolidBrush* brush = impl.brush.get();
    // Insertion order: later runs override earlier ones per-attribute.
    for (const AttributedTextRun& run : impl.runs) {
      if (run.start < b && run.end > a) {
        if (run.font)
          font = run.font->GetNative();
        if (run.brush)
          brush = run.brush.get();
      }
    }
    out->push_back(ResolvedSegment{a, b, font, brush});
  }
}

}  // namespace

AttributedTextImpl::AttributedTextImpl()
    // https://stackoverflow.com/questions/1203087
    : format(Gdiplus::StringFormat::GenericTypographic()) {}

AttributedTextImpl::~AttributedTextImpl() {}

AttributedText::AttributedText(const std::string& text, TextAttributes att)
    : AttributedText(base::UTF8ToWide(text), std::move(att)) {}

AttributedText::AttributedText(std::wstring text, TextAttributes att) {
  text_ = new AttributedTextImpl;
  text_->text = std::move(text);
  SetFormat(att.ToTextFormat());
  SetFont(att.font.get());
  SetColor(att.color);
}

AttributedText::~AttributedText() {
  delete text_;
}

void AttributedText::PlatformUpdateFormat() {
  text_->format.SetAlignment(ToGdi(format_.align));
  text_->format.SetLineAlignment(ToGdi(format_.valign));
  if (format_.ellipsis)
    text_->format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
  int flags = Gdiplus::StringFormatFlagsMeasureTrailingSpaces;
  if (!format_.wrap)
    flags |= Gdiplus::StringFormatFlagsNoWrap;
  text_->format.SetFormatFlags(flags);
}

void AttributedText::PlatformSetFontFor(scoped_refptr<Font> font,
                                        int start, int end) {
  if (start == 0 && end == -1) {
    // Whole-range assignment resets ranged fonts.
    text_->runs.erase(
        std::remove_if(text_->runs.begin(), text_->runs.end(),
                       [](const AttributedTextRun& r) { return r.font != nullptr; }),
        text_->runs.end());
    text_->font = std::move(font);
    return;
  }
  int total = static_cast<int>(text_->text.size());
  int rs = std::max(0, start);
  int re = end < 0 ? total : std::min(end, total);
  if (rs >= re)
    return;
  AttributedTextRun run;
  run.start = rs;
  run.end = re;
  run.font = std::move(font);
  text_->runs.push_back(std::move(run));
}

void AttributedText::PlatformSetColorFor(Color color, int start, int end) {
  if (start == 0 && end == -1) {
    // Whole-range assignment resets ranged colors.
    text_->runs.erase(
        std::remove_if(text_->runs.begin(), text_->runs.end(),
                       [](const AttributedTextRun& r) { return r.brush != nullptr; }),
        text_->runs.end());
    text_->brush.reset(new Gdiplus::SolidBrush(ToGdi(color)));
    return;
  }
  int total = static_cast<int>(text_->text.size());
  int rs = std::max(0, start);
  int re = end < 0 ? total : std::min(end, total);
  if (rs >= re)
    return;
  AttributedTextRun run;
  run.start = rs;
  run.end = re;
  run.brush.reset(new Gdiplus::SolidBrush(ToGdi(color)));
  text_->runs.push_back(std::move(run));
}

void LayoutAttributedText(const AttributedTextImpl& impl,
                          Gdiplus::Graphics& g,
                          const Gdiplus::RectF& box,
                          std::vector<GdiTextLine>* lines,
                          Gdiplus::SizeF* natural) {
  // Measuring format: typographic (no padding) + trailing spaces. Alignment
  // is applied as manual geometry below, never by GDI+.
  Gdiplus::StringFormat measure(Gdiplus::StringFormat::GenericTypographic());
  measure.SetFormatFlags(impl.format.GetFormatFlags() |
                         Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
  const bool wrap =
      (impl.format.GetFormatFlags() & Gdiplus::StringFormatFlagsNoWrap) == 0;
  // A huge (or non-finite) box means "no bounds": skip wrapping and manual
  // alignment offsets so measuring with FLT_MAX stays finite.
  const float box_w = box.Width;
  const bool bounded = std::isfinite(box_w) && box_w < 1e7f;

  const Gdiplus::Font* default_font = impl.font->GetNative();
  const float default_line_h = default_font->GetHeight(&g);

  // Split into logical lines at '\n' (a trailing '\n' yields an empty last
  // line, matching GDI+ MeasureString behavior after the "add a character"
  // compensation).
  std::vector<std::pair<int, int>> logical;
  {
    int begin = 0;
    int total = static_cast<int>(impl.text.size());
    for (int i = 0; i < total; ++i) {
      if (impl.text[i] == L'\n') {
        logical.emplace_back(begin, i);
        begin = i + 1;
      }
    }
    logical.emplace_back(begin, total);
  }

  lines->clear();
  float y = 0.f;
  float max_w = 0.f;
  for (size_t li = 0; li < logical.size(); ++li) {
    const int ls = logical[li].first;
    const int le = logical[li].second;
    // Resolve styled segments for this logical line.
    std::vector<ResolvedSegment> segments;
    ResolveSegments(impl, ls, le, &segments);
    if (segments.empty()) {
      // Empty line: keep the default line height.
      GdiTextLine line;
      line.x = 0.f;
      line.y = y;
      line.width = 0.f;
      line.height = default_line_h;
      lines->push_back(line);
      y += default_line_h;
      continue;
    }
    // Line height = tallest segment font on this logical line.
    float line_h = 0.f;
    for (const ResolvedSegment& seg : segments)
      line_h = std::max(line_h, seg.font->GetHeight(&g));
    // Stream segments into visual lines.
    std::vector<GdiTextSegment> out_segs;
    float x = 0.f;
    float line_w = 0.f;
    size_t i = 0;
    int cur = segments[0].start;  // start offset of the pending part
    while (i < segments.size()) {
      const ResolvedSegment seg = segments[i];
      const int seg_end = seg.end;
      const float w = MeasureSubString(impl.text, cur, seg_end, seg.font,
                                       measure, g);
      const bool fits = !bounded || !wrap || x + w <= box_w;
      if (fits || !wrap) {
        // Place the pending part whole (overflow is allowed when !wrap).
        GdiTextSegment part;
        part.font = seg.font;
        part.brush = seg.brush;
        part.x = x;
        part.start = cur;
        part.end = seg_end;
        out_segs.push_back(part);
        x += w;
        ++i;
        if (i < segments.size())
          cur = segments[i].start;
        continue;
      }
      if (x > 0.f) {
        // Segment does not fit after placed content: break the line here and
        // re-process the same pending part from a fresh line.
        GdiTextLine line;
        line.x = 0.f;
        line.y = y;
        line.width = line_w;
        line.height = line_h;
        line.segments = out_segs;
        lines->push_back(line);
        y += line_h;
        max_w = std::max(max_w, line_w);
        out_segs.clear();
        x = 0.f;
        line_w = 0.f;
        continue;
      }
      // At line start and still not fitting: take the longest fitting
      // prefix (binary search), at least one character to make progress.
      int lo = 1, hi = seg_end - cur, fit = 0;
      float fit_w = 0.f;
      while (lo <= hi) {
        int mid = (lo + hi) / 2;
        float mw = MeasureSubString(impl.text, cur, cur + mid, seg.font,
                                    measure, g);
        if (mw <= box_w) {
          fit = mid;
          fit_w = mw;
          lo = mid + 1;
        } else {
          hi = mid - 1;
        }
      }
      if (fit <= 0) {
        fit = 1;
        fit_w = MeasureSubString(impl.text, cur, cur + 1, seg.font, measure, g);
      }
      GdiTextSegment part;
      part.font = seg.font;
      part.brush = seg.brush;
      part.x = 0.f;
      part.start = cur;
      part.end = cur + fit;
      out_segs.push_back(part);
      GdiTextLine line;
      line.x = 0.f;
      line.y = y;
      line.width = fit_w;
      line.height = line_h;
      line.segments = out_segs;
      lines->push_back(line);
      y += line_h;
      max_w = std::max(max_w, fit_w);
      out_segs.clear();
      x = 0.f;
      line_w = 0.f;
      cur += fit;
      if (cur >= seg_end) {
        ++i;
        if (i < segments.size())
          cur = segments[i].start;
      }
    }
    // Flush the last visual line of this logical line.
    GdiTextLine line;
    line.x = 0.f;
    line.y = y;
    line.width = line_w;
    line.height = line_h;
    line.segments = out_segs;
    lines->push_back(line);
    y += line_h;
    max_w = std::max(max_w, line_w);
  }

  // Manual alignment: horizontal per line, vertical over the whole box.
  const Gdiplus::StringAlignment align = impl.format.GetAlignment();
  const Gdiplus::StringAlignment valign = impl.format.GetLineAlignment();
  const float natural_h = y;
  float off_y = 0.f;
  if (bounded) {
    if (valign == Gdiplus::StringAlignmentCenter)
      off_y = std::max(0.f, (box.Height - natural_h) / 2.f);
    else if (valign == Gdiplus::StringAlignmentFar)
      off_y = std::max(0.f, box.Height - natural_h);
  }
  for (auto& line : *lines) {
    float off_x = 0.f;
    if (bounded) {
      if (align == Gdiplus::StringAlignmentCenter)
        off_x = std::max(0.f, (box_w - line.width) / 2.f);
      else if (align == Gdiplus::StringAlignmentFar)
        off_x = std::max(0.f, box_w - line.width);
    }
    line.x = off_x;
    line.y += off_y;
  }
  *natural = Gdiplus::SizeF(max_w, natural_h);
}

RectF AttributedText::GetBoundsFor(const SizeF& size) const {
  base::win::ScopedGetDC dc(NULL);
  float scale_factor = GetScalingFactorFromDPI(::GetDeviceCaps(dc, LOGPIXELSX));

  Gdiplus::Graphics graphics(dc);
  // https://stackoverflow.com/questions/1203087/why-is-graphics-measurestring-returning-a-higher-than-expected-number
  graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
  Gdiplus::RectF bounds = ToGdi(RectF(ScaleSize(size, scale_factor)));

  if (!text_->runs.empty()) {
    // Ranged attributes: measure through the same layout used for drawing.
    std::vector<GdiTextLine> lines;
    Gdiplus::SizeF natural;
    LayoutAttributedText(*text_, graphics, bounds, &lines, &natural);
    return RectF(0.f, 0.f, natural.Width / scale_factor,
                 natural.Height / scale_factor);
  }

  // MeasureString does not take account of the last new line, add a character
  // to make it behave the same with other platforms.
  bool ends_with_newline = text_->text.size() > 0 &&
                           text_->text[text_->text.size() - 1] == L'\n';
  const std::wstring& text = ends_with_newline ? text_->text + L"a"
                                               : text_->text;
  Gdiplus::RectF rect;
  graphics.MeasureString(text.data(), static_cast<int>(text.size()),
                         text_->font->GetNative(), bounds,
                         &rect);
  return RectF(rect.X / scale_factor, rect.Y / scale_factor,
               rect.Width / scale_factor, rect.Height / scale_factor);
}

void AttributedText::SetText(const std::string& text) {
  text_->text = base::UTF8ToWide(text);
  // Clamp ranged attributes to the new text; drop fully out-of-range ones.
  int total = static_cast<int>(text_->text.size());
  text_->runs.erase(
      std::remove_if(text_->runs.begin(), text_->runs.end(),
                     [total](const AttributedTextRun& r) { return r.start >= total; }),
      text_->runs.end());
  for (auto& run : text_->runs) {
    if (run.end < 0 || run.end > total)
      run.end = total;
  }
}

std::string AttributedText::GetText() const {
  return base::WideToUTF8(text_->text);
}

}  // namespace nu
