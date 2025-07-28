#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libsd.h>
#include <malloc.h>

#include "../Audio.h"

#define SPU_VOICES_PER_CORE 24
#define SPU_NUM_CORES 2
#define AUDIO_MAX_CONTEXTS (SPU_VOICES_PER_CORE * SPU_NUM_CORES)

// Descriptive constants to replace magic numbers
#define SPU_PITCH_BASE 4096
#define SPU_VOLUME_MAX 127
#define SPU_DMA_CHANNEL 0

// --- Global state for managing all active audio contexts ---
static cc_bool voice_in_use[AUDIO_MAX_CONTEXTS];
static struct AudioContext* active_contexts[AUDIO_MAX_CONTEXTS];

// Forward declarations
static int AllocVoice(void);
static void FreeVoice(int idx);
static cc_result Audio_Update(struct AudioContext* ctx);


struct AudioBuffer {
	int available;
	int bytesLeft;
	void* samples;
};

struct AudioContext {
	int bufHead, channels;
	int voice_idx;
	int voice_handle;
	u32 spu_addr;
	u32 spu_buf_size;
	cc_bool playing;
	int playing_buf_idx;

	struct AudioBuffer bufs[AUDIO_MAX_BUFFERS];
	int count, sampleRate;
};
#define AUDIO_OVERRIDE_ALLOC
#include "../_AudioBase.h"
#include "../Funcs.h"

cc_bool AudioBackend_Init(void) {
	SifInitRpc(0);
	if (SifLoadModule("rom0:LIBSD", 0, NULL) < 0) return false;
	if (SifLoadModule("rom0:SDRDRV", 0, NULL) < 0) return false;

	if (sceSdInit(SD_INIT_COLD) != 0) return false;

	sceSdSetCoreAttr(SD_CORE_0 | SD_C_MVOLL, SPU_VOLUME_MAX);
	sceSdSetCoreAttr(SD_CORE_0 | SD_C_MVOLR, SPU_VOLUME_MAX);
	sceSdSetCoreAttr(SD_CORE_1 | SD_C_MVOLL, SPU_VOLUME_MAX);
	sceSdSetCoreAttr(SD_CORE_1 | SD_C_MVOLR, SPU_VOLUME_MAX);
	
	Mem_Set(voice_in_use, 0, sizeof(voice_in_use));
	Mem_Set(active_contexts, 0, sizeof(active_contexts));
	return true;
}

void AudioBackend_Tick(void) {
	for (int i = 0; i < AUDIO_MAX_CONTEXTS; i++) {
		if (active_contexts[i]) {
			Audio_Update(active_contexts[i]);
		}
	}
}

void AudioBackend_Free(void) {
	sceSdShutdown();
}

cc_result Audio_Init(struct AudioContext* ctx, int buffers) {
	if (!ctx) return ERR_NULL_POINTER;
	int idx = AllocVoice();
	if (idx == -1) return ERR_NOT_SUPPORTED;

	int core = idx / SPU_VOICES_PER_CORE;
	int voice_in_core = idx % SPU_VOICES_PER_CORE;

	ctx->voice_idx = idx;
	ctx->voice_handle = voice_in_core | (core << 8);
	ctx->spu_addr = 0;
	ctx->spu_buf_size = 0;
	ctx->playing = false;
	ctx->playing_buf_idx = -1;
	ctx->count   = buffers;
	ctx->bufHead = 0;

	Mem_Set(ctx->bufs, 0, sizeof(ctx->bufs));
	for (int i = 0; i < buffers; i++) {
		ctx->bufs[i].available = true;
	}
	
	active_contexts[idx] = ctx;
	return 0;
}

void Audio_Close(struct AudioContext* ctx) {
	if (!ctx || ctx->voice_idx == -1) return;

	int idx = ctx->voice_idx;
	active_contexts[idx] = NULL; // Deregister first
	
	sceSdVoiceTransStatus(SPU_DMA_CHANNEL, SD_TRANS_STATUS_STOP);
	sceSdSetSwitch(ctx->voice_handle, SD_S_KOFF);
	
	if (ctx->spu_addr) {
		sceSdFree(ctx->spu_addr);
	}
	FreeVoice(idx);
	
	ctx->voice_idx = -1; // Invalidate context
}

