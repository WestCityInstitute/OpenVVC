#include <string.h>
#include <stddef.h>
#include <limits.h>
#include "ovutils.h"
#include "ovmem.h"
#include "nvcl_private.h"
#include "nvcl_structures.h"
#include "ovframe.h"
#include "ovunits.h"
#include "ovdpb.h"
#include "dec_structures.h"
#include "ovdpb_internal.h"
#include "overror.h"
#include "slicedec.h"
#include "ovdec_internal.h"


#if 1
#endif

/* FIXME More global scope for this enum
 */
enum SliceType
{
   SLICE_B = 0,
   SLICE_P = 1,
   SLICE_I = 2,
};

const uint8_t SIZE_INT64 = 6;

static void tmvp_release_mv_planes(OVPicture *const pic);

static int dpb_init_params(OVDPB *dpb, OVDPBParams const *prm);

static void ovdpb_reset_decoded_ctus(OVPicture *const pic);

static void ovdpb_init_decoded_ctus(OVPicture *const pic, const OVPS *const ps);

static void ovdpb_uninit_decoded_ctus(OVPicture *const pic);

int
ovdpb_init(OVDPB **dpb_p, const OVPS *ps)
{
    #if 0
    OVDPB *dpb = *dpb_p;
    #endif
    int ret;

    *dpb_p = ov_mallocz(sizeof(**dpb_p));
    if (!*dpb_p) {
         ov_log(NULL, OVLOG_ERROR, "Failed DBP allocation attempt\n");
         return OVVC_ENOMEM;
    }

    ret = dpbpriv_init_framepool(&(*dpb_p)->internal, ps->sps);
    if (ret < 0) {
        goto failframepool;
    }

    /* FIXME handle temporal and sub layers*/
    dpb_init_params(*dpb_p, &ps->sps->dpb_parameters[ps->sps->sps_max_sublayers_minus1]);

    OVPicture *pic;
    int nb_dpb_pic = sizeof((*dpb_p)->pictures) / sizeof(*pic);
    for (int j = 0; j < nb_dpb_pic; j++) {
        pic = &(*dpb_p)->pictures[j];
        ovdpb_init_decoded_ctus(pic, ps);
    }


    return 0;

failframepool:
    ov_freep(dpb_p);
    return ret;
}

void
ovdpb_uninit(OVDPB **dpb_p)
{
    if (*dpb_p) {
        /* TODO
         * release all pics
         */
        ovdpb_flush_dpb(*dpb_p);
        dpbpriv_uninit_framepool(&(*dpb_p)->internal);

        ov_freep(dpb_p);
    }
}

/* Clean OVPicture from DPB and unref associated Frame */
static void
dpbpriv_release_pic(OVPicture *pic)
{
    if (pic->frame) {
        /* FIXME unref frame */
        ovframe_unref(&pic->frame);
        ov_log(NULL, OVLOG_TRACE, "Unref frame in pic %d\n", pic->poc);

        /* FIXME better existence check */
        if (pic->mv_plane0.mvs){
            tmvp_release_mv_planes(pic);
        }

        pic->rpl_info0.nb_refs = 0;
        pic->rpl_info1.nb_refs = 0;

        pic->tmvp.collocated_ref = NULL;

        pic->frame = NULL;
        
        /* Do not delete frame the frame will delete itself
         * when all its references are released
         */
        #if 0
        ov_freep(&pic->frame);
        #endif
    }
}

static int
dpb_init_params(OVDPB *dpb, OVDPBParams const *prm)
{
    dpb->max_nb_dpb_pic       = prm->dpb_max_dec_pic_buffering_minus1 + 1;
    dpb->max_nb_reorder_pic   = prm->dpb_max_num_reorder_pics;
    dpb->max_latency_increase = prm->dpb_max_latency_increase_plus1 - 1;
    return 0;
}


static int
derive_poc(int poc_lsb, int log2_max_poc_lsb, int prev_poc)
{
    int max_poc_lsb  = 1 << log2_max_poc_lsb;
    int prev_poc_lsb = prev_poc & (max_poc_lsb - 1);
    int poc_msb = prev_poc - prev_poc_lsb;
    if((poc_lsb < prev_poc_lsb) &&
      ((prev_poc_lsb - poc_lsb) >= (max_poc_lsb >> 1))){
        poc_msb += max_poc_lsb;
    } else if((poc_lsb > prev_poc_lsb) &&
             ((poc_lsb - prev_poc_lsb) > (max_poc_lsb >> 1))){
        poc_msb -= max_poc_lsb;
    }
    return poc_msb + poc_lsb;
}


void
ovdpb_unref_pic(OVPicture *pic, int flags)
{
    /* pic->frame can be NULL if context init failed */
    if (!pic->frame || !pic->frame->data[0])
        return;

    pthread_mutex_lock(&pic->pic_mtx);
    pic->flags &= ~flags;

    ovframe_unref(&pic->frame);

    pthread_mutex_unlock(&pic->pic_mtx);

    atomic_fetch_add_explicit(&pic->ref_count, -1, memory_order_acq_rel);
}

void
ovdpb_release_pic(OVDPB *dpb, OVPicture *pic)
{
    /* pic->frame can be NULL if context init failed */
    if (!pic->frame || !pic->frame->data[0])
        return;

    uint16_t ref_count = atomic_fetch_add_explicit(&pic->ref_count, 0, memory_order_acq_rel);

    /* If there is no more flags the picture can be
     * returned to the DPB;
     */
    pthread_mutex_lock(&pic->pic_mtx);
    if (! pic->flags && !ref_count) {
        /* Release TMVP  MV maps */
        ov_log(NULL, OVLOG_TRACE, "Release picture with poc %d\n", pic->poc);
        dpbpriv_release_pic(pic);
    }
    pthread_mutex_unlock(&pic->pic_mtx);
}


