#include <string.h>
#include "vm.h"
#include "value.h"
#include "memory.h"
#include "util.h"

KrkClass * Helper;
KrkClass * LicenseReader;

KrkValue krk_dirObject(int argc, KrkValue argv[], int hasKw) {
	if (argc != 1) return krk_runtimeError(vm.exceptions->argumentError, "wrong number of arguments or bad type, got %d\n", argc);

	/* Create a new list instance */
	KrkValue myList = krk_list_of(0,NULL,0);
	krk_push(myList);

	if (IS_INSTANCE(argv[0])) {
		/* Obtain self-reference */
		KrkInstance * self = AS_INSTANCE(argv[0]);

		/* First add each method of the class */
		for (size_t i = 0; i < self->_class->methods.capacity; ++i) {
			if (self->_class->methods.entries[i].key.type != VAL_KWARGS) {
				krk_writeValueArray(AS_LIST(myList),
					self->_class->methods.entries[i].key);
			}
		}

		/* Then add each field of the instance */
		for (size_t i = 0; i < self->fields.capacity; ++i) {
			if (self->fields.entries[i].key.type != VAL_KWARGS) {
				krk_writeValueArray(AS_LIST(myList),
					self->fields.entries[i].key);
			}
		}
	} else {
		if (IS_CLASS(argv[0])) {
			KrkClass * _class = AS_CLASS(argv[0]);
			for (size_t i = 0; i < _class->methods.capacity; ++i) {
				if (_class->methods.entries[i].key.type != VAL_KWARGS) {
					krk_writeValueArray(AS_LIST(myList),
						_class->methods.entries[i].key);
				}
			}
			for (size_t i = 0; i < _class->fields.capacity; ++i) {
				if (_class->fields.entries[i].key.type != VAL_KWARGS) {
					krk_writeValueArray(AS_LIST(myList),
						_class->fields.entries[i].key);
				}
			}
		}
		KrkClass * type = krk_getType(argv[0]);

		for (size_t i = 0; i < type->methods.capacity; ++i) {
			if (type->methods.entries[i].key.type != VAL_KWARGS) {
				krk_writeValueArray(AS_LIST(myList),
					type->methods.entries[i].key);
			}
		}
	}

	/* Prepare output value */
	krk_pop();
	return myList;
}


static KrkValue _len(int argc, KrkValue argv[], int hasKw) {
	if (argc != 1) return krk_runtimeError(vm.exceptions->argumentError, "len() takes exactly one argument");
	/* Shortcuts */
	if (IS_STRING(argv[0])) return INTEGER_VAL(AS_STRING(argv[0])->codesLength);
	if (IS_TUPLE(argv[0])) return INTEGER_VAL(AS_TUPLE(argv[0])->values.count);

	KrkClass * type = krk_getType(argv[0]);
	if (!type->_len) return krk_runtimeError(vm.exceptions->typeError, "object of type '%s' has no len()", krk_typeName(argv[0]));
	krk_push(argv[0]);

	return krk_callSimple(OBJECT_VAL(type->_len), 1, 0);
}

static KrkValue _dir(int argc, KrkValue argv[], int hasKw) {
	if (argc != 1) return krk_runtimeError(vm.exceptions->argumentError, "dir() takes exactly one argument");
	KrkClass * type = krk_getType(argv[0]);
	if (!type->_dir) {
		return krk_dirObject(argc,argv,hasKw); /* Fallback */
	}
	krk_push(argv[0]);
	return krk_callSimple(OBJECT_VAL(type->_dir), 1, 0);
}

static KrkValue _repr(int argc, KrkValue argv[], int hasKw) {
	if (argc != 1) return krk_runtimeError(vm.exceptions->argumentError, "repr() takes exactly one argument");

	/* Everything should have a __repr__ */
	KrkClass * type = krk_getType(argv[0]);
	krk_push(argv[0]);
	return krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0);
}

