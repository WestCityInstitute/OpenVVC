#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "ovutils.h"
#include "ovmem.h"
#include "overror.h"

#include "ovunits.h"

#include "nvcl.h"
#include "ovdec.h"
#include "ctudec.h"
#include "decinit.h"
#include "slicedec.h"
#include "dec_structures.h"
#include "ovdec_internal.h"
#include "post_proc.h"
/* FIXME
 * To be removed includes
 */
#include "ovdpb.h"
#include "ovthreads.h"

static const char *const decname = "Open VVC Decoder";

static const char *option_names[OVDEC_NB_OPTIONS] =
{
    "frame threads",
    "entry threads",
    "display_output"
};

struct OVVCSubDec;


/* Actions on unsupported NAL Unit types */
static int nalu_type_unsupported(enum OVNALUType nalu_type);

static int
nalu_type_unsupported(enum OVNALUType nalu_type)
{
    ov_log(NULL, 2, "Ignored unsupported NAL type : %d \n", nalu_type);

    return 0;
}

#if 0
/* TODO check Slice / Tiles / Sub Picture context */
static int
init_slice_decoder(OVVCDec *const dec, const OVNVCLCtx *const nvcl_ctx)
{
    const OVSH *const sh = nvcl_ctx->sh;
    int ret;

    #if 0
    init_slice_tools();
    #endif

    #if 0
    if (sh->nb_entry_points > 1){
        /* TILES or WPP */
        /* TODO loop other entry_points offsets and map entries to
         * tile / slice / sub picture decoder */
    } else {
        /* Only one slice in NAL Unit */
    }
    #endif
    return 0;
}
#endif

static int
ovdec_init_subdec_list(OVVCDec *dec)
{
    int ret;
    if (!dec->subdec_list) 
        dec->subdec_list = ov_mallocz(sizeof(OVSliceDec*) * dec->nb_frame_th);

    for (int i = 0; i < dec->nb_frame_th; ++i){
        dec->subdec_list[i] = ov_mallocz(sizeof(OVSliceDec));
        ret = slicedec_init(dec->subdec_list[i], dec->nb_entry_th);
        if (ret < 0) {
            return OVVC_ENOMEM;
        }
        dec->subdec_list[i]->th_slice.main_thread = &dec->main_thread;
        dec->subdec_list[i]->th_slice.output_thread = &dec->output_thread;
    }

    return 0;
}


