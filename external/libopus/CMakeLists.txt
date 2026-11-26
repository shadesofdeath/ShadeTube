# libopus 1.6.1 (Xiph.Org / Skype / Microsoft / Broadcom, BSD-3-Clause, see COPYING), DECODER ONLY: the SILK + CELT
# decoders, the multistream decoder and the packet helpers. Floating point, plain C (no SIMD / run-time CPU detection:
# decoding is a small fraction of a core either way), no DNN features (deep PLC, DRED, OSCE are off upstream too).
# Vendored unchanged from https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz
# (SHA-256 6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1); only the sources the build needs.
add_library(xiph_opus STATIC
    src/opus.c src/opus_decoder.c src/extensions.c src/opus_multistream.c src/opus_multistream_decoder.c
    celt/bands.c celt/celt.c celt/celt_decoder.c celt/cwrs.c celt/entcode.c celt/entdec.c celt/entenc.c
    celt/kiss_fft.c celt/laplace.c celt/mathops.c celt/mdct.c celt/modes.c celt/pitch.c celt/celt_lpc.c
    celt/quant_bands.c celt/rate.c celt/vq.c
    silk/CNG.c silk/code_signs.c silk/init_decoder.c silk/decode_core.c silk/decode_frame.c silk/decode_parameters.c
    silk/decode_indices.c silk/decode_pulses.c silk/decoder_set_fs.c silk/dec_API.c silk/gain_quant.c
    silk/NLSF_decode.c silk/PLC.c silk/shell_coder.c silk/tables_gain.c silk/tables_LTP.c silk/tables_NLSF_CB_NB_MB.c
    silk/tables_NLSF_CB_WB.c silk/tables_other.c silk/tables_pitch_lag.c silk/tables_pulses_per_block.c
    silk/NLSF_unpack.c silk/stereo_MS_to_LR.c silk/stereo_decode_pred.c silk/bwexpander_32.c silk/bwexpander.c
    silk/decode_pitch.c silk/inner_prod_aligned.c silk/lin2log.c silk/log2lin.c silk/LPC_analysis_filter.c
    silk/LPC_inv_pred_gain.c silk/table_LSF_cos.c silk/NLSF2A.c silk/NLSF_stabilize.c silk/pitch_est_tables.c
    silk/resampler.c silk/resampler_down2_3.c silk/resampler_down2.c silk/resampler_private_AR2.c
    silk/resampler_private_down_FIR.c silk/resampler_private_IIR_FIR.c silk/resampler_private_up2_HQ.c
    silk/resampler_rom.c silk/sum_sqr_shift.c silk/sort.c silk/LPC_fit.c silk/interpolate.c)
target_include_directories(xiph_opus PUBLIC include PRIVATE celt silk src)
target_compile_definitions(xiph_opus PRIVATE OPUS_BUILD USE_ALLOCA HAVE_LRINT HAVE_LRINTF ENABLE_HARDENING)
target_compile_options(xiph_opus PRIVATE /W0)
