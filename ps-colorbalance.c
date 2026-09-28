/* GEGL Operation: Photoshop Color Balance 8bit ONLY
 * Code structure fully mimics hslseven.c buffer row processing style
 * Algorithm strictly follows reverse-engineered PS Color Balance (Preserve Luminosity UNCHECKED)
 * NEW: Added Preserve Luminosity option, implemented by RGB delta zero-sum
 * REQUIREMENT: Input = Gamma encoded sRGB float (DO NOT use gegl:linear-rgb before this op)
 */
#include "config.h"
#include <glib/gi18n-lib.h>
#include <math.h>

#ifdef GEGL_PROPERTIES
// Shadows: Cyan-Red / Magenta-Green / Yellow-Blue, -100 ~ +100
property_double (shadow_cr, _("Shadow Cyan-Red"), 0.0)
    description (_("Shadow Cyan/Red slider (-100 Cyan ~ +100 Red)"))
    value_range (-100.0, 100.0)
    ui_range (-100.0, 100.0)

property_double (shadow_mg, _("Shadow Magenta-Green"), 0.0)
    description (_("Shadow Magenta/Green slider (-100 Magenta ~ +100 Green)"))
    value_range (-100.0, 100.0)
    ui_range (-100.0, 100.0)

property_double (shadow_yb, _("Shadow Yellow-Blue"), 0.0)
    description (_("Shadow Yellow/Blue slider (-100 Yellow ~ +100 Blue)"))
    value_range (-100.0, 100.0)
    ui_range (-100.0, 100.0)

// Midtones
property_double (midtone_cr, _("Midtone Cyan-Red"), 0.0)
    description (_("Midtone Cyan/Red slider (-100 Cyan ~ +100 Red)"))
    value_range (-100.0, 100.0)
    ui_range (-100.0, 100.0)

property_double (midtone_mg, _("Midtone Magenta-Green"), 0.0)
    description (_("Midtone Magenta/Green slider (-100 Magenta ~ +100 Green)"))
    value_range (-100.0, 100.0)
    ui_range (-100.0, 100.0)

property_double (midtone_yb, _("Midtone Yellow-Blue"), 0.0)
    description (_("Midtone Yellow/Blue slider (-100 Yellow ~ +100 Blue)"))
    value_range (-100.0, 100.0)
    ui_range (-100.0, 100.0)

// Highlights
property_double (highlight_cr, _("Highlight Cyan-Red"), 0.0)
    description (_("Highlight Cyan/Red slider (-100 Cyan ~ +100 Red)"))
    value_range (-100.0, 100.0)
    ui_range (-100.0, 100.0)

property_double (highlight_mg, _("Highlight Magenta-Green"), 0.0)
    description (_("Highlight Magenta/Green slider (-100 Magenta ~ +100 Green)"))
    value_range (-100.0, 100.0)
    ui_range (-100.0, 100.0)

property_double (highlight_yb, _("Highlight Yellow-Blue"), 0.0)
    description (_("Highlight Yellow/Blue slider (-100 Yellow ~ +100 Blue)"))
    value_range (-100.0, 100.0)
    ui_range (-100.0, 100.0)

// ========= NEW: Preserve Luminosity ( zero-sum delta method) =========
property_boolean (preserve_luminosity, _("Preserve Luminosity"), FALSE)
    description (_("Preserve luminosity RGB delta zero-sum compensation. Not perceptual L*."))

#else

#define GEGL_OP_FILTER
#define GEGL_OP_NAME     ps_colorbalance
#define GEGL_OP_C_SOURCE ps-colorbalance.c
#include "gegl-op.h"

#define FLOAT_EPS       1e-12f

static inline gfloat sanitize_f(gfloat v)
{
  if (!isfinite(v))
    return 0.0f;
  return v;
}

static inline gfloat trimResult(gfloat value)
{
    if (value < 0.0f) return 0.0f;
    if (value > 255.0f) return 255.0f;
    return value;
}

