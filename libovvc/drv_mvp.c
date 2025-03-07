#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ovdefs.h"
#include "ovutils.h"

/* FIXME find a place for mv structures */
#include "ctudec.h"
#include "drv_utils.h"
#include "dec_structures.h"
#include "ovdpb.h"

#define POS_MASK(x, w) ((uint64_t) 1 << ((((x + 1)) + ((w)))))

#define TMVP_POS_MASK(y) ((uint64_t) 1 << ((y) + 1))

#define OFFSET_BUFF(x,y) (35 + x + (y) * 34)
#define MV_CMP(mv0,mv1) ((mv0.x == mv1.x) && (mv0.y == mv1.y))
#define MV_CMP2(mv0,mv1) ((mv0.x == mv1.x) && (mv0.y == mv1.y) && (mv0.ref_idx == mv1.ref_idx))

#define PB_POS_IN_BUF(x,y) (35 + (x) + ((y) * 34))
#define TMVP_BUFF_STRIDE 17
#define TMVP_POS_IN_BUF(x,y) ((x >> 1) + (((y >> 1)) * TMVP_BUFF_STRIDE))

#define MV_MANTISSA_BITCOUNT 6
#define MV_MANTISSA_UPPER_LIMIT ((1 << (MV_MANTISSA_BITCOUNT - 1)) - 1)
#define MV_MANTISSA_LIMIT (1 << (MV_MANTISSA_BITCOUNT - 1))

#define MV_BITS  18
#define MV_MAX   ((1 << (MV_BITS - 1)) - 1)
#define MV_MIN  (-(1 << (MV_BITS - 1)))

#define LOG2_MIN_CU_S 2

OVMV
drv_change_precision_mv(OVMV mv, int src, int dst)
{
    int shift = dst - src;
    if (shift >= 0)
    {
        mv.x <<= shift;
        mv.y <<= shift;
    }
    else
    {
        shift *= -1;
        const int offset = 1 << (shift - 1);
        mv.x = mv.x >= 0 ? (mv.x + offset - 1) >> shift : (mv.x + offset) >> shift;
        mv.y = mv.y >= 0 ? (mv.y + offset - 1) >> shift : (mv.y + offset) >> shift;
    }
    return mv;
}

OVMV
drv_round_to_precision_mv(OVMV mv, int src, int dst)
{
    OVMV mv1 = drv_change_precision_mv(mv, src, dst);
    return drv_change_precision_mv(mv1, dst, src);
}

static inline uint8_t
mi_cmp(const VVCMergeInfo *const a, const VVCMergeInfo *const b)
{
    if (a->inter_dir == b->inter_dir) {
        uint8_t is_neq = 0;
        if (a->inter_dir & 0x1) {
            is_neq += !MV_CMP(a->mv0, b->mv0);
            is_neq += a->mv0.ref_idx != b->mv0.ref_idx;
        }
        if (a->inter_dir & 0x2) {
            is_neq += !MV_CMP(a->mv1, b->mv1);
            is_neq += a->mv1.ref_idx != b->mv1.ref_idx;
        }

        return !is_neq;
    }
    return 0;
}

/* Compress and uncompress Motion vectors used by TMVP
 * FIXME there might be a more straight forward way of
 * doing this
 */
static int
tmvp_round_mv(int32_t val)
{
  int sign  = val >> 31;

  if ((val ^ sign) - !!sign > 31) {
      int scale = floor_log2((val ^ sign) | MV_MANTISSA_UPPER_LIMIT) - (MV_MANTISSA_BITCOUNT - 1);
      int round = (1 << scale) >> 1;
      int n     = (val + round) >> scale;
      int exponent  = scale + ((n ^ sign) >> (MV_MANTISSA_BITCOUNT - 1));
      int mantissa  = (n & MV_MANTISSA_UPPER_LIMIT) | (sign << (MV_MANTISSA_BITCOUNT - 1));
      return (mantissa ^ MV_MANTISSA_LIMIT) << (exponent - !!exponent);
  } else {
      return val;
  }
}

static void
hmvp_add_cand_1(const struct HMVPLUT *const hmvp_lut,
                OVMV *const cand_list,
                int *const nb_cand, uint8_t inter_dir, uint8_t ref_idx, uint8_t opp_ref_idx)
{
    int max_nb_cand = OVMIN(4, hmvp_lut->nb_mv);
    int i;
    for (i = 1; i <= max_nb_cand && *nb_cand < 2;  ++i) {
        if(hmvp_lut->dir[i - 1] & inter_dir) {
            OVMV hmvp_cand = inter_dir & 0x1 ? hmvp_lut->hmv0[i - 1]
                                             : hmvp_lut->hmv1[i - 1];
            if (hmvp_cand.ref_idx == ref_idx) {
                cand_list[(*nb_cand)++] = hmvp_cand;
            }
        }

        if (*nb_cand == 2) {
            return;
        }

        if (hmvp_lut->dir[i - 1] & (3 - inter_dir)) {
            OVMV hmvp_cand = (3 - inter_dir) & 0x1 ? hmvp_lut->hmv0[i - 1]
                                                   : hmvp_lut->hmv1[i - 1];
            if (hmvp_cand.ref_idx == opp_ref_idx) {
                cand_list[(*nb_cand)++] = hmvp_cand;
            }
        }
    }
}

static uint8_t
hmvp_add_merge_cand(const struct HMVPLUT *const hmvp_lut,
                    OVMV *const cand_list,
                    OVMV *const cand_amvp,
                    uint8_t got_B1, uint8_t got_A1,
                    int *const nb_cand, uint8_t inter_dir, uint8_t mvp_idx,
                    uint8_t max_nb_mrg_cand_min1)
{
    int i;
    for (i = 1; i <= hmvp_lut->nb_mv;  ++i) {
        int mv_lut_idx = hmvp_lut->nb_mv - i;
        if (hmvp_lut->dir[mv_lut_idx] & inter_dir) {
            if (i > 2 || ((!got_B1 || !MV_CMP2(hmvp_lut->hmv0[mv_lut_idx], cand_amvp[0])) &&
                (!got_A1 || !MV_CMP2(hmvp_lut->hmv0[mv_lut_idx], cand_amvp[1])))) {
                cand_list[(*nb_cand)++] = inter_dir & 0x1 ? hmvp_lut->hmv0[mv_lut_idx]
                                                      : hmvp_lut->hmv1[mv_lut_idx];
                if (*nb_cand == mvp_idx + 1) {
                    return 1;
                }

                if (*nb_cand == max_nb_mrg_cand_min1) {
                   return 0;
                }
            }
        }
    }
    return 0;
}

static uint8_t
hmvp_add_merge_cand_b(const struct HMVPLUT *const hmvp_lut,
                      VVCMergeInfo *const cand_list,
                      VVCMergeInfo *const cand_amvp, uint8_t got_B1, uint8_t got_A1,
                      int *const nb_cand, uint8_t mvp_idx, uint8_t max_nb_mrg_min1)
{
    int i;
    for (i = 1; i <= hmvp_lut->nb_mv;  ++i) {
        int mv_lut_idx = hmvp_lut->nb_mv - i;
        VVCMergeInfo lut_mi;
        lut_mi.inter_dir = hmvp_lut->dir[mv_lut_idx];
        lut_mi.mv0 = hmvp_lut->hmv0[mv_lut_idx];
        lut_mi.mv1 = hmvp_lut->hmv1[mv_lut_idx];
        if (i > 2 || ((!got_B1 || !mi_cmp(&lut_mi, &cand_amvp[0])) &&
                      (!got_A1 || !mi_cmp(&lut_mi, &cand_amvp[1])))) {

            cand_list[(*nb_cand)++] = lut_mi;

            if (*nb_cand == mvp_idx + 1) {
                return 1;
            }

            if (*nb_cand == max_nb_mrg_min1) {
                return 0;
            }
        }
    }
    return 0;
}


static void
hmvp_update_lut(struct HMVPLUT *const hmvp_lut, OVMV mv)
{
    int max_nb_cand = OVMIN(5, hmvp_lut->nb_mv);
    int duplicated_mv = 0;
    int i;

    for (i = 0; i < max_nb_cand; ++i) {
        if (MV_CMP2(mv, hmvp_lut->hmv0[i])) {
            duplicated_mv = 1;
            break;
        }
    }

    if (duplicated_mv) {
        int j;

        for (j = i; j < max_nb_cand - 1; ++j) {
            hmvp_lut->hmv0[j] = hmvp_lut->hmv0[j + 1];
        }

        hmvp_lut->hmv0[j] = mv;

    } else if (hmvp_lut->nb_mv == 5) {
        int j;

        for (j = 1; j < 5; ++j) {
            hmvp_lut->hmv0[j - 1] = hmvp_lut->hmv0[j];
        }

        hmvp_lut->hmv0[4] = mv;

    } else {
        hmvp_lut->hmv0[hmvp_lut->nb_mv++] = mv;
    }

    hmvp_lut->dir[0]= 0x1;
    hmvp_lut->dir[1]= 0x1;
    hmvp_lut->dir[2]= 0x1;
    hmvp_lut->dir[3]= 0x1;
    hmvp_lut->dir[4]= 0x1;
}

static void
hmvp_update_lut_b(struct HMVPLUT *const hmvp_lut, OVMV mv0, OVMV mv1, uint8_t inter_dir)
{
    int max_nb_cand = OVMIN(5, hmvp_lut->nb_mv);
    uint8_t duplicated_mv = 0;
    int i;
    uint8_t *const dir_lut = hmvp_lut->dir;

    for (i = 0; i < max_nb_cand;  ++i) {
        if (dir_lut[i] == inter_dir) {
            switch (inter_dir) {
                case 0x1:
                    duplicated_mv = MV_CMP(mv0, hmvp_lut->hmv0[i]) && mv0.ref_idx == hmvp_lut->hmv0[i].ref_idx;
                    break;
                case 0x2:
                    duplicated_mv = MV_CMP(mv1, hmvp_lut->hmv1[i]) && mv1.ref_idx == hmvp_lut->hmv1[i].ref_idx;
                    break;
                case 0x3:
                    duplicated_mv = MV_CMP(mv0, hmvp_lut->hmv0[i]) &&
                        MV_CMP(mv1, hmvp_lut->hmv1[i]) && mv0.ref_idx == hmvp_lut->hmv0[i].ref_idx && mv1.ref_idx == hmvp_lut->hmv1[i].ref_idx;
                    break;
            }
            if (duplicated_mv) {
                break;
            }
        } else duplicated_mv = 0;
    }

    if (duplicated_mv) {
       int j;
       for (j = i; j < max_nb_cand - 1; ++j) {
           hmvp_lut->hmv0[j] = hmvp_lut->hmv0[j + 1];
           hmvp_lut->hmv1[j] = hmvp_lut->hmv1[j + 1];
           hmvp_lut->dir[j] = hmvp_lut->dir[j + 1];
       }
       hmvp_lut->hmv0[j] = mv0;
       hmvp_lut->hmv1[j] = mv1;
       hmvp_lut->dir[j]= inter_dir;
    } else if (hmvp_lut->nb_mv == 5) {
       int j;
       for (j = 1; j < 5; ++j) {
           hmvp_lut->hmv0[j - 1] = hmvp_lut->hmv0[j];
           hmvp_lut->hmv1[j - 1] = hmvp_lut->hmv1[j];
           hmvp_lut->dir[j - 1] = hmvp_lut->dir[j];
       }
       hmvp_lut->hmv0[4] = mv0;
       hmvp_lut->hmv1[4] = mv1;
       hmvp_lut->dir[4] = inter_dir;
    } else {
        hmvp_lut->hmv0[hmvp_lut->nb_mv] = mv0;
        hmvp_lut->hmv1[hmvp_lut->nb_mv] = mv1;
        hmvp_lut->dir[hmvp_lut->nb_mv++] = inter_dir;
    }
}

void
tmvp_inter_synchronization(OVPicture *ref_pic, int ctb_x, int ctb_y, int log2_ctu_s)
{
    const int pic_w = ref_pic->frame->width[0];
    const int pic_h = ref_pic->frame->height[0];
    
    /*Frame thread synchronization to ensure data is available
    */
    int nb_ctb_pic_w = (pic_w + ((1 << log2_ctu_s) - 1)) >> log2_ctu_s;
    int nb_ctb_pic_h = (pic_h + ((1 << log2_ctu_s) - 1)) >> log2_ctu_s;
    int br_ctu_x = OVMIN(ctb_x + 1, nb_ctb_pic_w-1);
    int br_ctu_y = OVMIN(ctb_y + 1, nb_ctb_pic_h-1);
    // ovdpb_synchro_ref_decoded_ctus(ref_pic, ctb_x, ctb_y, br_ctu_x, br_ctu_y);
    ref_pic->ovdpb_frame_synchro(ref_pic, ctb_x, ctb_y, br_ctu_x, br_ctu_y);
}


static void
load_ctb_tmvp(OVCTUDec *const ctudec, int ctb_x, int ctb_y)
{

    struct InterDRVCtx *const inter_ctx = &ctudec->drv_ctx.inter_ctx;
    struct VVCTMVP *const tmvp_ctx = &inter_ctx->tmvp_ctx;

    const struct MVPlane *plane0 = tmvp_ctx->col_plane0;
    const struct MVPlane *plane1 = tmvp_ctx->col_plane1;

    uint8_t log2_ctb_s = ctudec->part_ctx->log2_ctu_s;

    uint8_t nb_unit_ctb = (1 << log2_ctb_s) >> LOG2_MIN_CU_S;
    uint16_t nb_ctb_w = ctudec->nb_ctb_pic_w;

    const OVPicture* ref_pic = tmvp_ctx->col_ref;
    tmvp_inter_synchronization(ref_pic, ctb_x, ctb_y, log2_ctb_s);

    uint8_t is_border_pic = nb_ctb_w - 1 == ctb_x;

    if (plane0 || plane1) {
        uint16_t ctb_addr_rs = ctb_x + ctb_y * nb_ctb_w;
        int32_t nb_tmvp_unit = (1 << log2_ctb_s) >> 3;
        int32_t pln_stride = nb_tmvp_unit * nb_ctb_w;
        int32_t ctb_offset = ctb_x * nb_tmvp_unit + (ctb_y * nb_tmvp_unit * pln_stride);

        if (is_border_pic) {
            memset(tmvp_ctx->dir_map_v0, 0, sizeof(uint64_t) * (nb_unit_ctb + 2));
            memset(tmvp_ctx->dir_map_v1, 0, sizeof(uint64_t) * (nb_unit_ctb + 2));
        }

        if (plane0 && plane0->dirs) {
            const uint64_t *src_map = plane0->dirs + ctb_addr_rs * nb_unit_ctb;
                  uint64_t *dst_map = tmvp_ctx->dir_map_v0 + 1;
            const OVMV *src_mvs = plane0->mvs + ctb_offset;
                  OVMV *dst_mvs = tmvp_ctx->mvs0;
            int i;

            memcpy(dst_map, src_map, sizeof(uint64_t) * (nb_unit_ctb + !is_border_pic));
            for (i = 0; i < nb_tmvp_unit; ++i) {
                memcpy(dst_mvs, src_mvs, sizeof(*dst_mvs) * (nb_tmvp_unit + !is_border_pic));
                dst_mvs += TMVP_BUFF_STRIDE;
                src_mvs += pln_stride;
            }
        }

        if (plane1 && plane1->dirs) {
            const uint64_t *src_map = plane1->dirs + ctb_addr_rs * nb_unit_ctb;
                  uint64_t *dst_map = tmvp_ctx->dir_map_v1 + 1;
            const OVMV *src_mvs = plane1->mvs + ctb_offset;
                  OVMV *dst_mvs = tmvp_ctx->mvs1;
            int i;

            /*FIXME memory could be spared with smaller map size when possible */
            memcpy(dst_map, src_map, sizeof(uint64_t) * (nb_unit_ctb + !is_border_pic));
            for (i = 0; i < nb_tmvp_unit; ++i) {
                memcpy(dst_mvs, src_mvs, sizeof(*dst_mvs) * (nb_tmvp_unit + !is_border_pic));
                dst_mvs += TMVP_BUFF_STRIDE;
                src_mvs += pln_stride;
            }
        }
    }

    inter_ctx->tmvp_avail |= 1;
}