int
ovdpb_new_ref_pic(OVPicture *pic, int flags)
{
    int ret;

    atomic_fetch_add_explicit(&pic->ref_count, 1, memory_order_acq_rel);

    pthread_mutex_lock(&pic->pic_mtx);
    pic->flags |= flags;

    //Todopar: check this why need ** frame in this case?
    ret = ovframe_new_ref(&pic->frame, pic->frame);
    pthread_mutex_unlock(&pic->pic_mtx);

    if (ret < 0) {
        return ret;
    }

    /* TMVP */
    // dst->poc     = src->poc;
    // dst->flags   = src->flags;
    // dst->cvs_id  = src->cvs_id;

    return 0;
}

/* Remove reference flags on all picture */
static void
ovdpb_clear_refs(OVDPB *dpb)
{

    ov_log(NULL, OVLOG_DEBUG, "Release reference pictures\n");
    int nb_dpb_pic = sizeof(dpb->pictures) / sizeof(*dpb->pictures);
    int min_poc = INT_MAX;
    int min_idx = nb_dpb_pic;
    int nb_used_pic = 0;
    int i;

    //TODOdpb: use cvs_id for max_nb_dpb_pic
    /* Count pictures in current output target Coded Video Sequence
     */
    for (i = 0; i < nb_dpb_pic; i++) {
        OVPicture *pic = &dpb->pictures[i];
        // uint8_t is_output_cvs = pic->cvs_id == output_cvs_id;
        // if (is_output_cvs && pic->frame && pic->frame->data[0]) {
        if (pic->frame && pic->frame->data[0]) {
            nb_used_pic++;
        }
    }

    if (nb_used_pic >= dpb->max_nb_dpb_pic) {
        /* Determine the min POC among those pic
         */
        for (i = 0; i < nb_dpb_pic; i++) {
            OVPicture *pic = &dpb->pictures[i];
            uint16_t ref_count = atomic_fetch_add_explicit(&pic->ref_count, 0, memory_order_acq_rel);
            // is_output_cvs = pic->cvs_id == output_cvs_id;
            // if (is_output_cvs && pic->frame && pic->frame->data[0] && !ref_count) {
            if (pic->frame && pic->frame->data[0] && !ref_count && !pic->flags) {
                if (pic->poc < min_poc) {
                    min_poc = pic->poc;
                    min_idx = i;
                }
            }
        }

        /* Try to release picture with POC == to min_poc
         */
        if (min_idx < nb_dpb_pic) {
            OVPicture *pic = &dpb->pictures[min_idx];
            ovdpb_release_pic(dpb, pic);
        }
    }
}


/* All pictures are removed from the DPB */
void
ovdpb_flush_dpb(OVDPB *dpb)
{
    int i;
    const int nb_dpb_pic = sizeof(dpb->pictures) / sizeof(*dpb->pictures);
    // const uint8_t flags = ~0;

    for (i = 0; i < nb_dpb_pic; i++) {
        dpb->pictures[i].flags = 0; 
        atomic_init( &dpb->pictures[i].ref_count, 0);
        ovdpb_release_pic(dpb, &dpb->pictures[i]);
        ovdpb_uninit_decoded_ctus(&dpb->pictures[i]);
    }
}

/*FIXME rename to request new picture */
static OVPicture *
alloc_frame(OVDPB *dpb)
{
    int i, ret;
    const int nb_dpb_pic = sizeof(dpb->pictures) / sizeof(*dpb->pictures);
    for (i = 0; i < nb_dpb_pic; i++) {
        OVPicture *pic = &dpb->pictures[i];

        /* FIXME we could avoid checking for data in picture
         * if we are sure every unreference frame is set to NULL
         */
        if (pic->frame && pic->frame->data[0]) {
            continue;
        }

        ov_log(NULL, OVLOG_INFO, "Chose Picture : %d\n", i);
        ret = dpbpriv_request_frame(&dpb->internal, &pic->frame);
        
        if (ret < 0) {
            return NULL;
        }

        pic->flags = 0;
        atomic_init(&pic->ref_count, 0);
        return pic;
    }

    ov_log(NULL, OVLOG_ERROR, "DPB full\n");

    return NULL;
}

/* Allocate the current picture buffer */
int
ovdpb_init_current_pic(OVDPB *dpb, OVPicture **pic_p, int poc)
{
    OVPicture *pic;
    int i;
    const int nb_dpb_pic = sizeof(dpb->pictures) / sizeof(*dpb->pictures);

    *pic_p = NULL;

    /* check that this POC doesn't already exist */
    for (i = 0; i < nb_dpb_pic; i++) {
        OVPicture *pic = &dpb->pictures[i];

        if (pic->frame && pic->frame->data[0] && pic->cvs_id == dpb->cvs_id &&
            pic->poc == poc) {
            ov_log(NULL, OVLOG_ERROR, "Duplicate POC in a sequence: %d.\n",
                   poc);
            return OVVC_EINDATA;
        }
    }

    pic = alloc_frame(dpb);

    if (!pic) {
        return OVVC_ENOMEM;
    }

    *pic_p = pic;

    ovdpb_reset_decoded_ctus(pic);

    #if 0
    dpb->active_pic = pic;
    #endif

    // if (dpb->ps.ph_data->ph_pic_output_flag) {
    if (dpb->display_output) {
        ovdpb_new_ref_pic(pic, OV_OUTPUT_PIC_FLAG);
        ovdpb_new_ref_pic(pic, OV_IN_DECODING_PIC_FLAG);
    } else {
        ovdpb_new_ref_pic(pic, OV_IN_DECODING_PIC_FLAG);
        // ovdpb_new_ref_pic(pic, OV_ST_REF_PIC_FLAG);
    }

    pic->poc    = poc;
    pic->cvs_id = dpb->cvs_id;
    pic->frame->poc = poc;

    /* Copy display or conformance window properties */

    return 0;
}

