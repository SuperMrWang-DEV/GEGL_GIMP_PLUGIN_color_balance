/* GEGL Operation: Photoshop Color Balance 8bit ONLY
 * Algorithm: Reverse-engineered PS Color Balance (8bit sRGB gamma encoded)
 * Feature: Preserve Luminosity 
 *
 * -- Mode Switch --
 * preserve_luminosity = FALSE : original PS colorbalance 
 * preserve_luminosity = TRUE  : zero-sum (RGB-domain, NO HSL conversion)
 *
 * zero-sum rule for each color axis:
 *   CR (Cyan-Red) : modify R, G+B get total reverse offset equal to R delta
 *   MG (Magenta-Green) : modify G, R+B get total reverse offset equal to G delta
 *   YB (Yellow-Blue) : modify B, R+G get total reverse offset equal to B delta
 * Requirement: Input = Gamma encoded sRGB float 0~1. DO NOT use linear-rgb before this op.
 */
#include "config.h"
#include <glib/gi18n-lib.h>
#include <math.h>

#ifdef GEGL_PROPERTIES
// Shadows: Cyan-Red / Magenta-Green / Yellow-Blue, range -100 ~ +100
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

// Preserve Luminosity toggle
property_boolean (preserve_luminosity, _("Preserve Luminosity"), FALSE)
    description (_("Zero-sum mode. Unchecked: original PS color balance."))

#else

#define GEGL_OP_FILTER
#define GEGL_OP_NAME     ps_colorbalance
#define GEGL_OP_C_SOURCE ps-colorbalance.c
#include "gegl-op.h"

#define FLOAT_EPS       1e-12f

/**
 * sanitize_f: cleanup invalid float (inf/nan)
 */
static inline gfloat sanitize_f(gfloat v)
{
  if (!isfinite(v))
    return 0.0f;
  return v;
}

/**
 * trimResult: clamp 0~255 for 8bit space
 */
static inline gfloat trimResult(gfloat value)
{
    if (value < 0.0f) return 0.0f;
    if (value > 255.0f) return 255.0f;
    return value;
}

/**
 * gammaCorrection255: gamma pow curve on 0~255 input
 */
static inline gfloat gammaCorrection255(gfloat input8, gdouble gamma)
{
    input8 = CLAMP(input8, 0.0f, 255.0f);
    gfloat normalized = input8 / 255.0f;
    gfloat corrected = powf(normalized, (gfloat)gamma);
    gint outi = (gint)(corrected * 255.0f + 0.5f);
    if (outi > 255) outi = 255;
    return (gfloat)outi;
}

// ===== Highlight curve functions (original code unchanged) =====
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

// ===== Midtone curve functions (original code unchanged) =====
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

// ===== Shadow curve functions (original code unchanged) =====
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
 * colorbalance_8bit_kernel
 * Original raw kernel: apply single slider to ONE channel, return raw modified channel
 * This is your original PS curve + luminance weighted blend (shadow/mid/highlight)
 * @param inFloat: input channel 0~1
 * @param shd: shadow slider value
 * @param mid: midtone slider value
 * @param hlt: highlight slider value
 * @return modified single channel 0~1 (raw, no luminosity compensation)
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

    // luminance weight for shadow / midtone / highlight blend (your original weight)
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
 * apply_zero_sum
 *  complementary zero-sum compensation per color axis
 * For CR/MG/YB deltas: apply reverse compensation to other two channels
 * dr, dg, db: raw delta from original PS kernel
 * returns compensated dr,dg,db, delta sum dr+dg+db = 0
 */
static inline void apply_zero_sum(gfloat dr, gfloat dg, gfloat db,
                                         gfloat *dr_out, gfloat *dg_out, gfloat *db_out)
{
    //  rule: each color axis's delta must be balanced by other two channels
    // CR axis (Red): dr is primary change. G and B share total offset = dr
    // MG axis (Green): dg is primary change. R and B share total offset = dg
    // YB axis (Blue): db is primary change. R and G share total offset = db

    gfloat comp_gb = dr;   // CR: G+B together subtract dr
    gfloat comp_rb = dg;   // MG: R+B together subtract dg
    gfloat comp_rg = db;   // YB: R+G together subtract db

    // distribute compensation equally between the two complementary channels
    gfloat r_comp = (-comp_rb - comp_rg) / 2.0f;
    gfloat g_comp = (-comp_gb - comp_rg) / 2.0f;
    gfloat b_comp = (-comp_gb - comp_rb) / 2.0f;

    *dr_out = dr + r_comp;
    *dg_out = dg + g_comp;
    *db_out = db + b_comp;
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

  // read all slider parameters
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

      // Step 1: run your original raw PS kernel for each RGB channel
      gfloat r_raw = colorbalance_8bit_kernel(r_in, shd_cr, mid_cr, hlt_cr);
      gfloat g_raw = colorbalance_8bit_kernel(g_in, shd_mg, mid_mg, hlt_mg);
      gfloat b_raw = colorbalance_8bit_kernel(b_in, shd_yb, mid_yb, hlt_yb);

      gfloat r_out, g_out, b_out;
      if (preserve_lum)
      {
          //  complementary zero-sum mode
          gfloat dr_raw = r_raw - r_in;
          gfloat dg_raw = g_raw - g_in;
          gfloat db_raw = b_raw - b_in;

          gfloat dr_comp, dg_comp, db_comp;
          apply_zero_sum(dr_raw, dg_raw, db_raw, &dr_comp, &dg_comp, &db_comp);

          r_out = r_in + dr_comp;
          g_out = g_in + dg_comp;
          b_out = b_in + db_comp;
      }
      else
      {
          // NO preserve luminosity: original PS behavior, directly use raw result
          r_out = r_raw;
          g_out = g_raw;
          b_out = b_raw;
      }

      // final clamp to valid sRGB float range 0~1
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