static int
init_vcl_decoder(OVVCDec *const dec, OVSliceDec *sldec, const OVNVCLCtx *const nvcl_ctx,
                OVNALUnit * nalu, const OVNVCLReader *const rdr)
{

    int ret;
    int nb_sh_bytes = nvcl_num_bytes_read(rdr);

    ret = decinit_update_params(&dec->active_params, nvcl_ctx);
    if (ret < 0) {
        ov_log(dec, 3, "Failed to activate parameters\n");
        return ret;
    }

    if (!dec->dpb) {
         ret = ovdpb_init(&dec->dpb, &dec->active_params);
        dec->dpb->display_output = dec->display_output;
         if (ret < 0) {
             return ret;
         }
    }

    /* FIXME only if TMVP */
    //TODOpar: protect mv pool when more than one thread ?
    if (!dec->mv_pool) {
        ret = mvpool_init(&dec->mv_pool, &dec->active_params.pic_info);
    }

    //Temporary: copy active parameters
    slicedec_copy_params(sldec, &dec->active_params);

    /* FIXME clean way on new slice with address 0 */
#if 0
    if (dec->active_params.sh->sh_slice_address) {
#else
    if (1) {
#endif
        ret = ovdpb_init_picture(dec->dpb, &sldec->pic, sldec->active_params, nalu->type, sldec, dec);
        if (ret < 0) {
            return ret;
        }
    }
    //Add refs on nalu
    ov_nalu_new_ref(&sldec->th_slice.slice_nalu, nalu);

    /*FIXME return checks */
    ret = slicedec_init_lines(sldec, sldec->active_params);

    ret = decinit_set_entry_points(sldec->active_params, nalu, nb_sh_bytes);

    ret = slicedec_update_entry_decoders(sldec, sldec->active_params);

    return 0;
}


OVSliceDec *
ovdec_select_subdec(OVVCDec *const dec)
{
    #if USE_THREADS
    OVSliceDec **sldec_list = dec->subdec_list;
    int nb_threads = dec->nb_frame_th;
    struct MainThread* th_main = &dec->main_thread;

    OVSliceDec * slicedec;
    struct SliceThread* th_slice;
    do{
        //Unmark ref pict lists of decoded pics
        int min_idx_available = nb_threads;
        pthread_mutex_lock(&th_main->main_mtx);
        
        for(int i = nb_threads-1; i >= 0 ; i--){
            slicedec = sldec_list[i];
            th_slice = &slicedec->th_slice;
            
            if(th_slice->active_state == DECODING_FINISHED){
                min_idx_available = i;
                OVPicture *slice_pic = slicedec->pic;
                if(slice_pic && (slice_pic->flags & OV_IN_DECODING_PIC_FLAG)){
                    ov_log(NULL, OVLOG_TRACE, "Subdec %d Remove DECODING_PIC_FLAG POC: %d\n", min_idx_available, slice_pic->poc);
                    ovdpb_unref_pic(slice_pic, OV_IN_DECODING_PIC_FLAG);
                    ovdpb_unmark_ref_pic_lists(slicedec->slice_type, slice_pic);

                    pthread_mutex_lock(&th_slice->gnrl_mtx);
                    th_slice->active_state = IDLE;
                    pthread_mutex_unlock(&th_slice->gnrl_mtx);
                }
            }
            else if(th_slice->active_state == IDLE){
                min_idx_available = i;     
            }
        }

        if(min_idx_available < nb_threads){
            slicedec = sldec_list[min_idx_available];
            th_slice = &slicedec->th_slice;
        
            pthread_mutex_lock(&th_slice->gnrl_mtx);
            th_slice->active_state = ACTIVE;
            ov_log(NULL, OVLOG_TRACE, "Subdec %d selected\n", min_idx_available);
            pthread_mutex_unlock(&th_slice->gnrl_mtx);

            pthread_mutex_unlock(&th_main->main_mtx);
            return slicedec;
        }

        pthread_cond_wait(&th_main->main_cnd, &th_main->main_mtx);
        pthread_mutex_unlock(&th_main->main_mtx);

    } while(!th_main->kill);

    return NULL;

    #else
    OVSliceDec *sldec = vvcdec->subdec_list[0];
    #endif
}


static int
decode_nal_unit(OVVCDec *const vvcdec, OVNALUnit * nalu)
{
    OVNVCLReader rdr;
    OVNVCLCtx *const nvcl_ctx = &vvcdec->nvcl_ctx;
    enum OVNALUType nalu_type = nalu->type;
    /* FIXME add proper SH allocation */
    int ret;

    /* TODO init NVCLReader */
    nvcl_reader_init(&rdr, nalu->rbsp_data, (nalu->rbsp_size) << 3);

    switch (nalu_type) {
    case OVNALU_TRAIL:
    case OVNALU_STSA:
    case OVNALU_RADL:
    case OVNALU_RASL:
    case OVNALU_IDR_W_RADL:
    case OVNALU_IDR_N_LP:
    case OVNALU_CRA:
    case OVNALU_GDR:

        ret = nvcl_decode_nalu_sh(&rdr, nvcl_ctx, nalu_type);

        if (ret < 0) {
            return ret;
        } else {
            /*Select the first available subdecoder, or wait until one is available*/
            OVSliceDec *sldec = ovdec_select_subdec(vvcdec);
                
            ret = init_vcl_decoder(vvcdec, sldec, nvcl_ctx, nalu, &rdr);

            if (ret < 0) {
                slicedec_finish_decoding(sldec);
                goto failvcl;
            }
            /* Beyond this point unref current picture on failure
             */

            /* FIXME handle non rect entries later */
            ret = slicedec_decode_rect_entries(sldec, sldec->active_params);
 
            /* TODO start VCL decoder */
        }

        break;
    case OVNALU_VPS:
        ret = 0;
        if (ret < 0) {
            goto fail;
        }
        break;
    case OVNALU_SPS:
        ret = nvcl_decode_nalu_sps(&rdr, nvcl_ctx);
        if (ret < 0) {
            goto fail;
        }
        break;
    case OVNALU_PPS:
        ret = nvcl_decode_nalu_pps(&rdr, nvcl_ctx);
        if (ret < 0) {
            goto fail;
        }
        break;
    case OVNALU_PH:
        ret = nvcl_decode_nalu_ph(&rdr, nvcl_ctx);
        if (ret < 0) {
            goto fail;
        }
        break;
    case OVNALU_SUFFIX_APS:
    case OVNALU_PREFIX_APS:
        ret = nvcl_decode_nalu_aps(&rdr, nvcl_ctx);
        if (ret < 0) {
            goto fail;
        }
        break;
    case OVNALU_PREFIX_SEI:
    case OVNALU_SUFFIX_SEI:
        ret = nvcl_decode_nalu_sei(&rdr, nvcl_ctx);
        if (ret < 0)
            goto fail;
        break;
    case OVNALU_EOS:
    case OVNALU_EOB:
        /* TODO update DPB status (new cvs);
         * call dpb_uninit
         */
        break;
    case OVNALU_AUD:
        /* AUD is ignored it should be the first NALU if
         * present
         */
        break;
    default:
        nalu_type_unsupported(nalu_type);
    }

    return 0;

fail:
    /* TODO error hanling */
    return ret;

failvcl:
    //TODOpar: change 0
    if (vvcdec->subdec_list[0]->pic) {
        ovdpb_unref_pic(vvcdec->subdec_list[0]->pic, ~0);
    }
    return ret;
}

static int
vvc_decode_picture_unit(OVVCDec *dec, const OVPictureUnit *pu)
{
    int i;
    int ret;
    for (i = 0; i < pu->nb_nalus; ++i) {
        ret = decode_nal_unit(dec, pu->nalus[i]);
        if (ret < 0) {
            goto fail;
        }
    }
    return 0;

fail:
    /* Error processing if needed */
    return ret;
}

#if 0
static int
vvc_validate_access_unit(VVCContext *const s, const VVCPacket *const pkt)
{
    int eos_at_start = 1;
    int nb_vcl_nal = 0;
    int first_vcl_type = 0; /* Zero is non vcl Init value */
    int i;

    s->last_eos = s->eos;
    s->eos = 0;

    /*FIXME we could add a more in depth conformance check
     * e.g. for VCL and non VCL NALU order etc.
     * we coud also filter some NALUs out here if we 'd like
     * to ignore some NALUs type given decoder state
    */
    for (i = 0; i < pkt->nb_nals; i++) {
        int nal_type = pkt->nals[i].type;
        /*FIXME Check EOS is handled correctely afterwards
         * last_eos should trigger an increase in sequence_id
         * whereas eos is passed to next thread context
         */
        if (nal_type == OVNALU_EOB || nal_type == OVNALU_EOS) {
            if (eos_at_start) {
                s->last_eos = 1;
            } else {
                s->eos = 1;
            }
        } else {
            eos_at_start = 0;
        }

        if (nal_type <= 12) {
            if (!nb_vcl_nal)
                first_vcl_type = nal_type;
            else if (nal_type != first_vcl_type) {
                av_log(s, AV_LOG_ERROR, "Received NALUs of different types in same access unit\n");
                return AVERROR_INVALIDDATA;
            }
            ++nb_vcl_nal;
        }
    }

    if (nb_vcl_nal > 1) {
        /*FIXME we could probably decode first slice */
        av_log(s, AV_LOG_ERROR, "Multiple slice in same picture, this is unsupported\n");
        return AVERROR_INVALIDDATA;
    }

    return 0;
}
#endif

#if 0
static int
vvc_decode_access_unit(VVCContext *s, const uint8_t *buf, int length)
{
    int i, ret;
    VVCPacket *const pkt = &s->pkt;

    s->active_pic = NULL;

    /* split the input packet into NAL units, so we know the upper bound on the
     * number of slices in the frame */
    ret = ff_vvc_packet_split(pkt, buf, length, s->avctx, s->is_nalff,
                                s->nal_length_size, s->avctx->codec_id, 0);
    if (ret < 0) {
        av_log(s->avctx, AV_LOG_ERROR,
               "Error splitting the input into NAL units.\n");
        return ret;
    }

    ret = vvc_validate_access_unit(s, pkt);
    if (ret < 0) {
        return ret;
    }

    /* decode the NAL units */
    for (i = 0; i < pkt->nb_nals; i++) {
        const VVCNAL *const nal = &pkt->nals[i];

        ret = decode_nal_unit(s, nal);
        if (ret < 0) {
            av_log(s->avctx, AV_LOG_WARNING,
                   "Error parsing NAL unit #%d.\n", i);
            goto fail;
        }
    }

fail:
    if (s->active_pic && s->threads_type == FF_THREAD_FRAME)
        ff_thread_report_progress(&s->active_pic->tf, INT_MAX, 0);

    return ret;
}
#endif

int
ovdec_submit_picture_unit(OVVCDec *vvcdec, const OVPictureUnit *const pu)
{
    int ret = 0;

    #if 0
    if (!vvcdec->dmx) {
        OVVCDmx dmx;
        dmx = ovvcdmx_create();
        if (!dmx) {
            /* Failed to allocate AnnexB demuxer*/
            return -1;
        }
    }

    ret = ovcdmx_extract_pu_nal_units(OVVCDmx *dmx, OVVCPUPacket *pkt);
    if (ret < 0) {
        return ret;
    }
    #endif

    ret = vvc_decode_picture_unit(vvcdec, pu);

    return ret;
}


int
ovdec_receive_picture(OVVCDec *dec, OVFrame **frame_p)
{
    /* FIXME this is temporary request output from DPB
     * instead
     */
    OVDPB *dpb = dec->dpb;
    uint16_t out_cvs_id;
    int ret;

    if (!dpb) {
        ov_log(dec, OVLOG_ERROR, "No DPB on output request.\n");
        /* FIXME new return value */
        return OVVC_EINDATA;
    }

    #if 0
    /* FIXME here or inside function */
    ovframe_new_ref(frame_p, sldec->pic->frame);
    #endif

    OVPicture *pic = NULL;
    out_cvs_id = (dpb->cvs_id - 1) & 0xFF;
    ret = ovdpb_output_pic(dpb, &pic, out_cvs_id);

    if (pic) {
        *frame_p = pic->frame;
        pp_process_frame(pic->sei, dec->dpb, frame_p);

        //New ref if it is a frame already in a DPB pic
        if(*frame_p ==  pic->frame){
            ovframe_new_ref(frame_p, pic->frame);
        }
        /* we unref the picture even if ref failed the picture
         * will still be usable by the decoder if not bumped
         * */
        ovdpb_unref_pic(pic, OV_OUTPUT_PIC_FLAG | (pic->flags & OV_BUMPED_PIC_FLAG));
    }

    /*FIXME tmp */
    #if 0
    ovdpb_unref_pic(dec->dpb, sldec->pic, ~0);
    #endif
    return ret;

    return 0;
}

int
ovdec_drain_picture(OVVCDec *dec, OVFrame **frame_p)
{
    /* FIXME this is temporary request output from DPB
     * instead
     */
    OVDPB *dpb = dec->dpb;
    uint16_t out_cvs_id;
    int ret;

    ovdec_uninit_subdec_list(dec);

    if (!dpb) {
        ov_log(dec, OVLOG_ERROR, "No DPB on output request.\n");
        /* FIXME new return value */
        return OVVC_EINDATA;
    }

    OVPicture *pic = NULL;
    out_cvs_id = (dpb->cvs_id - 1) & 0xFF;
    ret = ovdpb_drain_frame(dpb, &pic, out_cvs_id);

    if (pic) {
        *frame_p = pic->frame;
        pp_process_frame(pic->sei, dec->dpb, frame_p);

        //New ref if it is a frame already in a DPB pic
        if(*frame_p ==  pic->frame){
            ovframe_new_ref(frame_p, pic->frame);
        }
        /* we unref the picture even if ref failed the picture
         * will still be usable by the decoder if not bumped
         * */
        ovdpb_unref_pic(pic, OV_OUTPUT_PIC_FLAG | (pic->flags & OV_BUMPED_PIC_FLAG));
    }

    return ret;
}

static int
set_display_output(OVVCDec *ovdec, int on_off)
{
    ovdec->display_output = !!on_off;
    return 0;
}

static int
set_nb_entry_threads(OVVCDec *ovdec, int nb_threads)
{
    ovdec->nb_entry_th = nb_threads;

    return 0;
}

static int
set_nb_frame_threads(OVVCDec *ovdec, int nb_threads)
{
    ovdec->nb_frame_th = nb_threads;

    return 0;
}

int
ovdec_set_option(OVVCDec *ovdec, enum OVOptions opt_id, int value)
{

    switch (opt_id){
        case OVDEC_NB_ENTRY_THREADS:
            set_nb_entry_threads(ovdec, value);
            break;
        case OVDEC_NB_FRAME_THREADS:
            set_nb_frame_threads(ovdec, value);
            break;
        case OVDEC_DISPLAY_OUTPUT:
            set_display_output(ovdec, value);
            break;
        default :
            if (opt_id < OVDEC_NB_OPTIONS) {
                ov_log(ovdec, OVLOG_ERROR, "Invalid option id %d.", opt_id);
                return OVVC_EINDATA;
            }
            break;
    }
    ov_log(ovdec, OVLOG_VERBOSE, "Option %s set to %d.\n", option_names[opt_id], value);

    return 0;
}

int
ovdec_init(OVVCDec **vvcdec, int display_output, int nb_frame_th, int nb_entry_th)
{

    if (nb_frame_th < 0) {
        nb_frame_th = 1;
    } else if (nb_frame_th == 0) {
        /* FIXME might not be available on every plateform */
        nb_frame_th = get_number_of_cores();
    }

    if (nb_entry_th < 1) {
        nb_entry_th = 1;
    }

    *vvcdec = ov_mallocz(sizeof(OVVCDec));

    if (*vvcdec == NULL) goto fail;

    (*vvcdec)->name = decname;

    ovdec_set_option(*vvcdec, OVDEC_NB_FRAME_THREADS, nb_frame_th);

    ovdec_set_option(*vvcdec, OVDEC_NB_ENTRY_THREADS, nb_entry_th);

    ovdec_set_option(*vvcdec, OVDEC_DISPLAY_OUTPUT, display_output);

    (*vvcdec)->display_output = !!display_output;

    ovdec_init_subdec_list(*vvcdec);

    return 0;

fail:
    /* TODO proper error management (ENOMEM)*/
    return -1;
}


void
ovdec_uninit_subdec_list(OVVCDec *vvcdec)
{
    OVSliceDec *sldec;

    if (vvcdec != NULL)
    {
        if (vvcdec->subdec_list) {
            for (int i = 0; i < vvcdec->nb_frame_th; ++i){
                sldec = vvcdec->subdec_list[i];
                slicedec_uninit(&sldec);
                ov_log(NULL, OVLOG_INFO, "Main joined thread: %d\n", i);
            }
            ov_freep(&vvcdec->subdec_list);
        }

        // ovthread_output_uninit(&vvcdec->output_thread);
    }
}

int
ovdec_close(OVVCDec *vvcdec)
{
    int not_dec;
    if (vvcdec != NULL) {

        not_dec = vvcdec->name != decname;

        if (not_dec) goto fail;

        nvcl_free_ctx(&vvcdec->nvcl_ctx);

        ovdec_uninit_subdec_list(vvcdec);

        ovdpb_uninit(&vvcdec->dpb);

        if (vvcdec->mv_pool) {
            mvpool_uninit(&vvcdec->mv_pool);
        }

        pthread_mutex_destroy(&vvcdec->main_thread.main_mtx);
        pthread_cond_destroy(&vvcdec->main_thread.main_cnd);
        ov_free(vvcdec);

        return 0;
    }

fail:
    ov_log(vvcdec, 3, "Trying to close a something not a decoder.\n");
    return -1;
}
