#include <kuroko/chunk.h>
#include <kuroko/memory.h>
#include <kuroko/vm.h>

void krk_initChunk(KrkChunk * chunk) {
	chunk->count = 0;
	chunk->capacity = 0;
	chunk->code = NULL;

	chunk->linesCount = 0;
	chunk->linesCapacity = 0;
	chunk->lines = NULL;
	chunk->filename = NULL;
	krk_initValueArray(&chunk->constants);
}

static void addLine(KrkChunk * chunk, size_t line) {
	if (chunk->linesCount && chunk->lines[chunk->linesCount-1].line == line) return;
	if (chunk->linesCapacity < chunk->linesCount + 1) {
		int old = chunk->linesCapacity;
		chunk->linesCapacity = GROW_CAPACITY(old);
		chunk->lines = GROW_ARRAY(KrkLineMap, chunk->lines, old, chunk->linesCapacity);
	}
	chunk->lines[chunk->linesCount] = (KrkLineMap){chunk->count, line};
	chunk->linesCount++;
}

void krk_writeChunk(KrkChunk * chunk, uint8_t byte, size_t line) {
	if (chunk->capacity < chunk->count + 1) {
		int old = chunk->capacity;
		chunk->capacity = GROW_CAPACITY(old);
		chunk->code = GROW_ARRAY(uint8_t, chunk->code, old, chunk->capacity);
	}

	chunk->code[chunk->count] = byte;
	addLine(chunk, line);
	chunk->count++;
}

void krk_freeChunk(KrkChunk * chunk) {
	FREE_ARRAY(uint8_t, chunk->code, chunk->capacity);
	FREE_ARRAY(size_t, chunk->lines, chunk->capacity);
	krk_freeValueArray(&chunk->constants);
	krk_initChunk(chunk);
}

size_t krk_addConstant(KrkChunk * chunk, KrkValue value) {
	krk_push(value);
	krk_writeValueArray(&chunk->constants, value);
	krk_pop();
	return chunk->constants.count - 1;
}

void krk_emitConstant(KrkChunk * chunk, size_t ind, size_t line) {
	if (ind >= 256) {
		krk_writeChunk(chunk, OP_CONSTANT_LONG, line);
		krk_writeChunk(chunk, 0xFF & (ind >> 16), line);
		krk_writeChunk(chunk, 0xFF & (ind >> 8), line);
		krk_writeChunk(chunk, 0xFF & (ind >> 0), line);
	} else {
		krk_writeChunk(chunk, OP_CONSTANT, line);
		krk_writeChunk(chunk, ind, line);
	}
}

size_t krk_writeConstant(KrkChunk * chunk, KrkValue value, size_t line) {
	size_t ind = krk_addConstant(chunk, value);
	krk_emitConstant(chunk, ind, line);
	return ind;
}
