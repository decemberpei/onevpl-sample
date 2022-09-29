#include "encode.h"
#include<iostream>

#include "vpl/mfxjpeg.h"
#include "vpl/mfxvideo.h"
#include "vpl/mfxdispatcher.h"

using namespace std;

mfxStatus ReadRawFrame(mfxFrameSurface1* surface, FILE* f) {
	mfxU16 w, h, i, pitch;
	size_t bytes_read;
	mfxU8* ptr;
	mfxFrameInfo* info = &surface->Info;
	mfxFrameData* data = &surface->Data;

	w = info->Width;
	h = info->Height;

	switch (info->FourCC) {
	case MFX_FOURCC_I420:
		// read luminance plane (Y)
		pitch = data->Pitch;
		ptr = data->Y;
		for (i = 0; i < h; i++) {
			bytes_read = (mfxU32)fread(ptr + i * pitch, 1, w, f);
			if (w != bytes_read)
				return MFX_ERR_MORE_DATA;
		}

		// read chrominance (U, V)
		pitch /= 2;
		h /= 2;
		w /= 2;
		ptr = data->U;
		for (i = 0; i < h; i++) {
			bytes_read = (mfxU32)fread(ptr + i * pitch, 1, w, f);
			if (w != bytes_read)
				return MFX_ERR_MORE_DATA;
		}

		ptr = data->V;
		for (i = 0; i < h; i++) {
			bytes_read = (mfxU32)fread(ptr + i * pitch, 1, w, f);
			if (w != bytes_read)
				return MFX_ERR_MORE_DATA;
		}
		break;
	case MFX_FOURCC_NV12:
		// Y
		pitch = data->Pitch;
		for (i = 0; i < h; i++) {
			bytes_read = fread(data->Y + i * pitch, 1, w, f);
			if (w != bytes_read)
				return MFX_ERR_MORE_DATA;
		}
		// UV
		h /= 2;
		for (i = 0; i < h; i++) {
			bytes_read = fread(data->UV + i * pitch, 1, w, f);
			if (w != bytes_read)
				return MFX_ERR_MORE_DATA;
		}
		break;
	case MFX_FOURCC_RGB4:
		// Y
		pitch = data->Pitch;
		for (i = 0; i < h; i++) {
			bytes_read = fread(data->B + i * pitch, 1, pitch, f);
			if (pitch != bytes_read)
				return MFX_ERR_MORE_DATA;
		}
		break;
	default:
		printf("Unsupported FourCC code, skip LoadRawFrame\n");
		break;
	}

	return MFX_ERR_NONE;
}

mfxStatus ReadRawFrame_InternalMem(mfxFrameSurface1* surface, FILE* f) {
	bool is_more_data = false;

	// Map makes surface writable by CPU for all implementations
	mfxStatus sts = surface->FrameInterface->Map(surface, MFX_MAP_WRITE);
	if (sts != MFX_ERR_NONE) {
		printf("mfxFrameSurfaceInterface->Map failed (%d)\n", sts);
		return sts;
	}

	sts = ReadRawFrame(surface, f);
	if (sts != MFX_ERR_NONE) {
		if (sts == MFX_ERR_MORE_DATA)
			is_more_data = true;
		else
			return sts;
	}

	// Unmap/release returns local device access for all implementations
	sts = surface->FrameInterface->Unmap(surface);
	if (sts != MFX_ERR_NONE) {
		printf("mfxFrameSurfaceInterface->Unmap failed (%d)\n", sts);
		return sts;
	}

	return (is_more_data == true) ? MFX_ERR_MORE_DATA : MFX_ERR_NONE;
}

void WriteEncodedStream(mfxBitstream& bs, FILE* f) {
	fwrite(bs.Data + bs.DataOffset, 1, bs.DataLength, f);
	bs.DataLength = 0;
	return;
}