static inline OVMV
tmvp_scale_mv(int scale, OVMV mv)
{
    mv.x = ov_clip((scale * mv.x + 128 - (scale * mv.x >= 0)) >> 8, MV_MIN, MV_MAX);
    mv.y = ov_clip((scale * mv.y + 128 - (scale * mv.y >= 0)) >> 8, MV_MIN, MV_MAX);
    return mv;
}

static int16_t
derive_tmvp_scale(int32_t dist_ref, int32_t dist_col)
{
    int32_t scale = 0;

    if (dist_ref == dist_col || !dist_col)
        return 256;

    /*FIXME POW2 clip */
    dist_ref = ov_clip(dist_ref, -128, 127);
    dist_col = ov_clip(dist_col, -128, 127);

    scale = dist_ref * ((0x4000 + OVABS(dist_col >> 1)) / dist_col);
    scale += 32;
    scale >>= 6;
    /* FIXME pow2_clip */
    scale = ov_clip(scale, -4096, 4095);

    return (int16_t)scale;
}

static OVMV
derive_mvp_candidates_1(struct InterDRVCtx *const inter_ctx,
                        const struct OVMVCtx *const mv_ctx,
                        uint8_t ref_idx,
                        uint8_t pb_x, uint8_t pb_y,
                        uint8_t nb_pb_w, uint8_t nb_pb_h,
                        uint8_t mvp_idx, uint8_t inter_dir,
                        const struct OVMVCtx *const mv_ctx_opp, uint8_t opp_ref_idx,
                        uint8_t prec_amvr, uint8_t is_small)
{
    const OVMV *const mv_buff = mv_ctx->mvs;
    const OVMV *const mv_buff_opp = mv_ctx_opp->mvs;
    uint64_t lft_col = mv_ctx->map.vfield[pb_x];
    uint64_t abv_row = mv_ctx->map.hfield[pb_y];

    uint64_t lft_col0 = mv_ctx_opp->map.vfield[pb_x];
    uint64_t abv_row0 = mv_ctx_opp->map.hfield[pb_y];

    OVMV cand[2] = {0};
    int nb_cand = 0;

    /* Derive candidates availability based on CTU inter fields */
    uint8_t cand_bl = !!(lft_col & POS_MASK(pb_y, nb_pb_h));     /*AO*/
    uint8_t cand_l  = !!(lft_col & POS_MASK(pb_y, nb_pb_h - 1)); /*A1*/
    uint8_t cand_tr = !!(abv_row & POS_MASK(pb_x, nb_pb_w));     /*B0*/
    uint8_t cand_t  = !!(abv_row & POS_MASK(pb_x, nb_pb_w - 1)); /*B1*/
    uint8_t cand_tl = !!(abv_row & POS_MASK(pb_x - 1, 0));      /*B2*/

    uint8_t cand_bl0 = !!(lft_col0 & POS_MASK(pb_y, nb_pb_h));     /*AO*/
    uint8_t cand_l0  = !!(lft_col0 & POS_MASK(pb_y, nb_pb_h - 1)); /*A1*/
    uint8_t cand_tr0 = !!(abv_row0 & POS_MASK(pb_x, nb_pb_w));     /*B0*/
    uint8_t cand_t0  = !!(abv_row0 & POS_MASK(pb_x, nb_pb_w - 1)); /*B1*/
    uint8_t cand_tl0 = !!(abv_row0 & POS_MASK(pb_x - 1, 0));      /*B2*/
    uint8_t found = 0;

    if (cand_bl | cand_bl0) {
        int pos_in_buff = OFFSET_BUFF(pb_x - 1, pb_y + nb_pb_h);
        if (cand_bl && mv_buff[pos_in_buff].ref_idx == ref_idx) {
            cand[nb_cand++] = mv_buff[pos_in_buff];
            found = 1;
        } else if (cand_bl0 && mv_buff_opp[pos_in_buff].ref_idx == opp_ref_idx) {
            cand[nb_cand++] = mv_buff_opp[pos_in_buff];
            found = 1;
        }
    }
    if (!found && (cand_l | cand_l0)) {
        int pos_in_buff = OFFSET_BUFF(pb_x - 1, pb_y + nb_pb_h - 1);
        if (cand_l && mv_buff[pos_in_buff].ref_idx == ref_idx) {
            cand[nb_cand++] = mv_buff[pos_in_buff];
        } else if (cand_l0 && mv_buff_opp[pos_in_buff].ref_idx == opp_ref_idx) {
            cand[nb_cand++] = mv_buff_opp[pos_in_buff];
        }
    }

    found = 0;

    if (!found && (cand_tr | cand_tr0)) {
        int pos_in_buff = OFFSET_BUFF(pb_x + nb_pb_w, pb_y - 1);
        if (cand_tr && mv_buff[pos_in_buff].ref_idx == ref_idx) {
            cand[nb_cand++] = mv_buff[pos_in_buff];
            found = 1;
        } else if (cand_tr0 && mv_buff_opp[pos_in_buff].ref_idx == opp_ref_idx) {
            cand[nb_cand++] = mv_buff_opp[pos_in_buff];
            found = 1;
        }
    }

    if (!found && (cand_t | cand_t0)) {
        int pos_in_buff = OFFSET_BUFF(pb_x + nb_pb_w - 1, pb_y - 1);
        if (cand_t && mv_buff[pos_in_buff].ref_idx == ref_idx) {
            cand[nb_cand++] = mv_buff[pos_in_buff];
            found = 1;
        } else if (cand_t0 && mv_buff_opp[pos_in_buff].ref_idx == opp_ref_idx) {
            cand[nb_cand++] = mv_buff_opp[pos_in_buff];
            found = 1;
        }
    }
    if (!found && (cand_tl | cand_tl0)) {
        int pos_in_buff = OFFSET_BUFF(pb_x - 1, pb_y - 1);
        if (cand_tl && mv_buff[pos_in_buff].ref_idx == ref_idx) {
            cand[nb_cand++] = mv_buff[pos_in_buff];
            found = 1;
        } else if (cand_tl0 && mv_buff_opp[pos_in_buff].ref_idx == opp_ref_idx) {
            cand[nb_cand++] = mv_buff_opp[pos_in_buff];
            found = 1;
        }
    }

    cand[0]  = drv_round_to_precision_mv(cand[0], MV_PRECISION_INTERNAL, prec_amvr);
    cand[1]  = drv_round_to_precision_mv(cand[1], MV_PRECISION_INTERNAL, prec_amvr);


    /* Remove on candidates if duplicated */
    if (nb_cand == 2) {
        if (MV_CMP(cand[0], cand[1])) {
            --nb_cand;
        }
    }

    if (inter_ctx->tmvp_enabled && nb_cand < 2 && !is_small) {
        const struct VVCTMVP *const tmvp = &inter_ctx->tmvp_ctx;
        uint64_t c1_col;
        uint64_t c0_col;
        uint64_t c1_col1;
        uint64_t c0_col1;
        uint8_t cand_c0;
        uint8_t cand_c1;
        uint8_t cand_c01;
        uint8_t cand_c11;
        int c1_x = pb_x + (nb_pb_w >> 1);
        int c1_y = pb_y + (nb_pb_h >> 1);
        int c0_x = pb_x + nb_pb_w;
        int c0_y = pb_y + nb_pb_h;
        uint8_t pos_8x8 = tmvp->ctudec->part_ctx->log2_min_cb_s == 2;

        if (!inter_ctx->tmvp_avail) {
            /* FIXME thread synchro */
            /*FIXME dirty ref to ctudec */
            OVCTUDec *ctudec = inter_ctx->tmvp_ctx.ctudec;
            load_ctb_tmvp(ctudec, ctudec->ctb_x, ctudec->ctb_y);
        }

        c0_x &= ~(pos_8x8);
        c1_x &= ~(pos_8x8);
        c0_y &= ~(pos_8x8);
        c1_y &= ~(pos_8x8);

        /* Derive availability based on CTB inter fields */
        c0_col  = tmvp->dir_map_v0[c0_x + 1];
        c0_col1 = tmvp->dir_map_v1[c0_x + 1];
        c1_col  = tmvp->dir_map_v0[c1_x + 1];
        c1_col1 = tmvp->dir_map_v1[c1_x + 1];

        cand_c0  = !!(c0_col  & TMVP_POS_MASK(c0_y));
        cand_c01 = !!(c0_col1 & TMVP_POS_MASK(c0_y));
        cand_c1  = !!(c1_col  & TMVP_POS_MASK(c1_y));
        cand_c11 = !!(c1_col1 & TMVP_POS_MASK(c1_y));

        uint8_t col_ref_l0 = tmvp->col_ref_l0;

        if ((!col_ref_l0 && !tmvp->ldc) || (tmvp->ldc && mv_ctx == &inter_ctx->mv_ctx0)) {
            if (cand_c0) {
                /* Candidate 0 in collocated picture 0 */
                int pos_in_buff = TMVP_POS_IN_BUF(c0_x, c0_y);
                OVMV c0 = tmvp->mvs0[pos_in_buff];
                int16_t col_ref_idx = c0.ref_idx;
                int16_t scale0 = derive_tmvp_scale(mv_ctx == &inter_ctx->mv_ctx0 ? inter_ctx->dist_ref_0[ref_idx] : inter_ctx->dist_ref_1[ref_idx], tmvp->dist_col_0[col_ref_idx]);
                c0.x = tmvp_round_mv(c0.x);
                c0.y = tmvp_round_mv(c0.y);
                c0 = tmvp_scale_mv(scale0, c0);
                c0 = drv_round_to_precision_mv(c0, MV_PRECISION_INTERNAL, prec_amvr);

                c0.ref_idx = ref_idx;
                cand[nb_cand++] = c0;
            } else if (cand_c01) {
                /* Candidate 0 in collocated picture 1 */
                int pos_in_buff = TMVP_POS_IN_BUF(c0_x, c0_y);
                OVMV c0 = tmvp->mvs1[pos_in_buff];
                int16_t col_ref_idx = c0.ref_idx;
                int16_t scale1 = derive_tmvp_scale(mv_ctx == &inter_ctx->mv_ctx0 ? inter_ctx->dist_ref_0[ref_idx] : inter_ctx->dist_ref_1[ref_idx], tmvp->dist_col_1[col_ref_idx]);
                c0.x = tmvp_round_mv(c0.x);
                c0.y = tmvp_round_mv(c0.y);
                c0 = tmvp_scale_mv(scale1, c0);
                c0 = drv_round_to_precision_mv(c0, MV_PRECISION_INTERNAL, prec_amvr);
                c0.ref_idx = ref_idx;
                cand[nb_cand++] = c0;
            } else if (cand_c1) {
                /* Candidate 1 in collocated picture 0 */
                int pos_in_buff = TMVP_POS_IN_BUF(c1_x, c1_y);
                OVMV c1 = tmvp->mvs0[pos_in_buff];
                int16_t col_ref_idx = c1.ref_idx;
                int16_t scale0 = derive_tmvp_scale(mv_ctx == &inter_ctx->mv_ctx0 ? inter_ctx->dist_ref_0[ref_idx] : inter_ctx->dist_ref_1[ref_idx], tmvp->dist_col_0[col_ref_idx]);
                c1.x = tmvp_round_mv(c1.x);
                c1.y = tmvp_round_mv(c1.y);
                c1 = tmvp_scale_mv(scale0, c1);
                c1 = drv_round_to_precision_mv(c1, MV_PRECISION_INTERNAL, prec_amvr);
                c1.ref_idx = ref_idx;
                cand[nb_cand++] = c1;
            } else if (cand_c11) {
                /* Candidate 1 in collocated picture 1 */
                int pos_in_buff = TMVP_POS_IN_BUF(c1_x, c1_y);
                OVMV c1 = tmvp->mvs1[pos_in_buff];
                int16_t col_ref_idx = c1.ref_idx;
                int16_t scale1 = derive_tmvp_scale(mv_ctx == &inter_ctx->mv_ctx0 ? inter_ctx->dist_ref_0[ref_idx] : inter_ctx->dist_ref_1[ref_idx], tmvp->dist_col_1[col_ref_idx]);
                c1.x = tmvp_round_mv(c1.x);
                c1.y = tmvp_round_mv(c1.y);
                c1 = tmvp_scale_mv(scale1, c1);
                c1 = drv_round_to_precision_mv(c1, MV_PRECISION_INTERNAL, prec_amvr);
                c1.ref_idx = ref_idx;
                cand[nb_cand++] = c1;
            }
        } else {
            if (cand_c01) {
                /* Candidate 0 in collocated picture 1 */
                int pos_in_buff = TMVP_POS_IN_BUF(c0_x, c0_y);
                OVMV c0 = tmvp->mvs1[pos_in_buff];
                int16_t col_ref_idx = c0.ref_idx;
                int16_t scale1 = derive_tmvp_scale(mv_ctx == &inter_ctx->mv_ctx0 ? inter_ctx->dist_ref_0[ref_idx] : inter_ctx->dist_ref_1[ref_idx], tmvp->dist_col_1[col_ref_idx]);
                c0.x = tmvp_round_mv(c0.x);
                c0.y = tmvp_round_mv(c0.y);
                c0 = tmvp_scale_mv(scale1, c0);
                c0 = drv_round_to_precision_mv(c0, MV_PRECISION_INTERNAL, prec_amvr);
                c0.ref_idx = ref_idx;
                cand[nb_cand++] = c0;
            } else if (cand_c0) {
                /* Candidate 0 in collocated picture 0 */
                int pos_in_buff = TMVP_POS_IN_BUF(c0_x, c0_y);
                OVMV c0 = tmvp->mvs0[pos_in_buff];
                int16_t col_ref_idx = c0.ref_idx;
                int16_t scale0 = derive_tmvp_scale(mv_ctx == &inter_ctx->mv_ctx0 ? inter_ctx->dist_ref_0[ref_idx] : inter_ctx->dist_ref_1[ref_idx], tmvp->dist_col_0[col_ref_idx]);
                c0.x = tmvp_round_mv(c0.x);
                c0.y = tmvp_round_mv(c0.y);
                c0 = tmvp_scale_mv(scale0, c0);
                c0 = drv_round_to_precision_mv(c0, MV_PRECISION_INTERNAL, prec_amvr);
                c0.ref_idx = ref_idx;
                cand[nb_cand++] = c0;
            } else if (cand_c11) {
                /* Candidate 1 in collocated picture 1 */
                int pos_in_buff = TMVP_POS_IN_BUF(c1_x, c1_y);
                OVMV c1 = tmvp->mvs1[pos_in_buff];
                int16_t col_ref_idx = c1.ref_idx;
                int16_t scale1 = derive_tmvp_scale(mv_ctx == &inter_ctx->mv_ctx0 ? inter_ctx->dist_ref_0[ref_idx] : inter_ctx->dist_ref_1[ref_idx], tmvp->dist_col_1[col_ref_idx]);
                c1.x = tmvp_round_mv(c1.x);
                c1.y = tmvp_round_mv(c1.y);
                c1 = tmvp_scale_mv(scale1, c1);
                c1 = drv_round_to_precision_mv(c1, MV_PRECISION_INTERNAL, prec_amvr);
                c1.ref_idx = ref_idx;
                cand[nb_cand++] = c1;
            } else if (cand_c1) {
                /* Candidate 1 in collocated picture 0 */
                int pos_in_buff = TMVP_POS_IN_BUF(c1_x, c1_y);
                OVMV c1 = tmvp->mvs0[pos_in_buff];
                int16_t col_ref_idx = c1.ref_idx;
                int16_t scale0 = derive_tmvp_scale(mv_ctx == &inter_ctx->mv_ctx0 ? inter_ctx->dist_ref_0[ref_idx] : inter_ctx->dist_ref_1[ref_idx], tmvp->dist_col_0[col_ref_idx]);
                c1.x = tmvp_round_mv(c1.x);
                c1.y = tmvp_round_mv(c1.y);
                c1 = tmvp_scale_mv(scale0, c1);
                c1 = drv_round_to_precision_mv(c1, MV_PRECISION_INTERNAL, prec_amvr);
                c1.ref_idx = ref_idx;
                cand[nb_cand++] = c1;
            }
        }
    }

    if (nb_cand < 2) {
        const struct HMVPLUT *hmvp_lut = &inter_ctx->hmvp_lut;
        hmvp_add_cand_1(hmvp_lut, cand, &nb_cand, inter_dir, ref_idx, opp_ref_idx);
    }

    while (nb_cand < 2) {
        OVMV zmv = {0};
        zmv.ref_idx = ref_idx;
        cand[nb_cand++] = zmv;
    }

    cand[0]  = drv_round_to_precision_mv(cand[0], MV_PRECISION_INTERNAL, prec_amvr);
    cand[1]  = drv_round_to_precision_mv(cand[1], MV_PRECISION_INTERNAL, prec_amvr);

    return cand[mvp_idx];
}