static KrkValue _ord(int argc, KrkValue argv[], int hasKw) {
	if (argc != 1) return krk_runtimeError(vm.exceptions->argumentError, "ord() takes exactly one argument");

	KrkClass * type = krk_getType(argv[0]);
	KrkValue method;
	if (krk_tableGet(&type->methods, vm.specialMethodNames[METHOD_ORD], &method)) {
		krk_push(argv[0]);
		return krk_callSimple(method, 1, 0);
	}
	return krk_runtimeError(vm.exceptions->argumentError, "ord() expected string of length 1, but got %s", krk_typeName(argv[0]));
}

static KrkValue _chr(int argc, KrkValue argv[], int hasKw) {
	if (argc != 1) return krk_runtimeError(vm.exceptions->argumentError, "chr() takes exactly one argument");

	KrkClass * type = krk_getType(argv[0]);
	KrkValue method;
	if (krk_tableGet(&type->methods, vm.specialMethodNames[METHOD_CHR], &method)) {
		krk_push(argv[0]);
		return krk_callSimple(method, 1, 0);
	}
	return krk_runtimeError(vm.exceptions->argumentError, "chr() expected an integer, but got %s", krk_typeName(argv[0]));
}

static KrkValue _hex(int argc, KrkValue argv[], int hasKw) {
	if (argc != 1 || !IS_INTEGER(argv[0])) return krk_runtimeError(vm.exceptions->argumentError, "hex() expects one int argument");
	char tmp[20];
	krk_integer_type x = AS_INTEGER(argv[0]);
	size_t len = snprintf(tmp, 20, "%s0x" PRIkrk_hex, x < 0 ? "-" : "", x < 0 ? -x : x);
	return OBJECT_VAL(krk_copyString(tmp,len));
}

static KrkValue _any(int argc, KrkValue argv[], int hasKw) {
#define unpackArray(counter, indexer) do { \
	for (size_t i = 0; i < counter; ++i) { \
		if (!krk_isFalsey(indexer)) return BOOLEAN_VAL(1); \
	} \
} while (0)
	unpackIterableFast(argv[0]);
#undef unpackArray
	return BOOLEAN_VAL(0);
}

static KrkValue _all(int argc, KrkValue argv[], int hasKw) {
#define unpackArray(counter, indexer) do { \
	for (size_t i = 0; i < counter; ++i) { \
		if (krk_isFalsey(indexer)) return BOOLEAN_VAL(0); \
	} \
} while (0)
	unpackIterableFast(argv[0]);
#undef unpackArray
	return BOOLEAN_VAL(1);
}

static KrkValue _print(int argc, KrkValue argv[], int hasKw) {
	KrkValue sepVal, endVal;
	char * sep = " "; size_t sepLen = 1;
	char * end = "\n"; size_t endLen = 1;
	if (hasKw) {
		if (krk_tableGet(AS_DICT(argv[argc]), OBJECT_VAL(S("sep")), &sepVal)) {
			if (!IS_STRING(sepVal)) return krk_runtimeError(vm.exceptions->typeError, "'sep' should be a string, not '%s'", krk_typeName(sepVal));
			sep = AS_CSTRING(sepVal);
			sepLen = AS_STRING(sepVal)->length;
		}
		if (krk_tableGet(AS_DICT(argv[argc]), OBJECT_VAL(S("end")), &endVal)) {
			if (!IS_STRING(endVal)) return krk_runtimeError(vm.exceptions->typeError, "'end' should be a string, not '%s'", krk_typeName(endVal));
			end = AS_CSTRING(endVal);
			endLen = AS_STRING(endVal)->length;
		}
	}
	for (int i = 0; i < argc; ++i) {
		KrkValue printable = argv[i];
		if (IS_STRING(printable)) { /* krk_printValue runs repr */
			/* Make sure we handle nil bits correctly. */
			for (size_t j = 0; j < AS_STRING(printable)->length; ++j) {
				fputc(AS_CSTRING(printable)[j], stdout);
			}
		} else {
			krk_printValue(stdout, printable);
		}
		char * thingToPrint = (i == argc - 1) ? end : sep;
		for (size_t j = 0; j < ((i == argc - 1) ? endLen : sepLen); ++j) {
			fputc(thingToPrint[j], stdout);
		}
	}
	return NONE_VAL();
}

/**
 * globals()
 *
 * Returns a dict of names -> values for all the globals.
 */
