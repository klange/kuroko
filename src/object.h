#pragma once

#include <stdio.h>

#include "kuroko.h"
#include "value.h"
#include "chunk.h"
#include "table.h"

#ifdef ENABLE_THREADING
#include <pthread.h>
#endif

#define isObjType(v,t) (IS_OBJECT(v) && (AS_OBJECT(v)->type == (t)))
#define OBJECT_TYPE(value) (AS_OBJECT(value)->type)
#define IS_STRING(value)   isObjType(value, OBJ_STRING)
#define AS_STRING(value)   ((KrkString *)AS_OBJECT(value))
#define AS_CSTRING(value)  (((KrkString *)AS_OBJECT(value))->chars)
#define IS_BYTES(value)    isObjType(value, OBJ_BYTES)
#define AS_BYTES(value)    ((KrkBytes*)AS_OBJECT(value))
#define IS_FUNCTION(value) isObjType(value, OBJ_FUNCTION)
#define AS_FUNCTION(value) ((KrkFunction *)AS_OBJECT(value))
#define IS_NATIVE(value)   isObjType(value, OBJ_NATIVE)
#define AS_NATIVE(value)   ((KrkNative *)AS_OBJECT(value))
#define IS_CLOSURE(value)  isObjType(value, OBJ_CLOSURE)
#define AS_CLOSURE(value)  ((KrkClosure *)AS_OBJECT(value))
#define IS_CLASS(value)    isObjType(value, OBJ_CLASS)
#define AS_CLASS(value)    ((KrkClass *)AS_OBJECT(value))
#define IS_INSTANCE(value) isObjType(value, OBJ_INSTANCE)
#define AS_INSTANCE(value) ((KrkInstance *)AS_OBJECT(value))
#define IS_BOUND_METHOD(value) isObjType(value, OBJ_BOUND_METHOD)
#define AS_BOUND_METHOD(value) ((KrkBoundMethod*)AS_OBJECT(value))

#define IS_TUPLE(value)    isObjType(value, OBJ_TUPLE)
#define AS_TUPLE(value)    ((KrkTuple *)AS_OBJECT(value))
#define IS_PROPERTY(value) isObjType(value, OBJ_PROPERTY)
#define AS_PROPERTY(value) ((KrkProperty *)AS_OBJECT(value))

typedef enum {
	OBJ_FUNCTION,
	OBJ_NATIVE,
	OBJ_CLOSURE,
	OBJ_STRING,
	OBJ_UPVALUE,
	OBJ_CLASS,
	OBJ_INSTANCE,
	OBJ_BOUND_METHOD,
	OBJ_TUPLE,
	OBJ_BYTES,
	OBJ_PROPERTY,
} ObjType;

struct Obj {
	ObjType type;
	unsigned char isMarked:1;
	unsigned char inRepr:1;
	unsigned char generation:2;
	unsigned char isImmortal:1;
	uint32_t hash;
	struct Obj * next;
};

typedef enum {
	KRK_STRING_ASCII = 0,
	KRK_STRING_UCS1  = 1,
	KRK_STRING_UCS2  = 2,
	KRK_STRING_UCS4  = 4,
	KRK_STRING_INVALID = 5,
} KrkStringType;

struct ObjString {
	KrkObj obj;
	KrkStringType type;
	size_t length;
	size_t codesLength;
	char * chars;
	void * codes;
};

typedef struct {
	KrkObj obj;
	size_t length;
	uint8_t * bytes;
} KrkBytes;

typedef struct KrkUpvalue {
	KrkObj obj;
	int location;
	KrkValue   closed;
	struct KrkUpvalue * next;
	struct ThreadState * owner;
} KrkUpvalue;

typedef struct {
	size_t id;
	size_t birthday;
	size_t deathday;
	KrkString * name;
} KrkLocalEntry;

struct KrkInstance;

typedef struct {
	KrkObj obj;
	short requiredArgs;
	short keywordArgs;
	size_t upvalueCount;
	KrkChunk chunk;
	KrkString * name;
	KrkString * docstring;
	KrkValueArray requiredArgNames;
	KrkValueArray keywordArgNames;
	size_t localNameCapacity;
	size_t localNameCount;
	KrkLocalEntry * localNames;
	unsigned char collectsArguments:1;
	unsigned char collectsKeywords:1;
	struct KrkInstance * globalsContext;
} KrkFunction;

typedef struct {
	KrkObj obj;
	KrkFunction * function;
	KrkUpvalue ** upvalues;
	size_t upvalueCount;
} KrkClosure;

typedef void (*KrkCleanupCallback)(struct KrkInstance *);

typedef struct KrkClass {
	KrkObj obj;
	KrkString * name;
	KrkString * filename;
	KrkString * docstring;
	struct KrkClass * base;
	KrkTable methods;
	KrkTable fields;
	size_t allocSize;
	KrkCleanupCallback _ongcscan;
	KrkCleanupCallback _ongcsweep;

	/* Quick access for common stuff */
	KrkObj * _getter;
	KrkObj * _setter;
	KrkObj * _getslice;
	KrkObj * _reprer;
	KrkObj * _tostr;
	KrkObj * _call;
	KrkObj * _init;
	KrkObj * _eq;
	KrkObj * _len;
	KrkObj * _enter;
	KrkObj * _exit;
	KrkObj * _delitem;
	KrkObj * _iter;
	KrkObj * _getattr;
	KrkObj * _dir;
	KrkObj * _setslice;
	KrkObj * _delslice;
} KrkClass;