static OVMV
vvc_derive_merge_mvp(const struct InterDRVCtx *const inter_ctx,
                     const struct OVMVCtx *const mv_ctx,
                     uint8_t pb_x, uint8_t pb_y,
                     uint8_t nb_pb_w, uint8_t nb_pb_h,
                     uint8_t merge_idx, uint8_t max_nb_merge_cand,
                     uint8_t is_small)
{
    const OVMV *const mv_buff = mv_ctx->mvs;

    uint64_t lft_col = mv_ctx->map.vfield[pb_x];
    uint64_t abv_row = mv_ctx->map.hfield[pb_y];

    uint8_t cand_bl = !!(lft_col & POS_MASK(pb_y, nb_pb_h));     /*AO*/
    uint8_t cand_l  = !!(lft_col & POS_MASK(pb_y, nb_pb_h - 1)); /*A1*/
    uint8_t cand_tr = !!(abv_row & POS_MASK(pb_x, nb_pb_w));     /*B0*/
    uint8_t cand_t  = !!(abv_row & POS_MASK(pb_x, nb_pb_w - 1)); /*B1*/
    uint8_t cand_tl = !!(abv_row & POS_MASK(pb_x - 1, 0));      /*B2*/
    OVMV cand[6];
    OVMV cand_amvp[5];
    OVMV mv_z = {-1};

    int nb_cand = 0;

    cand_amvp[0] = mv_z;
    if (cand_t) { /* B1 */
        int pos_in_buff = OFFSET_BUFF(pb_x + nb_pb_w - 1, pb_y - 1);
        OVMV mv_B1 = mv_buff[pos_in_buff];
        cand_amvp[0] = mv_buff[pos_in_buff];
        cand[nb_cand] = mv_B1;
        if (nb_cand++ == merge_idx)
            return mv_B1;
    }

    cand_amvp[1] = mv_z;
    if (cand_l) {
        int pos_in_buff = OFFSET_BUFF(pb_x - 1, pb_y + nb_pb_h - 1);
        OVMV mv_A1 = mv_buff[pos_in_buff];
        cand_amvp[1] = mv_buff[pos_in_buff];
        if (!cand_t || !MV_CMP2(mv_A1, cand_amvp[0])) {
            cand[nb_cand] = mv_A1;
            if (nb_cand++ == merge_idx)
                return mv_A1;
        }
    }

    cand_amvp[2] = mv_z;
    if (cand_tr) {
        int pos_in_buff = OFFSET_BUFF(pb_x + nb_pb_w, pb_y - 1);
        OVMV mv_B0 = mv_buff[pos_in_buff];
        cand_amvp[2] = mv_buff[pos_in_buff];
        if (!cand_t || !MV_CMP2(mv_B0, cand_amvp[0])) {
            cand[nb_cand] = mv_B0;
            if (nb_cand++ == merge_idx)
                return mv_B0;
        }
    }

    cand_amvp[3] = mv_z;
    if (cand_bl) {
        int pos_in_buff = OFFSET_BUFF(pb_x - 1, pb_y + nb_pb_h);
        OVMV mv_A0 = mv_buff[pos_in_buff];
        cand_amvp[3] = mv_buff[pos_in_buff];
        if (!cand_l || !MV_CMP2(mv_A0, cand_amvp[1])) {
            cand[nb_cand] = mv_A0;
            if (nb_cand++ == merge_idx)
                return mv_A0;
        }
    }

    cand_amvp[4] = mv_z;
    if (nb_cand < 4) {
        if (cand_tl) {
            int pos_in_buff = OFFSET_BUFF(pb_x - 1, pb_y - 1);
            OVMV mv_B2   = mv_buff[pos_in_buff];
            cand_amvp[4] = mv_buff[pos_in_buff];
            if ((!cand_l || !MV_CMP2(mv_B2, cand_amvp[1])) &&
                (!cand_t || !MV_CMP2(mv_B2, cand_amvp[0]))) {
                cand[nb_cand] = mv_B2;
                if (nb_cand++ == merge_idx)
                    return mv_B2;
            }
        }
    }

    if (inter_ctx->tmvp_enabled && !is_small) {
        const struct VVCTMVP *const tmvp = &inter_ctx->tmvp_ctx;
        uint64_t c1_col;
        uint64_t c0_col;
        uint64_t c1_col1;
        uint64_t c0_col1;
        uint8_t cand_c0;
        uint8_t cand_c1;
        uint8_t cand_c01;
        uint8_t cand_c11;
        int c1_x = pb_x + (nb_pb_w >> 1);
        int c1_y = pb_y + (nb_pb_h >> 1);
        int c0_x = pb_x + nb_pb_w;
        int c0_y = pb_y + nb_pb_h;
        uint8_t pos_8x8 = tmvp->ctudec->part_ctx->log2_min_cb_s == 2;

        if (!inter_ctx->tmvp_avail) {
            /* FIXME thread synchro */
            /*FIXME dirty ref to ctudec */
            OVCTUDec *ctudec = inter_ctx->tmvp_ctx.ctudec;
            load_ctb_tmvp(ctudec, ctudec->ctb_x, ctudec->ctb_y);
        }

        c0_x &= ~(pos_8x8);
        c1_x &= ~(pos_8x8);
        c0_y &= ~(pos_8x8);
        c1_y &= ~(pos_8x8);

        c0_col  = tmvp->dir_map_v0[c0_x + 1];
        c1_col  = tmvp->dir_map_v0[c1_x + 1];
        c0_col1 = tmvp->dir_map_v1[c0_x + 1];
        c1_col1 = tmvp->dir_map_v1[c1_x + 1];

        cand_c0  = !!(c0_col  & TMVP_POS_MASK(c0_y));
        cand_c01 = !!(c0_col1 & TMVP_POS_MASK(c0_y));
        cand_c1  = !!(c1_col  & TMVP_POS_MASK(c1_y));
        cand_c11 = !!(c1_col1 & TMVP_POS_MASK(c1_y));

        if (cand_c0) {
            int pos_in_buff = TMVP_POS_IN_BUF(c0_x, c0_y);
            OVMV c0 = tmvp->mvs0[pos_in_buff];
            uint8_t col_ref_idx = c0.ref_idx;
            int16_t scale = derive_tmvp_scale(inter_ctx->dist_ref_0[0], tmvp->dist_col_0[col_ref_idx]);
            c0.x = tmvp_round_mv(c0.x);
            c0.y = tmvp_round_mv(c0.y);
            c0 = tmvp_scale_mv(scale, c0);
            c0.bcw_idx_plus1 = 0;
            c0.prec_amvr = 0;
            c0.ref_idx = 0;
            cand[nb_cand] = c0;
            if (nb_cand++ == merge_idx)
                return c0;

        } else if (cand_c01) {
            int pos_in_buff = TMVP_POS_IN_BUF(c0_x, c0_y);
            OVMV c0 = tmvp->mvs1[pos_in_buff];
            uint8_t col_ref_idx = c0.ref_idx;
            int16_t scale = derive_tmvp_scale(inter_ctx->dist_ref_0[0], tmvp->dist_col_1[col_ref_idx]);
            c0.x = tmvp_round_mv(c0.x);
            c0.y = tmvp_round_mv(c0.y);
            c0 = tmvp_scale_mv(scale, c0);
            c0.bcw_idx_plus1 = 0;
            c0.prec_amvr = 0;
            c0.ref_idx = 0;
            cand[nb_cand] = c0;
            if (nb_cand++ == merge_idx)
                return c0;
        } else if (cand_c1) {
            int pos_in_buff = TMVP_POS_IN_BUF(c1_x, c1_y);
            OVMV c1 = tmvp->mvs0[pos_in_buff];
            uint8_t col_ref_idx = c1.ref_idx;
            int16_t scale = derive_tmvp_scale(inter_ctx->dist_ref_0[0], tmvp->dist_col_0[col_ref_idx]);
            c1.x = tmvp_round_mv(c1.x);
            c1.y = tmvp_round_mv(c1.y);
            c1 = tmvp_scale_mv(scale , c1);
            c1.bcw_idx_plus1 = 0;
            c1.prec_amvr = 0;
            c1.ref_idx = 0;
            cand[nb_cand] = c1;
            if (nb_cand++ == merge_idx)
                return c1;
        } else if (cand_c11) {
            int pos_in_buff = TMVP_POS_IN_BUF(c1_x, c1_y);
            OVMV c1 = tmvp->mvs1[pos_in_buff];
            uint8_t col_ref_idx = c1.ref_idx;
            int16_t scale = derive_tmvp_scale(inter_ctx->dist_ref_0[0], tmvp->dist_col_1[col_ref_idx]);
            c1.x = tmvp_round_mv(c1.x);
            c1.y = tmvp_round_mv(c1.y);
            c1 = tmvp_scale_mv(scale , c1);
            c1.bcw_idx_plus1 = 0;
            c1.prec_amvr = 0;
            c1.ref_idx = 0;
            cand[nb_cand] = c1;
            if (nb_cand++ == merge_idx)
                return c1;
        }
    }

    if (nb_cand != max_nb_merge_cand - 1) {
        uint8_t found;
        const struct HMVPLUT *hmvp_lut = &inter_ctx->hmvp_lut;

        found = hmvp_add_merge_cand(hmvp_lut, cand, cand_amvp, cand_t, cand_l, &nb_cand, 1, merge_idx, max_nb_merge_cand - 1);
        if (found) {
            return cand[nb_cand - 1];
        }
    }


    if (nb_cand > 1 && nb_cand < max_nb_merge_cand) {
        OVMV avg_mv = cand[0];
        avg_mv.x += cand[1].x;
        avg_mv.y += cand[1].y;
        avg_mv.x += 1 - (avg_mv.x >= 0);
        avg_mv.y += 1 - (avg_mv.y >= 0);
        avg_mv.x >>= 1;
        avg_mv.y >>= 1;
        if (cand[0].prec_amvr != cand[1].prec_amvr) {
            avg_mv.prec_amvr = 0;
        }
        if (nb_cand == merge_idx)
            return avg_mv;

        nb_cand++;
    }

    int8_t diff_cand = merge_idx - nb_cand;
    int8_t num_min_ref = inter_ctx->nb_active_ref0;
    int8_t ref_idx = 0;
    if (diff_cand <= num_min_ref - 1) ref_idx = diff_cand;

    /*FIXME ref_idx*/
    while (nb_cand < max_nb_merge_cand) {
        OVMV zmv = {0};
        zmv.ref_idx = ref_idx;
        cand[nb_cand++] = zmv;
    }

    return cand[nb_cand - 1];
}


