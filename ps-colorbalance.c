/* GEGL Operation: Photoshop Color Balance 8bit ONLY
 * Algorithm: Reverse-engineered PS Color Balance (8bit sRGB gamma encoded)
 * Feature: Preserve Luminosity
 *
 * -- Mode Switch --
 * preserve_luminosity = FALSE : original PS colorbalance (independent channel)
 * preserve_luminosity = TRUE  : pure zero-sum logic
 *                               (normalize by median, then sign-aware
 *                                cross-channel redistribution)
 *
 * Your shadow/midtone/highlight curves are preserved.
 * Only the slider values are transformed before entering the kernel.
 *
 * Requirement: Input = Gamma encoded sRGB float 0~1. DO NOT use linear-rgb before this op.
 */
#include "config.h"
#include <glib/gi18n-lib.h>
#include <math.h>

#ifdef GEGL_PROPERTIES
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

property_boolean (preserve_luminosity, _("Preserve Luminosity"), FALSE)
    description (_("Pure zero-sum logic. Unchecked: original PS color balance."))

#else

#define GEGL_OP_FILTER
#define GEGL_OP_NAME     ps_colorbalance
#define GEGL_OP_C_SOURCE ps-colorbalance.c
#include "gegl-op.h"

#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

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

// ===== Highlight curve functions (unchanged) =====
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

// ===== Midtone curve functions (unchanged) =====
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

// ===== Shadow curve functions (unchanged) =====
static inline gfloat shadowRight(gdouble value, gfloat input8)
{
    return gammaCorrection255(input8, 1.0 - (0.003 * value));
}
static inline gfloat shadowLeft(gdouble value, gdouble value_param, gfloat input8)
{
    gfloat temp = 0.004f * fabs((gfloat)value_param);
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
        return shadowLeft(value, value, input8);
}

/**
 * colorbalance_8bit_kernel (unchanged)
 * 单通道原始 PS kernel：三区曲线 + 亮度权重插值
 */
static inline gfloat colorbalance_8bit_kernel(gfloat inFloat,
                                              gdouble shd,
                                              gdouble mid,
                                              gdouble hlt)
{
    gfloat in8 = inFloat * 255.0f;
    gfloat s = shadow(shd, in8);
    gfloat m = midtone(mid, in8);
    gfloat h = highlight(hlt, in8);

    gfloat lum = in8 / 255.0f;
    gfloat w_shd, w_mid, w_hlt;

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

/**
 * normalize_three_median
 * 三个值排序，减去中位数。
 * 三值相等时全部变成 0。
 */
static inline void normalize_three_median(gdouble *v)
{
    gdouble a = v[0], b = v[1], c = v[2];
    gdouble median;
    if ((a <= b && b <= c) || (c <= b && b <= a))
        median = b;
    else if ((b <= a && a <= c) || (c <= a && a <= b))
        median = a;
    else
        median = c;
    v[0] -= median;
    v[1] -= median;
    v[2] -= median;
}

/**
 * compute_effective_sliders
 * 纯零和逻辑：
 *   1. 每组三个滑块减去中位数归一化；
 *   2. 高光：alpha_j = max(h_j,0) + sum_{k≠j} max(-h_k,0)
 *      阴影：beta_j  = max(-s_j,0) + sum_{k≠j} max(s_k,0)
 *   3. 中间调只归一化，直接作为有效滑块。
 *
 * 三滑块相等 → 归一化后全 0 → 有效滑块全 0 → kernel 返回输入。
 */
static void
compute_effective_sliders(const gdouble *shd_in,
                          const gdouble *mid_in,
                          const gdouble *hlt_in,
                          gdouble *eff_shd,
                          gdouble *eff_mid,
                          gdouble *eff_hlt)
{
    gdouble s[3] = { shd_in[0], shd_in[1], shd_in[2] };
    gdouble m[3] = { mid_in[0], mid_in[1], mid_in[2] };
    gdouble h[3] = { hlt_in[0], hlt_in[1], hlt_in[2] };

    normalize_three_median(s);
    normalize_three_median(m);
    normalize_three_median(h);

    for (int j = 0; j < 3; j++)
    {
        // ---- 阴影 ----
        // beta_j 是压暗幅度 → C 滑块取负
        gdouble beta_j = MAX(-s[j], 0.0);
        for (int k = 0; k < 3; k++)
            if (k != j)
                beta_j += MAX(s[k], 0.0);
        eff_shd[j] = -beta_j;

        // ---- 高光 ----
        // alpha_j 是提亮幅度 → C 滑块取正
        gdouble alpha_j = MAX(h[j], 0.0);
        for (int k = 0; k < 3; k++)
            if (k != j)
                alpha_j += MAX(-h[k], 0.0);
        eff_hlt[j] = alpha_j;

        // ---- 中间调 ----
        // 纯零和：只归一化，不引入任何算法系数
        eff_mid[j] = m[j];
    }

    // 统一 clamp 到 UI 范围
    for (int j = 0; j < 3; j++)
    {
        eff_shd[j] = CLAMP(eff_shd[j], -100.0, 100.0);
        eff_mid[j] = CLAMP(eff_mid[j], -100.0, 100.0);
        eff_hlt[j] = CLAMP(eff_hlt[j], -100.0, 100.0);
    }
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

  // 预计算有效滑块（只在 preserve_lum 时用）
  gdouble eff_shd[3] = { 0, 0, 0 };
  gdouble eff_mid[3] = { 0, 0, 0 };
  gdouble eff_hlt[3] = { 0, 0, 0 };

  if (preserve_lum)
  {
      gdouble shd_in[3] = { shd_cr, shd_mg, shd_yb };
      gdouble mid_in[3] = { mid_cr, mid_mg, mid_yb };
      gdouble hlt_in[3] = { hlt_cr, hlt_mg, hlt_yb };
      compute_effective_sliders(shd_in, mid_in, hlt_in, eff_shd, eff_mid, eff_hlt);
  }

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
      gfloat a    = in_line[px + 3];

      gfloat r_out, g_out, b_out;

      if (preserve_lum)
      {
          // 用有效滑块调用你原有的 kernel
          r_out = colorbalance_8bit_kernel(r_in, eff_shd[0], eff_mid[0], eff_hlt[0]);
          g_out = colorbalance_8bit_kernel(g_in, eff_shd[1], eff_mid[1], eff_hlt[1]);
          b_out = colorbalance_8bit_kernel(b_in, eff_shd[2], eff_mid[2], eff_hlt[2]);
      }
      else
      {
          // 原始 PS 行为：独立通道 kernel，用原始滑块
          r_out = colorbalance_8bit_kernel(r_in, shd_cr, mid_cr, hlt_cr);
          g_out = colorbalance_8bit_kernel(g_in, shd_mg, mid_mg, hlt_mg);
          b_out = colorbalance_8bit_kernel(b_in, shd_yb, mid_yb, hlt_yb);
      }

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
    "title",       _("PS Color Balance"),
    "description", _("PS Color Balance 8bit sRGB. "),
    "gimp:menu-path", "<Image>/Colors/myfilters",
    "gimp:menu-label", _("PS Color Balance 8bit..."),
    NULL);
}

#endif