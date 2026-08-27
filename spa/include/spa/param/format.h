/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_PARAM_FORMAT_H
#define SPA_PARAM_FORMAT_H

#include <spa/param/param.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_param
 * \{
 */

/** media type for SPA_TYPE_OBJECT_Format */
enum spa_media_type {
	SPA_MEDIA_TYPE_unknown,
	SPA_MEDIA_TYPE_audio,
	SPA_MEDIA_TYPE_video,
	SPA_MEDIA_TYPE_image,
	SPA_MEDIA_TYPE_binary,
	SPA_MEDIA_TYPE_stream,
	SPA_MEDIA_TYPE_application,
};

/** media subtype for SPA_TYPE_OBJECT_Format */
enum spa_media_subtype {
	SPA_MEDIA_SUBTYPE_unknown,
	SPA_MEDIA_SUBTYPE_raw,
	SPA_MEDIA_SUBTYPE_dsp,
	SPA_MEDIA_SUBTYPE_iec958,	/** S/PDIF */
	SPA_MEDIA_SUBTYPE_dsd,

	SPA_MEDIA_SUBTYPE_START_Audio	= 0x10000,
	SPA_MEDIA_SUBTYPE_mp3,
	SPA_MEDIA_SUBTYPE_aac,
	SPA_MEDIA_SUBTYPE_vorbis,
	SPA_MEDIA_SUBTYPE_wma,
	SPA_MEDIA_SUBTYPE_ra,
	SPA_MEDIA_SUBTYPE_sbc,
	SPA_MEDIA_SUBTYPE_adpcm,
	SPA_MEDIA_SUBTYPE_g723,
	SPA_MEDIA_SUBTYPE_g726,
	SPA_MEDIA_SUBTYPE_g729,
	SPA_MEDIA_SUBTYPE_amr,
	SPA_MEDIA_SUBTYPE_gsm,
	SPA_MEDIA_SUBTYPE_alac,		/** since 0.3.65 */
	SPA_MEDIA_SUBTYPE_flac,		/** since 0.3.65 */
	SPA_MEDIA_SUBTYPE_ape,		/** since 0.3.65 */
	SPA_MEDIA_SUBTYPE_opus,		/** since 0.3.68 */
	SPA_MEDIA_SUBTYPE_ac3,		/** since 1.5.1 */
	SPA_MEDIA_SUBTYPE_eac3,		/** since 1.5.1 */
	SPA_MEDIA_SUBTYPE_truehd,	/** since 1.5.1 */
	SPA_MEDIA_SUBTYPE_dts,		/** since 1.5.1 */
	SPA_MEDIA_SUBTYPE_mpegh,	/** since 1.5.1 */

	SPA_MEDIA_SUBTYPE_START_Video	= 0x20000,
	SPA_MEDIA_SUBTYPE_h264,
	SPA_MEDIA_SUBTYPE_mjpg,
	SPA_MEDIA_SUBTYPE_dv,
	SPA_MEDIA_SUBTYPE_mpegts,
	SPA_MEDIA_SUBTYPE_h263,
	SPA_MEDIA_SUBTYPE_mpeg1,
	SPA_MEDIA_SUBTYPE_mpeg2,
	SPA_MEDIA_SUBTYPE_mpeg4,
	SPA_MEDIA_SUBTYPE_xvid,
	SPA_MEDIA_SUBTYPE_vc1,
	SPA_MEDIA_SUBTYPE_vp8,
	SPA_MEDIA_SUBTYPE_vp9,
	SPA_MEDIA_SUBTYPE_bayer,
	SPA_MEDIA_SUBTYPE_h265,

	SPA_MEDIA_SUBTYPE_START_Image	= 0x30000,
	SPA_MEDIA_SUBTYPE_jpeg,

	SPA_MEDIA_SUBTYPE_START_Binary	= 0x40000,

	SPA_MEDIA_SUBTYPE_START_Stream	= 0x50000,
	SPA_MEDIA_SUBTYPE_midi,

	SPA_MEDIA_SUBTYPE_START_Application	= 0x60000,
	SPA_MEDIA_SUBTYPE_control,		/**< control stream, data contains
						  *  spa_pod_sequence with control info. */

	SPA_MEDIA_SUBTYPE_START_PipeWireAO = 0x1000000,
	SPA_MEDIA_SUBTYPE_ndarray = SPA_MEDIA_SUBTYPE_START_PipeWireAO,	/**< packed N-dimensional typed elements */
};

/**
 * Scalar representation of ndarray elements.
 *
 * Multi-byte values are little-endian. Complex elements contain the real
 * component followed by the imaginary component. The core set is append-only;
 * application-defined fixed-width scalar types start at START_CUSTOM.
 */
enum spa_element_type {
	SPA_ELEMENT_TYPE_UNKNOWN,
	SPA_ELEMENT_TYPE_BOOL8,			/**< Boolean byte; only 0 and 1 are valid */
	SPA_ELEMENT_TYPE_I8,			/**< signed 8-bit integer */
	SPA_ELEMENT_TYPE_U8,			/**< unsigned 8-bit integer */
	SPA_ELEMENT_TYPE_I16_LE,		/**< signed 16-bit integer, little-endian */
	SPA_ELEMENT_TYPE_U16_LE,		/**< unsigned 16-bit integer, little-endian */
	SPA_ELEMENT_TYPE_I32_LE,		/**< signed 32-bit integer, little-endian */
	SPA_ELEMENT_TYPE_U32_LE,		/**< unsigned 32-bit integer, little-endian */
	SPA_ELEMENT_TYPE_I64_LE,		/**< signed 64-bit integer, little-endian */
	SPA_ELEMENT_TYPE_U64_LE,		/**< unsigned 64-bit integer, little-endian */
	SPA_ELEMENT_TYPE_I128_LE,		/**< signed 128-bit integer, little-endian */
	SPA_ELEMENT_TYPE_U128_LE,		/**< unsigned 128-bit integer, little-endian */
	SPA_ELEMENT_TYPE_F8_E4M3FN,		/**< 8-bit E4M3 finite-numbers format */
	SPA_ELEMENT_TYPE_F8_E4M3FNUZ,		/**< 8-bit E4M3 finite, unsigned-zero format */
	SPA_ELEMENT_TYPE_F8_E5M2,		/**< 8-bit E5M2 format */
	SPA_ELEMENT_TYPE_F8_E5M2FNUZ,		/**< 8-bit E5M2 finite, unsigned-zero format */
	SPA_ELEMENT_TYPE_F16_LE,		/**< IEEE 754 binary16, little-endian */
	SPA_ELEMENT_TYPE_BF16_LE,		/**< bfloat16, little-endian */
	SPA_ELEMENT_TYPE_F32_LE,		/**< IEEE 754 binary32, little-endian */
	SPA_ELEMENT_TYPE_F64_LE,		/**< IEEE 754 binary64, little-endian */
	SPA_ELEMENT_TYPE_F128_LE,		/**< IEEE 754 binary128, little-endian */
	SPA_ELEMENT_TYPE_COMPLEX_F16_LE,	/**< two IEEE binary16 components */
	SPA_ELEMENT_TYPE_COMPLEX_BF16_LE,	/**< two bfloat16 components */
	SPA_ELEMENT_TYPE_COMPLEX_F32_LE,	/**< two IEEE binary32 components */
	SPA_ELEMENT_TYPE_COMPLEX_F64_LE,	/**< two IEEE binary64 components */
	SPA_ELEMENT_TYPE_COMPLEX_F128_LE,	/**< two IEEE binary128 components */

	SPA_ELEMENT_TYPE_START_CUSTOM = 0x10000,
};

/** Packed size of one core element, or zero for unknown/custom values. */
static inline uint32_t spa_element_type_size(enum spa_element_type type)
{
	switch (type) {
	case SPA_ELEMENT_TYPE_BOOL8:
	case SPA_ELEMENT_TYPE_I8:
	case SPA_ELEMENT_TYPE_U8:
	case SPA_ELEMENT_TYPE_F8_E4M3FN:
	case SPA_ELEMENT_TYPE_F8_E4M3FNUZ:
	case SPA_ELEMENT_TYPE_F8_E5M2:
	case SPA_ELEMENT_TYPE_F8_E5M2FNUZ:
		return 1;
	case SPA_ELEMENT_TYPE_I16_LE:
	case SPA_ELEMENT_TYPE_U16_LE:
	case SPA_ELEMENT_TYPE_F16_LE:
	case SPA_ELEMENT_TYPE_BF16_LE:
		return 2;
	case SPA_ELEMENT_TYPE_I32_LE:
	case SPA_ELEMENT_TYPE_U32_LE:
	case SPA_ELEMENT_TYPE_F32_LE:
	case SPA_ELEMENT_TYPE_COMPLEX_F16_LE:
	case SPA_ELEMENT_TYPE_COMPLEX_BF16_LE:
		return 4;
	case SPA_ELEMENT_TYPE_I64_LE:
	case SPA_ELEMENT_TYPE_U64_LE:
	case SPA_ELEMENT_TYPE_F64_LE:
	case SPA_ELEMENT_TYPE_COMPLEX_F32_LE:
		return 8;
	case SPA_ELEMENT_TYPE_I128_LE:
	case SPA_ELEMENT_TYPE_U128_LE:
	case SPA_ELEMENT_TYPE_F128_LE:
	case SPA_ELEMENT_TYPE_COMPLEX_F64_LE:
		return 16;
	case SPA_ELEMENT_TYPE_COMPLEX_F128_LE:
		return 32;
	default:
		return 0;
	}
}

/** contiguous storage order of ndarray elements */
enum spa_ndarray_layout {
	SPA_NDARRAY_LAYOUT_UNKNOWN,
	SPA_NDARRAY_LAYOUT_ROW_MAJOR,		/**< the last logical axis is contiguous */
	SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,	/**< the first logical axis is contiguous */
};

/** properties for audio SPA_TYPE_OBJECT_Format */
enum spa_format {
	SPA_FORMAT_START,

	SPA_FORMAT_mediaType,		/**< media type (Id enum spa_media_type) */
	SPA_FORMAT_mediaSubtype,	/**< media subtype (Id enum spa_media_subtype) */

	/* Audio format keys */
	SPA_FORMAT_START_Audio = 0x10000,
	SPA_FORMAT_AUDIO_format,		/**< audio format, (Id enum spa_audio_format) */
	SPA_FORMAT_AUDIO_flags,			/**< optional flags (Int) */
	SPA_FORMAT_AUDIO_rate,			/**< sample rate (Int) */
	SPA_FORMAT_AUDIO_channels,		/**< number of audio channels (Int) */
	SPA_FORMAT_AUDIO_position,		/**< channel positions (Id enum spa_audio_position) */

	SPA_FORMAT_AUDIO_iec958Codec,		/**< codec used (IEC958) (Id enum spa_audio_iec958_codec) */

	SPA_FORMAT_AUDIO_bitorder,		/**< bit order (Id enum spa_param_bitorder) */
	SPA_FORMAT_AUDIO_interleave,		/**< Interleave bytes (Int) */
	SPA_FORMAT_AUDIO_bitrate,		/**< bit rate (Int) */
	SPA_FORMAT_AUDIO_blockAlign,    	/**< audio data block alignment (Int) */

	SPA_FORMAT_AUDIO_AAC_streamFormat,	/**< AAC stream format, (Id enum spa_audio_aac_stream_format) */

	SPA_FORMAT_AUDIO_WMA_profile,		/**< WMA profile (Id enum spa_audio_wma_profile) */

	SPA_FORMAT_AUDIO_AMR_bandMode,		/**< AMR band mode (Id enum spa_audio_amr_band_mode) */

	SPA_FORMAT_AUDIO_MP3_channelMode,	/**< MP3 channel mode, (Id enum spa_audio_mp3_channel_mode) */

	SPA_FORMAT_AUDIO_DTS_extType,		/**< DTS extension type (Id enum spa_audio_dts_ext_type) */


	/* Video Format keys */
	SPA_FORMAT_START_Video = 0x20000,
	SPA_FORMAT_VIDEO_format,		/**< video format (Id enum spa_video_format) */
	SPA_FORMAT_VIDEO_modifier,		/**< format modifier (Long)
						  * use only with DMA-BUF and omit for other buffer types */
	SPA_FORMAT_VIDEO_size,			/**< size (Rectangle) */
	SPA_FORMAT_VIDEO_framerate,		/**< frame rate (Fraction) */
	SPA_FORMAT_VIDEO_maxFramerate,		/**< maximum frame rate (Fraction) */
	SPA_FORMAT_VIDEO_views,			/**< number of views (Int) */
	SPA_FORMAT_VIDEO_interlaceMode,		/**< (Id enum spa_video_interlace_mode) */
	SPA_FORMAT_VIDEO_pixelAspectRatio,	/**< (Rectangle) */
	SPA_FORMAT_VIDEO_multiviewMode,		/**< (Id enum spa_video_multiview_mode) */
	SPA_FORMAT_VIDEO_multiviewFlags,	/**< (Id enum spa_video_multiview_flags) */
	SPA_FORMAT_VIDEO_chromaSite,		/**< /Id enum spa_video_chroma_site) */
	SPA_FORMAT_VIDEO_colorRange,		/**< /Id enum spa_video_color_range) */
	SPA_FORMAT_VIDEO_colorMatrix,		/**< /Id enum spa_video_color_matrix) */
	SPA_FORMAT_VIDEO_transferFunction,	/**< /Id enum spa_video_transfer_function) */
	SPA_FORMAT_VIDEO_colorPrimaries,	/**< /Id enum spa_video_color_primaries) */
	SPA_FORMAT_VIDEO_profile,		/**< (Int) */
	SPA_FORMAT_VIDEO_level,			/**< (Int) */
	SPA_FORMAT_VIDEO_H264_streamFormat,	/**< (Id enum spa_h264_stream_format) */
	SPA_FORMAT_VIDEO_H264_alignment,	/**< (Id enum spa_h264_alignment) */
	SPA_FORMAT_VIDEO_H265_streamFormat,	/**< (Id enum spa_h265_stream_format) */
	SPA_FORMAT_VIDEO_H265_alignment,	/**< (Id enum spa_h265_alignment) */
	SPA_FORMAT_VIDEO_deviceId,	        /**< dev_t identifier (Bytes) */

	/* Image Format keys */
	SPA_FORMAT_START_Image = 0x30000,
	/* Binary Format keys */
	SPA_FORMAT_START_Binary = 0x40000,
	/* Stream Format keys */
	SPA_FORMAT_START_Stream = 0x50000,
	/* Application Format keys */
	SPA_FORMAT_START_Application = 0x60000,
	SPA_FORMAT_CONTROL_types,		/**< possible control types (flags choice Int,
						  *  mask of enum spa_control_type) */

	/* PipeWireAO NdArray format keys */
	SPA_FORMAT_START_NdArray = 0x1000000,
	SPA_FORMAT_NDARRAY_elementType,		/**< element type (Id enum spa_element_type) */
	SPA_FORMAT_NDARRAY_shape,		/**< positive logical dimensions (Array of Int) */
	SPA_FORMAT_NDARRAY_layout,		/**< storage order (Id enum spa_ndarray_layout) */
	SPA_FORMAT_NDARRAY_rate,		/**< optional sample rate (Fraction) */
	SPA_FORMAT_NDARRAY_schema,		/**< semantic schema identifier (String) */
	SPA_FORMAT_NDARRAY_profile,		/**< optional exact per-port interpretation
						  * profile (String); changing it requires
						  * format renegotiation */
};

SPA_STATIC_ASSERT(SPA_MEDIA_SUBTYPE_ndarray == 0x1000000,
		"PipeWireAO ndarray media subtype ABI");
SPA_STATIC_ASSERT(SPA_FORMAT_NDARRAY_elementType == 0x1000001,
		"PipeWireAO ndarray element-type key ABI");
SPA_STATIC_ASSERT(SPA_FORMAT_NDARRAY_shape == 0x1000002,
		"PipeWireAO ndarray shape key ABI");
SPA_STATIC_ASSERT(SPA_FORMAT_NDARRAY_layout == 0x1000003,
		"PipeWireAO ndarray layout key ABI");
SPA_STATIC_ASSERT(SPA_FORMAT_NDARRAY_rate == 0x1000004,
		"PipeWireAO ndarray rate key ABI");
SPA_STATIC_ASSERT(SPA_FORMAT_NDARRAY_schema == 0x1000005,
		"PipeWireAO ndarray schema key ABI");
SPA_STATIC_ASSERT(SPA_FORMAT_NDARRAY_profile == 0x1000006,
		"PipeWireAO ndarray profile key ABI");

#define SPA_KEY_FORMAT_DSP		"format.dsp"		/**< a predefined DSP format,
								  *  Ex. "32 bit float mono audio" */

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_PARAM_FORMAT_H */