static VVCMergeInfo
vvc_derive_merge_mvp_b(const struct InterDRVCtx *const inter_ctx,
                       uint8_t pb_x, uint8_t pb_y,
                       uint8_t nb_pb_w, uint8_t nb_pb_h,
                       uint8_t merge_idx, uint8_t max_nb_merge_cand,
                       uint8_t is_small)
{
    const struct OVMVCtx *mv_ctx0 = &inter_ctx->mv_ctx0;
    const struct OVMVCtx *mv_ctx1 = &inter_ctx->mv_ctx1;

    const OVMV *mv_buff0 = mv_ctx0->mvs;
    const OVMV *mv_buff1 = mv_ctx1->mvs;

    uint64_t lft_col0 = mv_ctx0->map.vfield[pb_x];
    uint64_t abv_row0 = mv_ctx0->map.hfield[pb_y];

    uint64_t lft_col1 = mv_ctx1->map.vfield[pb_x];
    uint64_t abv_row1 = mv_ctx1->map.hfield[pb_y];

    /*FIXME use flags for inter_dir and availability*/
    uint8_t cand_bl0 = !!(lft_col0 & POS_MASK(pb_y, nb_pb_h));     /*AO*/
    uint8_t cand_bl1 = !!(lft_col1 & POS_MASK(pb_y, nb_pb_h));     /*AO*/
    uint8_t cand_l0  = !!(lft_col0 & POS_MASK(pb_y, nb_pb_h - 1)); /*A1*/
    uint8_t cand_l1  = !!(lft_col1 & POS_MASK(pb_y, nb_pb_h - 1)); /*A1*/

    uint8_t cand_tr0 = !!(abv_row0 & POS_MASK(pb_x, nb_pb_w));     /*B0*/
    uint8_t cand_tr1 = !!(abv_row1 & POS_MASK(pb_x, nb_pb_w));     /*B0*/
    uint8_t cand_t0  = !!(abv_row0 & POS_MASK(pb_x, nb_pb_w - 1)); /*B1*/
    uint8_t cand_t1  = !!(abv_row1 & POS_MASK(pb_x, nb_pb_w - 1)); /*B1*/
    uint8_t cand_tl0 = !!(abv_row0 & POS_MASK(pb_x - 1, 0));      /*B2*/
    uint8_t cand_tl1 = !!(abv_row1 & POS_MASK(pb_x - 1, 0));      /*B2*/

    VVCMergeInfo cand[6];
    VVCMergeInfo cand_amvp[5];

    VVCMergeInfo mv_z = { .mv0 = {0}, .mv1 = {0}, .inter_dir = 3};

    int nb_cand = 0;

    cand_amvp[0] = mv_z;
    if (cand_t0 | cand_t1) { /* B1 */
        int pos_in_buff = OFFSET_BUFF(pb_x + nb_pb_w - 1, pb_y - 1);
        VVCMergeInfo mi_B1;
        mi_B1.mv0 = mv_buff0[pos_in_buff];
        mi_B1.mv1 = mv_buff1[pos_in_buff];
        mi_B1.inter_dir =  cand_t0 | (cand_t1 << 1);

        cand_amvp[0]  = mi_B1;

        cand[nb_cand] = mi_B1;
        if (nb_cand++ == merge_idx)
            return mi_B1;
    }

    cand_amvp[1] = mv_z;
    if (cand_l0 | cand_l1) {
        int pos_in_buff = OFFSET_BUFF(pb_x - 1, pb_y + nb_pb_h - 1);
        VVCMergeInfo mv_A1;
        mv_A1.mv0 = mv_buff0[pos_in_buff];
        mv_A1.mv1 = mv_buff1[pos_in_buff];
        mv_A1.inter_dir = cand_l0 | (cand_l1 << 1);

        cand_amvp[1] = mv_A1;
        if (!(cand_t0 | cand_t1) || !mi_cmp(&mv_A1, &cand_amvp[0])) {
            cand[nb_cand] = mv_A1;
            if (nb_cand++ == merge_idx)
                return mv_A1;
        }
    }

    if (cand_tr0 | cand_tr1) {
        int pos_in_buff = OFFSET_BUFF(pb_x + nb_pb_w, pb_y - 1);
        VVCMergeInfo mv_B0;
        mv_B0.mv0 = mv_buff0[pos_in_buff];
        mv_B0.mv1 = mv_buff1[pos_in_buff];
        mv_B0.inter_dir =  cand_tr0 | (cand_tr1 << 1);

        cand_amvp[2] = mv_B0;
        if (!(cand_t0 | cand_t1) || !mi_cmp(&mv_B0, &cand_amvp[0])) {
            cand[nb_cand] = mv_B0;
            if (nb_cand++ == merge_idx)
                return mv_B0;
        }
    }

    if (cand_bl0 | cand_bl1) {
        int pos_in_buff = OFFSET_BUFF(pb_x - 1, pb_y + nb_pb_h);
        VVCMergeInfo mv_A0;
        mv_A0.mv0 = mv_buff0[pos_in_buff];
        mv_A0.mv1 = mv_buff1[pos_in_buff];
        mv_A0.inter_dir = cand_bl0 | (cand_bl1 << 1);

        cand_amvp[3] = mv_A0;
        if (!(cand_l0 | cand_l1) || !mi_cmp(&mv_A0, &cand_amvp[1])) {
            cand[nb_cand] = mv_A0;
            if (nb_cand++ == merge_idx)
                return mv_A0;
        }
    }

    if (nb_cand < 4) {
        if (cand_tl0 | cand_tl1) {
            int pos_in_buff = OFFSET_BUFF(pb_x - 1, pb_y - 1);
            VVCMergeInfo mv_B2;
            mv_B2.mv0 = mv_buff0[pos_in_buff];
            mv_B2.mv1 = mv_buff1[pos_in_buff];
            mv_B2.inter_dir =  cand_tl0 | (cand_tl1 << 1);

            cand_amvp[4] = mv_B2;
            if ((!(cand_l0 | cand_l1) || !mi_cmp(&mv_B2, &cand_amvp[1])) &&
                (!(cand_t0 | cand_t1) || !mi_cmp(&mv_B2, &cand_amvp[0]))) {
                cand[nb_cand] = mv_B2;
                if (nb_cand++ == merge_idx)
                    return mv_B2;
            }
        }
    }

    if (inter_ctx->tmvp_enabled && !is_small) {
        const struct VVCTMVP *const tmvp = &inter_ctx->tmvp_ctx;
        uint8_t cand_c0;
        uint8_t cand_c1;
        uint8_t cand_c01;
        uint8_t cand_c11;
        uint64_t c0_col;
        uint64_t c0_col1;
        uint64_t c1_col;
        uint64_t c1_col1;

        int c1_x = pb_x + (nb_pb_w >> 1);
        int c1_y = pb_y + (nb_pb_h >> 1);
        int c0_x = pb_x + nb_pb_w;
        int c0_y = pb_y + nb_pb_h;

        uint8_t pos_8x8 = tmvp->ctudec->part_ctx->log2_min_cb_s == 2;

        if (!inter_ctx->tmvp_avail) {
            /* FIXME thread synchro */
            /*FIXME dirty ref to ctudec */
            OVCTUDec *ctudec = inter_ctx->tmvp_ctx.ctudec;
            load_ctb_tmvp(ctudec, ctudec->ctb_x, ctudec->ctb_y);
        }

        c0_x &= ~(pos_8x8);
        c1_x &= ~(pos_8x8);
        c0_y &= ~(pos_8x8);
        c1_y &= ~(pos_8x8);

        c0_col  = tmvp->dir_map_v0[c0_x + 1];
        c0_col1 = tmvp->dir_map_v1[c0_x + 1];
        c1_col  = tmvp->dir_map_v0[c1_x + 1];
        c1_col1 = tmvp->dir_map_v1[c1_x + 1];

        cand_c0  = !!(c0_col  & TMVP_POS_MASK(c0_y));
        cand_c01 = !!(c0_col1 & TMVP_POS_MASK(c0_y));
        cand_c1  = !!(c1_col  & TMVP_POS_MASK(c1_y));
        cand_c11 = !!(c1_col1 & TMVP_POS_MASK(c1_y));

        /*FIXME check whether TMVP candidates RPL order is correct*/
        if ((!tmvp->col_ref_l0)/* && !tmvp->ldc) || tmvp->ldc*/) {

            if (cand_c0 | cand_c01) {
                int pos_in_buff = TMVP_POS_IN_BUF(c0_x, c0_y);
                cand[nb_cand].inter_dir = 3;
                if (cand_c0) {
                    OVMV c0  = tmvp->mvs0[pos_in_buff];
                    uint8_t col_ref_idx = c0.ref_idx;
                    int16_t scale00 = derive_tmvp_scale(inter_ctx->dist_ref_0[0], tmvp->dist_col_0[col_ref_idx]);
                    int16_t scale10 = derive_tmvp_scale(inter_ctx->dist_ref_1[0], tmvp->dist_col_0[col_ref_idx]);
                    c0.x = tmvp_round_mv(c0.x);
                    c0.y = tmvp_round_mv(c0.y);
                    cand[nb_cand].mv0 = tmvp_scale_mv(scale00, c0);
                    cand[nb_cand].mv0.ref_idx = 0;
                    if (cand_c01 && tmvp->ldc) {
                        OVMV c01  = tmvp->mvs1[pos_in_buff];
                        col_ref_idx = c01.ref_idx;
                        int16_t scale11 = derive_tmvp_scale(inter_ctx->dist_ref_1[0], tmvp->dist_col_1[col_ref_idx]);
                        c01.x = tmvp_round_mv(c01.x);
                        c01.y = tmvp_round_mv(c01.y);
                        cand[nb_cand].mv1 = tmvp_scale_mv(scale11, c01);
                        cand[nb_cand].mv1.ref_idx = 0;
                    } else {
                        cand[nb_cand].mv1 = tmvp_scale_mv(scale10, c0);
                        cand[nb_cand].mv1.ref_idx = 0;
                    }
                    cand[nb_cand].mv0.bcw_idx_plus1 = 0;
                    cand[nb_cand].mv1.bcw_idx_plus1 = 0;
                    cand[nb_cand].mv0.prec_amvr = 0;
                    cand[nb_cand].mv1.prec_amvr = 0;
                    if (nb_cand++ == merge_idx)
                        return cand[merge_idx];
                } else {
                    OVMV c0  = tmvp->mvs1[pos_in_buff];
                    uint8_t col_ref_idx = c0.ref_idx;
                    int16_t scale01 = derive_tmvp_scale(inter_ctx->dist_ref_0[0], tmvp->dist_col_1[col_ref_idx]);
                    int16_t scale11 = derive_tmvp_scale(inter_ctx->dist_ref_1[0], tmvp->dist_col_1[col_ref_idx]);
                    c0.x = tmvp_round_mv(c0.x);
                    c0.y = tmvp_round_mv(c0.y);
                    cand[nb_cand].mv0 = tmvp_scale_mv(scale01, c0);
                    cand[nb_cand].mv1 = tmvp_scale_mv(scale11, c0);
                    cand[nb_cand].mv0.ref_idx = 0;
                    cand[nb_cand].mv1.ref_idx = 0;
                    cand[nb_cand].mv0.bcw_idx_plus1 = 0;
                    cand[nb_cand].mv1.bcw_idx_plus1 = 0;
                    cand[nb_cand].mv0.prec_amvr = 0;
                    cand[nb_cand].mv1.prec_amvr = 0;
                    if (nb_cand++ == merge_idx)
                        return cand[merge_idx];
                }
            } else if (cand_c1 | cand_c11) {
                int pos_in_buff = TMVP_POS_IN_BUF(c1_x, c1_y);
                cand[nb_cand].inter_dir = 3;
                if (cand_c1) {
                    OVMV c1  = tmvp->mvs0[pos_in_buff];
                    uint8_t col_ref_idx = c1.ref_idx;
                    int16_t scale00 = derive_tmvp_scale(inter_ctx->dist_ref_0[0], tmvp->dist_col_0[col_ref_idx]);
                    int16_t scale10 = derive_tmvp_scale(inter_ctx->dist_ref_1[0], tmvp->dist_col_0[col_ref_idx]);
                    c1.x = tmvp_round_mv(c1.x);
                    c1.y = tmvp_round_mv(c1.y);
                    cand[nb_cand].mv0 = tmvp_scale_mv(scale00, c1);
                    cand[nb_cand].mv0.ref_idx = 0;
                    if (cand_c11 && tmvp->ldc) {
                        OVMV c11  = tmvp->mvs1[pos_in_buff];
                        col_ref_idx = c11.ref_idx;
                        int16_t scale11 = derive_tmvp_scale(inter_ctx->dist_ref_1[0], tmvp->dist_col_1[col_ref_idx]);
                        c11.x = tmvp_round_mv(c11.x);
                        c11.y = tmvp_round_mv(c11.y);
                        cand[nb_cand].mv1 = tmvp_scale_mv(scale11, c11);
                        cand[nb_cand].mv1.ref_idx = 0;
                    } else {
                        cand[nb_cand].mv1 = tmvp_scale_mv(scale10, c1);
                        cand[nb_cand].mv1.ref_idx = 0;
                    }
                    cand[nb_cand].mv0.bcw_idx_plus1 = 0;
                    cand[nb_cand].mv1.bcw_idx_plus1 = 0;
                    cand[nb_cand].mv0.prec_amvr = 0;
                    cand[nb_cand].mv1.prec_amvr = 0;
                    if (nb_cand++ == merge_idx)
                        return cand[merge_idx];
                } else {
                    OVMV c1  = tmvp->mvs1[pos_in_buff];
                    uint8_t col_ref_idx = c1.ref_idx;
                    int16_t scale01 = derive_tmvp_scale(inter_ctx->dist_ref_0[0], tmvp->dist_col_1[col_ref_idx]);
                    int16_t scale11 = derive_tmvp_scale(inter_ctx->dist_ref_1[0], tmvp->dist_col_1[col_ref_idx]);
                    c1.x = tmvp_round_mv(c1.x);
                    c1.y = tmvp_round_mv(c1.y);
                    cand[nb_cand].mv0 = tmvp_scale_mv(scale01, c1);
                    cand[nb_cand].mv1 = tmvp_scale_mv(scale11, c1);
                    cand[nb_cand].mv0.ref_idx = 0;
                    cand[nb_cand].mv1.ref_idx = 0;
                    cand[nb_cand].mv0.bcw_idx_plus1 = 0;
                    cand[nb_cand].mv1.bcw_idx_plus1 = 0;
                    cand[nb_cand].mv0.prec_amvr = 0;
                    cand[nb_cand].mv1.prec_amvr = 0;
                    if (nb_cand++ == merge_idx)
                        return cand[merge_idx];
                }
            }
        } else {
            if (cand_c0 | cand_c01) {
                int pos_in_buff = TMVP_POS_IN_BUF(c0_x, c0_y);
                cand[nb_cand].inter_dir = 3;
                if (cand_c01) {
                    OVMV c0  = tmvp->mvs1[pos_in_buff];
                    uint8_t col_ref_idx = c0.ref_idx;
                    int16_t scale01 = derive_tmvp_scale(inter_ctx->dist_ref_0[0], tmvp->dist_col_1[col_ref_idx]);
                    int16_t scale11 = derive_tmvp_scale(inter_ctx->dist_ref_1[0], tmvp->dist_col_1[col_ref_idx]);
                    c0.x = tmvp_round_mv(c0.x);
                    c0.y = tmvp_round_mv(c0.y);
                    cand[nb_cand].mv1 = tmvp_scale_mv(scale11, c0);
                    cand[nb_cand].mv1.ref_idx = 0;

                    if (cand_c0 && tmvp->ldc) {
                        OVMV c00  = tmvp->mvs0[pos_in_buff];
                        col_ref_idx = c00.ref_idx;
                        int16_t scale00 = derive_tmvp_scale(inter_ctx->dist_ref_0[0], tmvp->dist_col_0[col_ref_idx]);
                        c00.x = tmvp_round_mv(c00.x);
                        c00.y = tmvp_round_mv(c00.y);
                        cand[nb_cand].mv0 = tmvp_scale_mv(scale00, c00);
                        cand[nb_cand].mv0.ref_idx = 0;
                    } else {
                        cand[nb_cand].mv0 = tmvp_scale_mv(scale01, c0);
                        cand[nb_cand].mv0.ref_idx = 0;
                    }
                    cand[nb_cand].mv0.bcw_idx_plus1 = 0;
                    cand[nb_cand].mv1.bcw_idx_plus1 = 0;
                    cand[nb_cand].mv0.prec_amvr = 0;
                    cand[nb_cand].mv1.prec_amvr = 0;
                    if (nb_cand++ == merge_idx)
                        return cand[merge_idx];
                } else {
                    OVMV c0  = tmvp->mvs0[pos_in_buff];
                    uint8_t col_ref_idx = c0.ref_idx;
                    int16_t scale00 = derive_tmvp_scale(inter_ctx->dist_ref_0[0], tmvp->dist_col_0[col_ref_idx]);
                    int16_t scale10 = derive_tmvp_scale(inter_ctx->dist_ref_1[0], tmvp->dist_col_0[col_ref_idx]);
                    c0.x = tmvp_round_mv(c0.x);
                    c0.y = tmvp_round_mv(c0.y);
                    cand[nb_cand].mv0 = tmvp_scale_mv(scale00, c0);
                    cand[nb_cand].mv1 = tmvp_scale_mv(scale10, c0);
                    cand[nb_cand].mv0.ref_idx = 0;
                    cand[nb_cand].mv1.ref_idx = 0;
                    cand[nb_cand].mv0.bcw_idx_plus1 = 0;
                    cand[nb_cand].mv1.bcw_idx_plus1 = 0;
                    cand[nb_cand].mv0.prec_amvr = 0;
                    cand[nb_cand].mv1.prec_amvr = 0;
                    if (nb_cand++ == merge_idx)
                        return cand[merge_idx];
                }
            } else if (cand_c1 | cand_c11) {
                int pos_in_buff = TMVP_POS_IN_BUF(c1_x, c1_y);
                cand[nb_cand].inter_dir = 3;
                if (cand_c11) {
                    OVMV c1  = tmvp->mvs1[pos_in_buff];
                    uint8_t col_ref_idx = c1.ref_idx;
                    int16_t scale01 = derive_tmvp_scale(inter_ctx->dist_ref_0[0], tmvp->dist_col_1[col_ref_idx]);
                    int16_t scale11 = derive_tmvp_scale(inter_ctx->dist_ref_1[0], tmvp->dist_col_1[col_ref_idx]);
                    c1.x = tmvp_round_mv(c1.x);
                    c1.y = tmvp_round_mv(c1.y);
                    cand[nb_cand].mv1 = tmvp_scale_mv(scale11, c1);
                    cand[nb_cand].mv1.ref_idx = 0;

                    if (cand_c1 && tmvp->ldc) {
                        OVMV c10  = tmvp->mvs0[pos_in_buff];
                        col_ref_idx = c10.ref_idx;
                        int16_t scale00 = derive_tmvp_scale(inter_ctx->dist_ref_0[0], tmvp->dist_col_0[col_ref_idx]);
                        c10.x = tmvp_round_mv(c10.x);
                        c10.y = tmvp_round_mv(c10.y);
                        cand[nb_cand].mv0 = tmvp_scale_mv(scale00, c10);
                        cand[nb_cand].mv0.ref_idx = 0;
                    } else {
                        cand[nb_cand].mv0 = tmvp_scale_mv(scale01, c1);
                        cand[nb_cand].mv0.ref_idx = 0;
                    }
                    cand[nb_cand].mv0.bcw_idx_plus1 = 0;
                    cand[nb_cand].mv1.bcw_idx_plus1 = 0;
                    cand[nb_cand].mv0.prec_amvr = 0;
                    cand[nb_cand].mv1.prec_amvr = 0;
                    if (nb_cand++ == merge_idx)
                        return cand[merge_idx];
                } else {
                    OVMV c1  = tmvp->mvs0[pos_in_buff];
                    uint8_t col_ref_idx = c1.ref_idx;
                    int16_t scale00 = derive_tmvp_scale(inter_ctx->dist_ref_0[0], tmvp->dist_col_0[col_ref_idx]);
                    int16_t scale10 = derive_tmvp_scale(inter_ctx->dist_ref_1[0], tmvp->dist_col_0[col_ref_idx]);
                    c1.x = tmvp_round_mv(c1.x);
                    c1.y = tmvp_round_mv(c1.y);
                    cand[nb_cand].mv0 = tmvp_scale_mv(scale00, c1);
                    cand[nb_cand].mv1 = tmvp_scale_mv(scale10, c1);
                    cand[nb_cand].mv0.ref_idx = 0;
                    cand[nb_cand].mv1.ref_idx = 0;
                    cand[nb_cand].mv0.bcw_idx_plus1 = 0;
                    cand[nb_cand].mv1.bcw_idx_plus1 = 0;
                    cand[nb_cand].mv0.prec_amvr = 0;
                    cand[nb_cand].mv1.prec_amvr = 0;
                    if (nb_cand++ == merge_idx)
                        return cand[merge_idx];
                }
            }
        }
    }

    if (nb_cand != max_nb_merge_cand - 1) {
        const struct HMVPLUT *const hmvp_lut = &inter_ctx->hmvp_lut;
        uint8_t found;

        found = hmvp_add_merge_cand_b(hmvp_lut, cand, cand_amvp, cand_t0 | cand_t1, cand_l0 | cand_l1, &nb_cand, merge_idx, max_nb_merge_cand - 1);
        if (found) {
            return cand[nb_cand - 1];
        }
    }


    if (nb_cand > 1 && nb_cand < max_nb_merge_cand) {
        VVCMergeInfo avg_mv = cand[0];

        avg_mv.inter_dir = cand[0].inter_dir & cand[1].inter_dir;

        if (avg_mv.inter_dir & 0x1) {
            avg_mv.mv0.x += cand[1].mv0.x;
            avg_mv.mv0.y += cand[1].mv0.y;
            avg_mv.mv0.x += 1 - (avg_mv.mv0.x >= 0);
            avg_mv.mv0.y += 1 - (avg_mv.mv0.y >= 0);
            avg_mv.mv0.x >>= 1;
            avg_mv.mv0.y >>= 1;
        } else if (cand[1].inter_dir & 0x1) {
            avg_mv.mv0 = cand[1].mv0;
            avg_mv.inter_dir |= 1;
            avg_mv.mv0.ref_idx = cand[1].mv0.ref_idx;
        } else if (cand[0].inter_dir & 0x1) {
            avg_mv.inter_dir |= 1;
        }

        if (avg_mv.inter_dir & 0x2) {
            avg_mv.mv1.x += cand[1].mv1.x;
            avg_mv.mv1.y += cand[1].mv1.y;
            avg_mv.mv1.x += 1 - (avg_mv.mv1.x >= 0);
            avg_mv.mv1.y += 1 - (avg_mv.mv1.y >= 0);
            avg_mv.mv1.x >>= 1;
            avg_mv.mv1.y >>= 1;
        } else if (cand[1].inter_dir & 0x2) {
            avg_mv.mv1 = cand[1].mv1;
            avg_mv.inter_dir |= 2;
            avg_mv.mv1.ref_idx = cand[1].mv1.ref_idx;
        } else if (cand[0].inter_dir & 0x2) {
            avg_mv.inter_dir |= 2;
        }

        if (nb_cand == merge_idx){
            avg_mv.mv0.bcw_idx_plus1 = 0;
            avg_mv.mv1.bcw_idx_plus1 = 0;
            uint8_t prec_amvr0 = cand[0].inter_dir & 0x1 ? cand[0].mv0.prec_amvr : cand[0].mv1.prec_amvr;
            uint8_t prec_amvr1 = cand[1].inter_dir & 0x1 ? cand[1].mv0.prec_amvr : cand[1].mv1.prec_amvr;
            avg_mv.mv0.prec_amvr = (prec_amvr0 == prec_amvr1) ? prec_amvr0 : 0 ;
            avg_mv.mv1.prec_amvr = avg_mv.mv0.prec_amvr ;
            return avg_mv;
        }

        nb_cand++;
    }
    /*FIXME saturation or ridx based on nb_active_ref*/
    int8_t diff_cand = merge_idx - nb_cand;
    int8_t num_min_ref = OVMIN(inter_ctx->nb_active_ref0, inter_ctx->nb_active_ref1);
    int8_t ridx = 0;
    if (diff_cand <= num_min_ref - 1)
        ridx = diff_cand;

    mv_z.inter_dir = 3;
    mv_z.mv0.ref_idx = ridx;
    mv_z.mv0.bcw_idx_plus1 = 0;
    mv_z.mv0.prec_amvr = 0;
    mv_z.mv1.ref_idx = ridx;
    mv_z.mv1.bcw_idx_plus1 = 0;
    mv_z.mv1.prec_amvr = 0;

    return mv_z;
}

