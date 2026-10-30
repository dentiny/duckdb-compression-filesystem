#include "brotli/decode.h"
#include "state.h"

#include <stdio.h>
#include <stdlib.h>

/* Construct the exhausted single-type command block at the root of
 * CVE-2026-111497, without requiring an enormous compressed fixture. Exercise
 * the public streaming entry point with both short and fast-path input. */
static int CheckInvalidBlockSwitch(size_t input_size) {
	uint8_t input[64] = {0};
	uint8_t output[64] = {0};
	const uint8_t *next_in = input;
	uint8_t *next_out = output;
	size_t available_in = input_size;
	size_t available_out = sizeof(output);
	BrotliDecoderState *state = BrotliDecoderCreateInstance(NULL, NULL, NULL);
	if (!state) {
		return 1;
	}
	/* Match the decoder's shared allocation for the block Huffman tables. */
	state->block_type_trees =
	    (HuffmanCode *)calloc(3 * (BROTLI_HUFFMAN_MAX_SIZE_258 + BROTLI_HUFFMAN_MAX_SIZE_26), sizeof(HuffmanCode));
	if (!state->block_type_trees) {
		BrotliDecoderDestroyInstance(state);
		return 1;
	}
	state->block_len_trees = state->block_type_trees + 3 * BROTLI_HUFFMAN_MAX_SIZE_258;
	state->state = BROTLI_STATE_COMMAND_BEGIN;
	state->num_block_types[1] = 1;
	state->block_length[1] = 0;

	BrotliDecoderResult result =
	    BrotliDecoderDecompressStream(state, &available_in, &next_in, &available_out, &next_out, NULL);
	int failed = result != BROTLI_DECODER_RESULT_ERROR ||
	             BrotliDecoderGetErrorCode(state) != BROTLI_DECODER_ERROR_FORMAT_BLOCK_SWITCH || next_out != output;
	/* A format error must remain terminal when the caller supplies more input. */
	next_in = input;
	available_in = sizeof(input);
	result = BrotliDecoderDecompressStream(state, &available_in, &next_in, &available_out, &next_out, NULL);
	failed |= result != BROTLI_DECODER_RESULT_ERROR || available_in != sizeof(input) ||
	          BrotliDecoderGetErrorCode(state) != BROTLI_DECODER_ERROR_FORMAT_BLOCK_SWITCH;
	BrotliDecoderDestroyInstance(state);
	if (failed) {
		fprintf(stderr, "Invalid block switch was not terminal (input size %zu)\n", input_size);
	}
	return failed;
}

int main(void) {
	return CheckInvalidBlockSwitch(0) | CheckInvalidBlockSwitch(1) | CheckInvalidBlockSwitch(64);
}
