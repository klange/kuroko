#pragma once

#include "kuroko.h"
#include "value.h"

/**
 * Opcodes
 *
 * These are pretty much entirely based on the clox opcodes from the book.
 * There's not really much else to add here, since the VM is sufficient for
 * our needs. Most of the interesting changes happen in the compiler.
 */
typedef enum {
	OP_ADD = 1,
	OP_BITAND,
	OP_BITNEGATE,
	OP_BITOR,
	OP_BITXOR,
	OP_CALL_STACK,
	OP_CLEANUP_WITH,
	OP_CLOSE_UPVALUE,
	OP_CREATE_PROPERTY,
	OP_DIVIDE,
	OP_DOCSTRING,
	OP_EQUAL,
	OP_FALSE,
	OP_FINALIZE,
	OP_GREATER,
	OP_INHERIT,
	OP_INVOKE_DELETE,
	OP_INVOKE_DELSLICE,
	OP_INVOKE_GETSLICE,
	OP_INVOKE_GETTER,
	OP_INVOKE_SETSLICE,
	OP_INVOKE_SETTER,
	OP_IS,
	OP_LESS,
	OP_MODULO,
	OP_MULTIPLY,
	OP_NEGATE,
	OP_NONE,
	OP_NOT,
	OP_POP,
	OP_POW,
	OP_RAISE,
	OP_RETURN,
	OP_SHIFTLEFT,
	OP_SHIFTRIGHT,
	OP_SUBTRACT,
	OP_SWAP,
	OP_TRUE,

	OP_CALL = 64,
	OP_CLASS,
	OP_CLOSURE,
	OP_CONSTANT,
	OP_DEFINE_GLOBAL,
	OP_DEL_GLOBAL,
	OP_DEL_PROPERTY,
	OP_DUP,
	OP_EXPAND_ARGS,
	OP_GET_GLOBAL,
	OP_GET_LOCAL,
	OP_GET_PROPERTY,
	OP_GET_SUPER,
	OP_GET_UPVALUE,
	OP_IMPORT,
	OP_IMPORT_FROM,
	OP_INC,
	OP_KWARGS,
	OP_METHOD,
	OP_SET_GLOBAL,
	OP_SET_LOCAL,
	OP_SET_PROPERTY,
	OP_SET_UPVALUE,
	OP_TUPLE,
	OP_UNPACK,

	OP_JUMP_IF_FALSE = 128,
	OP_JUMP_IF_TRUE,
	OP_JUMP,
	OP_LOOP,
	OP_PUSH_TRY,
	OP_PUSH_WITH,

	OP_CALL_LONG = 192,
	OP_CLASS_LONG,
	OP_CLOSURE_LONG,
	OP_CONSTANT_LONG,
	OP_DEFINE_GLOBAL_LONG,
	OP_DEL_GLOBAL_LONG,
	OP_DEL_PROPERTY_LONG,
	OP_DUP_LONG,
	OP_EXPAND_ARGS_LONG,
	OP_GET_GLOBAL_LONG,
	OP_GET_LOCAL_LONG,
	OP_GET_PROPERTY_LONG,
	OP_GET_SUPER_LONG,
	OP_GET_UPVALUE_LONG,
	OP_IMPORT_LONG,
	OP_IMPORT_FROM_LONG,
	OP_INC_LONG,
	OP_KWARGS_LONG,
	OP_METHOD_LONG,
	OP_SET_GLOBAL_LONG,
	OP_SET_LOCAL_LONG,
	OP_SET_PROPERTY_LONG,
	OP_SET_UPVALUE_LONG,
	OP_TUPLE_LONG,
	OP_UNPACK_LONG,
} KrkOpCode;

typedef struct {
	size_t startOffset;
	size_t line;
} KrkLineMap;

/**
 * Bytecode chunks
 */
typedef struct {
	size_t  count;
	size_t  capacity;
	uint8_t * code;

	size_t linesCount;
	size_t linesCapacity;
	KrkLineMap * lines;

	KrkString * filename;
	KrkValueArray constants;
} KrkChunk;

extern void krk_initChunk(KrkChunk * chunk);
extern void krk_writeChunk(KrkChunk * chunk, uint8_t byte, size_t line);
extern void krk_freeChunk(KrkChunk * chunk);
extern size_t krk_addConstant(KrkChunk * chunk, KrkValue value);
extern void krk_emitConstant(KrkChunk * chunk, size_t ind, size_t line);
extern size_t krk_writeConstant(KrkChunk * chunk, KrkValue value, size_t line);
