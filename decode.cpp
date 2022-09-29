#include<iostream>

#include "decode.h"

#include "vpl/mfxjpeg.h"
#include "vpl/mfxvideo.h"
#include "vpl/mfxdispatcher.h"

using namespace std;

mfxStatus ReadEncodedStream(mfxBitstream& bs, FILE* f) {
	mfxU8* p0 = bs.Data;
	mfxU8* p1 = bs.Data + bs.DataOffset;
	if (bs.DataOffset > bs.MaxLength - 1) {
		return MFX_ERR_NOT_ENOUGH_BUFFER;
	}
	if (bs.DataLength + bs.DataOffset > bs.MaxLength) {
		return MFX_ERR_NOT_ENOUGH_BUFFER;
	}
	for (mfxU32 i = 0; i < bs.DataLength; i++) {
		*(p0++) = *(p1++);
	}
	bs.DataOffset = 0;
	bs.DataLength += (mfxU32)fread(bs.Data + bs.DataLength, 1, bs.MaxLength - bs.DataLength, f);
	if (bs.DataLength == 0)
		return MFX_ERR_MORE_DATA;

	return MFX_ERR_NONE;
}

// Write raw I420 frame to file
mfxStatus WriteRawFrame(mfxFrameSurface1* surface, FILE* f) {
	mfxU16 w, h, i, pitch;
	mfxFrameInfo* info = &surface->Info;
	mfxFrameData* data = &surface->Data;

	w = info->Width;
	h = info->Height;

	// write the output to disk
	switch (info->FourCC) {
	case MFX_FOURCC_I420:
		// Y
		pitch = data->Pitch;
		for (i = 0; i < h; i++) {
			fwrite(data->Y + i * pitch, 1, w, f);
		}
		// U
		pitch /= 2;
		h /= 2;
		w /= 2;
		for (i = 0; i < h; i++) {
			fwrite(data->U + i * pitch, 1, w, f);
		}
		// V
		for (i = 0; i < h; i++) {
			fwrite(data->V + i * pitch, 1, w, f);
		}
		break;
	case MFX_FOURCC_NV12:
		// Y
		pitch = data->Pitch;
		for (i = 0; i < h; i++) {
			fwrite(data->Y + i * pitch, 1, w, f);
		}
		// UV
		h /= 2;
		for (i = 0; i < h; i++) {
			fwrite(data->UV + i * pitch, 1, w, f);
		}
		break;
	case MFX_FOURCC_RGB4:
		// Y
		pitch = data->Pitch;
		for (i = 0; i < h; i++) {
			fwrite(data->B + i * pitch, 1, pitch, f);
		}
		break;
	default:
		return MFX_ERR_UNSUPPORTED;
		break;
	}

	return MFX_ERR_NONE;
}

// Write raw frame to file
mfxStatus WriteRawFrame_InternalMem(mfxFrameSurface1* surface, FILE* f) {
	mfxStatus sts = surface->FrameInterface->Map(surface, MFX_MAP_READ);
	if (sts != MFX_ERR_NONE) {
		printf("mfxFrameSurfaceInterface->Map failed (%d)\n", sts);
		return sts;
	}

	sts = WriteRawFrame(surface, f);
	if (sts != MFX_ERR_NONE) {
		printf("Error in WriteRawFrame\n");
		return sts;
	}

	sts = surface->FrameInterface->Unmap(surface);
	if (sts != MFX_ERR_NONE) {
		printf("mfxFrameSurfaceInterface->Unmap failed (%d)\n", sts);
		return sts;
	}

	sts = surface->FrameInterface->Release(surface);
	if (sts != MFX_ERR_NONE) {
		printf("mfxFrameSurfaceInterface->Release failed (%d)\n", sts);
		return sts;
	}

	return sts;
}