static KrkValue _globals(int argc, KrkValue argv[], int hasKw) {
	/* Make a new empty dict */
	KrkValue dict = krk_dict_of(0, NULL, 0);
	krk_push(dict);
	/* Copy the globals table into it */
	krk_tableAddAll(krk_currentThread.frames[krk_currentThread.frameCount-1].globals, AS_DICT(dict));
	krk_pop();

	return dict;
}

/**
 * locals()
 *
 * This is a bit trickier. Local names are... complicated. But we can do this!
 */
static KrkValue _locals(int argc, KrkValue argv[], int hasKw) {
	KrkValue dict = krk_dict_of(0, NULL, 0);
	krk_push(dict);

	int index = 1;
	if (argc > 0 && IS_INTEGER(argv[0])) {
		if (AS_INTEGER(argv[0]) < 1) {
			return krk_runtimeError(vm.exceptions->indexError, "Frame index must be >= 1");
		}
		if (krk_currentThread.frameCount < (size_t)AS_INTEGER(argv[0])) {
			return krk_runtimeError(vm.exceptions->indexError, "Frame index out of range");
		}
		index = AS_INTEGER(argv[0]);
	}

	KrkCallFrame * frame = &krk_currentThread.frames[krk_currentThread.frameCount-index];
	KrkFunction * func = frame->closure->function;
	size_t offset = frame->ip - func->chunk.code;

	/* First, we'll populate with arguments */
	size_t slot = 0;
	for (short int i = 0; i < func->requiredArgs; ++i) {
		krk_tableSet(AS_DICT(dict),
			func->requiredArgNames.values[i],
			krk_currentThread.stack[frame->slots + slot]);
		slot++;
	}
	for (short int i = 0; i < func->keywordArgs; ++i) {
		krk_tableSet(AS_DICT(dict),
			func->keywordArgNames.values[i],
			krk_currentThread.stack[frame->slots + slot]);
		slot++;
	}
	if (func->collectsArguments) {
		krk_tableSet(AS_DICT(dict),
			func->requiredArgNames.values[func->requiredArgs],
			krk_currentThread.stack[frame->slots + slot]);
		slot++;
	}
	if (func->collectsKeywords) {
		krk_tableSet(AS_DICT(dict),
			func->keywordArgNames.values[func->keywordArgs],
			krk_currentThread.stack[frame->slots + slot]);
		slot++;
	}
	/* Now we need to find out what non-argument locals are valid... */
	for (size_t i = 0; i < func->localNameCount; ++i) {
		if (func->localNames[i].birthday <= offset &&
			func->localNames[i].deathday >= offset) {
			krk_tableSet(AS_DICT(dict),
				OBJECT_VAL(func->localNames[i].name),
				krk_currentThread.stack[frame->slots + func->localNames[i].id]);
		}
	}

	return krk_pop();
}

static KrkValue _isinstance(int argc, KrkValue argv[], int hasKw) {
	if (argc != 2) return krk_runtimeError(vm.exceptions->argumentError, "isinstance expects 2 arguments, got %d", argc);
	if (IS_CLASS(argv[1])) {
		return BOOLEAN_VAL(krk_isInstanceOf(argv[0], AS_CLASS(argv[1])));
	} else if (IS_TUPLE(argv[1])) {
		for (size_t i = 0; i < AS_TUPLE(argv[1])->values.count; ++i) {
			if (IS_CLASS(AS_TUPLE(argv[1])->values.values[i]) && krk_isInstanceOf(argv[0], AS_CLASS(AS_TUPLE(argv[1])->values.values[i]))) {
				return BOOLEAN_VAL(1);
			}
		}
		return BOOLEAN_VAL(0);
	} else {
		return krk_runtimeError(vm.exceptions->typeError, "isinstance() arg 2 must be class or tuple");
	}
}