static int
compute_ref_poc(const OVRPL *const rpl, struct RPLInfo *const rpl_info, int32_t poc)
{
    const int nb_refs = rpl->num_ref_entries;
    int i;
    rpl_info->nb_refs = nb_refs;
    for (i = 0; i < nb_refs; ++i) {
        const struct RefPic *const rp = &rpl->rp_list[i];
        struct RefInfo *const rinfo = &rpl_info->ref_info[i];
        enum RefType ref_type = rp->st_ref_pic_flag ? ST_REF
                                                    : (rp->inter_layer_ref_pic_flag ? ILRP_REF
                                                                                    : LT_REF);
        int ref_poc = 0;
        rinfo->type = ref_type;

        switch (ref_type) {
        case ST_REF:
           ref_poc = !rp->strp_entry_sign_flag ? poc +  rp->abs_delta_poc_st + 1
                                               : poc - (rp->abs_delta_poc_st + 1);
           rinfo->poc = ref_poc;

        break;
        case LT_REF:
           /* FIXME
            *    - Handle when read in header (ltrp_in_header_flag)
            *    - Compute msb part of POC when needed
            */
           ref_poc = rp->rpls_poc_lsb_lt;

           rinfo->poc = ref_poc;
           ov_log(NULL, OVLOG_WARNING, "Partially supported Long Term Ref \n");

        break;
        case ILRP_REF:
           ov_log(NULL, OVLOG_ERROR, "Unsupported Inter Layer Ref \n");
           rinfo->poc = ref_poc;
        break;
        }

        poc = ref_poc;

    }
    return 0;
}


static int
vvc_mark_refs(OVDPB *dpb, const OVRPL *rpl, int32_t poc, struct RPLInfo *rpl_info, const OVPicture **dst_rpl)
{
    int i, j;
    const int nb_dpb_pic = sizeof(dpb->pictures) / sizeof(*dpb->pictures);

    compute_ref_poc(rpl, rpl_info, poc);

    for (i = 0;  i < rpl->num_ref_active_entries; ++i){
        int16_t ref_poc  = rpl_info->ref_info[i].poc;
        int16_t ref_type = rpl_info->ref_info[i].type;
        uint8_t flag = ref_type == ST_REF ? OV_ST_REF_PIC_FLAG : OV_LT_REF_PIC_FLAG;
        OVPicture *ref_pic;
        uint8_t found = 0;
        for (j = 0; j < nb_dpb_pic; j++) {
            ref_pic = &dpb->pictures[j];
            if (ref_pic->poc == ref_poc){
                if(ref_pic->frame && ref_pic->frame->data[0]){
                    found = 1;
                    ov_log(NULL, OVLOG_TRACE, "Mark active reference %d for picture %d\n", ref_poc, dpb->poc);
                    ref_pic->flags &= ~(OV_LT_REF_PIC_FLAG | OV_ST_REF_PIC_FLAG);
                    ovdpb_new_ref_pic(ref_pic, flag);
                    dst_rpl[i] = ref_pic; 
                }
            }
        }

        if (!found){
            /* If reference picture is not in the DPB we try create a new
             * Picture with requested POC ID in the DPB
             */
            ov_log(NULL, OVLOG_ERROR, "Generating missing reference %d for picture %d\n", ref_poc, dpb->poc);
            ref_pic = alloc_frame(dpb);

            if (ref_pic == NULL){
                return OVVC_ENOMEM;
            }

            ovdpb_report_decoded_frame(ref_pic);

            ref_pic->poc    = ref_poc;
            ref_pic->cvs_id = dpb->cvs_id;

            ref_pic->flags  = 0;
            // ref_pic->flags &= ~(OV_LT_REF_PIC_FLAG | OV_ST_REF_PIC_FLAG);
            ovdpb_new_ref_pic(ref_pic, OV_ST_REF_PIC_FLAG);

            /*FIXME  Set output / corrupt flag ? */
            dst_rpl[i] = ref_pic; 
            #if 0
            return 0;
            #endif
        }
    }

    /* Mark non active refrences pictures as used for reference */
    for (; i < rpl->num_ref_entries; ++i) {
        int16_t ref_poc  = rpl_info->ref_info[i].poc;
        int16_t ref_type = rpl_info->ref_info[i].type;
        uint8_t flag = ref_type == ST_REF ? OV_ST_REF_PIC_FLAG : OV_LT_REF_PIC_FLAG;
        OVPicture *ref_pic;
        for (j = 0; j < nb_dpb_pic; j++) {
            ref_pic = &dpb->pictures[j];
            if (ref_pic->poc == ref_poc){
                if(ref_pic->frame && ref_pic->frame->data[0]){
                    ov_log(NULL, OVLOG_TRACE, "Mark non active reference %d for picture %d\n", ref_poc, dpb->poc);
                    ref_pic->flags &= ~(OV_LT_REF_PIC_FLAG | OV_ST_REF_PIC_FLAG);
                    ref_pic->flags |= flag;
                    // ovdpb_new_ref_pic(ref_pic, flag);
                }
            }
        }
    }

    return 0;
}