static void
fill_mvp_map(struct OVMVCtx *const mv_ctx, OVMV mv,
             int pb_x, int pb_y, int nb_pb_w, int nb_pb_h)
{
    int i, j;

    ctu_field_set_rect_bitfield(&mv_ctx->map, pb_x, pb_y, nb_pb_w, nb_pb_h);

    for (j = 0; j < nb_pb_h; ++j) {
        for (i = 0; i < nb_pb_w; ++i) {
            memcpy(&mv_ctx->mvs[PB_POS_IN_BUF(pb_x + i, pb_y + j)], &mv, sizeof(OVMV));
        }
    }
}

static void
fill_tmvp_map(struct OVMV *const tmvp_mv, OVMV mv,
              int pb_x, int pb_y, int nb_pb_w, int nb_pb_h)
{
    /* Align MVs on 8x8 grid */
    int i, j;

    int x0 = pb_x << LOG2_MIN_CU_S;
    int y0 = pb_y << LOG2_MIN_CU_S;
    int pb_w = nb_pb_w << LOG2_MIN_CU_S;
    int pb_h = nb_pb_h << LOG2_MIN_CU_S;

    OVMV *dst_mv = tmvp_mv;

    /* FIXME check if necessary reset */
    mv.bcw_idx_plus1 = 0;
    mv.prec_amvr = 0;

    for (j = 0; j < pb_h; j += 4) {
        for (i = 0; i < pb_w; i += 4) {
            if (!((x0 + i) & 0x7) && !((y0 + j) & 0x7))
                dst_mv[((x0 + i) >> 3) + ((y0 + j) >> 3) * 16] = mv;
        }
    }
}

#define LF_MV_THRESHOLD 8

/* FIXME DBF MV related could be done at start of DBF processing on CTU
 * when slice is inter
 */


static inline uint8_t
mv_threshold_check(OVMV a, OVMV b)
{
    uint32_t abs_delta_x = abs(a.x - b.x);
    uint32_t abs_delta_y = abs(a.y - b.y);

    uint8_t chk = (abs_delta_x >= LF_MV_THRESHOLD) || (abs_delta_y >= LF_MV_THRESHOLD);

    return chk;
}

static uint64_t
check_dbf_enabled_p(const int16_t *dist_ref_p, const int16_t *dist_ref_q, OVMV mv_p0, OVMV mv_q0)
{
    int16_t ref0_p = dist_ref_p[mv_p0.ref_idx];

    int16_t ref0_q = dist_ref_q[mv_q0.ref_idx];
    #if 0
    uint8_t ref1_p = mv_p1.ref_idx;
    uint8_t ref1_q = mv_q1.ref_idx;

    uint8_t paired_ref_pq  = (ref0_p == ref0_q) && (ref1_p == ref1_q);
    uint8_t swapped_ref_pq = (ref0_p == ref1_q) && (ref1_p == ref0_q);

    /* FIXME ref check can be done on q */
    uint8_t coupled_l0_l1 = ref0_p == ref1_p; // Same L0 & L1
    #endif
    uint8_t bs = 1;

    /* No need to check for both paired and swapped since coupled L0 L1 implies 
     * paired_ref_pq == swapped_ref_pq
     */
    #if 0
    if ((coupled_l0_l1) && (paired_ref_pq)) {
        bs  = mv_threshold_check(mv_q0, mv_p0) || mv_threshold_check(mv_q1, mv_p1); 
        bs &= mv_threshold_check(mv_q1, mv_p0) || mv_threshold_check(mv_q0, mv_p1); 
    } else if (paired_ref_pq){
    #endif
    if (ref0_p == ref0_q) {
        bs  = mv_threshold_check(mv_q0, mv_p0);
    }
        //bs |= mv_threshold_check(mv_q1, mv_p1); 
        #if 0
    } else if (swapped_ref_pq) {
        bs  = mv_threshold_check(mv_q1, mv_p0); 
        bs |= mv_threshold_check(mv_q0, mv_p1); 
    }
    #endif

    return (uint64_t)bs;
}

static uint64_t
check_dbf_enabled(const struct InterDRVCtx *const inter_ctx,
                  OVMV mv_p0, OVMV mv_p1, OVMV mv_q0, OVMV mv_q1)
{
    const int16_t *dist_0 = inter_ctx->dist_ref_0;
    const int16_t *dist_1 = inter_ctx->dist_ref_1;

    int16_t ref0_p = dist_0[mv_p0.ref_idx];
    int16_t ref1_p = dist_1[mv_p1.ref_idx];

    int16_t ref0_q = dist_0[mv_q0.ref_idx];
    int16_t ref1_q = dist_1[mv_q1.ref_idx];

    uint8_t paired_ref_pq  = (ref0_p == ref0_q) && (ref1_p == ref1_q);
    uint8_t swapped_ref_pq = (ref0_p == ref1_q) && (ref1_p == ref0_q);

    /* FIXME ref check can be done on q */
    uint8_t coupled_l0_l1 = ref0_p == ref1_p; // Same L0 & L1
    uint8_t bs = 1;

    /* No need to check for both paired and swapped since coupled L0 L1 implies 
     * paired_ref_pq == swapped_ref_pq
     */
    if ((coupled_l0_l1) && (paired_ref_pq)) {
        bs  = mv_threshold_check(mv_q0, mv_p0) || mv_threshold_check(mv_q1, mv_p1); 
        bs &= mv_threshold_check(mv_q1, mv_p0) || mv_threshold_check(mv_q0, mv_p1); 
    } else if (paired_ref_pq){
        bs  = mv_threshold_check(mv_q0, mv_p0);
        bs |= mv_threshold_check(mv_q1, mv_p1); 
    } else if (swapped_ref_pq) {
        bs  = mv_threshold_check(mv_q1, mv_p0); 
        bs |= mv_threshold_check(mv_q0, mv_p1); 
    }

    return (uint64_t)bs;
}

static void
dbf_mv_check_abv_p(const struct InterDRVCtx *const inter_ctx,
                   struct DBFInfo *const dbf_info,
                   struct OVMVCtx *const mv_ctx0, struct OVMVCtx *const mv_ctx1,
                   OVMV mv0,
                   int x0_unit, int y0_unit,
                   int nb_unit_w, int nb_unit_h)
{
    uint64_t unit_msk_w = (uint64_t)((uint64_t)1 << nb_unit_w) - 1llu;

    uint64_t abv0_msk = (mv_ctx0->map.hfield[y0_unit] >> (x0_unit + 1)) & unit_msk_w;
    uint64_t abv1_msk = (mv_ctx1->map.hfield[y0_unit] >> (x0_unit + 1)) & unit_msk_w;

    uint64_t bs1_map_h = (dbf_info->bs1_map.hor[y0_unit] >> (2 + x0_unit)) & unit_msk_w;
    const int16_t *dist_ref_q = mv_ctx0 == &inter_ctx->mv_ctx0 ? inter_ctx->dist_ref_0
                                                               : inter_ctx->dist_ref_1;

    /* Avoid checking already set bs1 or bs2.
     * Note if no MV in map the other PU is intra so boundary strength is already 2
     * This way we only check condition on MVs when required
     */
    uint64_t chk_abv = (abv0_msk ^ abv1_msk) & (~bs1_map_h);

    /* Init checked boundary strength map part to 0 it will be disabled if all MVs
     * are in the same ref * and the delta MVs are inferior to
     * integer MV precision.
     */
    uint64_t dst_map_h = (~chk_abv) & unit_msk_w;

    if (chk_abv) {
        const OVMV *mv_abv0 = &mv_ctx0->mvs[PB_POS_IN_BUF(x0_unit, y0_unit - 1)];
        const OVMV *mv_abv1 = &mv_ctx1->mvs[PB_POS_IN_BUF(x0_unit, y0_unit - 1)];
        uint8_t pos_shift = 0;
        do {
            uint8_t nb_skipped_blk = ov_ctz64(chk_abv);
            abv0_msk >>= nb_skipped_blk;
            uint8_t is_l0 = abv0_msk & 0x1;
            const int16_t *dist_ref_p = is_l0 ? ((mv_ctx0 == &inter_ctx->mv_ctx0) ? inter_ctx->dist_ref_0 : inter_ctx->dist_ref_1) : ((mv_ctx0 == &inter_ctx->mv_ctx1) ? inter_ctx->dist_ref_0 : inter_ctx->dist_ref_1);

            mv_abv0   += nb_skipped_blk;
            mv_abv1   += nb_skipped_blk;

            pos_shift += nb_skipped_blk;

            uint64_t abv_th = check_dbf_enabled_p(dist_ref_p, dist_ref_q, is_l0 ? *mv_abv0 : *mv_abv1, mv0);

            dst_map_h |= abv_th << pos_shift;

            mv_abv0++;
            mv_abv1++;

            pos_shift++;

            abv0_msk >>= 1;
            chk_abv >>= nb_skipped_blk + 1;

        } while (chk_abv);
    }

    dbf_info->bs1_map.hor[y0_unit] |= (bs1_map_h | dst_map_h) << (x0_unit + 2);
}

static void
dbf_mv_check_lft_p(const struct InterDRVCtx *const inter_ctx,
                   struct DBFInfo *const dbf_info,
                   struct OVMVCtx *const mv_ctx0, struct OVMVCtx *const mv_ctx1,
                   OVMV mv0,
                   int x0_unit, int y0_unit,
                   int nb_unit_w, int nb_unit_h)
{
    uint64_t unit_msk_h = (uint64_t)((uint64_t)1 << nb_unit_h) - 1llu;

    uint64_t lft0_msk = (mv_ctx0->map.vfield[x0_unit] >> (y0_unit + 1)) & unit_msk_h;
    uint64_t lft1_msk = (mv_ctx1->map.vfield[x0_unit] >> (y0_unit + 1)) & unit_msk_h;

    uint64_t bs1_map_v = (dbf_info->bs1_map.ver[x0_unit] >> y0_unit)       & unit_msk_h;
    const int16_t *dist_ref_q = mv_ctx0 == &inter_ctx->mv_ctx0 ? inter_ctx->dist_ref_0
                                                               : inter_ctx->dist_ref_1;

    /* Avoid checking already set bs1 or bs2.
     * Note if no MV in map the other PU is intra so boundary strength is already 2
     * This way we only check condition on MVs when required
     */
    uint64_t chk_lft = (lft0_msk ^ lft1_msk) & (~bs1_map_v);

    /* Init checked boundary strength map part to 0 it will be disabled if all MVs
     * are in the same ref * and the delta MVs are inferior to
     * integer MV precision.
     */
    uint64_t dst_map_v = (~chk_lft) & unit_msk_h;

    if (chk_lft) {
        const OVMV *mv_lft0 = &mv_ctx0->mvs[PB_POS_IN_BUF(x0_unit - 1, y0_unit)];
        const OVMV *mv_lft1 = &mv_ctx1->mvs[PB_POS_IN_BUF(x0_unit - 1, y0_unit)];

        uint8_t pos_shift = 0;
        do {
            uint8_t nb_skipped_blk = ov_ctz64(chk_lft);

            lft0_msk >>= nb_skipped_blk;

            uint8_t is_l0 = lft0_msk & 0x1;
            const int16_t *dist_ref_p = is_l0 ? ((mv_ctx0 == &inter_ctx->mv_ctx0) ? inter_ctx->dist_ref_0 : inter_ctx->dist_ref_1) : ((mv_ctx0 == &inter_ctx->mv_ctx1) ? inter_ctx->dist_ref_0 : inter_ctx->dist_ref_1);

            mv_lft0   += 34 * nb_skipped_blk;
            mv_lft1   += 34 * nb_skipped_blk;

            pos_shift += nb_skipped_blk;

            uint64_t lft_th = check_dbf_enabled_p(dist_ref_p, dist_ref_q, is_l0 ? *mv_lft0 : *mv_lft1, mv0);

            dst_map_v |= lft_th << pos_shift;

            mv_lft0 += 34;
            mv_lft1 += 34;

            pos_shift++;

            lft0_msk >>= 1;
            chk_lft  >>= nb_skipped_blk + 1;

        } while (chk_lft);
    }
    dbf_info->bs1_map.ver[x0_unit] |= dst_map_v << y0_unit;
}