static KrkValue _module_repr(int argc, KrkValue argv[], int hasKw) {
	KrkInstance * self = AS_INSTANCE(argv[0]);

	KrkValue name = NONE_VAL();
	krk_tableGet(&self->fields, vm.specialMethodNames[METHOD_NAME], &name);

	if (!IS_STRING(name)) {
		return OBJECT_VAL(S("<module>"));
	}

	KrkValue file = NONE_VAL();
	krk_tableGet(&self->fields, vm.specialMethodNames[METHOD_FILE], &file);

	size_t allocSize = 50 + AS_STRING(name)->length + (IS_STRING(file) ? AS_STRING(file)->length : 20);
	char * tmp = malloc(allocSize);
	size_t len;
	if (IS_STRING(file)) {
		len = snprintf(tmp, allocSize, "<module '%s' from '%s'>", AS_CSTRING(name), AS_CSTRING(file));
	} else {
		len = snprintf(tmp, allocSize, "<module '%s' (built-in)>", AS_CSTRING(name));
	}

	KrkValue out = OBJECT_VAL(krk_copyString(tmp, len));
	free(tmp);
	return out;
}

static KrkValue obj_hash(int argc, KrkValue argv[], int hasKw) {
	return INTEGER_VAL(krk_hashValue(argv[0]));
}

/**
 * object.__str__() / object.__repr__()
 *
 * Base method for all objects to implement __str__ and __repr__.
 * Generally converts to <instance of [TYPE]> and for actual object
 * types (functions, classes, instances, strings...) also adds the pointer
 * address of the object on the heap.
 *
 * Since all types have at least a pseudo-class that should eventually
 * inheret from object() and this is object.__str__ / object.__repr__,
 * all types should have a string representation available through
 * those methods.
 */
static KrkValue _strBase(int argc, KrkValue argv[], int hasKw) {
	KrkClass * type = krk_getType(argv[0]);
	size_t allocSize = sizeof("<instance of . at 0x1234567812345678>") + type->name->length;
	char * tmp = malloc(allocSize);
	size_t len;
	if (IS_OBJECT(argv[0])) {
		len = snprintf(tmp, allocSize, "<instance of %s at %p>", type->name->chars, (void*)AS_OBJECT(argv[0]));
	} else {
		len = snprintf(tmp, allocSize, "<instance of %s>", type->name->chars);
	}
	KrkValue out = OBJECT_VAL(krk_copyString(tmp, len));
	free(tmp);
	return out;
}

static KrkValue _type(int argc, KrkValue argv[], int hasKw) {
	return OBJECT_VAL(krk_getType(argv[0]));
}

KRK_FUNC(getattr,{
	FUNCTION_TAKES_AT_LEAST(2);
	KrkValue object = argv[0];
	CHECK_ARG(1,str,KrkString*,property);
	return krk_valueGetAttribute(object, property->chars);
})


#define IS_Helper(o)  (krk_isInstanceOf(o, Helper))
#define AS_Helper(o)  (AS_INSTANCE(o))
#define IS_LicenseReader(o) (krk_isInstanceOf(o, LicenseReader))
#define AS_LicenseReader(o) (AS_INSTANCE(o))

#define CURRENT_CTYPE KrkInstance *
#define CURRENT_NAME  self

KRK_METHOD(Helper,__repr__,{
	return OBJECT_VAL(S("Type help() for more help, or help(obj) to describe an object."));
})

KRK_METHOD(Helper,__call__,{
	METHOD_TAKES_AT_MOST(1);
	if (!krk_doRecursiveModuleLoad(S("help"))) return NONE_VAL();
	KrkValue helpModule = krk_pop();
	KrkValue callable = NONE_VAL();

	if (argc == 2) {
		krk_tableGet(&AS_INSTANCE(helpModule)->fields, OBJECT_VAL(S("simple")), &callable);
		krk_push(argv[1]);
	} else {
		krk_tableGet(&AS_INSTANCE(helpModule)->fields, OBJECT_VAL(S("interactive")), &callable);
	}

	if (!IS_NONE(callable)) {
		return krk_callSimple(callable, argc == 2, 0);
	}

	return krk_runtimeError(vm.exceptions->typeError, "unexpected error");
})

KRK_METHOD(LicenseReader,__repr__,{
	return OBJECT_VAL(S("Copyright 2020-2021 K. Lange <klange@toaruos.org>. Type `license()` for more information."));
})