cc_result Audio_SetFormat(struct AudioContext* ctx, int channels, int sampleRate, int playbackRate) {
	if (!ctx || ctx->voice_idx == -1) return ERR_INVALID_ARGUMENT;

	sampleRate = Audio_AdjustSampleRate(sampleRate, playbackRate);
	ctx->channels   = channels;
	ctx->sampleRate = sampleRate;

	u16 pitch = (u16)(((float)sampleRate / 48000.0f) * (float)SPU_PITCH_BASE);
	if (sceSdSetParam(ctx->voice_handle | SD_VP_PITCH, pitch) < 0) return ERR_INVALID_STATE;
	return 0;
}

cc_result Audio_SetVolume(struct AudioContext* ctx, int volume) {
	if (!ctx || ctx->voice_idx == -1) return ERR_INVALID_ARGUMENT;
	u8 spu_vol = (volume * SPU_VOLUME_MAX) / 255;
	if (sceSdSetParam(ctx->voice_handle | SD_VP_VOLL, spu_vol) < 0) return ERR_INVALID_STATE;
	if (sceSdSetParam(ctx->voice_handle | SD_VP_VOLR, spu_vol) < 0) return ERR_INVALID_STATE;
	return 0;
}

cc_result Audio_QueueChunk(struct AudioContext* ctx, struct AudioChunk* chunk) {
	if (!ctx || ctx->voice_idx == -1) return ERR_INVALID_ARGUMENT;
	for (int i = 0; i < ctx->count; i++) {
		int idx = (ctx->bufHead + i) % ctx->count;
		struct AudioBuffer* buf = &ctx->bufs[idx];
		if (!buf->available) continue;

		buf->samples   = chunk->data;
		buf->bytesLeft = chunk->size;
		buf->available = false;

		if (chunk->size > ctx->spu_buf_size) {
			if (ctx->spu_addr) sceSdFree(ctx->spu_addr);
			ctx->spu_buf_size = chunk->size;
			ctx->spu_addr = sceSdMaloc(ctx->spu_buf_size);
			if (ctx->spu_addr == 0) {
				ctx->spu_buf_size = 0;
				return ERR_OUT_OF_MEMORY;
			}
		}
		return 0;
	}
	return ERR_INVALID_ARGUMENT;
}

cc_result Audio_Play(struct AudioContext* ctx) {
	if (!ctx || ctx->voice_idx == -1) return ERR_INVALID_ARGUMENT;
	ctx->playing = true;
	return 0;
}

cc_result Audio_Poll(struct AudioContext* ctx, int* inUse) {
	if (!ctx) return ERR_NULL_POINTER;
	int count = 0;
	for (int i = 0; i < ctx->count; i++) {
		if (!ctx->bufs[i].available) count++;
	}
	*inUse = count;
	return 0;
}

static cc_result Audio_Update(struct AudioContext* ctx) {
	if (!ctx->playing || ctx->voice_idx == -1) return 0;

	if (ctx->playing_buf_idx == -1 || (sceSdGetSwitch(ctx->voice_handle) & SD_S_ENDX)) {
		if (ctx->playing_buf_idx != -1) {
			ctx->bufs[ctx->playing_buf_idx].available = true;
			ctx->playing_buf_idx = -1;
		}

		struct AudioBuffer* next_buf = &ctx->bufs[ctx->bufHead];
		if (next_buf->available) {
			ctx->playing = false;
			return 0;
		}

		FlushCache(0);

		if (sceSdVoiceTrans(SPU_DMA_CHANNEL, SD_TRANS_WRITE | SD_TRANS_MODE_DMA, 
		                    next_buf->samples, ctx->spu_addr, next_buf->bytesLeft) < 0) {
			return ERR_INVALID_STATE;
		}
		sceSdVoiceTransStatus(SPU_DMA_CHANNEL, SD_TRANS_STATUS_WAIT);

		if (sceSdSetParam(ctx->voice_handle | SD_VP_ADDR, ctx->spu_addr) < 0) return ERR_INVALID_STATE;
		if (sceSdSetSwitch(ctx->voice_handle, SD_S_KON) < 0) return ERR_INVALID_STATE;

		ctx->playing_buf_idx = ctx->bufHead;
		ctx->bufHead = (ctx->bufHead + 1) % ctx->count;
	}
	return 0;
}