static inline gfloat gammaCorrection255(gfloat input8, gdouble gamma)
{
    input8 = CLAMP(input8, 0.0f, 255.0f);
    gfloat normalized = input8 / 255.0f;
    gfloat corrected = powf(normalized, (gfloat)gamma);
    gint outi = (gint)(corrected * 255.0f + 0.5f);
    if (outi > 255) outi = 255;
    return (gfloat)outi;
}

// ===== Highlight =====
static inline gfloat highlightRight(gdouble value, gfloat input8)
{
    gfloat result = (1.0f / (1.0f - 0.004f * (gfloat)value)) * input8;
    return trimResult(result);
}
static inline gfloat highlightLeft(gdouble value, gfloat input8)
{
    return gammaCorrection255(input8, 1.0 - (0.004 * value));
}
static inline gfloat highlight(gdouble value, gfloat input8)
{
    if (value >= 0)
        return highlightRight(value, input8);
    else
        return highlightLeft(value, input8);
}

// ===== Midtone =====
static inline gfloat midtoneRight(gdouble value, gfloat input8)
{
    return gammaCorrection255(input8, 1.0 - (0.005 * value));
}
static inline gfloat midtoneLeft(gdouble value, gfloat input8)
{
    return gammaCorrection255(input8, 1.0 - (0.01 * value));
}
static inline gfloat midtone(gdouble value, gfloat input8)
{
    if (value >=0)
        return midtoneRight(value, input8);
    else
        return midtoneLeft(value, input8);
}

// ===== Shadow =====
static inline gfloat shadowRight(gdouble value, gfloat input8)
{
    return gammaCorrection255(input8, 1.0 - (0.003 * value));
}
static inline gfloat shadowLeft(gdouble value, gfloat input8)
{
    gfloat temp = 0.004f * fabs((gfloat)value);
    gfloat temp1 = (input8 / 255.0f) - temp;
    gfloat temp2 = 1.0f - temp;
    gfloat result = (temp1 / temp2) * 255.0f;
    return trimResult(result);
}
static inline gfloat shadow(gdouble value, gfloat input8)
{
    if (value >=0)
        return shadowRight(value, input8);
    else
        return shadowLeft(value, input8);
}

// Core per-channel kernel
// chVal: PS滑块值(-100~100), inFloat: 0~1 sRGB float
static inline gfloat colorbalance_8bit_kernel(gfloat inFloat,
                                              gdouble shd,
                                              gdouble mid,
                                              gdouble hlt)
{
    gfloat in8 = inFloat * 255.0f;
    gfloat s = shadow(shd, in8);
    gfloat m = midtone(mid, in8);
    gfloat h = highlight(hlt, in8);

    // PS色彩平衡：阴影/中间调/高光三个曲线混合
    // 加权混合逻辑：像素亮度决定权重，低亮度取shadow，中间取midtone，高亮度取highlight
    gfloat lum = in8 / 255.0f;
    gfloat w_shd, w_mid, w_hlt;

    // 简单权重 
    if (lum < 0.33f) {
        w_shd = 1.0f - lum / 0.33f;
        w_mid = lum / 0.33f;
        w_hlt = 0;
    } else if (lum < 0.66f) {
        w_shd = 0;
        w_mid = 1.0f - (lum - 0.33f)/0.33f;
        w_hlt = (lum -0.33f)/0.33f;
    } else {
        w_shd = 0;
        w_mid = 0;
        w_hlt = 1.0f;
    }
    gfloat out8 = s * w_shd + m * w_mid + h * w_hlt;
    out8 = trimResult(out8);
    return out8 / 255.0f;
}

static void
prepare(GeglOperation *op)
{
  gegl_operation_set_format(op, "input",  babl_format("RGBA float"));
  gegl_operation_set_format(op, "output", babl_format("RGBA float"));
}