KRK_METHOD(LicenseReader,__call__,{
	METHOD_TAKES_NONE();
	if (!krk_doRecursiveModuleLoad(S("help"))) return NONE_VAL();
	KrkValue helpModule = krk_pop();

	KrkValue text = NONE_VAL();
	krk_tableGet(&AS_INSTANCE(helpModule)->fields, OBJECT_VAL(S("__licenseText")), &text);

	if (IS_STRING(text)) {
		printf("%s\n", AS_CSTRING(text));
		return NONE_VAL();
	}

	return krk_runtimeError(vm.exceptions->typeError, "unexpected error");
})

static KrkValue _property_repr(int argc, KrkValue argv[], int hasKw) {
	if (argc != 1 || !IS_PROPERTY(argv[0])) return krk_runtimeError(vm.exceptions->typeError, "?");
	struct StringBuilder sb = {0};
	pushStringBuilderStr(&sb, "Property(", 9);

	KrkValue method = AS_PROPERTY(argv[0])->method;

	if (IS_NATIVE(method)) {
		pushStringBuilderStr(&sb, (char*)AS_NATIVE(method)->name, strlen(AS_NATIVE(method)->name));
	} else if (IS_CLOSURE(method)) {
		pushStringBuilderStr(&sb, AS_CLOSURE(method)->function->name->chars, AS_CLOSURE(method)->function->name->length);
	}

	pushStringBuilder(&sb,')');
	return finishStringBuilder(&sb);
}

static KrkValue _property_doc(int argc, KrkValue argv[], int hasKw) {
	if (argc != 1 || !IS_PROPERTY(argv[0])) return krk_runtimeError(vm.exceptions->typeError, "?");
	KrkValue method = AS_PROPERTY(argv[0])->method;
	if (IS_NATIVE(method) && AS_NATIVE(method)->doc) {
		return OBJECT_VAL(krk_copyString(AS_NATIVE(method)->doc, strlen(AS_NATIVE(method)->doc)));
	} else if (IS_CLOSURE(method)) {
		return OBJECT_VAL(AS_CLOSURE(method)->function->docstring);
	}
	return NONE_VAL();
}

static KrkValue _property_name(int argc, KrkValue argv[], int hasKw) {
	if (argc != 1 || !IS_PROPERTY(argv[0])) return krk_runtimeError(vm.exceptions->typeError, "?");
	KrkValue method = AS_PROPERTY(argv[0])->method;
	if (IS_NATIVE(method) && AS_NATIVE(method)->name) {
		return OBJECT_VAL(krk_copyString(AS_NATIVE(method)->name, strlen(AS_NATIVE(method)->name)));
	} else if (IS_CLOSURE(method)) {
		return OBJECT_VAL(AS_CLOSURE(method)->function->name);
	}
	return NONE_VAL();
}

static KrkValue _property_method(int argc, KrkValue argv[], int hasKw) {
	if (argc != 1 || !IS_PROPERTY(argv[0])) return krk_runtimeError(vm.exceptions->typeError, "?");
	return AS_PROPERTY(argv[0])->method;
}