static void
dbf_mv_check_p(const struct InterDRVCtx *const inter_ctx,
               struct DBFInfo *const dbf_info,
               struct OVMVCtx *const mv_ctx0, struct OVMVCtx *const mv_ctx1,
               OVMV mv0,
               int x0_unit, int y0_unit,
               int nb_unit_w, int nb_unit_h)
{
    uint64_t unit_msk_w = (uint64_t)((uint64_t)1 << nb_unit_w) - 1llu;
    uint64_t unit_msk_h = (uint64_t)((uint64_t)1 << nb_unit_h) - 1llu;

    uint64_t abv0_msk = (mv_ctx0->map.hfield[y0_unit] >> (x0_unit + 1)) & unit_msk_w;
    uint64_t abv1_msk = (mv_ctx1->map.hfield[y0_unit] >> (x0_unit + 1)) & unit_msk_w;

    uint64_t lft0_msk = (mv_ctx0->map.vfield[x0_unit] >> (y0_unit + 1)) & unit_msk_h;
    uint64_t lft1_msk = (mv_ctx1->map.vfield[x0_unit] >> (y0_unit + 1)) & unit_msk_h;

    uint64_t bs1_map_h = (dbf_info->bs1_map.hor[y0_unit] >> (2 + x0_unit)) & unit_msk_w;
    uint64_t bs1_map_v = (dbf_info->bs1_map.ver[x0_unit] >> y0_unit)       & unit_msk_h;
    const int16_t *dist_ref_q = mv_ctx0 == &inter_ctx->mv_ctx0 ? inter_ctx->dist_ref_0
                                                               : inter_ctx->dist_ref_1;

    /* Avoid checking already set bs1 or bs2.
     * Note if no MV in map the other PU is intra so boundary strength is already 2
     * This way we only check condition on MVs when required
     */
    uint64_t chk_abv = (abv0_msk ^ abv1_msk) & (~bs1_map_h);
    uint64_t chk_lft = (lft0_msk ^ lft1_msk) & (~bs1_map_v);

    /* Init checked boundary strength map part to 0 it will be disabled if all MVs
     * are in the same ref * and the delta MVs are inferior to
     * integer MV precision.
     */
    uint64_t dst_map_h = (~chk_abv) & unit_msk_w;
    uint64_t dst_map_v = (~chk_lft) & unit_msk_h;

    if (chk_abv) {
        const OVMV *mv_abv0 = &mv_ctx0->mvs[PB_POS_IN_BUF(x0_unit, y0_unit - 1)];
        const OVMV *mv_abv1 = &mv_ctx1->mvs[PB_POS_IN_BUF(x0_unit, y0_unit - 1)];
        uint8_t pos_shift = 0;
        do {
            uint8_t nb_skipped_blk = ov_ctz64(chk_abv);
            abv0_msk >>= nb_skipped_blk;
            uint8_t is_l0 = abv0_msk & 0x1;
            const int16_t *dist_ref_p = is_l0 ? ((mv_ctx0 == &inter_ctx->mv_ctx0) ? inter_ctx->dist_ref_0 : inter_ctx->dist_ref_1) : ((mv_ctx0 == &inter_ctx->mv_ctx1) ? inter_ctx->dist_ref_0 : inter_ctx->dist_ref_1);

            mv_abv0   += nb_skipped_blk;
            mv_abv1   += nb_skipped_blk;

            pos_shift += nb_skipped_blk;

            uint64_t abv_th = check_dbf_enabled_p(dist_ref_p, dist_ref_q, is_l0 ? *mv_abv0 : *mv_abv1, mv0);

            dst_map_h |= abv_th << pos_shift;

            mv_abv0++;
            mv_abv1++;

            pos_shift++;

            abv0_msk >>= 1;
            chk_abv >>= nb_skipped_blk + 1;

        } while (chk_abv);
    }

    if (chk_lft) {
        const OVMV *mv_lft0 = &mv_ctx0->mvs[PB_POS_IN_BUF(x0_unit - 1, y0_unit)];
        const OVMV *mv_lft1 = &mv_ctx1->mvs[PB_POS_IN_BUF(x0_unit - 1, y0_unit)];

        uint8_t pos_shift = 0;
        do {
            uint8_t nb_skipped_blk = ov_ctz64(chk_lft);

            lft0_msk >>= nb_skipped_blk;

            uint8_t is_l0 = lft0_msk & 0x1;
            const int16_t *dist_ref_p = is_l0 ? ((mv_ctx0 == &inter_ctx->mv_ctx0) ? inter_ctx->dist_ref_0 : inter_ctx->dist_ref_1) : ((mv_ctx0 == &inter_ctx->mv_ctx1) ? inter_ctx->dist_ref_0 : inter_ctx->dist_ref_1);

            mv_lft0   += 34 * nb_skipped_blk;
            mv_lft1   += 34 * nb_skipped_blk;

            pos_shift += nb_skipped_blk;

            uint64_t lft_th = check_dbf_enabled_p(dist_ref_p, dist_ref_q, is_l0 ? *mv_lft0 : *mv_lft1, mv0);

            dst_map_v |= lft_th << pos_shift;

            mv_lft0 += 34;
            mv_lft1 += 34;

            pos_shift++;

            lft0_msk >>= 1;
            chk_lft  >>= nb_skipped_blk + 1;

        } while (chk_lft);
    }
    dbf_info->bs1_map.hor[y0_unit] |= (bs1_map_h | dst_map_h) << (x0_unit + 2);
    dbf_info->bs1_map.ver[x0_unit] |= dst_map_v << y0_unit;
}

static void
dbf_mv_check_abv_b(const struct InterDRVCtx *const inter_ctx,
                   struct DBFInfo *const dbf_info,
                   struct OVMVCtx *const mv_ctx0, struct OVMVCtx *const mv_ctx1,
                   OVMV mv0, OVMV mv1,
                   int x0_unit, int y0_unit,
                   int nb_unit_w, int nb_unit_h)
{
    uint64_t unit_msk_w = (uint64_t)((uint64_t)1 << nb_unit_w) - 1llu;

    uint64_t abv0_msk = (mv_ctx0->map.hfield[y0_unit] >> (x0_unit + 1)) & unit_msk_w;
    uint64_t abv1_msk = (mv_ctx1->map.hfield[y0_unit] >> (x0_unit + 1)) & unit_msk_w;

    uint64_t bs1_map_h = (dbf_info->bs1_map.hor[y0_unit] >> (2 + x0_unit)) & unit_msk_w;

    /* Note if no MV in map the other PU is intra so boundary strength is already 2
     * This way we only check condition on MVs when required
     */
    uint64_t chk_abv = (abv0_msk & abv1_msk) & (~bs1_map_h);

    /* Init checked boundary strength map part to 0 it will be disabled if all MVs
     * are in the same ref * and the delta MVs are inferior to
     * integer MV precision.
     */
    uint64_t dst_map_h = (~chk_abv) & unit_msk_w;

    if (chk_abv) {
        const OVMV *mv_abv0 = &mv_ctx0->mvs[PB_POS_IN_BUF(x0_unit, y0_unit - 1)];
        const OVMV *mv_abv1 = &mv_ctx1->mvs[PB_POS_IN_BUF(x0_unit, y0_unit - 1)];
        uint8_t pos_shift = 0;
        do {
            uint8_t nb_skipped_blk = ov_ctz64(chk_abv);

            mv_abv0   += nb_skipped_blk;
            mv_abv1   += nb_skipped_blk;
            pos_shift += nb_skipped_blk;

            uint64_t abv_th = check_dbf_enabled(inter_ctx, *mv_abv0, *mv_abv1, mv0, mv1);

            dst_map_h |= abv_th << pos_shift;

            mv_abv0++;
            mv_abv1++;
            pos_shift++;

            chk_abv >>= nb_skipped_blk + 1;

        } while (chk_abv);
    }

    dbf_info->bs1_map.hor[y0_unit] |= dst_map_h << (x0_unit + 2);
}

static void
dbf_mv_check_lft_b(const struct InterDRVCtx *const inter_ctx,
                   struct DBFInfo *const dbf_info,
                   struct OVMVCtx *const mv_ctx0, struct OVMVCtx *const mv_ctx1,
                   OVMV mv0, OVMV mv1,
                   int x0_unit, int y0_unit,
                   int nb_unit_w, int nb_unit_h)
{
    uint64_t unit_msk_h = (uint64_t)((uint64_t)1 << nb_unit_h) - 1llu;

    uint64_t lft0_msk = (mv_ctx0->map.vfield[x0_unit] >> (y0_unit + 1)) & unit_msk_h;
    uint64_t lft1_msk = (mv_ctx1->map.vfield[x0_unit] >> (y0_unit + 1)) & unit_msk_h;

    uint64_t bs1_map_v = (dbf_info->bs1_map.ver[x0_unit] >> y0_unit)       & unit_msk_h;

    /* Note if no MV in map the other PU is intra so boundary strength is already 2
     * This way we only check condition on MVs when required
     */
    uint64_t chk_lft = (lft0_msk & lft1_msk) & (~bs1_map_v);

    /* Init checked boundary strength map part to 0 it will be disabled if all MVs
     * are in the same ref * and the delta MVs are inferior to
     * integer MV precision.
     */
    uint64_t dst_map_v = (~chk_lft) & unit_msk_h;

    if (chk_lft) {
        const OVMV *mv_lft0 = &mv_ctx0->mvs[PB_POS_IN_BUF(x0_unit - 1, y0_unit)];
        const OVMV *mv_lft1 = &mv_ctx1->mvs[PB_POS_IN_BUF(x0_unit - 1, y0_unit)];

        uint8_t pos_shift = 0;
        do {
            uint8_t nb_skipped_blk = ov_ctz64(chk_lft);
            mv_lft0   += 34 * nb_skipped_blk;
            mv_lft1   += 34 * nb_skipped_blk;
            pos_shift += nb_skipped_blk;

            uint64_t lft_th = check_dbf_enabled(inter_ctx, *mv_lft0, *mv_lft1, mv0, mv1);

            dst_map_v |= lft_th << pos_shift;

            mv_lft0 += 34;
            mv_lft1 += 34;
            pos_shift++;

            chk_lft >>= nb_skipped_blk + 1;

        } while (chk_lft);
    }
    dbf_info->bs1_map.ver[x0_unit] |= dst_map_v << y0_unit;
}

static void
dbf_mv_check_b(const struct InterDRVCtx *const inter_ctx,
               struct DBFInfo *const dbf_info,
               struct OVMVCtx *const mv_ctx0, struct OVMVCtx *const mv_ctx1,
               OVMV mv0, OVMV mv1,
               int x0_unit, int y0_unit,
               int nb_unit_w, int nb_unit_h)
{
    uint64_t unit_msk_w = (uint64_t)((uint64_t)1 << nb_unit_w) - 1llu;
    uint64_t unit_msk_h = (uint64_t)((uint64_t)1 << nb_unit_h) - 1llu;

    uint64_t abv0_msk = (mv_ctx0->map.hfield[y0_unit] >> (x0_unit + 1)) & unit_msk_w;
    uint64_t lft0_msk = (mv_ctx0->map.vfield[x0_unit] >> (y0_unit + 1)) & unit_msk_h;

    uint64_t abv1_msk = (mv_ctx1->map.hfield[y0_unit] >> (x0_unit + 1)) & unit_msk_w;
    uint64_t lft1_msk = (mv_ctx1->map.vfield[x0_unit] >> (y0_unit + 1)) & unit_msk_h;

    uint64_t bs1_map_h = (dbf_info->bs1_map.hor[y0_unit] >> (2 + x0_unit)) & unit_msk_w;
    uint64_t bs1_map_v = (dbf_info->bs1_map.ver[x0_unit] >> y0_unit)       & unit_msk_h;

    /* Note if no MV in map the other PU is intra so boundary strength is already 2
     * This way we only check condition on MVs when required
     */
    uint64_t chk_abv = (abv0_msk & abv1_msk) & (~bs1_map_h);
    uint64_t chk_lft = (lft0_msk & lft1_msk) & (~bs1_map_v);

    /* Init checked boundary strength map part to 0 it will be disabled if all MVs
     * are in the same ref * and the delta MVs are inferior to
     * integer MV precision.
     */
    uint64_t dst_map_h = (~chk_abv) & unit_msk_w;
    uint64_t dst_map_v = (~chk_lft) & unit_msk_h;

    if (chk_abv) {
        const OVMV *mv_abv0 = &mv_ctx0->mvs[PB_POS_IN_BUF(x0_unit, y0_unit - 1)];
        const OVMV *mv_abv1 = &mv_ctx1->mvs[PB_POS_IN_BUF(x0_unit, y0_unit - 1)];
        uint8_t pos_shift = 0;
        do {
            uint8_t nb_skipped_blk = ov_ctz64(chk_abv);

            mv_abv0   += nb_skipped_blk;
            mv_abv1   += nb_skipped_blk;
            pos_shift += nb_skipped_blk;

            uint64_t abv_th = check_dbf_enabled(inter_ctx, *mv_abv0, *mv_abv1, mv0, mv1);

            dst_map_h |= abv_th << pos_shift;

            mv_abv0++;
            mv_abv1++;
            pos_shift++;

            chk_abv >>= nb_skipped_blk + 1;

        } while (chk_abv);
    }

    if (chk_lft) {
        const OVMV *mv_lft0 = &mv_ctx0->mvs[PB_POS_IN_BUF(x0_unit - 1, y0_unit)];
        const OVMV *mv_lft1 = &mv_ctx1->mvs[PB_POS_IN_BUF(x0_unit - 1, y0_unit)];

        uint8_t pos_shift = 0;
        do {
            uint8_t nb_skipped_blk = ov_ctz64(chk_lft);
            mv_lft0   += 34 * nb_skipped_blk;
            mv_lft1   += 34 * nb_skipped_blk;
            pos_shift += nb_skipped_blk;

            uint64_t lft_th = check_dbf_enabled(inter_ctx, *mv_lft0, *mv_lft1, mv0, mv1);

            dst_map_v |= lft_th << pos_shift;

            mv_lft0 += 34;
            mv_lft1 += 34;
            pos_shift++;

            chk_lft >>= nb_skipped_blk + 1;

        } while (chk_lft);
    }
    dbf_info->bs1_map.hor[y0_unit] |= dst_map_h << (x0_unit + 2);
    dbf_info->bs1_map.ver[x0_unit] |= dst_map_v << y0_unit;
}

static void
update_gpm_mv_ctx_b(struct InterDRVCtx *const inter_ctx,
                    const OVMV mv0, const OVMV mv1,
                    uint8_t pb_x, uint8_t  pb_y,
                    uint8_t nb_pb_w, uint8_t nb_pb_h,
                    uint8_t inter_dir)
{
    /*FIXME Use specific DBF update function if DBF is disabled */
    /*FIXME Find a better way to retrieve dbf_info */
    if (inter_dir == 3) {
        struct OVMVCtx *const mv_ctx0 = &inter_ctx->mv_ctx0;
        struct OVMVCtx *const mv_ctx1 = &inter_ctx->mv_ctx1;

        fill_tmvp_map(inter_ctx->tmvp_mv[0].mvs, mv0, pb_x, pb_y, nb_pb_w, nb_pb_h);
        fill_tmvp_map(inter_ctx->tmvp_mv[1].mvs, mv1, pb_x, pb_y, nb_pb_w, nb_pb_h);

        fill_mvp_map(mv_ctx0, mv0, pb_x, pb_y, nb_pb_w, nb_pb_h);
        fill_mvp_map(mv_ctx1, mv1, pb_x, pb_y, nb_pb_w, nb_pb_h);

    } else if (inter_dir & 0x2) {
        struct OVMVCtx *const mv_ctx1 = &inter_ctx->mv_ctx1;

        fill_tmvp_map(inter_ctx->tmvp_mv[1].mvs, mv1, pb_x, pb_y, nb_pb_w, nb_pb_h);

        fill_mvp_map(mv_ctx1, mv1, pb_x, pb_y, nb_pb_w, nb_pb_h);

    } else if (inter_dir & 0x1) {
        struct OVMVCtx *const mv_ctx0 = &inter_ctx->mv_ctx0;

        fill_tmvp_map(inter_ctx->tmvp_mv[0].mvs, mv0, pb_x, pb_y, nb_pb_w, nb_pb_h);

        fill_mvp_map(mv_ctx0, mv0, pb_x, pb_y, nb_pb_w, nb_pb_h);
    }
}

