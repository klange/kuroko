/**
 * @file obj_gen.c
 * @brief Generator objects.
 *
 * Generator objects track runtime state so they can be resumed and yielded from.
 * Any function with a `yield` statement in its body is implicitly transformed
 * into a generator object when called.
 */
#include <string.h>
#include "vm.h"
#include "value.h"
#include "memory.h"
#include "util.h"
#include "debug.h"

static KrkClass * generator;
/**
 * @brief Generator object implementation.
 * @extends KrkInstance
 */
struct generator {
	KrkInstance inst;
	KrkClosure * closure;
	KrkValue * args;
	size_t argCount;
	uint8_t * ip;
	int running;
	int started;
	KrkValue result;
};

#define AS_generator(o) ((struct generator *)AS_OBJECT(o))
#define IS_generator(o) (krk_isInstanceOf(o, generator))

#define CURRENT_CTYPE struct generator *
#define CURRENT_NAME  self

static void _generator_gcscan(KrkInstance * _self) {
	struct generator * self = (struct generator*)_self;
	krk_markObject((KrkObj*)self->closure);
	for (size_t i = 0; i < self->argCount; ++i) {
		krk_markValue(self->args[i]);
	}
	krk_markValue(self->result);
}

static void _generator_gcsweep(KrkInstance * self) {
	free(((struct generator*)self)->args);
}

static void _set_generator_done(struct generator * self) {
	self->ip = NULL;
}

/**
 * @brief Create a generator object from a closure and set of arguments.
 *
 * Initializes the generator object, attaches the argument list, and sets up
 * the execution state to point to the start of the function's code object.
 *
 * @param closure  Function object to transform.
 * @param argsIn   Array of arguments passed to the call.
 * @param argCount Number of arguments in @p argsIn
 * @return A @ref generator object.
 */
KrkInstance * krk_buildGenerator(KrkClosure * closure, KrkValue * argsIn, size_t argCount) {
	/* Copy the args */
	KrkValue * args = malloc(sizeof(KrkValue) * (argCount));
	memcpy(args, argsIn, sizeof(KrkValue) * argCount);

	/* Create a generator object */
	struct generator * self = (struct generator *)krk_newInstance(generator);
	self->args = args;
	self->argCount = argCount;
	self->closure = closure;
	self->ip = self->closure->function->chunk.code;
	self->result = NONE_VAL();
	return (KrkInstance *)self;
}

KRK_METHOD(generator,__repr__,{
	METHOD_TAKES_NONE();

	size_t estimatedLength = sizeof("<generator object  at 0x1234567812345678>") + 1 + self->closure->function->name->length;
	char * tmp = malloc(estimatedLength);
	size_t lenActual = snprintf(tmp, estimatedLength, "<generator object %s at %p>",
		self->closure->function->name->chars,
		(void*)self);

	return OBJECT_VAL(krk_takeString(tmp,lenActual));
})

KRK_METHOD(generator,__iter__,{
	METHOD_TAKES_NONE();
	return OBJECT_VAL(self);
})

KRK_METHOD(generator,__call__,{
	METHOD_TAKES_AT_MOST(1);
	if (!self->ip) return OBJECT_VAL(self);
	/* Prepare frame */
	KrkCallFrame * frame = &krk_currentThread.frames[krk_currentThread.frameCount++];
	frame->closure = self->closure;
	frame->ip      = self->ip;
	frame->slots   = krk_currentThread.stackTop - krk_currentThread.stack;
	frame->outSlots = frame->slots;
	frame->globals = &self->closure->function->globalsContext->fields;

	/* Stick our stack on their stack */
	for (size_t i = 0; i < self->argCount; ++i) {
		krk_push(self->args[i]);
	}

	if (self->started) {
		krk_pop();
		if (argc > 1) {
			krk_push(argv[1]);
		} else {
			krk_push(NONE_VAL());
		}
	}

	/* Jump into the iterator */
	self->running = 1;
	size_t stackBefore = krk_currentThread.stackTop - krk_currentThread.stack;
	KrkValue result = krk_runNext();
	size_t stackAfter = krk_currentThread.stackTop - krk_currentThread.stack;
	self->running = 0;

	self->started = 1;

	if (IS_KWARGS(result) && AS_INTEGER(result) == 0) {
		self->result = krk_pop();
		_set_generator_done(self);
		return OBJECT_VAL(self);
	}

	/* Was there an exception? */
	if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) {
		_set_generator_done(self);
		return NONE_VAL();
	}

	/* Determine the stack state */
	if (stackAfter > stackBefore) {
		size_t newArgs = stackAfter - stackBefore;
		self->args = realloc(self->args, sizeof(KrkValue) * (self->argCount + newArgs));
		self->argCount += newArgs;
	} else if (stackAfter < stackBefore) {
		size_t deadArgs = stackBefore - stackAfter;
		self->args = realloc(self->args, sizeof(KrkValue) * (self->argCount - deadArgs));
		self->argCount -= deadArgs;
	}

	/* Save stack entries */
	memcpy(self->args, krk_currentThread.stackTop - self->argCount, sizeof(KrkValue) * self->argCount);
	self->ip      = frame->ip;

	krk_currentThread.stackTop = krk_currentThread.stack + frame->slots;

	return result;
})

KRK_METHOD(generator,send,{
	METHOD_TAKES_EXACTLY(1);
	if (!self->started && !IS_NONE(argv[1])) {
		return krk_runtimeError(vm.exceptions->typeError, "Can not send non-None value to just-started generator");
	}
	return FUNC_NAME(generator,__call__)(argc,argv,0);
})

KRK_METHOD(generator,__finish__,{
	METHOD_TAKES_NONE();
	return self->result;
})

/*
 * For compatibility with Python...
 */
KRK_METHOD(generator,gi_running,{
	METHOD_TAKES_NONE();
	return BOOLEAN_VAL(self->running);
})

_noexport
void _createAndBind_generatorClass(void) {
	generator = ADD_BASE_CLASS(vm.baseClasses->generatorClass, "generator", vm.baseClasses->objectClass);
	generator->allocSize = sizeof(struct generator);
	generator->_ongcscan = _generator_gcscan;
	generator->_ongcsweep = _generator_gcsweep;
	BIND_METHOD(generator,__iter__);
	BIND_METHOD(generator,__call__);
	BIND_METHOD(generator,__repr__);
	BIND_METHOD(generator,__finish__);
	BIND_METHOD(generator,send);
	BIND_PROP(generator,gi_running);
	krk_defineNative(&generator->methods, "__str__", FUNC_NAME(generator,__repr__));
	krk_finalizeClass(generator);
}