static int
vvc_unmark_refs(struct RPLInfo *rpl_info, const OVPicture **dst_rpl)
{
    int i;
    OVPicture *ref_pic;
    for (i = 0;  i < rpl_info->nb_refs; ++i){
        ref_pic = dst_rpl[i];
        int16_t ref_poc  = rpl_info->ref_info[i].poc;
        int16_t ref_type = rpl_info->ref_info[i].type;
        uint8_t flag = ref_type == ST_REF ? OV_ST_REF_PIC_FLAG : OV_LT_REF_PIC_FLAG;
        // uint8_t flag = 0;

        if(ref_pic != 0){
            ov_log(NULL, OVLOG_TRACE, "Unmark active reference %d\n", ref_poc);
            ovdpb_unref_pic(ref_pic, flag);    
        }
        else{
            ov_log(NULL, OVLOG_TRACE, "Unmark non active reference %d\n", ref_poc);
        }
    }
    return 0;
}

int
ovdpb_drain_frame(OVDPB *dpb, OVPicture **out, int output_cvs_id)
{
    do {
        const int nb_dpb_pic = sizeof(dpb->pictures) / sizeof(*dpb->pictures);
        int nb_output = 0;
        int min_poc   = INT_MAX;
        int min_idx   = INT_MAX;
        int i;

        #if 0
        if (dpb->sh.no_output_of_prior_pics_flag == 1 && dpb->no_rasl_output_flag == 1) {
        #else
        if (0) {
        #endif
            /* Do not output previously decoded picture which are not already bumped
             * Note that they can still be used by current pic
             */
            for (i = 0; i < nb_dpb_pic; i++) {
                OVPicture *pic = &dpb->pictures[i];
                uint8_t not_bumped = !(pic->flags & OV_BUMPED_PIC_FLAG);
                uint8_t not_current = pic->poc != dpb->poc;
                uint8_t is_output_cvs = pic->cvs_id == output_cvs_id;
                if (not_bumped && not_current && is_output_cvs) {
                    ovdpb_unref_pic(pic, OV_OUTPUT_PIC_FLAG);
                }
            }
        }

        /* Count pic marked for output in output cvs and find the min poc_id */
        for (i = 0; i < nb_dpb_pic; i++) {
            OVPicture *pic = &dpb->pictures[i];
            uint8_t output_flag = (pic->flags & OV_OUTPUT_PIC_FLAG);
            uint8_t is_output_cvs = pic->cvs_id == output_cvs_id;
            if (output_flag && is_output_cvs) {
                nb_output++;
                if (pic->poc < min_poc || nb_output == 1) {
                    min_poc = pic->poc;
                    min_idx = i;
                }
            }
        }

        /* If the number of pic to output is less than max_num_reorder_pics
         * in current cvs we wait for more pic before outputting any
         */
        if (nb_output) {
            OVPicture *pic = &dpb->pictures[min_idx];
            *out = pic;
            return nb_output;
        }

        /* If no output pic found increase cvs_id and retry */
        if (output_cvs_id != dpb->cvs_id) {
            output_cvs_id = (output_cvs_id + 1) & 0xff;
        } else {
            break;
        }

    } while (1);

    ov_log(NULL, OVLOG_TRACE, "No picture to output\n");

    return 0;
}


int
ovdpb_output_pic(OVDPB *dpb, OVPicture **out, int output_cvs_id)
{
    do {
        const int nb_dpb_pic = sizeof(dpb->pictures) / sizeof(*dpb->pictures);
        int nb_output = 0;
        int min_poc   = INT_MAX;
        int min_idx   = nb_dpb_pic;
        int i;
        uint8_t in_decoding;

        #if 0
        if (dpb->sh.no_output_of_prior_pics_flag == 1 && dpb->no_rasl_output_flag == 1) {
        #else
        if (0) {
        #endif
            /* Do not output previously decoded picture which are not already bumped
             * Note that they can still be used by current pic
             */
            for (i = 0; i < nb_dpb_pic; i++) {
                OVPicture *pic = &dpb->pictures[i];
                uint8_t not_bumped = !(pic->flags & OV_BUMPED_PIC_FLAG);
                uint8_t is_output_cvs = pic->cvs_id == output_cvs_id;
                if (not_bumped && is_output_cvs) {
                    ovdpb_unref_pic(pic, OV_OUTPUT_PIC_FLAG);
                }
            }
        }

        /* Count pic marked for output in output cvs and find the min poc_id */
        for (i = 0; i < nb_dpb_pic; i++) {
            OVPicture *pic = &dpb->pictures[i];
            uint8_t output_flag = (pic->flags & OV_OUTPUT_PIC_FLAG);
            uint8_t is_output_cvs = pic->cvs_id == output_cvs_id;
            if (output_flag && is_output_cvs) {
                in_decoding = (pic->flags & OV_IN_DECODING_PIC_FLAG);
                if(!in_decoding){
                    nb_output ++;
                }
                if (pic->poc < min_poc ){
                    min_poc = pic->poc;
                    if(!in_decoding){
                        min_idx = i;
                    }
                    else{
                        min_idx = nb_dpb_pic;
                    }
                }
            }
        }

        /* If the number of pic to output is less than max_num_reorder_pics
         * in current cvs we wait for more pic before outputting any
         */
        if (output_cvs_id == dpb->cvs_id && nb_output <= dpb->max_nb_reorder_pic) {
            return 0;
        }

        if (min_idx < nb_dpb_pic) {
            OVPicture *pic = &dpb->pictures[min_idx];
            *out = pic;
            return nb_output;
        }

        /* If no output pic found increase cvs_id and retry */
        if (output_cvs_id != dpb->cvs_id) {
            output_cvs_id = (output_cvs_id + 1) & 0xff;
        } else {
            break;
        }

    } while (1);

    ov_log(NULL, OVLOG_TRACE, "No picture to output\n");

    return 0;
}

/*FIXME
 *   There might be better ways instead of always looping over
 *   the whole DPB and check for POC and CVS.
 */
void
ovdpb_bump_frame(OVDPB *dpb, uint32_t poc, uint16_t output_cvs_id)
{
    int nb_dpb_pic = sizeof(dpb->pictures) / sizeof(*dpb->pictures);
    int min_poc = INT_MAX;
    int nb_output_pic = 0;
    int i;

    /* Count pictures in current output target Coded Video Sequence
     * which does not correspond to current picture
     */
    for (i = 0; i < nb_dpb_pic; i++) {
        OVPicture *pic = &dpb->pictures[i];
        uint8_t flags = (pic->flags);
        uint8_t is_output_cvs = pic->cvs_id == output_cvs_id;
        uint8_t not_current = pic->poc != poc;
        if (flags && is_output_cvs && not_current) {
            nb_output_pic++;
        }
    }

    if (nb_output_pic >= dpb->max_nb_dpb_pic) {
        /* Determine the min POC among those pic
         */
        for (i = 0; i < nb_dpb_pic; i++) {
            OVPicture *pic = &dpb->pictures[i];
            uint8_t flags = (pic->flags);
            uint8_t is_output_cvs = pic->cvs_id == output_cvs_id;
            uint8_t output_flag = (pic->flags & OV_OUTPUT_PIC_FLAG);
            uint8_t not_current = pic->poc != poc;
            if (flags && output_flag && is_output_cvs && not_current) {
                if (pic->poc < min_poc) {
                    min_poc = pic->poc;
                }
            }
        }

        /* Mark with bumping Flag picture with POC <= to min_poc
         */
        for (i = 0; i < nb_dpb_pic; i++) {
            OVPicture *pic = &dpb->pictures[i];
            uint8_t output_flag = (pic->flags & OV_OUTPUT_PIC_FLAG);
            uint8_t is_output_cvs = pic->cvs_id == output_cvs_id;
            /* Note if the current pic can be also bumped */
            if (output_flag && is_output_cvs && pic->poc <= min_poc) {
                pic->flags |= OV_BUMPED_PIC_FLAG;
                // ovdpb_new_ref_pic(pic, OV_BUMPED_PIC_FLAG);
            }
        }
        nb_output_pic--;
    }
}


int
ovdpb_unmark_ref_pic_lists(uint8_t slice_type, OVPicture * current_pic)
{
    int ret;

    ret = vvc_unmark_refs(&current_pic->rpl_info0, current_pic->rpl0);
    if (ret < 0) {
        goto fail;
    }

    if (slice_type == SLICE_B){
        ret = vvc_unmark_refs(&current_pic->rpl_info1, current_pic->rpl1);
        if (ret < 0) {
            goto fail;
        }
    }
    return 0;

fail:
    /* FIXME ref_marking failed but we allocated a new ref
     * to replace the missing one
     */
    return 1;
}

static int
mark_ref_pic_lists(OVDPB *const dpb, uint8_t slice_type, const struct OVRPL *const rpl0,
                   const struct OVRPL *const rpl1, OVSliceDec *const sldec)
{
    const int nb_dpb_pic = sizeof(dpb->pictures) / sizeof(*dpb->pictures);
    uint32_t poc = dpb->poc;
    int i, ret;
    OVPicture *current_pic = NULL;

    /* This is the same as clear_refs except we do not remove
     * flags on current picture
     */
    for (i = 0; i < nb_dpb_pic; i++) {
        OVPicture *pic = &dpb->pictures[i];

        if (pic->cvs_id == dpb->cvs_id && pic->poc == dpb->poc) {
            current_pic = pic;
            continue;
        }

        pic->flags &= ~(OV_LT_REF_PIC_FLAG | OV_ST_REF_PIC_FLAG);
    }

    ret = vvc_mark_refs(dpb, rpl0, poc, &current_pic->rpl_info0, current_pic->rpl0);
    if (ret < 0) {
        goto fail;
    }

    if (slice_type == SLICE_B){
        ret = vvc_mark_refs(dpb, rpl1, poc, &current_pic->rpl_info1, current_pic->rpl1);
        if (ret < 0) {
            goto fail;
        }
    }

    /* Unreference all non marked Picture */
    ovdpb_clear_refs(dpb);

    return 0;

fail:
    /* FIXME ref_marking failed but we allocated a new ref
     * to replace the missing one
     */
    return 1;
}


int16_t
tmvp_compute_scale(int32_t dist_current, int32_t dist_colocated)
{
    int scale;
    if (dist_current == dist_colocated || !dist_colocated)
        return 256;

    /*FIXME POW2 clip */
    dist_current   = ov_clip(dist_current, -128, 127);
    dist_colocated = ov_clip(dist_colocated, -128, 127);

    scale = dist_current * ((0x4000 + OVABS(dist_colocated >> 1)) / dist_colocated);
    scale += 32;
    scale >>= 6;
    /* FIXME pow2_clip */
    scale = ov_clip(scale, -4096, 4095);
    return (int16_t)scale;
}

static void
tmvp_release_mv_planes(OVPicture *const pic)
{
    /* FIXME check mv planes exists */
    if (pic->mv_plane0.dirs) {
        mvpool_release_mv_plane(&pic->mv_plane0);
    }

    if (pic->mv_plane1.dirs) {
        mvpool_release_mv_plane(&pic->mv_plane1);
    }
}

static int
tmvp_request_mv_plane(OVPicture *const pic, const OVVCDec *ovdec, uint8_t slice_type)
{
    struct MVPool *pool = ovdec->mv_pool;
    int ret;

    ret = mvpool_request_mv_plane(pool, &pic->mv_plane0);
    if (ret < 0) {
        return ret;
    }

    if (slice_type == SLICE_B) {
        ret = mvpool_request_mv_plane(pool, &pic->mv_plane1);
        if (ret < 0) {
            mvpool_release_mv_plane(&pic->mv_plane0);
            return ret;
        }
    }

    return 0;
}

static void
tmvp_set_mv_scales(struct TMVPInfo *const tmvp_ctx, OVPicture *const pic,
                   const OVPicture *const col_pic)
{
    /*TODO scale for every ref in RPL + don't use col pic but ref pic*/
    int32_t dist_ref0 = pic->poc - pic->rpl_info0.ref_info[0].poc;
    int32_t dist_ref1 = pic->poc - pic->rpl_info1.ref_info[0].poc;

    int32_t dist_col0 = col_pic->rpl_info0.nb_refs ? col_pic->poc - col_pic->rpl_info0.ref_info[0].poc : 0;
    int32_t dist_col1 = col_pic->rpl_info1.nb_refs ? col_pic->poc - col_pic->rpl_info1.ref_info[0].poc : 0;
    const struct RPLInfo *const col_rpl0 = &col_pic->rpl_info0;
    const struct RPLInfo *const col_rpl1 = &col_pic->rpl_info1;
    const struct RPLInfo *const rpl0 = &pic->rpl_info0;
    const struct RPLInfo *const rpl1 = &pic->rpl_info1;
    const int32_t col_poc = col_pic->poc;
    const int32_t poc = pic->poc;

    tmvp_ctx->scale00 = tmvp_compute_scale(dist_ref0, dist_col0);
    tmvp_ctx->scale01 = tmvp_compute_scale(dist_ref0, dist_col1);

    tmvp_ctx->scale10 = tmvp_compute_scale(dist_ref1, dist_col0);
    tmvp_ctx->scale11 = tmvp_compute_scale(dist_ref1, dist_col1);

    for (int i = 0; i < rpl0->nb_refs; ++i) {
        tmvp_ctx->dist_ref_0[i] = poc - rpl0->ref_info[i].poc;
    }

    for (int i = 0; i < rpl1->nb_refs; ++i) {
        tmvp_ctx->dist_ref_1[i] = poc - rpl1->ref_info[i].poc;
    }

    for (int i = 0; i < col_rpl0->nb_refs; ++i) {
        tmvp_ctx->dist_col_0[i] = col_poc - col_rpl0->ref_info[i].poc;
    }

    for (int i = 0; i < col_rpl1->nb_refs; ++i) {
        tmvp_ctx->dist_col_1[i] = col_poc - col_rpl1->ref_info[i].poc;
    }
}

static int
init_tmvp_info(struct TMVPInfo *const tmvp_ctx, OVPicture *const pic, const OVPS *const ps, const OVVCDec *ovdec)
{
    const OVPH *ph = ps->ph;
    const OVSH *sh = ps->sh;

    uint8_t slice_type = sh->sh_slice_type;

    /* Init / update MV Pool */

    /* FIXME use sps_log2_parallel_merge_level_minus2 ?*/

    /* Do not asssocite MV map if the picture will not use
     * inter slices
     */

    /* The picture can contain inter slice thus Motions Vector */
    if (ph->ph_inter_slice_allowed_flag) {

        /* Request MV buffer to MV Pool */
        tmvp_request_mv_plane(pic, ovdec, slice_type);
    }

    /* The current picture might use TMVP */
    if(ph->ph_temporal_mvp_enabled_flag) {

        /* Compute TMVP scales */

        /* Find collocated ref and associate MV fields info */
        if (ph->ph_collocated_from_l0_flag || sh->sh_collocated_from_l0_flag || sh->sh_slice_type == SLICE_P) {
            /* FIXME idx can be ph */
            int ref_idx = sh->sh_collocated_ref_idx;
            const OVPicture *col_pic = pic->rpl0[ref_idx];
            tmvp_ctx->col_info.ref_idx_rpl0 = ref_idx;
            tmvp_ctx->col_info.ref_idx_rpl1 = -1;
            for (int i = 0; i < 16; ++i){
                if (pic->rpl1[i] == col_pic){
                    tmvp_ctx->col_info.ref_idx_rpl1 = i;
                }
            }
            tmvp_ctx->collocated_ref = col_pic;

            tmvp_set_mv_scales(tmvp_ctx, pic, col_pic);


        } else {
            /* FIXME idx can be ph */
            int ref_idx = sh->sh_collocated_ref_idx;
            const OVPicture *col_pic = pic->rpl1[ref_idx];
            tmvp_ctx->col_info.ref_idx_rpl1 = ref_idx;
            tmvp_ctx->col_info.ref_idx_rpl0 = -1;
            for (int i = 0; i < 16; ++i){
                if (pic->rpl0[i] == col_pic){
                    tmvp_ctx->col_info.ref_idx_rpl0 = i;
                }
            }
            tmvp_ctx->collocated_ref = col_pic;

            tmvp_set_mv_scales(tmvp_ctx, pic, col_pic);

        }
    }

    return 0;
}

/* TODO rename to ovdpb_init_pic();*/
int
ovdpb_init_picture(OVDPB *dpb, OVPicture **pic_p, const OVPS *const ps, uint8_t nalu_type,
                   OVSliceDec *const sldec, const OVVCDec *ovdec)
{

    const OVSH  *const sh  = ps->sh;
    int ret = 0;
    uint32_t poc = dpb->poc;
    uint8_t cra_flag = 0;
    uint8_t idr_flag = 0;

    idr_flag |= nalu_type == OVNALU_IDR_W_RADL;
    idr_flag |= nalu_type == OVNALU_IDR_N_LP;

    /*FIXME Clarify how GDR NALO are supposed to be handled
     * At the current time we consider it to have the same
     * effect as a CRA
     */
    cra_flag |= nalu_type == OVNALU_CRA;
    cra_flag |= nalu_type == OVNALU_GDR;

    #if 0
    /* This means decoder was flushed or previous thread received EOS/EOB
     * thus sequence changed in previous thread thus we wait for a refresh
     * picture
     * FIXME we need to handle this from DPB state
     */
    if (dpb->max_ra == INT_MAX) {
        if (idr_cra_flag & (VVC_CRA_NAL_FLAG)) {
            dpb->max_ra = dpb->poc;
        } else if (idr_cra_flag & VVC_IDR_NAL_FLAG){
            dpb->max_ra = INT_MIN;
        }
    }

    dpb->no_rasl_output_flag = dpb->last_eos && (idr_cra_flag &
                              (VVC_IDR_NAL_FLAG | VVC_CRA_NAL_FLAG));
    #endif

    /* TODO move to dec init */
    if (idr_flag){
        /* New IDR involves a POC refresh and mark the start of
         * a new coded video sequence
         */
        dpb->cvs_id = (dpb->cvs_id + 1) & 0xFF;
        poc = 0;
    } else {
        /* FIXME arg should be last_poc */
        poc = derive_poc(ps->ph->ph_pic_order_cnt_lsb,
                         ps->sps->sps_log2_max_pic_order_cnt_lsb_minus4 + 4,
                         poc);
    }

    dpb->poc = poc;

    /* If the NALU is an Refresh Picture all previous pictures in DPB
     * can be unreferenced
     */
    if (idr_flag | cra_flag) {
        ovdpb_clear_refs(dpb);
    }

    /* FIXME test bumping here */
    /* Mark previous pic for output */
    if (idr_flag | cra_flag) {
        /* FIXME */
        uint16_t out_cvs_id = (dpb->cvs_id - idr_flag) & 0xFF;
        ovdpb_bump_frame(dpb, poc, out_cvs_id);
    }

    /* Find an available place in DPB and allocate/retrieve available memory
     * for the current picture data from the Frame Pool
     */
    ret = ovdpb_init_current_pic(dpb, pic_p, poc);
    if (ret < 0) {
        goto fail;
    }

    copy_sei_params(&(*pic_p)->sei, ovdec->active_params.sei);
    
    ov_log(NULL, OVLOG_TRACE, "DPB start new picture POC: %d\n", (*pic_p)->poc);

    /* If the picture is not an IDR Picture we set all flags to
     * FIXME in VVC we might still get some ref pic list in IDR
     * Slices it is not clear whether we should still mark them
     * or not
     */
    if (!idr_flag && !cra_flag) {
        const OVRPL *rpl0 = sh->hrpl.rpl0;
        const OVRPL *rpl1 = sh->hrpl.rpl1;
        uint8_t slice_type = sh->sh_slice_type;
        mark_ref_pic_lists(dpb, slice_type, rpl0, rpl1, sldec);
    }

    /* Init picture TMVP info */
    if (ps->sps->sps_temporal_mvp_enabled_flag) {
        ret = init_tmvp_info(&(*pic_p)->tmvp, *pic_p, ps, ovdec);
    }

    return ret;

fail:
    #if 0
    if (dpb->active_pic){
        ovdpb_unref_pic(dpb, dpb->active_pic, ~0);
        dpb->active_pic = NULL;
    }
    #endif

    return ret;
}

void
ovdpb_init_decoded_ctus(OVPicture *const pic, const OVPS *const ps)
{   
    struct PicDecodedCtusInfo* decoded_ctus = &pic->decoded_ctus;
    if(!decoded_ctus->mask){
        decoded_ctus->mask_h = ps->pic_info.nb_ctb_h;
        decoded_ctus->mask_w = (ps->pic_info.nb_ctb_w >> SIZE_INT64) + 1;
        decoded_ctus->mask = ov_mallocz(decoded_ctus->mask_h * sizeof(uint64_t*));
        for(int i = 0; i < decoded_ctus->mask_h; i++)
            decoded_ctus->mask[i] = ov_mallocz(decoded_ctus->mask_w * sizeof(uint64_t));
    }

    pic->ovdpb_frame_synchro = ovdpb_synchro_ref_decoded_ctus;
}

void
ovdpb_uninit_decoded_ctus(OVPicture *const pic)
{   
    struct PicDecodedCtusInfo* decoded_ctus = &pic->decoded_ctus;
    if(decoded_ctus->mask){
        for(int i = 0; i < decoded_ctus->mask_h; i++)
            ov_freep(&decoded_ctus->mask[i]);
        ov_freep(&decoded_ctus->mask);
    }
}

void xctu_to_mask(uint64_t* mask, int mask_w, int xmin_ctu, int xmax_ctu)
{
    int sub_xmin_ctu, sub_xmax_ctu;
    for(int i = (xmin_ctu >> SIZE_INT64); i <= (xmax_ctu >> SIZE_INT64); i++)
    {
        mask[i] = 0;
        sub_xmin_ctu = xmin_ctu > (i<<SIZE_INT64)     ? xmin_ctu % (1<<SIZE_INT64) : 0;
        sub_xmax_ctu = xmax_ctu < ((i+1)<<SIZE_INT64) ? xmax_ctu % (1<<SIZE_INT64) : (1<<SIZE_INT64)-1;
        for(int ii = sub_xmin_ctu; ii <= sub_xmax_ctu; ii++)
            mask[i] |= 1 << ii;
    }
}

void
ovdpb_report_decoded_ctu_line(OVPicture *const pic, int y_ctu, int xmin_ctu, int xmax_ctu)
{
    struct PicDecodedCtusInfo* decoded_ctus = &pic->decoded_ctus;
    int mask_w = decoded_ctus->mask_w;
    uint64_t mask[mask_w];
    xctu_to_mask(mask, mask_w, xmin_ctu, xmax_ctu);

    pthread_mutex_lock(&decoded_ctus->ref_mtx);
    for(int i = 0; i < mask_w; i++)
        decoded_ctus->mask[y_ctu][i] |= mask[i];
    pthread_cond_broadcast(&decoded_ctus->ref_cnd);
    pthread_mutex_unlock(&decoded_ctus->ref_mtx);
    // ov_log(NULL, OVLOG_TRACE, "update_decoded_ctus POC %d line %d\n", pic->poc, y_ctu);
}

void
ovdpb_report_decoded_frame(OVPicture *const pic)
{
    struct PicDecodedCtusInfo* decoded_ctus = &pic->decoded_ctus;
    pthread_mutex_lock(&decoded_ctus->ref_mtx);
    for(int i = 0; i < decoded_ctus->mask_h; i++){
        memset(decoded_ctus->mask[i], 0xFF, decoded_ctus->mask_w * sizeof(int64_t));
    }
    pthread_cond_broadcast(&decoded_ctus->ref_cnd);
    pthread_mutex_unlock(&decoded_ctus->ref_mtx);

    pic->ovdpb_frame_synchro = ovdpb_no_synchro;
}

static void
ovdpb_reset_decoded_ctus(OVPicture *const pic)
{
    struct PicDecodedCtusInfo* decoded_ctus = &pic->decoded_ctus;
    pthread_mutex_lock(&decoded_ctus->ref_mtx);
    for(int i = 0; i < decoded_ctus->mask_h; i++){
        if (decoded_ctus->mask)
        memset(decoded_ctus->mask[i], 0, decoded_ctus->mask_w * sizeof(int64_t));
    }
    pthread_mutex_unlock(&decoded_ctus->ref_mtx);

    pic->ovdpb_frame_synchro = ovdpb_synchro_ref_decoded_ctus;
}

void
ovdpb_get_lines_decoded_ctus(OVPicture *const pic, uint64_t* decoded, int y_start, int y_end )
{
    // ov_log(NULL, OVLOG_DEBUG, "Get decoded_ctus ref POC %d lines %d,%d \n", pic->poc, y_start, y_end);
    struct PicDecodedCtusInfo* decoded_ctus = &pic->decoded_ctus;
    int mask_w = decoded_ctus->mask_w;
    // for(int i = y_start; i <= y_end; i++)
    //     memcpy(decoded[i], decoded_ctus->mask[i], decoded_ctus->mask_w * sizeof(int64_t)) ;
    memcpy(&decoded[y_start*mask_w], decoded_ctus->mask[y_start], (y_end-y_start) * mask_w * sizeof(int64_t)) ;
}

void
ovdpb_no_synchro(OVPicture *const ref_pic, int tl_ctu_x, int tl_ctu_y, int br_ctu_x, int br_ctu_y)
{
    return;
}

void
ovdpb_synchro_ref_decoded_ctus(OVPicture *const ref_pic, int tl_ctu_x, int tl_ctu_y, int br_ctu_x, int br_ctu_y)
{
    struct PicDecodedCtusInfo* decoded_ctus = &ref_pic->decoded_ctus;

    //TODOpar: store previous decoded_ctus of ref_pic in local memory.
    //Avoid to fetch decoded_ctus variable when not needed.
    int mask_w = decoded_ctus->mask_w;
    int mask_h = decoded_ctus->mask_h;
    uint64_t wanted_mask[mask_w];
    xctu_to_mask(wanted_mask, mask_w, tl_ctu_x, br_ctu_x);

    //TODOpar: create a mutex + ref_cnd by ctu line ?
    uint8_t all_ctus_available;
    do{
        pthread_mutex_lock(&decoded_ctus->ref_mtx);

        all_ctus_available = 1;
        for(int ctu_y = tl_ctu_y; ctu_y <= br_ctu_y; ctu_y++ ){
            for(int i = 0; i < mask_w; i++)
                all_ctus_available = all_ctus_available && ((decoded_ctus->mask[ctu_y][i] & wanted_mask[i]) == wanted_mask[i]);
        }
        if(!all_ctus_available)
        {
            // ov_log(NULL, OVLOG_DEBUG, "Wait ref POC %d lines %d,%d \n", ref_pic->poc, tl_ctu_x, tl_ctu_y);
            pthread_cond_wait(&decoded_ctus->ref_cnd, &decoded_ctus->ref_mtx);
        }

        pthread_mutex_unlock(&decoded_ctus->ref_mtx);
    }while(!all_ctus_available);
}