static void
update_mv_ctx_b(struct InterDRVCtx *const inter_ctx,
                const OVMV mv0, const OVMV mv1,
                uint8_t pb_x, uint8_t  pb_y,
                uint8_t nb_pb_w, uint8_t nb_pb_h,
                uint8_t inter_dir)
{
    /*FIXME Use specific DBF update function if DBF is disabled */
    /*FIXME Find a better way to retrieve dbf_info */
    struct DBFInfo *const dbf_info = &inter_ctx->tmvp_ctx.ctudec->dbf_info;
    if (inter_dir == 3) {
        struct OVMVCtx *const mv_ctx0 = &inter_ctx->mv_ctx0;
        struct OVMVCtx *const mv_ctx1 = &inter_ctx->mv_ctx1;

        fill_tmvp_map(inter_ctx->tmvp_mv[0].mvs, mv0, pb_x, pb_y, nb_pb_w, nb_pb_h);
        fill_tmvp_map(inter_ctx->tmvp_mv[1].mvs, mv1, pb_x, pb_y, nb_pb_w, nb_pb_h);

        fill_mvp_map(mv_ctx0, mv0, pb_x, pb_y, nb_pb_w, nb_pb_h);
        fill_mvp_map(mv_ctx1, mv1, pb_x, pb_y, nb_pb_w, nb_pb_h);

        dbf_mv_check_b(inter_ctx, dbf_info, mv_ctx0, mv_ctx1, mv0, mv1, pb_x, pb_y, nb_pb_w, nb_pb_h);

    } else if (inter_dir & 0x2) {
        struct OVMVCtx *const mv_ctx0 = &inter_ctx->mv_ctx0;
        struct OVMVCtx *const mv_ctx1 = &inter_ctx->mv_ctx1;

        fill_tmvp_map(inter_ctx->tmvp_mv[1].mvs, mv1, pb_x, pb_y, nb_pb_w, nb_pb_h);

        fill_mvp_map(mv_ctx1, mv1, pb_x, pb_y, nb_pb_w, nb_pb_h);

        dbf_mv_check_p(inter_ctx, dbf_info, mv_ctx1, mv_ctx0, mv1, pb_x, pb_y, nb_pb_w, nb_pb_h);

    } else if (inter_dir & 0x1) {
        struct OVMVCtx *const mv_ctx0 = &inter_ctx->mv_ctx0;
        struct OVMVCtx *const mv_ctx1 = &inter_ctx->mv_ctx1;

        fill_tmvp_map(inter_ctx->tmvp_mv[0].mvs, mv0, pb_x, pb_y, nb_pb_w, nb_pb_h);

        fill_mvp_map(mv_ctx0, mv0, pb_x, pb_y, nb_pb_w, nb_pb_h);

        dbf_mv_check_p(inter_ctx, dbf_info, mv_ctx0, mv_ctx1, mv0, pb_x, pb_y, nb_pb_w, nb_pb_h);
    }
}

static void
gpm_dbf_check(struct InterDRVCtx *const inter_ctx,
              uint8_t pb_x, uint8_t pb_y,
              uint8_t nb_pb_w, uint8_t nb_pb_h)
{
    struct DBFInfo *const dbf_info = &inter_ctx->tmvp_ctx.ctudec->dbf_info;
    struct OVMVCtx *const mv_ctx0 = &inter_ctx->mv_ctx0;
    struct OVMVCtx *const mv_ctx1 = &inter_ctx->mv_ctx1;

    uint64_t msk0_v = mv_ctx0->map.vfield[pb_x + 1] >> (1 + pb_y);
    uint64_t msk1_v = mv_ctx1->map.vfield[pb_x + 1] >> (1 + pb_y);

    uint64_t msk0_h = mv_ctx0->map.hfield[pb_y + 1] >> (1 + pb_x);
    uint64_t msk1_h = mv_ctx1->map.hfield[pb_y + 1] >> (1 + pb_x);

    int i;

    for (i = 0; i < nb_pb_w; ++i) {
        OVMV mv0 = mv_ctx0->mvs[35 + pb_x + i + pb_y * 34];
        OVMV mv1 = mv_ctx1->mvs[35 + pb_x + i + pb_y * 34];
        uint8_t inter_dir = (msk0_h & 0x1) | ((msk1_h & 0x1) << 1);

        if (inter_dir == 3) {
            dbf_mv_check_abv_b(inter_ctx, dbf_info, mv_ctx0, mv_ctx1, mv0, mv1, pb_x + i, pb_y, 1, 1);
        } else if (inter_dir & 0x2) {
            dbf_mv_check_abv_p(inter_ctx, dbf_info, mv_ctx1, mv_ctx0, mv1, pb_x + i, pb_y, 1, 1);

        } else if (inter_dir & 0x1) {
            dbf_mv_check_abv_p(inter_ctx, dbf_info, mv_ctx0, mv_ctx1, mv0, pb_x + i, pb_y, 1, 1);
        }
        msk0_h >>= 1;
        msk1_h >>= 1;
    }

    for (i = 0; i < nb_pb_h; ++i) {
        OVMV mv0 = mv_ctx0->mvs[35 + pb_x + (pb_y + i) * 34];
        OVMV mv1 = mv_ctx1->mvs[35 + pb_x + (pb_y + i) * 34];
        uint8_t inter_dir = (msk0_v & 0x1) | ((msk1_v & 0x1) << 1);
        if (inter_dir == 3) {
            dbf_mv_check_lft_b(inter_ctx, dbf_info, mv_ctx0, mv_ctx1, mv0, mv1, pb_x, pb_y + i, 1, 1);
        } else if (inter_dir & 0x2) {
            dbf_mv_check_lft_p(inter_ctx, dbf_info, mv_ctx1, mv_ctx0, mv1, pb_x, pb_y + i, 1, 1);

        } else if (inter_dir & 0x1) {
            dbf_mv_check_lft_p(inter_ctx, dbf_info, mv_ctx0, mv_ctx1, mv0, pb_x, pb_y + i, 1, 1);
        }
        msk0_v >>= 1;
        msk1_v >>= 1;
    }
}


static void
update_mv_ctx(struct InterDRVCtx *const inter_ctx,
              const OVMV mv,
              uint8_t pb_x, uint8_t  pb_y,
              uint8_t nb_pb_w, uint8_t nb_pb_h,
              uint8_t inter_dir)
{
    /*FIXME Use specific DBF update function if DBF is disabled */
    /*FIXME Find a better way to retrieve dbf_info */
    struct DBFInfo *const dbf_info = &inter_ctx->tmvp_ctx.ctudec->dbf_info;
    if (inter_dir & 0x2) {
        struct OVMVCtx *const mv_ctx0 = &inter_ctx->mv_ctx0;
        struct OVMVCtx *const mv_ctx1 = &inter_ctx->mv_ctx1;

        fill_tmvp_map(inter_ctx->tmvp_mv[1].mvs, mv, pb_x, pb_y, nb_pb_w, nb_pb_h);

        fill_mvp_map(mv_ctx1, mv, pb_x, pb_y, nb_pb_w, nb_pb_h);

        dbf_mv_check_p(inter_ctx, dbf_info, mv_ctx1, mv_ctx0, mv, pb_x, pb_y, nb_pb_w, nb_pb_h);

    } else if (inter_dir & 0x1) {
        struct OVMVCtx *const mv_ctx0 = &inter_ctx->mv_ctx0;
        struct OVMVCtx *const mv_ctx1 = &inter_ctx->mv_ctx1;

        fill_tmvp_map(inter_ctx->tmvp_mv[0].mvs, mv, pb_x, pb_y, nb_pb_w, nb_pb_h);

        fill_mvp_map(mv_ctx0, mv, pb_x, pb_y, nb_pb_w, nb_pb_h);

        dbf_mv_check_p(inter_ctx, dbf_info, mv_ctx0, mv_ctx1, mv, pb_x, pb_y, nb_pb_w, nb_pb_h);

    }

    hmvp_update_lut_b(&inter_ctx->hmvp_lut, mv, mv, 0x1);
}

static void
update_gpm_mv_ctx(struct InterDRVCtx *const inter_ctx,
                const OVMV mv0, const OVMV mv1,
                VVCMergeInfo mv_info0, VVCMergeInfo mv_info1,
                uint8_t pb_x, uint8_t  pb_y,
                uint8_t nb_pb_w, uint8_t nb_pb_h,
                uint8_t inter_dir0, uint8_t inter_dir1)
{   
    VVCMergeInfo mv_info;
    const struct VVCGPM *const gpm_info = &inter_ctx->gpm_ctx;

    uint8_t inter_dir = inter_dir0 | inter_dir1;

    /* FIXME can probably be simplified
     * check usage from loop
     */
    if (inter_dir == 0x1) {
        mv_info.mv0 = mv_info1.mv0;
    } else if (inter_dir == 0x2) {
        mv_info.mv1 = mv_info1.mv1;
    } else {
        if (inter_dir0 == 1 && inter_dir1 == 2) {
            mv_info.mv0     = mv_info0.mv0;
            mv_info.mv1     = mv_info1.mv1;
        } else if (inter_dir0 == 2 && inter_dir1 == 1) {
            mv_info.mv0     = mv_info1.mv0;
            mv_info.mv1     = mv_info0.mv1;
        }
    }

    mv_info.inter_dir = inter_dir;

    int split_dir = gpm_info->split_dir;

    int16_t angle = g_GeoParams[split_dir][0];

    int d_idx = g_GeoParams[split_dir][1];

    int x_dis = g_Dis[angle];
    int y_dis = g_Dis[(angle + (GEO_NUM_ANGLES >> 2)) % GEO_NUM_ANGLES];

    uint8_t flip = angle >= 13 && angle <= 27;

    /* FIXME use absolute coordinates instead */
    int offset_x = (-(int)nb_pb_w * 4) >> 1;
    int offset_y = (-(int)nb_pb_h * 4) >> 1;

    if (d_idx > 0) {
        if ((angle & 0xF) == 8 || ((angle & 0xF) && nb_pb_h >= nb_pb_w)) {
            offset_y += angle < 16 ? ((d_idx * nb_pb_h) >> 1) : -(int)((d_idx * nb_pb_h) >> 1);
        } else {
            offset_x += angle < 16 ? ((d_idx * nb_pb_w) >> 1) : -(int)((d_idx * nb_pb_w) >> 1);
        }
    }

    for (int y = 0; y < nb_pb_h; y++) {
        int lookup_y = (((4 * y + offset_y) << 1) + 5) * y_dis;

        for (int x = 0; x < nb_pb_w; x++) {
            int motion_idx = (((4 * x + offset_x) << 1) + 5) * x_dis + lookup_y;
            int tpm_mask = abs(motion_idx) < 32 ? 2 : (motion_idx <= 0 ? (1 - flip) : flip);

            if (tpm_mask == 2) {

                #if 0
                if (mv_info.inter_dir == 1) {
                    mv_info.mv1.x = mv_info.mv1.y = 0;
                } else if (mv_info.inter_dir == 2) {
                    mv_info.mv0.x = mv_info.mv0.y = 0;
                }
                #endif

                update_gpm_mv_ctx_b(inter_ctx, mv_info.mv0, mv_info.mv1, pb_x + x, pb_y + y, 
                                1, 1, mv_info.inter_dir);

            } else if (tpm_mask == 0) {

                #if 0
                if (inter_dir0 == 1) {
                    mv_info0.mv1.x = mv_info0.mv1.y = 0;
                } else if (inter_dir0 == 2) {
                    mv_info0.mv0.x = mv_info0.mv0.y = 0;
                }
                #endif

                update_gpm_mv_ctx_b(inter_ctx, mv_info0.mv0, mv_info0.mv1, pb_x + x, pb_y + y, 
                                    1, 1, inter_dir0);
            } else {

                #if 0
                if (inter_dir1 == 1) {
                    mv_info1.mv1.x = mv_info1.mv1.y = 0;
                } else if (inter_dir1 == 2) {
                    mv_info1.mv0.x = mv_info1.mv0.y = 0;
                }
                #endif

                update_gpm_mv_ctx_b(inter_ctx, mv_info1.mv0, mv_info1.mv1, pb_x + x, pb_y + y, 
                                    1, 1, inter_dir1);
            }
        }
    }
    gpm_dbf_check(inter_ctx, pb_x, pb_y, nb_pb_w, nb_pb_h);
}


/* Derive motion vectors and update motion maps */
VVCMergeInfo
drv_mvp_b(struct InterDRVCtx *const inter_ctx,
          uint8_t x0, uint8_t y0,
          uint8_t log2_cb_w, uint8_t log2_cb_h,
          OVMV mvd0, OVMV mvd1, int prec_amvr,
          uint8_t mvp_idx0, uint8_t mvp_idx1, uint8_t bcw_idx,
          uint8_t inter_dir, uint8_t ref_idx0, uint8_t ref_idx1,
          uint8_t is_small)
{
    OVMV mv0 = {0}, mv1 = {0};
    VVCMergeInfo mv_info;
    uint8_t x0_unit = x0 >> LOG2_MIN_CU_S;
    uint8_t y0_unit = y0 >> LOG2_MIN_CU_S;
    uint8_t nb_unit_w = (1 << log2_cb_w) >> LOG2_MIN_CU_S;
    uint8_t nb_unit_h = (1 << log2_cb_h) >> LOG2_MIN_CU_S;

    uint8_t opp_ref_idx0 = 0xFF;
    uint8_t opp_ref_idx1 = 0xFF;

    for (int i = 0; i < inter_ctx->nb_active_ref1; i ++) {
         if (inter_ctx->rpl0[ref_idx0] == inter_ctx->rpl1[i])
             opp_ref_idx0 = i;
    }

    for (int i = 0; i < inter_ctx->nb_active_ref0; i ++) {
         if (inter_ctx->rpl1[ref_idx1] == inter_ctx->rpl0[i])
             opp_ref_idx1 = i;
    }

    /* FIXME can we combine mvp derivation for bi pred */
    if (inter_dir & 0x1) {
        struct OVMVCtx *const mv_ctx0 = &inter_ctx->mv_ctx0;
        struct OVMVCtx *const mv_ctx1 = &inter_ctx->mv_ctx1;

        mv0 = derive_mvp_candidates_1(inter_ctx, mv_ctx0, ref_idx0,
                                      x0_unit, y0_unit, nb_unit_w, nb_unit_h,
                                      mvp_idx0, inter_dir & 0x1, mv_ctx1, opp_ref_idx0,
                                      prec_amvr, is_small);

        mvd0 = drv_change_precision_mv(mvd0, prec_amvr, MV_PRECISION_INTERNAL);

        mv0.bcw_idx_plus1 = bcw_idx + 1;
        mv0.prec_amvr = prec_amvr;
        mv0.x += mvd0.x;
        mv0.y += mvd0.y;
        mv0.ref_idx = ref_idx0;
    }

    if (inter_dir & 0x2) {
        struct OVMVCtx *const mv_ctx1 = &inter_ctx->mv_ctx1;
        struct OVMVCtx *const mv_ctx0 = &inter_ctx->mv_ctx0;

        mv1 = derive_mvp_candidates_1(inter_ctx, mv_ctx1, ref_idx1,
                                      x0_unit, y0_unit,
                                      nb_unit_w, nb_unit_h,
                                      mvp_idx1, inter_dir & 0x2, mv_ctx0, opp_ref_idx1,
                                      prec_amvr, is_small);

        mvd1 = drv_change_precision_mv(mvd1, prec_amvr, MV_PRECISION_INTERNAL);

        mv1.bcw_idx_plus1 = bcw_idx + 1;
        mv1.prec_amvr = prec_amvr;
        mv1.x += mvd1.x;
        mv1.y += mvd1.y;
        mv1.ref_idx = ref_idx1;
    }

    mv_info.inter_dir = inter_dir;
    mv_info.mv0 = mv0;
    mv_info.mv1 = mv1;

    /* Update for next pass */
    update_mv_ctx_b(inter_ctx, mv0, mv1, x0_unit, y0_unit, nb_unit_w,
                    nb_unit_h, inter_dir);

    hmvp_update_lut_b(&inter_ctx->hmvp_lut, mv0, mv1, inter_dir);

    return mv_info;
}