static gboolean
process(GeglOperation       *op,
        GeglBuffer          *in_buf,
        GeglBuffer          *out_buf,
        const GeglRectangle *roi,
        gint                 level)
{
  if (!roi || roi->width <= 0 || roi->height <= 0)
    return TRUE;

  gdouble shd_cr, shd_mg, shd_yb;
  gdouble mid_cr, mid_mg, mid_yb;
  gdouble hlt_cr, hlt_mg, hlt_yb;
  gboolean preserve_lum;

  g_object_get(G_OBJECT(op),
    "shadow-cr", &shd_cr,
    "shadow-mg", &shd_mg,
    "shadow-yb", &shd_yb,
    "midtone-cr", &mid_cr,
    "midtone-mg", &mid_mg,
    "midtone-yb", &mid_yb,
    "highlight-cr", &hlt_cr,
    "highlight-mg", &hlt_mg,
    "highlight-yb", &hlt_yb,
    "preserve-luminosity", &preserve_lum,
    NULL);

  gint stride = roi->width * 4;
  gfloat *in_line  = g_new(gfloat, stride);
  gfloat *out_line = g_new(gfloat, stride);

  for (gint y = 0; y < roi->height; y++)
  {
    GeglRectangle row_rect = { roi->x, roi->y + y, roi->width, 1 };

    gegl_buffer_get(in_buf, &row_rect, 1.0f, babl_format("RGBA float"),
                     in_line, GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_NONE);

    for (gint x = 0; x < roi->width; x++)
    {
      gint px = x * 4;
      gfloat r_in = in_line[px + 0];
      gfloat g_in = in_line[px + 1];
      gfloat b_in = in_line[px + 2];
      gfloat a = in_line[px + 3];

      // 原有PS色彩平衡计算
      gfloat r_raw = colorbalance_8bit_kernel(r_in, shd_cr, mid_cr, hlt_cr);
      gfloat g_raw = colorbalance_8bit_kernel(g_in, shd_mg, mid_mg, hlt_mg);
      gfloat b_raw = colorbalance_8bit_kernel(b_in, shd_yb, mid_yb, hlt_yb);

      gfloat r_out, g_out, b_out;
      if (preserve_lum)
      {
          //计算调色带来的增量，让总增量之和=0
          gfloat dr = r_raw - r_in;
          gfloat dg = g_raw - g_in;
          gfloat db = b_raw - b_in;
          gfloat sum_d = dr + dg + db;
          gfloat offset = sum_d / 3.0f;

          dr -= offset;
          dg -= offset;
          db -= offset;

          r_out = r_in + dr;
          g_out = g_in + dg;
          b_out = b_in + db;
      }
      else
      {
          // 不勾选保留明度，原生PS行为不变
          r_out = r_raw;
          g_out = g_raw;
          b_out = b_raw;
      }

      // Clamp 0~1
      r_out = CLAMP(r_out, 0.0f, 1.0f);
      g_out = CLAMP(g_out, 0.0f, 1.0f);
      b_out = CLAMP(b_out, 0.0f, 1.0f);

      out_line[px + 0] = r_out;
      out_line[px + 1] = g_out;
      out_line[px + 2] = b_out;
      out_line[px + 3] = sanitize_f(a);
    }

    gegl_buffer_set(out_buf, &row_rect, 0, babl_format("RGBA float"),
                     out_line, GEGL_AUTO_ROWSTRIDE);
  }

  g_free(in_line);
  g_free(out_line);
  return TRUE;
}

static void
gegl_op_class_init(GeglOpClass *klass)
{
  GeglOperationClass       *oclass = GEGL_OPERATION_CLASS(klass);
  GeglOperationFilterClass *fclass = GEGL_OPERATION_FILTER_CLASS(klass);

  oclass->prepare = prepare;
  fclass->process = process;

  gegl_operation_class_set_keys(oclass,
    "name",        "lb:ps-colorbalance",
    "title",       _("Color Balance"),
    "description", _("Replicate Photoshop Color Balance Adjustment Layer. Preserve Luminosity:RGB delta zero-sum (RGB sum preserved, not perceptual L*). 8bit sRGB algorithm."),
    "gimp:menu-path", "<Image>/Colors/myfilters",
    "gimp:menu-label", _("PS Color Balance 8bit..."),
    NULL);
}

#endif