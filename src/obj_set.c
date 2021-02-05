#include <string.h>
#include "vm.h"
#include "object.h"
#include "memory.h"
#include "util.h"

static KrkClass * set;
struct Set {
	KrkInstance inst;
	KrkTable entries;
};

#define IS_set(o) krk_isInstanceOf(o,set)
#define AS_set(o) ((struct Set*)AS_OBJECT(o))

static void _set_gcscan(KrkInstance * self) {
	krk_markTable(&((struct Set*)self)->entries);
}

static void _set_gcsweep(KrkInstance * self) {
	krk_freeTable(&((struct Set*)self)->entries);
}

static KrkClass * setiterator;
struct SetIterator {
	KrkInstance inst;
	KrkValue set;
	size_t i;
};
#define IS_setiterator(o) krk_isInstanceOf(o,setiterator)
#define AS_setiterator(o) ((struct SetIterator*)AS_OBJECT(o))

static void _setiterator_gcscan(KrkInstance * self) {
	krk_markValue(((struct SetIterator*)self)->set);
}

#define CURRENT_CTYPE struct Set *
#define CURRENT_NAME  self

#define unpackArray(counter, indexer) for (size_t i = 0; i < counter; ++i) { \
		krk_tableSet(&self->entries, indexer, BOOLEAN_VAL(1)); \
}

KRK_METHOD(set,__init__,{
	METHOD_TAKES_AT_MOST(1);
	krk_initTable(&self->entries);
	if (argc == 2) {
		KrkValue value = argv[1];
		if (IS_TUPLE(value)) {
			unpackArray(AS_TUPLE(value)->values.count, AS_TUPLE(value)->values.values[i]);
		} else if (IS_INSTANCE(value) && AS_INSTANCE(value)->_class == vm.baseClasses.listClass) {
			unpackArray(AS_LIST(value)->count, AS_LIST(value)->values[i]);
		} else if (IS_INSTANCE(value) && AS_INSTANCE(value)->_class == vm.baseClasses.dictClass) {
			unpackArray(AS_DICT(value)->count, krk_dict_nth_key_fast(AS_DICT(value)->capacity, AS_DICT(value)->entries, i));
		} else if (IS_STRING(value)) {
			unpackArray(AS_STRING(value)->codesLength, krk_string_get(2,(KrkValue[]){value,INTEGER_VAL(i)},0));
		} else {
			KrkClass * type = krk_getType(argv[1]);
			if (type->_iter) {
				/* Create the iterator */
				size_t stackOffset = vm.stackTop - vm.stack;
				krk_push(argv[1]);
				krk_push(krk_callSimple(OBJECT_VAL(type->_iter), 1, 0));

				do {
					/* Call it until it gives us itself */
					krk_push(vm.stack[stackOffset]);
					krk_push(krk_callSimple(krk_peek(0), 0, 1));
					if (krk_valuesSame(vm.stack[stackOffset], krk_peek(0))) {
						/* We're done. */
						krk_pop(); /* The result of iteration */
						krk_pop(); /* The iterator */
						break;
					}
					krk_tableSet(&self->entries, krk_peek(0), BOOLEAN_VAL(1));
					krk_pop();
				} while (1);
			} else {
				return krk_runtimeError(vm.exceptions.typeError, "'%s' object is not iterable", krk_typeName(value));
			}
		}
	}
	return argv[0];
})

KRK_METHOD(set,__contains__,{
	METHOD_TAKES_EXACTLY(1);
	KrkValue _unused;
	return BOOLEAN_VAL(krk_tableGet(&self->entries, argv[1], &_unused));
})

KRK_METHOD(set,__repr__,{
	METHOD_TAKES_NONE();
	if (((KrkObj*)self)->inRepr) return OBJECT_VAL(S("{...}")); /* Can this happen? */
	if (!self->entries.capacity) return OBJECT_VAL(S("set()"));
	((KrkObj*)self)->inRepr = 1;
	struct StringBuilder sb = {0};
	pushStringBuilder(&sb,'{');

	size_t c = 0;
	size_t len = self->entries.capacity;
	for (size_t i = 0; i < len; ++i) {
		KrkTableEntry * entry = &self->entries.entries[i];
		if (IS_KWARGS(entry->key)) continue;
		if (c > 0) {
			pushStringBuilderStr(&sb, ", ", 2);
		}
		c++;

		KrkClass * type = krk_getType(entry->key);
		krk_push(entry->key);
		KrkValue result = krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0);
		if (IS_STRING(result)) {
			pushStringBuilderStr(&sb, AS_CSTRING(result), AS_STRING(result)->length);
		}
	}

	pushStringBuilder(&sb,'}');
	((KrkObj*)self)->inRepr = 0;
	return finishStringBuilder(&sb);
})