OVMV
drv_mmvd_merge_mvp(struct InterDRVCtx *const inter_ctx,
                   const struct OVMVCtx *const mv_ctx,
                   uint8_t x0, uint8_t y0,
                   uint8_t log2_cb_w, uint8_t log2_cb_h,
                   uint8_t merge_idx, uint8_t max_nb_cand)
{

    static const uint8_t ref_mvd_cands[8] = { 1, 2, 4, 8, 16, 32, 64, 128};
    uint8_t x0_unit = x0 >> LOG2_MIN_CU_S;
    uint8_t y0_unit = y0 >> LOG2_MIN_CU_S;
    uint8_t nb_unit_w = (1 << log2_cb_w) >> LOG2_MIN_CU_S;
    uint8_t nb_unit_h = (1 << log2_cb_h) >> LOG2_MIN_CU_S;

    int smvd_mrg_idx = merge_idx / MMVD_MAX_REFINE_NUM;

    OVMV mv = vvc_derive_merge_mvp(inter_ctx, mv_ctx,
                                   x0_unit, y0_unit,
                                   nb_unit_w, nb_unit_h, smvd_mrg_idx,
                                   max_nb_cand, log2_cb_w + log2_cb_h <= 5);


    OVMV mvd0;

    int f_pos_group = merge_idx / (MMVD_BASE_MV_NUM * MMVD_MAX_REFINE_NUM);

    int idx = merge_idx;
    idx -= f_pos_group  * (MMVD_BASE_MV_NUM * MMVD_MAX_REFINE_NUM);
    idx -= smvd_mrg_idx * (MMVD_MAX_REFINE_NUM);

    int offset = (uint16_t)ref_mvd_cands[(idx >> 2)] << 2;

    offset <<= inter_ctx->mmvd_shift;

    int f_pos = idx - ((idx >> 2) << 2);

    if (f_pos == 0) {
        mvd0.x = offset;
        mvd0.y = 0;
    } else if (f_pos == 1) {
        mvd0.x = -offset;
        mvd0.y = 0;
    } else if (f_pos == 2) {
        mvd0.x = 0;
        mvd0.y = offset;
    } else {
        mvd0.x = 0;
        mvd0.y = -offset;
    }

    mv.x += mvd0.x;
    mv.y += mvd0.y;

    /* Force to RPL_0 */
    update_mv_ctx(inter_ctx, mv, x0_unit, y0_unit,
                    nb_unit_w, nb_unit_h, 0x1);



    return mv;
}


OVMV
drv_merge_mvp(struct InterDRVCtx *const inter_ctx,
              const struct OVMVCtx *const mv_ctx,
              uint8_t x0, uint8_t y0,
              uint8_t log2_cb_w, uint8_t log2_cb_h,
              uint8_t merge_idx, uint8_t max_nb_merge_cand)
{
    uint8_t x0_unit = x0 >> LOG2_MIN_CU_S;
    uint8_t y0_unit = y0 >> LOG2_MIN_CU_S;
    uint8_t nb_unit_w = (1 << log2_cb_w) >> LOG2_MIN_CU_S;
    uint8_t nb_unit_h = (1 << log2_cb_h) >> LOG2_MIN_CU_S;

    OVMV mv0 = vvc_derive_merge_mvp(inter_ctx, mv_ctx, x0_unit, y0_unit,
                                    nb_unit_w, nb_unit_h, merge_idx,
                                    max_nb_merge_cand,  log2_cb_w + log2_cb_h <= 5);

    update_mv_ctx(inter_ctx, mv0, x0_unit, y0_unit, nb_unit_w,
                  nb_unit_h, 1);
    return mv0;
}


OVMV
drv_mvp_mvd(struct InterDRVCtx *const inter_ctx,
            const struct OVMVCtx *const mv_ctx,
            OVMV mvd, int prec_amvr,
            uint8_t x0, uint8_t y0,
            uint8_t log2_cb_w, uint8_t log2_cb_h,
            uint8_t mvp_idx, uint8_t inter_dir,
            uint8_t ref_idx0, uint8_t ref_idx1)
{
    OVMV mv;

    uint8_t x0_unit = x0 >> LOG2_MIN_CU_S;
    uint8_t y0_unit = y0 >> LOG2_MIN_CU_S;
    uint8_t nb_unit_w = (1 << log2_cb_w) >> LOG2_MIN_CU_S;
    uint8_t nb_unit_h = (1 << log2_cb_h) >> LOG2_MIN_CU_S;
    uint8_t opp_ref_idx0 = 0xFF;
    uint8_t opp_ref_idx1 = 0xFF;

    for (int i = 0; i < inter_ctx->nb_active_ref1; i ++) {
         if (inter_ctx->rpl0[ref_idx0] == inter_ctx->rpl1[i])
             opp_ref_idx0 = i;
    }

    for (int i = 0; i < inter_ctx->nb_active_ref0; i ++) {
         if (inter_ctx->rpl1[ref_idx1] == inter_ctx->rpl0[i])
             opp_ref_idx1 = i;
    }


    const struct OVMVCtx *const mv_ctx_opp = &inter_ctx->mv_ctx0 == mv_ctx ? &inter_ctx->mv_ctx1 :
        &inter_ctx->mv_ctx0;

    uint8_t ref_idx = &inter_ctx->mv_ctx0 == mv_ctx  ? ref_idx0 : ref_idx1;
    uint8_t ref_idx_opp = &inter_ctx->mv_ctx0 == mv_ctx  ? opp_ref_idx0 : opp_ref_idx1;

    mv = derive_mvp_candidates_1(inter_ctx, mv_ctx, ref_idx,
                                 x0_unit, y0_unit, nb_unit_w, nb_unit_h,
                                 mvp_idx, 1, mv_ctx_opp, ref_idx_opp, prec_amvr, 0);

    mv.ref_idx = ref_idx;

    mvd = drv_change_precision_mv(mvd, prec_amvr, MV_PRECISION_INTERNAL);

    mv.x += mvd.x;
    mv.y += mvd.y;

    update_mv_ctx(inter_ctx, mv, x0_unit, y0_unit, nb_unit_w, nb_unit_h,
                  inter_dir);

   return mv;
}

VVCMergeInfo
drv_mmvd_merge_mvp_b(struct InterDRVCtx *const inter_ctx,
                     uint8_t x0, uint8_t y0,
                     uint8_t log2_pu_w, uint8_t log2_pu_h,
                     uint8_t merge_idx,
                     uint8_t max_nb_cand, uint8_t is_small)
{

    static const uint8_t ref_mvd_cands[8] = { 1, 2, 4, 8, 16, 32, 64, 128};
    /* FIXME better input to avoid div */
    int smvd_mrg_idx = merge_idx / MMVD_MAX_REFINE_NUM;
    uint8_t pb_x = x0 >> LOG2_MIN_CU_S;
    uint8_t pb_y = y0 >> LOG2_MIN_CU_S;
    uint8_t nb_pb_w = (1 << log2_pu_w) >> LOG2_MIN_CU_S;
    uint8_t nb_pb_h = (1 << log2_pu_h) >> LOG2_MIN_CU_S;

    VVCMergeInfo mv_info = vvc_derive_merge_mvp_b(inter_ctx, pb_x, pb_y,
                                                  nb_pb_w, nb_pb_h, smvd_mrg_idx,
                                                  max_nb_cand, is_small);


    int idx = merge_idx;

    int f_pos_group = merge_idx / (MMVD_BASE_MV_NUM * MMVD_MAX_REFINE_NUM);

    idx -= f_pos_group * (MMVD_BASE_MV_NUM * MMVD_MAX_REFINE_NUM);
    idx -= smvd_mrg_idx * (MMVD_MAX_REFINE_NUM);

    int f_pos_step = idx >> 2;

    int f_pos = idx - (f_pos_step << 2);

    int offset = (uint16_t)ref_mvd_cands[f_pos_step] << 2;

    OVMV mvd0, mvd1;

    int ref0 = mv_info.mv0.ref_idx;
    int ref1 = mv_info.mv1.ref_idx;
    offset <<= inter_ctx->mmvd_shift;

    if (mv_info.inter_dir == 0x3){
        /* FIXME handle LT ref differently */
        int ref_type0 = inter_ctx->rpl_info0->ref_info[ref0].type;
        int ref_type1 = inter_ctx->rpl_info1->ref_info[ref1].type;

        if (f_pos == 0) {
            mvd0.x = offset;
            mvd0.y = 0;
        } else if (f_pos == 1) {
            mvd0.x = -offset;
            mvd0.y = 0;
        } else if (f_pos == 2) {
            mvd0.x = 0;
            mvd0.y = offset;
        } else {
            mvd0.x = 0;
            mvd0.y = -offset;
        }

        uint8_t is_lterm0 = (ref_type0 == LT_REF);
        uint8_t is_lterm1 = (ref_type1 == LT_REF);
        int32_t dist_ref0 = inter_ctx->dist_ref_0[ref0];
        int32_t dist_ref1 = inter_ctx->dist_ref_1[ref1];

        /* Same ref */
        if (dist_ref0 == dist_ref1){
            mvd1.x = mvd0.x;
            mvd1.y = mvd0.y;
        } else if (abs(dist_ref0) < abs(dist_ref1)){
            int scale = tmvp_compute_scale(dist_ref0, dist_ref1);
            mvd1.x = mvd0.x;
            mvd1.y = mvd0.y;
            if (is_lterm0 || is_lterm1){
                if (dist_ref1 * dist_ref0 <= 0){
                    mvd0 = tmvp_scale_mv(-1, mvd1);
                }
            } else {
                mvd0 = tmvp_scale_mv(scale, mvd1);
            }
        } else {
            int scale = tmvp_compute_scale(dist_ref1, dist_ref0);

            mvd1.x = mvd0.x;
            mvd1.y = mvd0.y;

            if (is_lterm0 || is_lterm1){
                if ((dist_ref1) * (dist_ref0) <= 0) {
                    mvd1 = tmvp_scale_mv(-1, mvd0);
                }
            } else {
                mvd1 = tmvp_scale_mv(scale, mvd0);
            }
        }
    } else if (mv_info.inter_dir == 0x1) {

        if (f_pos == 0) {
            mvd0.x = offset;
            mvd0.y = 0;
        } else if (f_pos == 1) {
            mvd0.x = -offset;
            mvd0.y = 0;
        } else if (f_pos == 2) {
            mvd0.x = 0;
            mvd0.y = offset;
        } else {
            mvd0.x = 0;
            mvd0.y = -offset;
        }

    } else if (mv_info.inter_dir == 0x2) {

        if (f_pos == 0) {
            mvd1.x = offset;
            mvd1.y = 0;
        } else if (f_pos == 1) {
            mvd1.x = -offset;
            mvd1.y = 0;
        } else if (f_pos == 2) {
            mvd1.x = 0;
            mvd1.y = offset;
        } else {
            mvd1.x = 0;
            mvd1.y = -offset;
        }

    }

    mv_info.mv0.x += mvd0.x;
    mv_info.mv0.y += mvd0.y;

    mv_info.mv1.x += mvd1.x;
    mv_info.mv1.y += mvd1.y;

    /* FIXME check before ? */
    if (is_small && mv_info.inter_dir == 0x3) {
        mv_info.inter_dir = 0x1;
    }

    update_mv_ctx_b(inter_ctx, mv_info.mv0, mv_info.mv1, pb_x, pb_y,
                    nb_pb_w, nb_pb_h, mv_info.inter_dir);


    hmvp_update_lut_b(&inter_ctx->hmvp_lut, mv_info.mv0, mv_info.mv1, mv_info.inter_dir);

    return mv_info;
}

void 
drv_gpm_merge_mvp_b(struct InterDRVCtx *const inter_ctx,
                    uint8_t x0, uint8_t y0,
                    uint8_t log2_pu_w, uint8_t log2_pu_h,
                    uint8_t max_nb_cand, uint8_t is_small)
{
    struct VVCGPM* gpm_ctx = &inter_ctx->gpm_ctx;
    VVCMergeInfo mv_info0, mv_info1;
    uint8_t pb_x = x0 >> LOG2_MIN_CU_S;
    uint8_t pb_y = y0 >> LOG2_MIN_CU_S;
    uint8_t nb_pb_w = (1 << log2_pu_w) >> LOG2_MIN_CU_S;
    uint8_t nb_pb_h = (1 << log2_pu_h) >> LOG2_MIN_CU_S;

    mv_info0 = vvc_derive_merge_mvp_b(inter_ctx, pb_x, pb_y,
                                     nb_pb_w, nb_pb_h, gpm_ctx->merge_idx0,
                                     max_nb_cand, is_small);

    if (gpm_ctx->merge_idx0 != gpm_ctx->merge_idx1) { 
        mv_info1 = vvc_derive_merge_mvp_b(inter_ctx, pb_x, pb_y,
                                         nb_pb_w, nb_pb_h, gpm_ctx->merge_idx1,
                                         max_nb_cand, is_small);
    } else{
        mv_info1 = mv_info0;
    }

    mv_info0.mv0.bcw_idx_plus1 = 0;
    mv_info0.mv1.bcw_idx_plus1 = 0;
    mv_info1.mv0.bcw_idx_plus1 = 0;
    mv_info1.mv1.bcw_idx_plus1 = 0;
    mv_info0.mv0.prec_amvr = 0;
    mv_info0.mv1.prec_amvr = 0;
    mv_info1.mv0.prec_amvr = 0;
    mv_info1.mv1.prec_amvr = 0;

    uint8_t parity = gpm_ctx->merge_idx0 & 1;

    if (mv_info0.inter_dir & (0x01 + parity)) {
        gpm_ctx->inter_dir0 = 1 + parity;
        gpm_ctx->mv0 = parity ? mv_info0.mv1 : mv_info0.mv0; 
    } else if (mv_info0.inter_dir & (0x02 - parity)) {
        gpm_ctx->inter_dir0 = 2 - parity;
        gpm_ctx->mv0 = parity ? mv_info0.mv0 : mv_info0.mv1; 
    }   

    parity = gpm_ctx->merge_idx1 & 1;

    if(mv_info1.inter_dir & (0x01 + parity)) {
        gpm_ctx->inter_dir1 = 1 + parity;
        gpm_ctx->mv1 = parity ? mv_info1.mv1 : mv_info1.mv0; 
    } else if (mv_info1.inter_dir & (0x02 - parity)) {
        gpm_ctx->inter_dir1 = 2 - parity;
        gpm_ctx->mv1 = parity ? mv_info1.mv0 : mv_info1.mv1; 
    }  

    update_gpm_mv_ctx(inter_ctx, gpm_ctx->mv0, gpm_ctx->mv1, mv_info0, mv_info1, pb_x, pb_y,
                    nb_pb_w, nb_pb_h, gpm_ctx->inter_dir0, gpm_ctx->inter_dir1);
}

VVCMergeInfo
drv_merge_mvp_b(struct InterDRVCtx *const inter_ctx,
                uint8_t x0, uint8_t y0,
                uint8_t log2_cb_w, uint8_t log2_cb_h,
                uint8_t merge_idx,
                uint8_t max_nb_cand, uint8_t is_small)
{
    VVCMergeInfo mv_info;
    uint8_t x0_unit = x0 >> LOG2_MIN_CU_S;
    uint8_t y0_unit = y0 >> LOG2_MIN_CU_S;
    uint8_t nb_unit_w = (1 << log2_cb_w) >> LOG2_MIN_CU_S;
    uint8_t nb_unit_h = (1 << log2_cb_h) >> LOG2_MIN_CU_S;

    mv_info = vvc_derive_merge_mvp_b(inter_ctx, x0_unit, y0_unit,
                                     nb_unit_w, nb_unit_h, merge_idx,
                                     max_nb_cand, is_small);

    if (is_small && mv_info.inter_dir == 3) {
        mv_info.inter_dir = 0x1;
    }

    update_mv_ctx_b(inter_ctx, mv_info.mv0, mv_info.mv1, x0_unit, y0_unit,
                    nb_unit_w, nb_unit_h, mv_info.inter_dir);

    hmvp_update_lut_b(&inter_ctx->hmvp_lut, mv_info.mv0, mv_info.mv1, mv_info.inter_dir);

    return mv_info;
}