/*########################################################################################################################*
*------------------------------------------------------Stream context-----------------------------------------------------*
*#########################################################################################################################*/
cc_result StreamContext_SetFormat(struct AudioContext* ctx, int channels, int sampleRate, int playbackRate) { return Audio_SetFormat(ctx, channels, sampleRate, playbackRate); }
cc_result StreamContext_Enqueue(struct AudioContext* ctx, struct AudioChunk* chunk) { return Audio_QueueChunk(ctx, chunk); }
cc_result StreamContext_Play(struct AudioContext* ctx) { return Audio_Play(ctx); }

cc_result StreamContext_Pause(struct AudioContext* ctx) {
	if (!ctx || ctx->voice_idx == -1) return ERR_INVALID_ARGUMENT;
	ctx->playing = false;
	if (sceSdSetSwitch(ctx->voice_handle, SD_S_KOFF) < 0) return ERR_INVALID_STATE;
	return 0;
}

cc_result StreamContext_Update(struct AudioContext* ctx, int* inUse) { return Audio_Poll(ctx, inUse); }

/*########################################################################################################################*
*------------------------------------------------------Sound context------------------------------------------------------*
*#########################################################################################################################*/
// Sound context functions remain the same, they just call the base functions which now have better error checking.
cc_bool SoundContext_FastPlay(struct AudioContext* ctx, struct AudioData* data) { return true; }
cc_result SoundContext_PlayData(struct AudioContext* ctx, struct AudioData* data) {
    cc_result res;
	if ((res = Audio_SetFormat(ctx,  data->channels, data->sampleRate, data->rate))) return res;
	if ((res = Audio_QueueChunk(ctx, &data->chunk))) return res;
	if ((res = Audio_Play(ctx))) return res;
	return 0;
}
cc_result SoundContext_PollBusy(struct AudioContext* ctx, cc_bool* isBusy) {
	if (!ctx) return ERR_NULL_POINTER;
	int inUse = 0;
	cc_result res = Audio_Poll(ctx, &inUse);
	if (res) return res;
	*isBusy = (ctx->voice_idx != -1) && (ctx->playing || (inUse > 0));
	return 0;
}

/*########################################################################################################################*
*--------------------------------------------------------Audio misc-------------------------------------------------------*
*#########################################################################################################################*/
cc_bool Audio_DescribeError(cc_result res, cc_string* dst) { return false; }
static int totalSize;
cc_result Audio_AllocChunks(cc_uint32 size, struct AudioChunk* chunks, int numChunks) {
	size = (size + 63) & ~63;
	void* dst = memalign(64, size * numChunks);
	if (!dst) return ERR_OUT_OF_MEMORY;
	totalSize += size * numChunks;
	for (int i = 0; i < numChunks; i++) {
		chunks[i].data = dst + size * i;
		chunks[i].size = size;
	}
	return 0;
}
void Audio_FreeChunks(struct AudioChunk* chunks, int numChunks) {
	if (chunks && chunks[0].data) {
		free(chunks[0].data);
	}
}

/*########################################################################################################################*
*----------------------------------------------------PS2 specific helpers-------------------------------------------------*
*#########################################################################################################################*/
static int AllocVoice(void) {
	int i, state;
	int voice_idx = -1;

	// This is a critical section as it modifies the global voice_in_use array.
	// Disable interrupts to prevent race conditions from other threads or handlers.
	state = DI();
	for (i = 0; i < AUDIO_MAX_CONTEXTS; i++) { // Corrected loop boundary
		if (!voice_in_use[i]) {
			voice_in_use[i] = true;
			voice_idx = i;
			break;
		}
	}
	EI(state);
	return voice_idx;
}

static void FreeVoice(int idx) {
	if (idx < 0 || idx >= AUDIO_MAX_CONTEXTS) return;
	int state;

	// This is a critical section.
	state = DI();
	voice_in_use[idx] = false;
	EI(state);
}