void encode() {

	bool isDraining = false;
	bool isStillGoing = true;
	mfxU32 framenum = 0;

	FILE* source = fopen("decoded.yuv", "rb");
	FILE* sink = fopen("encoded.h264", "wb");
	mfxBitstream bitstream = {};

	mfxLoader loader = NULL;
	mfxConfig cfg;
	mfxVariant cfgVal;
	mfxSession session = NULL;
	mfxIMPL impl;

	mfxStatus sts = MFX_ERR_NONE;

	mfxVideoParam encodeParams = {};
	mfxFrameSurface1* encSurfaceIn = NULL;

	mfxSyncPoint syncp = {};

	loader = MFXLoad();
	if (NULL == loader)
		goto cleanup;

	cfg = MFXCreateConfig(loader);
	if (NULL == cfg)
		goto cleanup;

	cfgVal.Type = MFX_VARIANT_TYPE_U32;
	cfgVal.Data.U32 = MFX_CODEC_AVC;
	sts = MFXSetConfigFilterProperty(cfg, (mfxU8*)"mfxImplDescription.mfxEncoderDescription.encoder.CodecID", cfgVal);
	if (MFX_ERR_NONE != sts)
		goto cleanup;

	sts = MFXCreateSession(loader, 0, &session);
	if (MFX_ERR_NONE != sts)
		goto cleanup;

	sts = MFXQueryIMPL(session, &impl);
	if (MFX_ERR_NONE != sts)
		goto cleanup;

	// Initialize encode parameters
	encodeParams.mfx.CodecId = MFX_CODEC_AVC;
	encodeParams.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;
	encodeParams.mfx.TargetKbps = 4000;
	encodeParams.mfx.RateControlMethod = MFX_RATECONTROL_VBR;
	encodeParams.mfx.FrameInfo.FrameRateExtN = 30;
	encodeParams.mfx.FrameInfo.FrameRateExtD = 1;
	if (MFX_IMPL_SOFTWARE == 0) {
		encodeParams.mfx.FrameInfo.FourCC = MFX_FOURCC_I420;
	}
	else {
		encodeParams.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
	}
#define ALIGN16(value)           (((value + 15) >> 4) << 4)
	encodeParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
	encodeParams.mfx.FrameInfo.CropW = 1920;
	encodeParams.mfx.FrameInfo.CropH = 1080;
	encodeParams.mfx.FrameInfo.Width = ALIGN16(1920);
	encodeParams.mfx.FrameInfo.Height = ALIGN16(1080);
	encodeParams.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;

	// Initialize encoder
	sts = MFXVideoENCODE_Init(session, &encodeParams);
	if (MFX_ERR_NONE != sts)
		goto cleanup;

	bitstream.MaxLength = 2000000;
	bitstream.Data = (mfxU8*)calloc(bitstream.MaxLength, sizeof(mfxU8));

	printf("Input colorspace: ");
	switch (encodeParams.mfx.FrameInfo.FourCC) {
	case MFX_FOURCC_I420: // CPU input
		printf("I420 (aka yuv420p)\n");
		break;
	case MFX_FOURCC_NV12: // GPU input
		printf("NV12\n");
		break;
	default:
		printf("Unsupported color format\n");
		goto cleanup;
		break;
	}

	while (isStillGoing == true) {
		// Load a new frame if not draining
		if (isDraining == false) {
			sts = MFXMemory_GetSurfaceForEncode(session, &encSurfaceIn);
			if (MFX_ERR_NONE != sts)
				goto cleanup;

			sts = ReadRawFrame_InternalMem(encSurfaceIn, source);
			if (sts != MFX_ERR_NONE)
				isDraining = true;
		}

		sts = MFXVideoENCODE_EncodeFrameAsync(session,
			NULL,
			(isDraining == true) ? NULL : encSurfaceIn,
			&bitstream,
			&syncp);

		if (!isDraining) {
			sts = encSurfaceIn->FrameInterface->Release(encSurfaceIn);
			if (MFX_ERR_NONE != sts)
				goto cleanup;
		}
		switch (sts) {
		case MFX_ERR_NONE:
			// MFX_ERR_NONE and syncp indicate output is available
			if (syncp) {
				// Encode output is not available on CPU until sync operation
				// completes
				sts = MFXVideoCORE_SyncOperation(session, syncp, 100);
				if (MFX_ERR_NONE != sts)
					goto cleanup;

				WriteEncodedStream(bitstream, sink);
				framenum++;
			}
			break;
		case MFX_ERR_NOT_ENOUGH_BUFFER:
			// This example deliberatly uses a large output buffer with immediate
			// write to disk for simplicity. Handle when frame size exceeds
			// available buffer here
			break;
		case MFX_ERR_MORE_DATA:
			// The function requires more data to generate any output
			if (isDraining == true)
				isStillGoing = false;
			break;
		case MFX_ERR_DEVICE_LOST:
			// For non-CPU implementations,
			// Cleanup if device is lost
			break;
		case MFX_WRN_DEVICE_BUSY:
			// For non-CPU implementations,
			// Wait a few milliseconds then try again
			break;
		default:
			printf("unknown status %d\n", sts);
			isStillGoing = false;
			break;
		}
	}
cleanup:

	if (source)
		fclose(source);
	if (sink)
		fclose(sink);

	MFXVideoENCODE_Close(session);
	MFXClose(session);

	if (bitstream.Data)
		free(bitstream.Data);

	if (loader)
		MFXUnload(loader);

	cout << "encode." << endl;
}