void decode() {
	cout << "decode." << endl;

	mfxConfig cfg;
	mfxVariant cfgVal[3];
	mfxSession session = NULL;
	mfxIMPL impl;
	mfxBitstream bitstream = {};
	mfxVideoParam decodeParams = {};

	mfxStatus sts = MFX_ERR_NONE;

	bool isDraining = false;
	bool isStillGoing = true;
	mfxFrameSurface1* decSurfaceOut = NULL;
	mfxSyncPoint syncp = {};
	mfxU32 framenum = 0;

	FILE* source = fopen("test-1080p.h264", "rb");
	FILE* sink = fopen("decoded.yuv", "wb");

	mfxLoader loader = MFXLoad();
	if (NULL == loader)
		goto cleanup;

	cfg = MFXCreateConfig(loader);
	if (NULL == cfg)
		goto cleanup;

	cfgVal[1].Type = MFX_VARIANT_TYPE_U32;
	cfgVal[1].Data.U32 = MFX_CODEC_AVC;
	sts = MFXSetConfigFilterProperty(cfg, (mfxU8*)"mfxImplDescription.mfxDecoderDescription.decoder.CodecID", cfgVal[1]);
	if (MFX_ERR_NONE != sts)
		goto cleanup;

	sts = MFXCreateSession(loader, 0, &session);
	if (MFX_ERR_NONE != sts)
		goto cleanup;

	sts = MFXQueryIMPL(session, &impl);
	if (MFX_ERR_NONE != sts)
		goto cleanup;

	bitstream.MaxLength = 2000000;
	bitstream.Data = (mfxU8*)calloc(bitstream.MaxLength, sizeof(mfxU8));
	if (!bitstream.Data)
		goto cleanup;
	bitstream.CodecId = MFX_CODEC_AVC;

	sts = ReadEncodedStream(bitstream, source);
	if (MFX_ERR_NONE != sts)
		goto cleanup;

	decodeParams.mfx.CodecId = MFX_CODEC_AVC;
	decodeParams.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
	sts = MFXVideoDECODE_DecodeHeader(session, &bitstream, &decodeParams);
	if (MFX_ERR_NONE != sts)
		goto cleanup;

	sts = MFXVideoDECODE_Init(session, &decodeParams);
	if (MFX_ERR_NONE != sts)
		goto cleanup;
	printf("Output colorspace: ");
	switch (decodeParams.mfx.FrameInfo.FourCC) {
	case MFX_FOURCC_I420: // CPU output
		printf("I420 (aka yuv420p)\n");
		break;
	case MFX_FOURCC_NV12: // GPU output
		printf("NV12\n");
		break;
	default:
		printf("Unsupported color format\n");
		goto cleanup;
	}

	while (isStillGoing == true) {
		// Load encoded stream if not draining
		if (isDraining == false) {
			sts = ReadEncodedStream(bitstream, source);
			if (sts != MFX_ERR_NONE)
				isDraining = true;
		}

		sts = MFXVideoDECODE_DecodeFrameAsync(session,
			(isDraining) ? NULL : &bitstream,
			NULL,
			&decSurfaceOut,
			&syncp);

		switch (sts) {
		case MFX_ERR_NONE:
			do {
				sts = decSurfaceOut->FrameInterface->Synchronize(decSurfaceOut, 100);
				if (MFX_ERR_NONE == sts) {
					sts = WriteRawFrame_InternalMem(decSurfaceOut, sink);
					if (MFX_ERR_NONE != sts)
						goto cleanup;
					framenum++;
				}
			} while (sts == MFX_WRN_IN_EXECUTION);
			break;
		case MFX_ERR_MORE_DATA:
			// The function requires more bitstream at input before decoding can
			// proceed
			if (isDraining)
				isStillGoing = false;
			break;
		case MFX_ERR_MORE_SURFACE:
			// The function requires more frame surface at output before decoding
			// can proceed. This applies to external memory allocations and should
			// not be expected for a simple internal allocation case like this
			break;
		case MFX_ERR_DEVICE_LOST:
			// For non-CPU implementations,
			// Cleanup if device is lost
			break;
		case MFX_WRN_DEVICE_BUSY:
			// For non-CPU implementations,
			// Wait a few milliseconds then try again
			break;
		case MFX_WRN_VIDEO_PARAM_CHANGED:
			// The decoder detected a new sequence header in the bitstream.
			// Video parameters may have changed.
			// In external memory allocation case, might need to reallocate the
			// output surface
			break;
		case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM:
			// The function detected that video parameters provided by the
			// application are incompatible with initialization parameters. The
			// application should close the component and then reinitialize it
			break;
		case MFX_ERR_REALLOC_SURFACE:
			// Bigger surface_work required. May be returned only if
			// mfxInfoMFX::EnableReallocRequest was set to ON during initialization.
			// This applies to external memory allocations and should not be
			// expected for a simple internal allocation case like this
			break;
		default:
			printf("unknown status %d\n", sts);
			isStillGoing = false;
			break;
		}
	}

	printf("Decoded %d frames. \n", framenum);
cleanup:

	MFXVideoDECODE_Close(session);
	MFXClose(session);

	if (loader)
		MFXUnload(loader);

	if (bitstream.Data)
		free(bitstream.Data);

	if (source)
		fclose(source);
	if (sink)
		fclose(sink);
}