KRK_METHOD(set,__and__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,set,struct Set*,them);

	KrkValue outSet = OBJECT_VAL(krk_newInstance(set));
	krk_push(outSet);
	FUNC_NAME(set,__init__)(1,&outSet,0);

	KrkClass * type = krk_getType(argv[1]);
	KrkValue contains;
	if (!krk_tableGet(&type->methods, OBJECT_VAL(S("__contains__")), &contains))
		return krk_runtimeError(vm.exceptions.typeError, "unsupported operand type for &");

	size_t len = self->entries.capacity;
	for (size_t i = 0; i < len; ++i) {
		KrkTableEntry * entry = &self->entries.entries[i];
		if (IS_KWARGS(entry->key)) continue;

		krk_push(argv[1]);
		krk_push(entry->key);
		KrkValue result = krk_callSimple(contains, 2, 0);

		if (IS_BOOLEAN(result) && AS_BOOLEAN(result)) {
			krk_tableSet(&AS_set(outSet)->entries, entry->key, BOOLEAN_VAL(1));
		}
	}

	return krk_pop();
})

KRK_METHOD(set,__or__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,set,struct Set*,them);

	KrkValue outSet = OBJECT_VAL(krk_newInstance(set));
	krk_push(outSet);
	FUNC_NAME(set,__init__)(1,&outSet,0);

	krk_tableAddAll(&self->entries, &AS_set(outSet)->entries);
	krk_tableAddAll(&them->entries, &AS_set(outSet)->entries);

	return krk_pop();
})

KRK_METHOD(set,__len__,{
	METHOD_TAKES_NONE();
	return INTEGER_VAL(self->entries.count);
})

KRK_METHOD(set,add,{
	METHOD_TAKES_EXACTLY(1);
	krk_tableSet(&self->entries, argv[1], BOOLEAN_VAL(1));
})

KRK_METHOD(set,remove,{
	METHOD_TAKES_EXACTLY(1);
	if (!krk_tableDelete(&self->entries, argv[1]))
		return krk_runtimeError(vm.exceptions.keyError, "key error");
})

KRK_METHOD(set,discard,{
	METHOD_TAKES_EXACTLY(1);
	krk_tableDelete(&self->entries, argv[1]);
})

KRK_METHOD(set,clear,{
	METHOD_TAKES_NONE();
	krk_freeTable(&self->entries);
	krk_initTable(&self->entries);
})

FUNC_SIG(setiterator,__init__);

KRK_METHOD(set,__iter__,{
	METHOD_TAKES_NONE();
	KrkInstance * output = krk_newInstance(setiterator);
	krk_push(OBJECT_VAL(output));
	FUNC_NAME(setiterator,__init__)(2,(KrkValue[]){krk_peek(0), argv[0]}, 0);
	return krk_pop();
})

#undef CURRENT_CTYPE
#define CURRENT_CTYPE struct SetIterator *

KRK_METHOD(setiterator,__init__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,set,void*,source);
	self->set = argv[1];
	self->i = 0;
	return argv[0];
})

KRK_METHOD(setiterator,__call__,{
	METHOD_TAKES_NONE();
	do {
		if (self->i >= AS_set(self->set)->entries.capacity) return argv[0];
		if (!IS_KWARGS(AS_set(self->set)->entries.entries[self->i].key)) {
			krk_push(AS_set(self->set)->entries.entries[self->i].key);
			self->i++;
			return krk_pop();
		}
		self->i++;
	} while (1);
})

_noexport
void _createAndBind_setClass(void) {
	krk_makeClass(vm.builtins, &set, "set", vm.objectClass);
	set->allocSize = sizeof(struct Set);
	set->_ongcscan = _set_gcscan;
	set->_ongcsweep = _set_gcsweep;
	BIND_METHOD(set,__init__);
	BIND_METHOD(set,__repr__);
	BIND_METHOD(set,__len__);
	BIND_METHOD(set,__and__);
	BIND_METHOD(set,__or__);
	BIND_METHOD(set,__contains__);
	BIND_METHOD(set,__iter__);
	BIND_METHOD(set,add);
	BIND_METHOD(set,remove);
	BIND_METHOD(set,discard);
	BIND_METHOD(set,clear);
	krk_defineNative(&set->methods, ".__str__", FUNC_NAME(set,__repr__));
	krk_finalizeClass(set);

	krk_makeClass(vm.builtins, &setiterator, "setiterator", vm.objectClass);
	setiterator->allocSize = sizeof(struct SetIterator);
	setiterator->_ongcscan = _setiterator_gcscan;
	BIND_METHOD(setiterator,__init__);
	BIND_METHOD(setiterator,__call__);
	krk_finalizeClass(setiterator);
}