_noexport
void _createAndBind_builtins(void) {
	vm.baseClasses->objectClass = krk_newClass(S("object"), NULL);
	krk_push(OBJECT_VAL(vm.baseClasses->objectClass));

	krk_defineNative(&vm.baseClasses->objectClass->methods, ":__class__", _type);
	krk_defineNative(&vm.baseClasses->objectClass->methods, ".__dir__", krk_dirObject);
	krk_defineNative(&vm.baseClasses->objectClass->methods, ".__str__", _strBase);
	krk_defineNative(&vm.baseClasses->objectClass->methods, ".__repr__", _strBase); /* Override if necesary */
	krk_defineNative(&vm.baseClasses->objectClass->methods, ".__hash__", obj_hash);
	krk_finalizeClass(vm.baseClasses->objectClass);
	vm.baseClasses->objectClass->docstring = S("Base class for all types.");

	vm.baseClasses->moduleClass = krk_newClass(S("module"), vm.baseClasses->objectClass);
	krk_push(OBJECT_VAL(vm.baseClasses->moduleClass));
	krk_defineNative(&vm.baseClasses->moduleClass->methods, ".__repr__", _module_repr);
	krk_defineNative(&vm.baseClasses->moduleClass->methods, ".__str__", _module_repr);
	krk_finalizeClass(vm.baseClasses->moduleClass);
	vm.baseClasses->moduleClass->docstring = S("Type of imported modules and packages.");

	vm.builtins = krk_newInstance(vm.baseClasses->moduleClass);
	krk_attachNamedObject(&vm.modules, "__builtins__", (KrkObj*)vm.builtins);
	krk_attachNamedObject(&vm.builtins->fields, "object", (KrkObj*)vm.baseClasses->objectClass);
	krk_pop();
	krk_pop();

	krk_attachNamedObject(&vm.builtins->fields, "__name__", (KrkObj*)S("__builtins__"));
	krk_attachNamedValue(&vm.builtins->fields, "__file__", NONE_VAL());
	krk_attachNamedObject(&vm.builtins->fields, "__doc__",
		(KrkObj*)S("Internal module containing built-in functions and classes."));

	krk_makeClass(vm.builtins, &vm.baseClasses->propertyClass, "Property", vm.baseClasses->objectClass);
	krk_defineNative(&vm.baseClasses->propertyClass->methods, ".__repr__", _property_repr);
	krk_defineNative(&vm.baseClasses->propertyClass->methods, ":__doc__", _property_doc);
	krk_defineNative(&vm.baseClasses->propertyClass->methods, ":__name__", _property_name);
	krk_defineNative(&vm.baseClasses->propertyClass->methods, ":__method__", _property_method);
	krk_finalizeClass(vm.baseClasses->propertyClass);

	krk_makeClass(vm.builtins, &Helper, "Helper", vm.baseClasses->objectClass);
	Helper->docstring = S("Special object that prints a helpful message when passed to @ref repr");
	BIND_METHOD(Helper,__call__)->doc = "@arguments obj=None\nPrints the help documentation attached to @p obj or "
		"starts the interactive help system.";
	BIND_METHOD(Helper,__repr__);
	krk_finalizeClass(Helper);
	krk_attachNamedObject(&vm.builtins->fields, "help", (KrkObj*)krk_newInstance(Helper));

	krk_makeClass(vm.builtins, &LicenseReader, "LicenseReader", vm.baseClasses->objectClass);
	LicenseReader->docstring = S("Special object that prints Kuroko's copyright information when passed to @ref repr");
	BIND_METHOD(LicenseReader,__call__)->doc = "Print the full license statement.";
	BIND_METHOD(LicenseReader,__repr__);
	krk_finalizeClass(LicenseReader);
	krk_attachNamedObject(&vm.builtins->fields, "license", (KrkObj*)krk_newInstance(LicenseReader));

	BUILTIN_FUNCTION("isinstance", _isinstance, "Determine if an object is an instance of the given class or one if its subclasses.");
	BUILTIN_FUNCTION("globals", _globals, "Return a mapping of names in the current global namespace.");
	BUILTIN_FUNCTION("locals", _locals, "Return a mapping of names in the current local namespace.");
	BUILTIN_FUNCTION("dir", _dir, "Return a list of known property names for a given object.");
	BUILTIN_FUNCTION("len", _len, "Return the length of a given sequence object.");
	BUILTIN_FUNCTION("repr", _repr, "Produce a string representation of the given object.");
	BUILTIN_FUNCTION("print", _print, "Print values to the standard output descriptor.");
	BUILTIN_FUNCTION("ord", _ord, "Obtain the ordinal integer value of a codepoint or byte.");
	BUILTIN_FUNCTION("chr", _chr, "Convert an integer codepoint to its string representation.");
	BUILTIN_FUNCTION("hex", _hex, "Convert an integer value to a hexadecimal string.");
	BUILTIN_FUNCTION("any", _any, "Returns True if at least one element in the given iterable is truthy, False otherwise.");
	BUILTIN_FUNCTION("all", _all, "Returns True if every element in the given iterable is truthy, False otherwise.");
	BUILTIN_FUNCTION("getattr", FUNC_NAME(krk,getattr), "Obtain a property of an object as if it were accessed by the dot operator.");
}