typedef struct KrkInstance {
	KrkObj obj;
	KrkClass * _class;
	KrkTable fields;
} KrkInstance;

typedef struct {
	KrkObj obj;
	KrkValue receiver;
	KrkObj * method;
} KrkBoundMethod;

typedef KrkValue (*NativeFn)(int argCount, KrkValue* args, int hasKwargs);
typedef struct {
	KrkObj obj;
	NativeFn function;
	const char * name;
	const char * doc;
	int isMethod;
} KrkNative;

typedef struct {
	KrkObj obj;
	KrkValueArray values;
} KrkTuple;

typedef struct {
	KrkObj obj;
	KrkValue method;
} KrkProperty;

typedef struct {
	KrkInstance inst;
	KrkValueArray values;
#ifdef ENABLE_THREADING
	pthread_rwlock_t rwlock;
#endif
} KrkList;

typedef struct {
	KrkInstance inst;
	KrkTable entries;
} KrkDict;

struct DictItems {
	KrkInstance inst;
	KrkValue dict;
	size_t i;
};

struct DictKeys {
	KrkInstance inst;
	KrkValue dict;
	size_t i;
};

#define AS_LIST(value) (&((KrkList *)AS_OBJECT(value))->values)
#define AS_DICT(value) (&((KrkDict *)AS_OBJECT(value))->entries)

/**
 * @brief Yield ownership of a C string to the GC and obtain a string object.
 *
 * Creates a string object represented by the characters in 'chars' and of
 * length 'length'. The source string must be nil-terminated and must
 * remain valid for the lifetime of the object, as its ownership is yielded
 * to the GC. Useful for strings which were allocated on the heap by
 * other mechanisms.
 *
 * 'chars' must be a nil-terminated C string representing a UTF-8
 * character sequence.
 *
 * @param chars  C string to take ownership of.
 * @param length Length of the C string.
 * @return A string object.
 */
extern KrkString * krk_takeString(char * chars, size_t length);

/**
 * @brief Obtain a string object representation of the given C string.
 *
 * Converts the C string 'chars' into a string object by checking the
 * string table for it. If the string table does not have an equivalent
 * string, a new one will be created by copying 'chars'.
 *
 * 'chars' must be a nil-terminated C string representing a UTF-8
 * character sequence.
 *
 * @param chars  C string to convert to a string object.
 * @param length Length of the C string.
 * @return A string object.
 */
extern KrkString * krk_copyString(const char * chars, size_t length);

/**
 * @brief Ensure that a codepoint representation of a string is available.
 *
 * Obtain an untyped pointer to the codepoint representation of a string.
 * If the string does not have a codepoint representation allocated, it will
 * be generated by this function and remain with the string for the duration
 * of its lifetime.
 *
 * @param string String to obtain the codepoint representation of.
 * @return A pointer to the bytes of the codepoint representation.
 */
extern void * krk_unicodeString(KrkString * string);

/**
 * @brief Obtain the codepoint at a given index in a string.
 *
 * This is a convenience function which ensures that a Unicode codepoint
 * representation has been generated and returns the codepoint value at
 * the requested index. If you need to find multiple codepoints, it
 * is recommended that you use the KRK_STRING_FAST macro after calling
 * krk_unicodeString instead.
 *
 * @note This function does not perform any bounds checking.
 *
 * @param string String to index into.
 * @param index  Offset of the codepoint to obtain.
 * @return Integer representation of the codepoint at the requested index.
 */
extern uint32_t krk_unicodeCodepoint(KrkString * string, size_t index);

/**
 * @brief Convert an integer codepoint to a UTF-8 byte representation.
 *
 * Converts a single codepoint to a sequence of bytes containing the
 * UTF-8 representation. 'out' must be allocated by the caller.
 *
 * @param value Codepoint to encode.
 * @param out   Array to write UTF-8 sequence into.
 * @return The length of the UTF-8 sequence, in bytes.
 */
extern size_t krk_codepointToBytes(krk_integer_type value, unsigned char * out);

/* Internal stuff. */
extern KrkFunction *    krk_newFunction(void);
extern KrkNative * krk_newNative(NativeFn function, const char * name, int type);
extern KrkClosure *     krk_newClosure(KrkFunction * function);
extern KrkUpvalue *     krk_newUpvalue(int slot);
extern KrkClass *       krk_newClass(KrkString * name, KrkClass * base);
extern KrkInstance *    krk_newInstance(KrkClass * _class);
extern KrkBoundMethod * krk_newBoundMethod(KrkValue receiver, KrkObj * method);
extern KrkTuple *       krk_newTuple(size_t length);
extern KrkProperty * krk_newProperty(KrkValue method);

#define KRK_STRING_FAST(string,offset)  (uint32_t)\
	(string->type <= 1 ? ((uint8_t*)string->codes)[offset] : \
	(string->type == 2 ? ((uint16_t*)string->codes)[offset] : \
	((uint32_t*)string->codes)[offset]))

#define CODEPOINT_BYTES(cp) (cp < 0x80 ? 1 : (cp < 0x800 ? 2 : (cp < 0x10000 ? 3 : 4)))

extern KrkBytes * krk_newBytes(size_t length, uint8_t * source);
extern void krk_bytesUpdateHash(KrkBytes * bytes);

extern void krk_tupleUpdateHash(KrkTuple * self);
