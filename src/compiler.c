#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "kuroko.h"
#include "compiler.h"
#include "memory.h"
#include "scanner.h"
#include "object.h"
#include "debug.h"
#include "vm.h"

/**
 * There's nothing really especially different here compared to the Lox
 * compiler from Crafting Interpreters. A handful of additional pieces
 * of functionality are added, and some work is done to make blocks use
 * indentation instead of braces, but the basic layout and operation
 * of the compiler are the same top-down Pratt parser.
 *
 * The parser error handling has been improved over the Lox compiler with
 * the addition of column offsets and a printed copy of the original source
 * line and the offending token.
 *
 * String parsing also includes escape sequence support, so you can print
 * quotation marks properly, as well as escape sequences for terminals.
 *
 * One notable part of the compiler is the handling of list comprehensions.
 * In order to support Python-style syntax, the parser has been set up to
 * support rolling back to a previous state, so that when the compiler sees
 * an expression with references to a variable that has yet to be defined it
 * will first output the expression as if that variable was a global, then it
 * will see the 'in', rewind, parse the rest of the list comprehension, and
 * then output the expression as a loop body, with the correct local references.
 *
 * if/else and try/except blocks also have to similarly handle rollback cases
 * as they can not peek forward to see if a statement after an indentation
 * block is an else/except.
 */

typedef struct {
	KrkToken current;
	KrkToken previous;
	int hadError;
	int panicMode;
	int eatingWhitespace;
} Parser;

typedef enum {
	PREC_NONE,
	PREC_ASSIGNMENT, /* = */
	PREC_TERNARY,
	PREC_OR,         /* or */
	PREC_AND,        /* and */
	PREC_COMPARISON, /* < > <= >= in 'not in' */
	PREC_BITOR,      /* | */
	PREC_BITXOR,     /* ^ */
	PREC_BITAND,     /* & */
	PREC_SHIFT,      /* << >> */
	PREC_TERM,       /* + - */
	PREC_FACTOR,     /* * / % */
	PREC_UNARY,      /* ! - not */
	PREC_EXPONENT,   /* ** */
	PREC_CALL,       /* . () */
	PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(int);

typedef struct {
	const char * name;
	ParseFn prefix;
	ParseFn infix;
	Precedence precedence;
} ParseRule;

typedef struct {
	KrkToken name;
	ssize_t depth;
	int isCaptured;
} Local;

typedef struct {
	size_t index;
	int    isLocal;
} Upvalue;

typedef enum {
	TYPE_FUNCTION,
	TYPE_MODULE,
	TYPE_METHOD,
	TYPE_INIT,
	TYPE_LAMBDA,
	TYPE_STATIC,
	TYPE_PROPERTY,
} FunctionType;

typedef struct Compiler {
	struct Compiler * enclosing;
	KrkFunction * function;
	FunctionType type;
	size_t localCount;
	size_t scopeDepth;
	size_t localsSpace;
	Local  * locals;
	size_t upvaluesSpace;
	Upvalue * upvalues;

	size_t loopLocalCount;
	size_t breakCount;
	size_t breakSpace;
	int * breaks;
	size_t continueCount;
	size_t continueSpace;
	int * continues;

	size_t localNameCapacity;
} Compiler;

typedef struct ClassCompiler {
	struct ClassCompiler * enclosing;
	KrkToken name;
} ClassCompiler;

static Parser parser;
static Compiler * current = NULL;
static ClassCompiler * currentClass = NULL;
static int inDel = 0;

static KrkChunk * currentChunk() {
	return &current->function->chunk;
}

#define EMIT_CONSTANT_OP(opc, arg) do { if (arg < 256) { emitBytes(opc, arg); } \
	else { emitBytes(opc ## _LONG, arg >> 16); emitBytes(arg >> 8, arg); } } while (0)

static int isMethod(int type) {
	return type == TYPE_METHOD || type == TYPE_INIT || type == TYPE_PROPERTY;
}

static void initCompiler(Compiler * compiler, FunctionType type) {
	compiler->enclosing = current;
	current = compiler;
	compiler->function = NULL;
	compiler->type = type;
	compiler->scopeDepth = 0;
	compiler->function = krk_newFunction();
	compiler->function->globalsContext = (KrkInstance*)krk_currentThread.module;
	compiler->localCount = 0;
	compiler->localsSpace = 8;
	compiler->locals = GROW_ARRAY(Local,NULL,0,8);
	compiler->upvaluesSpace = 0;
	compiler->upvalues = NULL;
	compiler->breakCount = 0;
	compiler->breakSpace = 0;
	compiler->breaks = NULL;
	compiler->continueCount = 0;
	compiler->continueSpace = 0;
	compiler->continues = NULL;
	compiler->loopLocalCount = 0;
	compiler->localNameCapacity = 0;

	if (type != TYPE_MODULE) {
		current->function->name = krk_copyString(parser.previous.start, parser.previous.length);
	}

	if (isMethod(type)) {
		Local * local = &current->locals[current->localCount++];
		local->depth = 0;
		local->isCaptured = 0;
		local->name.start = "self";
		local->name.length = 4;
	}
}

static void parsePrecedence(Precedence precedence);
static ssize_t parseVariable(const char * errorMessage);
static void variable(int canAssign);
static void defineVariable(size_t global);
static ssize_t identifierConstant(KrkToken * name);
static ssize_t resolveLocal(Compiler * compiler, KrkToken * name);
static ParseRule * getRule(KrkTokenType type);
static void defDeclaration();
static void expression();
static void statement();
static void declaration();
static void or_(int canAssign);
static void ternary(int canAssign);
static void and_(int canAssign);
static KrkToken classDeclaration();
static void declareVariable();
static void namedVariable(KrkToken name, int canAssign);
static void addLocal(KrkToken name);
static void string(int canAssign);
static KrkToken decorator(size_t level, FunctionType type);
static void call(int canAssign);

static void finishError(KrkToken * token) {
	size_t i = 0;
	while (token->linePtr[i] && token->linePtr[i] != '\n') i++;

	krk_attachNamedObject(&AS_INSTANCE(krk_currentThread.currentException)->fields, "line",   (KrkObj*)krk_copyString(token->linePtr, i));
	krk_attachNamedObject(&AS_INSTANCE(krk_currentThread.currentException)->fields, "file",   (KrkObj*)currentChunk()->filename);
	krk_attachNamedValue (&AS_INSTANCE(krk_currentThread.currentException)->fields, "lineno", INTEGER_VAL(token->line));
	krk_attachNamedValue (&AS_INSTANCE(krk_currentThread.currentException)->fields, "colno",  INTEGER_VAL(token->col));
	krk_attachNamedValue (&AS_INSTANCE(krk_currentThread.currentException)->fields, "width",  INTEGER_VAL(token->literalWidth));

	if (current->function->name) {
		krk_attachNamedObject(&AS_INSTANCE(krk_currentThread.currentException)->fields, "func", (KrkObj*)current->function->name);
	} else {
		KrkValue name = NONE_VAL();
		krk_tableGet(&krk_currentThread.module->fields, vm.specialMethodNames[METHOD_NAME], &name);
		krk_attachNamedValue(&AS_INSTANCE(krk_currentThread.currentException)->fields, "func", name);
	}

	parser.panicMode = 1;
	parser.hadError = 1;
}

#define error(...) do { if (parser.panicMode) break; krk_runtimeError(vm.exceptions->syntaxError, __VA_ARGS__); finishError(&parser.previous); } while (0)
#define errorAtCurrent(...) do { if (parser.panicMode) break; krk_runtimeError(vm.exceptions->syntaxError, __VA_ARGS__); finishError(&parser.current); } while (0)

static void advance() {
	parser.previous = parser.current;

	for (;;) {
		parser.current = krk_scanToken();

		if (parser.eatingWhitespace &&
			(parser.current.type == TOKEN_INDENTATION || parser.current.type == TOKEN_EOL)) continue;

#ifdef ENABLE_SCAN_TRACING
		if (krk_currentThread.flags & KRK_ENABLE_SCAN_TRACING) {
			fprintf(stderr, "[%s<%d> %d:%d '%.*s'] ",
				getRule(parser.current.type)->name,
				(int)parser.current.type,
				(int)parser.current.line,
				(int)parser.current.col,
				(int)parser.current.length,
				parser.current.start);
		}
#endif

		if (parser.current.type == TOKEN_RETRY) continue;
		if (parser.current.type != TOKEN_ERROR) break;

		errorAtCurrent(parser.current.start);
	}
}

static void startEatingWhitespace() {
	parser.eatingWhitespace++;
	if (parser.current.type == TOKEN_INDENTATION || parser.current.type == TOKEN_EOL) advance();
}

static void stopEatingWhitespace() {
	if (parser.eatingWhitespace == 0) {
		error("Internal scanner error: Invalid nesting of `startEatingWhitespace`/`stopEatingWhitespace` calls.");
	}
	parser.eatingWhitespace--;
}

static void consume(KrkTokenType type, const char * message) {
	if (parser.current.type == type) {
		advance();
		return;
	}

	errorAtCurrent(message);
}

static int check(KrkTokenType type) {
	return parser.current.type == type;
}

static int match(KrkTokenType type) {
	if (!check(type)) return 0;
	advance();
	return 1;
}

static int identifiersEqual(KrkToken * a, KrkToken * b) {
	return (a->length == b->length && memcmp(a->start, b->start, a->length) == 0);
}

static KrkToken syntheticToken(const char * text) {
	KrkToken token;
	token.start = text;
	token.length = (int)strlen(text);
	return token;
}

static void emitByte(uint8_t byte) {
	krk_writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2) {
	emitByte(byte1);
	emitByte(byte2);
}

static void emitReturn() {
	if (current->type == TYPE_INIT) {
		emitBytes(OP_GET_LOCAL, 0);
	} else if (current->type == TYPE_MODULE) {
		/* Un-pop the last stack value */
		emitBytes(OP_GET_LOCAL, 0);
	} else if (current->type != TYPE_LAMBDA) {
		emitByte(OP_NONE);
	}
	emitByte(OP_RETURN);
}

static KrkFunction * endCompiler() {
	KrkFunction * function = current->function;

	for (size_t i = 0; i < current->function->localNameCount; i++) {
		if (current->function->localNames[i].deathday == 0) {
			current->function->localNames[i].deathday = currentChunk()->count;
		}
	}
	current->function->localNames = GROW_ARRAY(KrkLocalEntry, current->function->localNames, \
		current->localNameCapacity, current->function->localNameCount); /* Shorten this down for runtime */

	emitReturn();

	/* Attach contants for arguments */
	for (int i = 0; i < function->requiredArgs; ++i) {
		KrkValue value = OBJECT_VAL(krk_copyString(current->locals[i].name.start, current->locals[i].name.length));
		krk_push(value);
		krk_writeValueArray(&function->requiredArgNames, value);
		krk_pop();
	}
	for (int i = 0; i < function->keywordArgs; ++i) {
		KrkValue value = OBJECT_VAL(krk_copyString(current->locals[i+function->requiredArgs].name.start,
			current->locals[i+function->requiredArgs].name.length));
		krk_push(value);
		krk_writeValueArray(&function->keywordArgNames, value);
		krk_pop();
	}
	size_t args = current->function->requiredArgs + current->function->keywordArgs;
	if (current->function->collectsArguments) {
		KrkValue value = OBJECT_VAL(krk_copyString(current->locals[args].name.start,
			current->locals[args].name.length));
		krk_push(value);
		krk_writeValueArray(&function->keywordArgNames, value);
		krk_pop();
		args++;
	}
	if (current->function->collectsKeywords) {
		KrkValue value = OBJECT_VAL(krk_copyString(current->locals[args].name.start,
			current->locals[args].name.length));
		krk_push(value);
		krk_writeValueArray(&function->keywordArgNames, value);
		krk_pop();
		args++;
	}

#ifdef ENABLE_DISASSEMBLY
	if ((krk_currentThread.flags & KRK_ENABLE_DISASSEMBLY) && !parser.hadError) {
		krk_disassembleChunk(stderr, function, function->name ? function->name->chars : "<module>");
		fprintf(stderr, "Function metadata: requiredArgs=%d keywordArgs=%d upvalueCount=%d\n",
			function->requiredArgs, function->keywordArgs, (int)function->upvalueCount);
		fprintf(stderr, "__doc__: \"%s\"\n", function->docstring ? function->docstring->chars : "");
		fprintf(stderr, "Constants: ");
		for (size_t i = 0; i < currentChunk()->constants.count; ++i) {
			fprintf(stderr, "%d: ", (int)i);
			krk_printValueSafe(stderr, currentChunk()->constants.values[i]);
			if (i != currentChunk()->constants.count - 1) {
				fprintf(stderr, ", ");
			}
		}
		fprintf(stderr, "\nRequired arguments: ");
		int i = 0;
		for (; i < function->requiredArgs; ++i) {
			fprintf(stderr, "%.*s%s",
				(int)current->locals[i].name.length,
				current->locals[i].name.start,
				(i == function->requiredArgs - 1) ? "" : ", ");
		}
		fprintf(stderr, "\nKeyword arguments: ");
		for (; i < function->requiredArgs + function->keywordArgs; ++i) {
			fprintf(stderr, "%.*s=None%s",
				(int)current->locals[i].name.length,
				current->locals[i].name.start,
				(i == function->keywordArgs - 1) ? "" : ", ");
		}
		fprintf(stderr, "\n");
	}
#endif

	current = current->enclosing;
	return function;
}

static void freeCompiler(Compiler * compiler) {
	FREE_ARRAY(Local,compiler->locals, compiler->localsSpace);
	FREE_ARRAY(Upvalue,compiler->upvalues, compiler->upvaluesSpace);
	FREE_ARRAY(int,compiler->breaks, compiler->breakSpace);
	FREE_ARRAY(int,compiler->continues, compiler->continueSpace);
}

static size_t emitConstant(KrkValue value) {
	return krk_writeConstant(currentChunk(), value, parser.previous.line);
}

static void number(int canAssign) {
	const char * start = parser.previous.start;
	int base = 10;

	/*  These special cases for hexadecimal, binary, octal values. */
	if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
		base = 16;
		start += 2;
	} else if (start[0] == '0' && (start[1] == 'b' || start[1] == 'B')) {
		base = 2;
		start += 2;
	} else if (start[0] == '0' && (start[1] == 'o' || start[1] == 'O')) {
		base = 8;
		start += 2;
	}

	/* If it wasn't a special base, it may be a floating point value. */
	if (base == 10) {
		for (size_t j = 0; j < parser.previous.length; ++j) {
			if (parser.previous.start[j] == '.') {
				double value = strtod(start, NULL);
				emitConstant(FLOATING_VAL(value));
				return;
			}
		}
	}

	/* If we got here, it's an integer of some sort. */
	krk_integer_type value = parseStrInt(start, NULL, base);
	emitConstant(INTEGER_VAL(value));
}

static void binary(int canAssign) {
	KrkTokenType operatorType = parser.previous.type;
	ParseRule * rule = getRule(operatorType);
	parsePrecedence((Precedence)(rule->precedence + 1));

	switch (operatorType) {
		case TOKEN_BANG_EQUAL:    emitBytes(OP_EQUAL, OP_NOT); break;
		case TOKEN_EQUAL_EQUAL:   emitByte(OP_EQUAL); break;
		case TOKEN_GREATER:       emitByte(OP_GREATER); break;
		case TOKEN_GREATER_EQUAL: emitBytes(OP_LESS, OP_NOT); break;
		case TOKEN_LESS:          emitByte(OP_LESS); break;
		case TOKEN_LESS_EQUAL:    emitBytes(OP_GREATER, OP_NOT); break;

		case TOKEN_PIPE:        emitByte(OP_BITOR); break;
		case TOKEN_CARET:       emitByte(OP_BITXOR); break;
		case TOKEN_AMPERSAND:   emitByte(OP_BITAND); break;
		case TOKEN_LEFT_SHIFT:  emitByte(OP_SHIFTLEFT); break;
		case TOKEN_RIGHT_SHIFT: emitByte(OP_SHIFTRIGHT); break;

		case TOKEN_PLUS:     emitByte(OP_ADD); break;
		case TOKEN_MINUS:    emitByte(OP_SUBTRACT); break;
		case TOKEN_ASTERISK: emitByte(OP_MULTIPLY); break;
		case TOKEN_POW:      emitByte(OP_POW); break;
		case TOKEN_SOLIDUS:  emitByte(OP_DIVIDE); break;
		case TOKEN_MODULO:   emitByte(OP_MODULO); break;
		case TOKEN_IN:       emitByte(OP_EQUAL); break;
		default: return;
	}
}

static int matchAssignment(void) {
	return (parser.current.type >= TOKEN_EQUAL && parser.current.type <= TOKEN_MODULO_EQUAL) ? (advance(), 1) : 0;
}

static int matchEndOfDel(void) {
	return check(TOKEN_COMMA) || check(TOKEN_EOL) || check(TOKEN_EOF) || check(TOKEN_SEMICOLON);
}

static void assignmentValue(void) {
	KrkTokenType type = parser.previous.type;
	if (type == TOKEN_PLUS_PLUS || type == TOKEN_MINUS_MINUS) {
		emitConstant(INTEGER_VAL(1));
	} else {
		expression();
	}

	switch (type) {
		case TOKEN_PIPE_EQUAL:      emitByte(OP_BITOR); break;
		case TOKEN_CARET_EQUAL:     emitByte(OP_BITXOR); break;
		case TOKEN_AMP_EQUAL:       emitByte(OP_BITAND); break;
		case TOKEN_LSHIFT_EQUAL:    emitByte(OP_SHIFTLEFT); break;
		case TOKEN_RSHIFT_EQUAL:    emitByte(OP_SHIFTRIGHT); break;

		case TOKEN_PLUS_EQUAL:      emitByte(OP_ADD); break;
		case TOKEN_PLUS_PLUS:       emitByte(OP_ADD); break;
		case TOKEN_MINUS_EQUAL:     emitByte(OP_SUBTRACT); break;
		case TOKEN_MINUS_MINUS:     emitByte(OP_SUBTRACT); break;
		case TOKEN_ASTERISK_EQUAL:  emitByte(OP_MULTIPLY); break;
		case TOKEN_POW_EQUAL:       emitByte(OP_POW); break;
		case TOKEN_SOLIDUS_EQUAL:   emitByte(OP_DIVIDE); break;
		case TOKEN_MODULO_EQUAL:    emitByte(OP_MODULO); break;

		default:
			error("Unexpected operand in assignment");
			break;
	}
}

static void get_(int canAssign) {
	int isSlice = 0;
	if (match(TOKEN_COLON)) {
		emitByte(OP_NONE);
		isSlice = 1;
	} else {
		expression();
	}
	if (isSlice || match(TOKEN_COLON)) {
		if (isSlice && match(TOKEN_COLON)) {
			error("Step value not supported in slice.");
			return;
		}
		if (match(TOKEN_RIGHT_SQUARE)) {
			emitByte(OP_NONE);
		} else {
			expression();
			consume(TOKEN_RIGHT_SQUARE, "Expected ending square bracket after slice.");
		}
		if (canAssign && match(TOKEN_EQUAL)) {
			expression();
			emitByte(OP_INVOKE_SETSLICE);
		} else if (canAssign && matchAssignment()) {
			/* o s e */
			emitBytes(OP_DUP, 2); /* o s e o */
			emitBytes(OP_DUP, 2); /* o s e o s */
			emitBytes(OP_DUP, 2); /* o s e o s e */
			emitByte(OP_INVOKE_GETSLICE); /* o s e v */
			assignmentValue();
			emitByte(OP_INVOKE_SETSLICE);
		} else if (inDel && matchEndOfDel()) {
			emitByte(OP_INVOKE_DELSLICE);
			inDel = 2;
		} else {
			emitByte(OP_INVOKE_GETSLICE);
		}
	} else {
		consume(TOKEN_RIGHT_SQUARE, "Expected ending square bracket after index.");
		if (canAssign && match(TOKEN_EQUAL)) {
			expression();
			emitByte(OP_INVOKE_SETTER);
		} else if (canAssign && matchAssignment()) {
			emitBytes(OP_DUP, 1); /* o e o */
			emitBytes(OP_DUP, 1); /* o e o e */
			emitByte(OP_INVOKE_GETTER); /* o e v */
			assignmentValue(); /* o e v a */
			emitByte(OP_INVOKE_SETTER); /* r */
		} else if (inDel && matchEndOfDel()) {
			if (!canAssign || inDel != 1) {
				error("Invalid del target");
			} else if (canAssign) {
				emitByte(OP_INVOKE_DELETE);
				inDel = 2;
			}
		} else {
			emitByte(OP_INVOKE_GETTER);
		}
	}
}

static void dot(int canAssign) {
	if (match(TOKEN_LEFT_PAREN)) {
		startEatingWhitespace();
		size_t argCount = 0;
		size_t argSpace = 1;
		ssize_t * args  = GROW_ARRAY(ssize_t,NULL,0,1);

		do {
			if (argSpace < argCount + 1) {
				size_t old = argSpace;
				argSpace = GROW_CAPACITY(old);
				args = GROW_ARRAY(ssize_t,args,old,argSpace);
			}
			consume(TOKEN_IDENTIFIER, "Expected attribute name");
			size_t ind = identifierConstant(&parser.previous);
			args[argCount++] = ind;
		} while (match(TOKEN_COMMA));

		stopEatingWhitespace();
		consume(TOKEN_RIGHT_PAREN, "Expected ) after attribute list");

		if (canAssign && match(TOKEN_EQUAL)) {
			size_t expressionCount = 0;
			do {
				expressionCount++;
				expression();
			} while (match(TOKEN_COMMA));

			if (expressionCount == 1 && argCount > 1) {
				EMIT_CONSTANT_OP(OP_UNPACK, argCount);
			} else if (expressionCount > 1 && argCount == 1) {
				EMIT_CONSTANT_OP(OP_TUPLE, expressionCount);
			} else if (expressionCount != argCount) {
				error("Invalid assignment to attribute pack");
				goto _dotDone;
			}

			for (size_t i = argCount; i > 0; i--) {
				if (i != 1) {
					emitBytes(OP_DUP, i);
					emitByte(OP_SWAP);
				}
				EMIT_CONSTANT_OP(OP_SET_PROPERTY, args[i-1]);
				if (i != 1) {
					emitByte(OP_POP);
				}
			}
		} else {
			for (size_t i = 0; i < argCount; i++) {
				emitBytes(OP_DUP,0);
				EMIT_CONSTANT_OP(OP_GET_PROPERTY,args[i]);
				emitByte(OP_SWAP);
			}
			emitByte(OP_POP);
			emitBytes(OP_TUPLE,argCount);
		}

_dotDone:
		FREE_ARRAY(ssize_t,args,argSpace);
		return;
	}
	consume(TOKEN_IDENTIFIER, "Expected property name");
	size_t ind = identifierConstant(&parser.previous);
	if (canAssign && match(TOKEN_EQUAL)) {
		expression();
		EMIT_CONSTANT_OP(OP_SET_PROPERTY, ind);
	} else if (canAssign && matchAssignment()) {
		emitBytes(OP_DUP, 0); /* Duplicate the object */
		EMIT_CONSTANT_OP(OP_GET_PROPERTY, ind);
		assignmentValue();
		EMIT_CONSTANT_OP(OP_SET_PROPERTY, ind);
	} else if (inDel && matchEndOfDel()) {
		if (!canAssign || inDel != 1) {
			error("Invalid del target");
		} else {
			EMIT_CONSTANT_OP(OP_DEL_PROPERTY, ind);
			inDel = 2;
		}
	} else {
		EMIT_CONSTANT_OP(OP_GET_PROPERTY, ind);
	}
}

static void in_(int canAssign) {
	parsePrecedence(PREC_COMPARISON);
	KrkToken contains = syntheticToken("__contains__");
	ssize_t ind = identifierConstant(&contains);
	EMIT_CONSTANT_OP(OP_GET_PROPERTY, ind);
	emitByte(OP_SWAP);
	emitBytes(OP_CALL,1);
}

static void not_(int canAssign) {
	consume(TOKEN_IN, "infix not must be followed by in\n");
	in_(canAssign);
	emitByte(OP_NOT);
}

static void is_(int canAssign) {
	int invert = match(TOKEN_NOT);
	parsePrecedence(PREC_COMPARISON);
	emitByte(OP_IS);
	if (invert) emitByte(OP_NOT);
}

static void literal(int canAssign) {
	switch (parser.previous.type) {
		case TOKEN_FALSE: emitByte(OP_FALSE); break;
		case TOKEN_NONE:  emitByte(OP_NONE); break;
		case TOKEN_TRUE:  emitByte(OP_TRUE); break;
		default: return;
	}
}

static void expression() {
	parsePrecedence(PREC_ASSIGNMENT);
}

static void letDeclaration(void) {
	size_t argCount = 0;
	size_t argSpace = 1;
	ssize_t * args  = GROW_ARRAY(ssize_t,NULL,0,1);

	do {
		if (argSpace < argCount + 1) {
			size_t old = argSpace;
			argSpace = GROW_CAPACITY(old);
			args = GROW_ARRAY(ssize_t,args,old,argSpace);
		}
		ssize_t ind = parseVariable("Expected variable name.");
		if (current->scopeDepth > 0) {
			/* Need locals space */
			args[argCount++] = current->localCount - 1;
		} else {
			args[argCount++] = ind;
		}
	} while (match(TOKEN_COMMA));

	if (match(TOKEN_EQUAL)) {
		size_t expressionCount = 0;
		do {
			expressionCount++;
			expression();
		} while (match(TOKEN_COMMA));
		if (expressionCount == 1 && argCount > 1) {
			EMIT_CONSTANT_OP(OP_UNPACK, argCount);
		} else if (expressionCount == argCount) {
			/* Do nothing */
		} else if (expressionCount > 1 && argCount == 1) {
			EMIT_CONSTANT_OP(OP_TUPLE, expressionCount);
		} else {
			error("Invalid sequence unpack in 'let' statement");
			goto _letDone;
		}
	} else {
		/* Need to nil it */
		for (size_t i = 0; i < argCount; ++i) {
			emitByte(OP_NONE);
		}
	}

	if (current->scopeDepth == 0) {
		for (size_t i = argCount; i > 0; i--) {
			defineVariable(args[i-1]);
		}
	} else {
		for (size_t i = 0; i < argCount; i++) {
			current->locals[current->localCount - 1 - i].depth = current->scopeDepth;
		}
	}

_letDone:
	if (!match(TOKEN_EOL) && !match(TOKEN_EOF)) {
		error("Expected end of line after 'let' statement.");
	}

	FREE_ARRAY(ssize_t,args,argSpace);
	return;
}

static void synchronize() {
	while (parser.current.type != TOKEN_EOF) {
		if (parser.previous.type == TOKEN_EOL) return;

		switch (parser.current.type) {
			case TOKEN_CLASS:
			case TOKEN_DEF:
			case TOKEN_LET:
			case TOKEN_FOR:
			case TOKEN_IF:
			case TOKEN_WHILE:
			case TOKEN_RETURN:
				return;
			default: break;
		}

		advance();
	}
}

static void declaration() {
	if (check(TOKEN_DEF)) {
		defDeclaration();
	} else if (match(TOKEN_LET)) {
		letDeclaration();
	} else if (check(TOKEN_CLASS)) {
		KrkToken className = classDeclaration();
		size_t classConst = identifierConstant(&className);
		parser.previous = className;
		declareVariable();
		defineVariable(classConst);
	} else if (check(TOKEN_AT)) {
		decorator(0, TYPE_FUNCTION);
	} else if (match(TOKEN_EOL) || match(TOKEN_EOF)) {
		return;
	} else if (check(TOKEN_INDENTATION)) {
		return;
	} else {
		statement();
	}

	if (parser.panicMode) synchronize();
}

static void expressionStatement() {
	expression();
	emitByte(OP_POP);
}

static void beginScope() {
	current->scopeDepth++;
}

static void endScope() {
	current->scopeDepth--;
	while (current->localCount > 0 &&
	       current->locals[current->localCount - 1].depth > (ssize_t)current->scopeDepth) {
		for (size_t i = 0; i < current->function->localNameCount; i++) {
			if (current->function->localNames[i].id == current->localCount - 1) {
				current->function->localNames[i].deathday = (size_t)currentChunk()->count;
			}
		}
		if (current->locals[current->localCount - 1].isCaptured) {
			emitByte(OP_CLOSE_UPVALUE);
		} else {
			emitByte(OP_POP);
		}
		current->localCount--;
	}
}

static int emitJump(uint8_t opcode) {
	emitByte(opcode);
	emitBytes(0xFF, 0xFF);
	return currentChunk()->count - 2;
}

static void patchJump(int offset) {
	int jump = currentChunk()->count - offset - 2;
	if (jump > 0xFFFF) {
		error("Unsupported far jump (we'll get there)");
	}

	currentChunk()->code[offset] = (jump >> 8) & 0xFF;
	currentChunk()->code[offset + 1] =  (jump) & 0xFF;
}

static void block(size_t indentation, const char * blockName) {
	if (match(TOKEN_EOL)) {
		if (check(TOKEN_INDENTATION)) {
			size_t currentIndentation = parser.current.length;
			if (currentIndentation <= indentation) return;
			advance();
			if (!strcmp(blockName,"def") && (match(TOKEN_STRING) || match(TOKEN_BIG_STRING))) {
				size_t before = currentChunk()->count;
				string(parser.previous.type == TOKEN_BIG_STRING);
				/* That wrote to the chunk, rewind it; this should only ever go back two bytes
				 * because this should only happen as the first thing in a function definition,
				 * and thus this _should_ be the first constant and thus opcode + one-byte operand
				 * to OP_CONSTANT, but just to be safe we'll actually use the previous offset... */
				currentChunk()->count = before;
				/* Retreive the docstring from the constant table */
				current->function->docstring = AS_STRING(currentChunk()->constants.values[currentChunk()->constants.count-1]);
				consume(TOKEN_EOL,"Garbage after docstring defintion");
				if (!check(TOKEN_INDENTATION) || parser.current.length != currentIndentation) {
					error("Expected at least one statement in function with docstring.");
				}
				advance();
			}
			declaration();
			while (check(TOKEN_INDENTATION)) {
				if (parser.current.length < currentIndentation) break;
				advance();
				declaration();
				if (check(TOKEN_EOL)) {
					advance();
				}
			};
#ifdef ENABLE_SCAN_TRACING
			if (krk_currentThread.flags & KRK_ENABLE_SCAN_TRACING) {
				fprintf(stderr, "\n\nfinished with block %s (ind=%d) on line %d, sitting on a %s (len=%d)\n\n",
					blockName, (int)indentation, (int)parser.current.line,
					getRule(parser.current.type)->name, (int)parser.current.length);
			}
#endif
		}
	} else {
		statement();
	}
}

static void doUpvalues(Compiler * compiler, KrkFunction * function) {
	assert(!!function->upvalueCount == !!compiler->upvalues);
	for (size_t i = 0; i < function->upvalueCount; ++i) {
		emitByte(compiler->upvalues[i].isLocal ? 1 : 0);
		if (i > 255) {
			emitByte((compiler->upvalues[i].index >> 16) & 0xFF);
			emitByte((compiler->upvalues[i].index >> 8) & 0xFF);
		}
		emitByte((compiler->upvalues[i].index) & 0xFF);
	}
}

static void function(FunctionType type, size_t blockWidth) {
	Compiler compiler;
	initCompiler(&compiler, type);
	compiler.function->chunk.filename = compiler.enclosing->function->chunk.filename;

	beginScope();

	if (isMethod(type)) current->function->requiredArgs = 1;

	int hasCollectors = 0;

	consume(TOKEN_LEFT_PAREN, "Expected start of parameter list after function name.");
	startEatingWhitespace();
	if (!check(TOKEN_RIGHT_PAREN)) {
		do {
			if (match(TOKEN_SELF)) {
				if (!isMethod(type)) {
					error("Invalid use of `self` as a function paramenter.");
				}
				continue;
			}
			if (match(TOKEN_ASTERISK) || check(TOKEN_POW)) {
				if (match(TOKEN_POW)) {
					if (hasCollectors == 2) {
						error("Duplicate ** in parameter list.");
						return;
					}
					hasCollectors = 2;
					current->function->collectsKeywords = 1;
				} else {
					if (hasCollectors) {
						error("Syntax error.");
						return;
					}
					hasCollectors = 1;
					current->function->collectsArguments = 1;
				}
				/* Collect a name, specifically "args" or "kwargs" are commont */
				ssize_t paramConstant = parseVariable("Expect parameter name.");
				defineVariable(paramConstant);
				/* Make that a valid local for this function */
				size_t myLocal = current->localCount - 1;
				EMIT_CONSTANT_OP(OP_GET_LOCAL, myLocal);
				/* Check if it's equal to the unset-kwarg-sentinel value */
				emitConstant(KWARGS_VAL(0));
				emitByte(OP_IS);
				int jumpIndex = emitJump(OP_JUMP_IF_FALSE);
				/* And if it is, set it to the appropriate type */
				beginScope();
				KrkToken synth = syntheticToken(hasCollectors == 1 ? "listOf" : "dictOf");
				namedVariable(synth, 0);
				emitBytes(OP_CALL, 0);
				EMIT_CONSTANT_OP(OP_SET_LOCAL, myLocal);
				emitByte(OP_POP); /* local value */
				endScope();
				/* Otherwise pop the comparison. */
				patchJump(jumpIndex);
				emitByte(OP_POP); /* comparison value */
				continue;
			}
			ssize_t paramConstant = parseVariable("Expect parameter name.");
			defineVariable(paramConstant);
			if (match(TOKEN_EQUAL)) {
				/*
				 * We inline default arguments by checking if they are equal
				 * to a sentinel value and replacing them with the requested
				 * argument. This allows us to send None (useful) to override
				 * defaults that are something else. This essentially ends
				 * up as the following at the top of the function:
				 * if param == KWARGS_SENTINEL:
				 *     param = EXPRESSION
				 */
				size_t myLocal = current->localCount - 1;
				EMIT_CONSTANT_OP(OP_GET_LOCAL, myLocal);
				emitConstant(KWARGS_VAL(0));
				emitByte(OP_EQUAL);
				int jumpIndex = emitJump(OP_JUMP_IF_FALSE);
				beginScope();
				expression(); /* Read expression */
				EMIT_CONSTANT_OP(OP_SET_LOCAL, myLocal);
				emitByte(OP_POP); /* local value */
				endScope();
				patchJump(jumpIndex);
				emitByte(OP_POP);
				current->function->keywordArgs++;
			} else {
				current->function->requiredArgs++;
			}
		} while (match(TOKEN_COMMA));
	}
	stopEatingWhitespace();
	consume(TOKEN_RIGHT_PAREN, "Expected end of parameter list.");

	consume(TOKEN_COLON, "Expected colon after function signature.");
	block(blockWidth,"def");

	KrkFunction * function = endCompiler();
	size_t ind = krk_addConstant(currentChunk(), OBJECT_VAL(function));
	EMIT_CONSTANT_OP(OP_CLOSURE, ind);
	doUpvalues(&compiler, function);
	freeCompiler(&compiler);
}

static void method(size_t blockWidth) {
	/* This is actually "inside of a class definition", and that might mean
	 * arbitrary blank lines we need to accept... Sorry. */
	if (match(TOKEN_EOL)) {
		return;
	}

	/* def method(...): - just like functions; unlike Python, I'm just always
	 * going to assign `self` because Lox always assigns `this`; it should not
	 * show up in the initializer list; I may add support for it being there
	 * as a redundant thing, just to make more Python stuff work with changes. */
	if (check(TOKEN_AT)) {
		decorator(0, TYPE_METHOD);
	} else if (match(TOKEN_IDENTIFIER)) {
		emitBytes(OP_DUP, 0); /* SET_PROPERTY will pop class */
		size_t ind = identifierConstant(&parser.previous);
		consume(TOKEN_EQUAL, "Class field must have value.");
		expression();
		EMIT_CONSTANT_OP(OP_SET_PROPERTY, ind);
		emitByte(OP_POP); /* Value of expression replaces dup of class*/
		if (!match(TOKEN_EOL) && !match(TOKEN_EOF)) {
			errorAtCurrent("Expected end of line after class attribut declaration");
		}
	} else if (match(TOKEN_PASS)) {
		/* bah */
		consume(TOKEN_EOL, "Expected linefeed after 'pass' in class body.");
	} else {
		consume(TOKEN_DEF, "expected a definition, got nothing");
		consume(TOKEN_IDENTIFIER, "expected method name");
		size_t ind = identifierConstant(&parser.previous);
		FunctionType type = TYPE_METHOD;

		if (parser.previous.length == 8 && memcmp(parser.previous.start, "__init__", 8) == 0) {
			type = TYPE_INIT;
		}

		function(type, blockWidth);
		EMIT_CONSTANT_OP(OP_METHOD, ind);
	}
}

static KrkToken classDeclaration() {
	size_t blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	advance(); /* Collect the `class` */

	consume(TOKEN_IDENTIFIER, "Expected class name.");
	Compiler subcompiler;
	initCompiler(&subcompiler, TYPE_LAMBDA);
	subcompiler.function->chunk.filename = subcompiler.enclosing->function->chunk.filename;

	beginScope();

	KrkToken className = parser.previous;
	size_t constInd = identifierConstant(&parser.previous);
	declareVariable();

	EMIT_CONSTANT_OP(OP_CLASS, constInd);
	defineVariable(constInd);

	ClassCompiler classCompiler;
	classCompiler.name = parser.previous;
	classCompiler.enclosing = currentClass;
	currentClass = &classCompiler;
	int hasSuperclass = 0;

	if (match(TOKEN_LEFT_PAREN)) {
		startEatingWhitespace();
		if (!check(TOKEN_RIGHT_PAREN)) {
			expression();
			hasSuperclass = 1;
		}
		stopEatingWhitespace();
		consume(TOKEN_RIGHT_PAREN, "Expected ) after superclass.");
	}

	if (!hasSuperclass) {
		KrkToken Object = syntheticToken("object");
		size_t ind = identifierConstant(&Object);
		EMIT_CONSTANT_OP(OP_GET_GLOBAL, ind);
	}

	beginScope();
	addLocal(syntheticToken("super"));
	defineVariable(0);

	if (hasSuperclass) {
		namedVariable(className, 0);
		emitByte(OP_INHERIT);
	}

	namedVariable(className, 0);

	consume(TOKEN_COLON, "Expected colon after class");
	if (match(TOKEN_EOL)) {
		if (check(TOKEN_INDENTATION)) {
			size_t currentIndentation = parser.current.length;
			if (currentIndentation <= blockWidth) {
				errorAtCurrent("Unexpected indentation level for class");
			}
			advance();
			if (match(TOKEN_STRING) || match(TOKEN_BIG_STRING)) {
				string(parser.previous.type == TOKEN_BIG_STRING);
				emitByte(OP_DOCSTRING);
				consume(TOKEN_EOL,"Garbage after docstring defintion");
				if (!check(TOKEN_INDENTATION) || parser.current.length != currentIndentation) {
					goto _pop_class;
				}
				advance();
			}
			method(currentIndentation);
			while (check(TOKEN_INDENTATION)) {
				if (parser.current.length < currentIndentation) break;
				advance(); /* Pass the indentation */
				method(currentIndentation);
			}
#ifdef ENABLE_SCAN_TRACING
			if (krk_currentThread.flags & KRK_ENABLE_SCAN_TRACING) fprintf(stderr, "Exiting from class definition on %s\n", getRule(parser.current.type)->name);
#endif
			/* Exit from block */
		}
	} /* else empty class (and at end of file?) we'll allow it for now... */
_pop_class:
	emitByte(OP_FINALIZE);
	currentClass = currentClass->enclosing;
	KrkFunction * makeclass = endCompiler();
	size_t indFunc = krk_addConstant(currentChunk(), OBJECT_VAL(makeclass));
	EMIT_CONSTANT_OP(OP_CLOSURE, indFunc);
	doUpvalues(&subcompiler, makeclass);
	freeCompiler(&subcompiler);
	emitBytes(OP_CALL, 0);

	return className;
}

static void markInitialized() {
	if (current->scopeDepth == 0) return;
	current->locals[current->localCount - 1].depth = current->scopeDepth;
}

static void lambda() {
	Compiler lambdaCompiler;
	parser.previous = syntheticToken("<lambda>");
	initCompiler(&lambdaCompiler, TYPE_LAMBDA);
	lambdaCompiler.function->chunk.filename = lambdaCompiler.enclosing->function->chunk.filename;
	beginScope();

	if (!check(TOKEN_COLON)) {
		do {
			ssize_t paramConstant = parseVariable("Expect parameter name.");
			defineVariable(paramConstant);
			current->function->requiredArgs++;
		} while (match(TOKEN_COMMA));
	}

	consume(TOKEN_COLON, "expected : after lambda arguments");
	expression();

	KrkFunction * lambda = endCompiler();
	size_t ind = krk_addConstant(currentChunk(), OBJECT_VAL(lambda));
	EMIT_CONSTANT_OP(OP_CLOSURE, ind);
	doUpvalues(&lambdaCompiler, lambda);
	freeCompiler(&lambdaCompiler);
}

static void defDeclaration() {
	size_t blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	advance(); /* Collect the `def` */

	ssize_t global = parseVariable("Expected function name.");
	markInitialized();
	function(TYPE_FUNCTION, blockWidth);
	defineVariable(global);
}

static KrkToken decorator(size_t level, FunctionType type) {
	size_t blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	advance(); /* Collect the `@` */

	KrkToken funcName = {0};
	int haveCallable = 0;

	/* hol'up, let's special case some stuff */
	KrkToken at_staticmethod = syntheticToken("staticmethod");
	KrkToken at_property = syntheticToken("property");
	if (identifiersEqual(&at_staticmethod, &parser.current)) {
		if (level != 0 || type != TYPE_METHOD) {
			error("Invalid use of @staticmethod, which must be the top decorator of a class method.");
			return funcName;
		}
		advance();
		type = TYPE_STATIC;
		emitBytes(OP_DUP, 0); /* SET_PROPERTY will pop class */
	} else if (identifiersEqual(&at_property, &parser.current)) {
		if (level != 0 || type != TYPE_METHOD) {
			error("Invalid use of @property, which must be the top decorator of a class method.");
			return funcName;
		}
		advance();
		type = TYPE_PROPERTY;
		emitBytes(OP_DUP, 0);
	} else {
		/* Collect an identifier */
		expression();
		haveCallable = 1;
	}

	consume(TOKEN_EOL, "Expected line feed after decorator.");
	if (blockWidth) {
		consume(TOKEN_INDENTATION, "Expected next line after decorator to have same indentation.");
		if (parser.previous.length != blockWidth) error("Expected next line after decorator to have same indentation.");
	}

	if (check(TOKEN_DEF)) {
		/* We already checked for block level */
		advance();
		consume(TOKEN_IDENTIFIER, "Expected function name.");
		funcName = parser.previous;
		if (type == TYPE_METHOD && funcName.length == 8 && !memcmp(funcName.start,"__init__",8)) {
			type = TYPE_INIT;
		}
		function(type, blockWidth);
	} else if (check(TOKEN_AT)) {
		funcName = decorator(level+1, type);
	} else if (check(TOKEN_CLASS)) {
		if (type != TYPE_FUNCTION) {
			error("Invalid decorator applied to class");
			return funcName;
		}
		funcName = classDeclaration();
	} else {
		error("Expected a function declaration or another decorator.");
		return funcName;
	}

	if (haveCallable)
		emitBytes(OP_CALL, 1);

	if (level == 0) {
		if (type == TYPE_FUNCTION) {
			parser.previous = funcName;
			declareVariable();
			size_t ind = (current->scopeDepth > 0) ? 0 : identifierConstant(&funcName);
			defineVariable(ind);
		} else if (type == TYPE_STATIC) {
			size_t ind = identifierConstant(&funcName);
			EMIT_CONSTANT_OP(OP_SET_PROPERTY, ind);
			emitByte(OP_POP);
		} else if (type == TYPE_PROPERTY) {
			emitByte(OP_CREATE_PROPERTY);
			size_t ind = identifierConstant(&funcName);
			EMIT_CONSTANT_OP(OP_SET_PROPERTY, ind);
			emitByte(OP_POP);
		} else {
			size_t ind = identifierConstant(&funcName);
			EMIT_CONSTANT_OP(OP_METHOD, ind);
		}
	}

	return funcName;
}

static void emitLoop(int loopStart) {

	/* Patch continue statements to point to here, before the loop operation (yes that's silly) */
	while (current->continueCount > 0 && current->continues[current->continueCount-1] > loopStart) {
		patchJump(current->continues[current->continueCount-1]);
		current->continueCount--;
	}

	emitByte(OP_LOOP);

	int offset = currentChunk()->count - loopStart + 2;
	if (offset > 0xFFFF) error("offset too big");
	emitBytes(offset >> 8, offset);

	/* Patch break statements */
}

static void withStatement() {
	/* TODO: Multiple items, I'm feeling lazy. */

	/* We only need this for block() */
	size_t blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;

	/* Collect the with token that started this statement */
	advance();

	beginScope();
	expression();

	if (match(TOKEN_AS)) {
		consume(TOKEN_IDENTIFIER, "Expected variable name after 'as'");
		size_t ind = identifierConstant(&parser.previous);
		declareVariable();
		defineVariable(ind);
	} else {
		/* Otherwise we want an unnamed local; TODO: Wait, can't we do this for iterable counts? */
		addLocal(syntheticToken(""));
		markInitialized();
	}

	consume(TOKEN_COLON, "Expected ':' after with statement");

	addLocal(syntheticToken(""));
	int withJump = emitJump(OP_PUSH_WITH);
	markInitialized();

	beginScope();
	block(blockWidth,"with");
	endScope();

	patchJump(withJump);
	emitByte(OP_CLEANUP_WITH);

	/* Scope exit pops context manager */
	endScope();
}

static void ifStatement() {
	/* Figure out what block level contains us so we can match our partner else */
	size_t blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	KrkToken myPrevious = parser.previous;

	/* Collect the if token that started this statement */
	advance();

	/* Collect condition expression */
	expression();

	/* if EXPR: */
	consume(TOKEN_COLON, "Expect ':' after condition.");

	int thenJump = emitJump(OP_JUMP_IF_FALSE);
	emitByte(OP_POP);

	/* Start a new scope and enter a block */
	beginScope();
	block(blockWidth,"if");
	endScope();

	int elseJump = emitJump(OP_JUMP);
	patchJump(thenJump);
	emitByte(OP_POP);

	/* See if we have a matching else block */
	if (blockWidth == 0 || (check(TOKEN_INDENTATION) && (parser.current.length == blockWidth))) {
		/* This is complicated */
		KrkToken previous;
		if (blockWidth) {
			previous = parser.previous;
			advance();
		}
		if (match(TOKEN_ELSE) || check(TOKEN_ELIF)) {
			if (parser.current.type == TOKEN_ELIF || check(TOKEN_IF)) {
				parser.previous = myPrevious;
				ifStatement(); /* Keep nesting */
			} else {
				consume(TOKEN_COLON, "Expect ':' after else.");
				beginScope();
				block(blockWidth,"else");
				endScope();
			}
		} else if (!check(TOKEN_EOF) && !check(TOKEN_EOL)) {
			krk_ungetToken(parser.current);
			parser.current = parser.previous;
			if (blockWidth) {
				parser.previous = previous;
			}
		} else {
			advance(); /* Ignore this blank indentation line */
		}
	}

	patchJump(elseJump);
}

static void patchBreaks(int loopStart) {
	/* Patch break statements to go here, after the loop operation and operand. */
	while (current->breakCount > 0 && current->breaks[current->breakCount-1] > loopStart) {
		patchJump(current->breaks[current->breakCount-1]);
		current->breakCount--;
	}
}

static void breakStatement() {
	if (current->breakSpace < current->breakCount + 1) {
		size_t old = current->breakSpace;
		current->breakSpace = GROW_CAPACITY(old);
		current->breaks = GROW_ARRAY(int,current->breaks,old,current->breakSpace);
	}

	for (size_t i = current->loopLocalCount; i < current->localCount; ++i) {
		emitByte(OP_POP);
	}
	current->breaks[current->breakCount++] = emitJump(OP_JUMP);
}

static void continueStatement() {
	if (current->continueSpace < current->continueCount + 1) {
		size_t old = current->continueSpace;
		current->continueSpace = GROW_CAPACITY(old);
		current->continues = GROW_ARRAY(int,current->continues,old,current->continueSpace);
	}

	for (size_t i = current->loopLocalCount; i < current->localCount; ++i) {
		emitByte(OP_POP);
	}
	current->continues[current->continueCount++] = emitJump(OP_JUMP);
}

static void whileStatement() {
	size_t blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	advance();

	int loopStart = currentChunk()->count;

	expression();
	consume(TOKEN_COLON, "Expect ':' after condition.");

	int exitJump = emitJump(OP_JUMP_IF_FALSE);
	emitByte(OP_POP);

	int oldLocalCount = current->loopLocalCount;
	current->loopLocalCount = current->localCount;
	beginScope();
	block(blockWidth,"while");
	endScope();

	current->loopLocalCount = oldLocalCount;
	emitLoop(loopStart);
	patchJump(exitJump);
	emitByte(OP_POP);
	patchBreaks(loopStart);
}

static void forStatement() {
	/* I'm not sure if I want this to be more like Python or C/Lox/etc. */
	size_t blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	advance();

	/* For now this is going to be kinda broken */
	beginScope();

	ssize_t loopInd = current->localCount;
	ssize_t varCount = 0;
	int matchedEquals = 0;
	do {
		ssize_t ind = parseVariable("Expected name for loop iterator.");
		if (match(TOKEN_EQUAL)) {
			matchedEquals = 1;
			expression();
		} else {
			emitByte(OP_NONE);
		}
		defineVariable(ind);
		varCount++;
	} while (match(TOKEN_COMMA));

	int loopStart;
	int exitJump;

	if (!matchedEquals && match(TOKEN_IN)) {

		/* ITERABLE.__iter__() */
		beginScope();
		expression();
		endScope();

		KrkToken _it = syntheticToken("");
		size_t indLoopIter = current->localCount;
		addLocal(_it);
		defineVariable(indLoopIter);

		KrkToken _iter = syntheticToken("__iter__");
		ssize_t ind = identifierConstant(&_iter);
		EMIT_CONSTANT_OP(OP_GET_PROPERTY, ind);
		emitBytes(OP_CALL, 0);

		/* assign */
		EMIT_CONSTANT_OP(OP_SET_LOCAL, indLoopIter);

		/* LOOP STARTS HERE */
		loopStart = currentChunk()->count;

		/* Call the iterator */
		EMIT_CONSTANT_OP(OP_GET_LOCAL, indLoopIter);
		emitBytes(OP_CALL, 0);

		/* Assign the result to our loop index */
		EMIT_CONSTANT_OP(OP_SET_LOCAL, loopInd);

		/* Get the loop iterator again */
		EMIT_CONSTANT_OP(OP_GET_LOCAL, indLoopIter);
		emitByte(OP_EQUAL);
		exitJump = emitJump(OP_JUMP_IF_TRUE);
		emitByte(OP_POP);

		if (varCount > 1) {
			EMIT_CONSTANT_OP(OP_GET_LOCAL, loopInd);
			EMIT_CONSTANT_OP(OP_UNPACK, varCount);
			for (ssize_t i = loopInd + varCount - 1; i >= loopInd; i--) {
				EMIT_CONSTANT_OP(OP_SET_LOCAL, i);
				emitByte(OP_POP);
			}
		}

	} else {
		consume(TOKEN_SEMICOLON,"expect ; after var declaration in for loop");
		loopStart = currentChunk()->count;

		beginScope();
		do {
			expression(); /* condition */
		} while (match(TOKEN_COMMA));
		endScope();
		exitJump = emitJump(OP_JUMP_IF_FALSE);
		emitByte(OP_POP);

		if (check(TOKEN_SEMICOLON)) {
			advance();
			int bodyJump = emitJump(OP_JUMP);
			int incrementStart = currentChunk()->count;
			beginScope();
			do {
				expression();
			} while (match(TOKEN_COMMA));
			endScope();
			emitByte(OP_POP);

			emitLoop(loopStart);
			loopStart = incrementStart;
			patchJump(bodyJump);
		}
	}

	consume(TOKEN_COLON,"expect :");

	int oldLocalCount = current->loopLocalCount;
	current->loopLocalCount = current->localCount;
	beginScope();
	block(blockWidth,"for");
	endScope();

	current->loopLocalCount = oldLocalCount;
	emitLoop(loopStart);
	patchJump(exitJump);
	emitByte(OP_POP);
	patchBreaks(loopStart);

	endScope();
}

static void returnStatement() {
	if (check(TOKEN_EOL) || check(TOKEN_EOF)) {
		emitReturn();
	} else {
		if (current->type == TYPE_INIT) {
			error("Can not return values from __init__");
		}
		expression();
		emitByte(OP_RETURN);
	}
}

static void tryStatement() {
	size_t blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	advance();
	consume(TOKEN_COLON, "Expect ':' after try.");

	/* Make sure we are in a local scope so this ends up on the stack */
	beginScope();
	int tryJump = emitJump(OP_PUSH_TRY);
	addLocal(syntheticToken("exception"));
	defineVariable(0);

	beginScope();
	block(blockWidth,"try");
	endScope();

	int successJump = emitJump(OP_JUMP);
	patchJump(tryJump);

	if (blockWidth == 0 || (check(TOKEN_INDENTATION) && (parser.current.length == blockWidth))) {
		KrkToken previous;
		if (blockWidth) {
			previous = parser.previous;
			advance();
		}
		if (match(TOKEN_EXCEPT)) {
			consume(TOKEN_COLON, "Expect ':' after except.");
			beginScope();
			block(blockWidth,"except");
			endScope();
		} else if (!check(TOKEN_EOL) && !check(TOKEN_EOF)) {
			krk_ungetToken(parser.current);
			parser.current = parser.previous;
			if (blockWidth) {
				parser.previous = previous;
			}
		} else {
			advance(); /* Ignore this blank indentation line */
		}
	}

	patchJump(successJump);
	endScope(); /* will pop the exception handler */
}

static void raiseStatement() {
	expression();
	emitByte(OP_RAISE);
}


static size_t importModule(KrkToken * startOfName) {
	consume(TOKEN_IDENTIFIER, "Expected module name");
	*startOfName = parser.previous;
	while (match(TOKEN_DOT)) {
		if (startOfName->start + startOfName->literalWidth != parser.previous.start) {
			error("Unexpected whitespace after module path element");
			return 0;
		}
		startOfName->literalWidth += parser.previous.literalWidth;
		startOfName->length += parser.previous.length;
		consume(TOKEN_IDENTIFIER, "Expected module path element after '.'");
		if (startOfName->start + startOfName->literalWidth != parser.previous.start) {
			error("Unexpected whitespace after '.'");
			return 0;
		}
		startOfName->literalWidth += parser.previous.literalWidth;
		startOfName->length += parser.previous.length;
	}
	size_t ind = identifierConstant(startOfName);
	EMIT_CONSTANT_OP(OP_IMPORT, ind);
	return ind;
}

static void importStatement() {
	do {
		KrkToken firstName = parser.current;
		KrkToken startOfName;
		size_t ind = importModule(&startOfName);
		if (match(TOKEN_AS)) {
			consume(TOKEN_IDENTIFIER, "Expected identifier after `as`");
			ind = identifierConstant(&parser.previous);
		} else if (startOfName.length != firstName.length) {
			/**
			 * We imported foo.bar.baz and 'baz' is now on the stack with no name.
			 * But while doing that, we built a chain so that foo and foo.bar are
			 * valid modules that already exist in the module table. We want to
			 * have 'foo.bar.baz' be this new object, so remove 'baz', reimport
			 * 'foo' directly, and put 'foo' into the appropriate namespace.
			 */
			emitByte(OP_POP);
			parser.previous = firstName;
			ind = identifierConstant(&firstName);
			EMIT_CONSTANT_OP(OP_IMPORT, ind);
		}
		declareVariable();
		defineVariable(ind);
	} while (match(TOKEN_COMMA));
}

static void fromImportStatement() {
	KrkToken startOfName;
	importModule(&startOfName);
	consume(TOKEN_IMPORT, "Expected 'import' after module name");
	do {
		consume(TOKEN_IDENTIFIER, "Expected member name");
		size_t member = identifierConstant(&parser.previous);
		emitBytes(OP_DUP, 0); /* Duplicate the package object so we can GET_PROPERTY on it? */
		EMIT_CONSTANT_OP(OP_IMPORT_FROM, member);
		if (match(TOKEN_AS)) {
			consume(TOKEN_IDENTIFIER, "Expected identifier after `as`");
			member = identifierConstant(&parser.previous);
		}
		if (current->scopeDepth) {
			/* Swaps the original module and the new possible local so it can be in the right place */
			emitByte(OP_SWAP);
		}
		declareVariable();
		defineVariable(member);
	} while (match(TOKEN_COMMA));
	emitByte(OP_POP); /* Pop the remaining copy of the module. */
}

static void delStatement() {
	do {
		inDel = 1;
		expression();
	} while (match(TOKEN_COMMA));
	inDel = 0;
}

static void statement() {
	if (match(TOKEN_EOL) || match(TOKEN_EOF)) {
		return; /* Meaningless blank line */
	}

	if (check(TOKEN_IF)) {
		ifStatement();
	} else if (check(TOKEN_WHILE)) {
		whileStatement();
	} else if (check(TOKEN_FOR)) {
		forStatement();
	} else if (check(TOKEN_TRY)) {
		tryStatement();
	} else if (check(TOKEN_WITH)) {
		withStatement();
	} else {
		/* These statements don't eat line feeds, so we need expect to see another one. */
_anotherSimpleStatement:
		if (match(TOKEN_RAISE)) {
			raiseStatement();
		} else if (match(TOKEN_RETURN)) {
			returnStatement();
		} else if (match(TOKEN_IMPORT)) {
			importStatement();
		} else if (match(TOKEN_FROM)) {
			fromImportStatement();
		} else if (match(TOKEN_BREAK)) {
			breakStatement();
		} else if (match(TOKEN_CONTINUE)) {
			continueStatement();
		} else if (match(TOKEN_DEL)) {
			delStatement();
		} else if (match(TOKEN_PASS)) {
			/* Do nothing. */
		} else {
			expressionStatement();
		}
		if (match(TOKEN_SEMICOLON)) goto _anotherSimpleStatement;
		if (!match(TOKEN_EOL) && !match(TOKEN_EOF)) {
			errorAtCurrent("Unexpected token after statement.");
		}
	}
}

static void unary(int canAssign) {
	KrkTokenType operatorType = parser.previous.type;

	parsePrecedence(PREC_UNARY);

	switch (operatorType) {
		case TOKEN_MINUS: emitByte(OP_NEGATE); break;
		case TOKEN_TILDE: emitByte(OP_BITNEGATE); break;

		/* These are equivalent */
		case TOKEN_BANG:
		case TOKEN_NOT:
			emitByte(OP_NOT);
			break;

		default: return;
	}
}

static int isHex(int c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static void string(int type) {
	/* We'll just build with a flexible array like everything else. */
	size_t stringCapacity = 0;
	size_t stringLength   = 0;
	char * stringBytes    = 0;
#define PUSH_CHAR(c) do { if (stringCapacity < stringLength + 1) { \
		size_t old = stringCapacity; stringCapacity = GROW_CAPACITY(old); \
		stringBytes = GROW_ARRAY(char, stringBytes, old, stringCapacity); \
	} stringBytes[stringLength++] = c; } while (0)

#define PUSH_HEX(n, type) do { \
	char tmpbuf[10] = {0}; \
	for (size_t i = 0; i < n; ++i) { \
		if (c + i + 2 == end || !isHex(c[i+2])) { \
			error("truncated \\%c escape", type); \
			return; \
		} \
		tmpbuf[i] = c[i+2]; \
	} \
	unsigned long value = strtoul(tmpbuf, NULL, 16); \
	if (value >= 0x110000) { \
		error("invalid codepoint in \\%c escape", type); \
	} \
	if (isBytes) { \
		PUSH_CHAR(value); \
		break; \
	} \
	unsigned char bytes[5] = {0}; \
	size_t len = krk_codepointToBytes(value, bytes); \
	for (size_t i = 0; i < len; i++) PUSH_CHAR(bytes[i]); \
} while (0)

	int isBytes = (parser.previous.type == TOKEN_PREFIX_B);
	int isFormat = (parser.previous.type == TOKEN_PREFIX_F);

	int atLeastOne = 0;
	const char * lineBefore = krk_tellScanner().linePtr;
	size_t lineNo = krk_tellScanner().line;

	if ((isBytes || isFormat) && !(match(TOKEN_STRING) || match(TOKEN_BIG_STRING))) {
		error("Expected string after prefix? (Internal error - scanner should not have produced this.)");
	}

	/* This should capture everything but the quotes. */
	do {
		int type = parser.previous.type == TOKEN_BIG_STRING ? 3 : 1;
		const char * c = parser.previous.start + type;
		const char * end = parser.previous.start + parser.previous.length - type;
		while (c < end) {
			if (*c == '\\') {
				switch (c[1]) {
					case '\\': PUSH_CHAR('\\'); break;
					case '\'': PUSH_CHAR('\''); break;
					case '\"': PUSH_CHAR('\"'); break;
					case 'a': PUSH_CHAR('\a'); break;
					case 'b': PUSH_CHAR('\b'); break;
					case 'f': PUSH_CHAR('\f'); break;
					case 'n': PUSH_CHAR('\n'); break;
					case 'r': PUSH_CHAR('\r'); break;
					case 't': PUSH_CHAR('\t'); break;
					case 'v': PUSH_CHAR('\v'); break;
					case '[': PUSH_CHAR('\033'); break;
					case 'x': {
						PUSH_HEX(2,'x');
						c += 2;
					} break;
					case 'u': {
						if (isBytes) {
							PUSH_CHAR(c[0]);
							PUSH_CHAR(c[1]);
						} else {
							PUSH_HEX(4,'u');
							c += 4;
						}
					} break;
					case 'U': {
						if (isBytes) {
							PUSH_CHAR(c[0]);
							PUSH_CHAR(c[1]);
						} else {
							PUSH_HEX(8,'U');
							c += 8;
						}
					} break;
					case '\n': break;
					default:
						/* TODO octal */
						PUSH_CHAR(c[0]);
						c++;
						continue;
				}
				c += 2;
			} else if (isFormat && *c == '{') {
				if (!atLeastOne || stringLength) { /* Make sure there's a string for coersion reasons */
					emitConstant(OBJECT_VAL(krk_copyString(stringBytes,stringLength)));
					if (atLeastOne) emitByte(OP_ADD);
					atLeastOne = 1;
				}
				stringLength = 0;
				KrkScanner beforeExpression = krk_tellScanner();
				Parser  parserBefore = parser;
				KrkScanner inner = (KrkScanner){.start=c+1, .cur=c+1, .linePtr=lineBefore, .line=lineNo, .startOfLine = 0, .hasUnget = 0};
				krk_rewindScanner(inner);
				advance();
				expression();
				if (parser.hadError) {
					FREE_ARRAY(char,stringBytes,stringCapacity);
					return;
				}
				inner = krk_tellScanner(); /* To figure out how far to advance c */
				krk_rewindScanner(beforeExpression); /* To get us back to where we were with a string token */
				parser = parserBefore;
				c = inner.start;
				KrkToken which = syntheticToken("str");
				if (*c == '!') {
					c++;
					/* Conversion specifiers, must only be one */
					if (*c == 'r') {
						which = syntheticToken("repr");
					} else if (*c == 's') {
						which = syntheticToken("str");
					} else {
						error("Unsupported conversion flag for f-string expression");
						goto _cleanupError;
					}
					c++;
				}
				size_t ind = identifierConstant(&which);
				EMIT_CONSTANT_OP(OP_GET_GLOBAL, ind);
				emitByte(OP_SWAP);
				emitBytes(OP_CALL, 1);
				if (*c == ':') {
					/* TODO format specs */
					error("Format spec not supported in f-string");
					goto _cleanupError;
				}
				if (*c != '}') {
					error("Expected closing } after expression in f-string");
					goto _cleanupError;
				}
				if (atLeastOne) emitByte(OP_ADD);
				atLeastOne = 1;
				c++;
			} else {
				if (*(unsigned char*)c > 127 && isBytes) {
					error("bytes literal can only contain ASCII characters");
					goto _cleanupError;
				}
				PUSH_CHAR(*c);
				c++;
			}
		}
	} while ((!isBytes || match(TOKEN_PREFIX_B)) && (match(TOKEN_STRING) || match(TOKEN_BIG_STRING)));
	if (isBytes && (match(TOKEN_STRING) || match(TOKEN_BIG_STRING))) {
		error("can not mix bytes and string literals");
		goto _cleanupError;
	}
	if (isBytes) {
		KrkBytes * bytes = krk_newBytes(0,NULL);
		bytes->bytes = (uint8_t*)stringBytes;
		bytes->length = stringLength;
		krk_bytesUpdateHash(bytes);
		emitConstant(OBJECT_VAL(bytes));
		return;
	}
	if (!isFormat || stringLength || !atLeastOne) {
		emitConstant(OBJECT_VAL(krk_copyString(stringBytes,stringLength)));
		if (atLeastOne) emitByte(OP_ADD);
	}
	FREE_ARRAY(char,stringBytes,stringCapacity);
#undef PUSH_CHAR
	return;
_cleanupError:
	FREE_ARRAY(char,stringBytes,stringCapacity);
}

static size_t addUpvalue(Compiler * compiler, ssize_t index, int isLocal) {
	size_t upvalueCount = compiler->function->upvalueCount;
	for (size_t i = 0; i < upvalueCount; ++i) {
		Upvalue * upvalue = &compiler->upvalues[i];
		if ((ssize_t)upvalue->index == index && upvalue->isLocal == isLocal) {
			return i;
		}
	}
	if (upvalueCount + 1 > compiler->upvaluesSpace) {
		size_t old = compiler->upvaluesSpace;
		compiler->upvaluesSpace = GROW_CAPACITY(old);
		compiler->upvalues = GROW_ARRAY(Upvalue,compiler->upvalues,old,compiler->upvaluesSpace);
	}
	compiler->upvalues[upvalueCount].isLocal = isLocal;
	compiler->upvalues[upvalueCount].index = index;
	return compiler->function->upvalueCount++;
}

static ssize_t resolveUpvalue(Compiler * compiler, KrkToken * name) {
	if (compiler->enclosing == NULL) return -1;
	ssize_t local = resolveLocal(compiler->enclosing, name);
	if (local != -1) {
		compiler->enclosing->locals[local].isCaptured = 1;
		return addUpvalue(compiler, local, 1);
	}
	ssize_t upvalue = resolveUpvalue(compiler->enclosing, name);
	if (upvalue != -1) {
		return addUpvalue(compiler, upvalue, 0);
	}
	return -1;
}

#define OP_NONE_LONG -1
#define DO_VARIABLE(opset,opget,opdel) do { \
	if (canAssign && match(TOKEN_EQUAL)) { \
		expression(); \
		EMIT_CONSTANT_OP(opset, arg); \
	} else if (canAssign && matchAssignment()) { \
		EMIT_CONSTANT_OP(opget, arg); \
		assignmentValue(); \
		EMIT_CONSTANT_OP(opset, arg); \
	} else if (inDel && matchEndOfDel()) {\
		if (opdel == OP_NONE || !canAssign || inDel != 1) { error("Invalid del target"); } else { \
		EMIT_CONSTANT_OP(opdel, arg); inDel = 2; } \
	} else { \
		EMIT_CONSTANT_OP(opget, arg); \
	} } while (0)

static void namedVariable(KrkToken name, int canAssign) {
	ssize_t arg = resolveLocal(current, &name);
	if (arg != -1) {
		DO_VARIABLE(OP_SET_LOCAL, OP_GET_LOCAL, OP_NONE);
	} else if ((arg = resolveUpvalue(current, &name)) != -1) {
		DO_VARIABLE(OP_SET_UPVALUE, OP_GET_UPVALUE, OP_NONE);
	} else {
		arg = identifierConstant(&name);
		DO_VARIABLE(OP_SET_GLOBAL, OP_GET_GLOBAL, OP_DEL_GLOBAL);
	}
}
#undef DO_VARIABLE

static void variable(int canAssign) {
	namedVariable(parser.previous, canAssign);
}

static void self(int canAssign) {
	if (currentClass == NULL) {
		error("Invalid reference to `self` outside of a class method.");
		return;
	}
	variable(0);
}

static void super_(int canAssign) {
	if (currentClass == NULL) {
		error("Invalid reference to `super` outside of a class.");
	}
	consume(TOKEN_LEFT_PAREN, "Expected `super` to be called.");
	consume(TOKEN_RIGHT_PAREN, "`super` can not take arguments.");
	consume(TOKEN_DOT, "Expected a field of `super()` to be referenced.");
	consume(TOKEN_IDENTIFIER, "Expected a field name.");
	size_t ind = identifierConstant(&parser.previous);
	namedVariable(syntheticToken("self"), 0);
	namedVariable(syntheticToken("super"), 0);
	EMIT_CONSTANT_OP(OP_GET_SUPER, ind);
}

static void comprehension(KrkScanner scannerBefore, Parser parserBefore, const char buildFunc[], void (*inner)(ssize_t loopCounter)) {
	/* Compile list comprehension as a function */
	Compiler subcompiler;
	initCompiler(&subcompiler, TYPE_FUNCTION);
	subcompiler.function->chunk.filename = subcompiler.enclosing->function->chunk.filename;

	beginScope();

	/* for i=0, */
	emitConstant(INTEGER_VAL(0));
	size_t indLoopCounter = current->localCount;
	addLocal(syntheticToken(""));
	defineVariable(indLoopCounter);

	/* x in... */
	ssize_t loopInd = current->localCount;
	ssize_t varCount = 0;
	do {
		defineVariable(parseVariable("Expected name for iteration variable."));
		emitByte(OP_NONE);
		defineVariable(loopInd);
		varCount++;
	} while (match(TOKEN_COMMA));

	consume(TOKEN_IN, "Only iterator loops (for ... in ...) are allowed in comprehensions.");

	beginScope();
	parsePrecedence(PREC_OR); /* Otherwise we can get trapped on a ternary */
	endScope();

	/* iterable... */
	size_t indLoopIter = current->localCount;
	addLocal(syntheticToken(""));
	defineVariable(indLoopIter);

	/* Now try to call .__iter__ on the result to produce our iterator */
	KrkToken _iter = syntheticToken("__iter__");
	ssize_t ind = identifierConstant(&_iter);
	EMIT_CONSTANT_OP(OP_GET_PROPERTY, ind);
	emitBytes(OP_CALL, 0);

	/* Assign the resulting iterator to indLoopIter */
	EMIT_CONSTANT_OP(OP_SET_LOCAL, indLoopIter);

	/* Mark the start of the loop */
	int loopStart = currentChunk()->count;

	/* Call the iterator to get a value for our list */
	EMIT_CONSTANT_OP(OP_GET_LOCAL, indLoopIter);
	emitBytes(OP_CALL, 0);

	/* Assign the result to our loop index */
	EMIT_CONSTANT_OP(OP_SET_LOCAL, loopInd);

	/* Compare the iterator to the loop index;
	 * our iterators return themselves to say they are done;
	 * this allows them to return None without any issue,
	 * and there's no feasible way they can return themselves without
	 * our intended sentinel meaning, right? Surely? */
	EMIT_CONSTANT_OP(OP_GET_LOCAL, indLoopIter);
	emitByte(OP_EQUAL);
	int exitJump = emitJump(OP_JUMP_IF_TRUE);
	emitByte(OP_POP);

	/* Unpack tuple */
	if (varCount > 1) {
		EMIT_CONSTANT_OP(OP_GET_LOCAL, loopInd);
		EMIT_CONSTANT_OP(OP_UNPACK, varCount);
		for (ssize_t i = loopInd + varCount - 1; i >= loopInd; i--) {
			EMIT_CONSTANT_OP(OP_SET_LOCAL, i);
			emitByte(OP_POP);
		}
	}

	if (match(TOKEN_IF)) {
		parsePrecedence(PREC_OR);
		int acceptJump = emitJump(OP_JUMP_IF_TRUE);
		emitByte(OP_POP); /* Pop condition */
		emitLoop(loopStart);
		patchJump(acceptJump);
		emitByte(OP_POP); /* Pop condition */
	}

	/* Now we can rewind the scanner to have it parse the original
	 * expression that uses our iterated values! */
	KrkScanner scannerAfter = krk_tellScanner();
	Parser  parserAfter = parser;
	krk_rewindScanner(scannerBefore);
	parser = parserBefore;

	beginScope();
	inner(indLoopCounter);
	endScope();

	/* Then we can put the parser back to where it was at the end of
	 * the iterator expression and continue. */
	krk_rewindScanner(scannerAfter);
	parser = parserAfter;

	/* We keep a counter so we can keep track of how many arguments
	 * are on the stack, which we need in order to find the listOf()
	 * method above; having run the expression and generated an
	 * item which is now on the stack, increment the counter */
	EMIT_CONSTANT_OP(OP_INC, indLoopCounter);
	/* ... and loop back to the iterator call. */
	emitLoop(loopStart);

	/* Finally, at this point, we've seen the iterator produce itself
	 * and we're done receiving objects, so mark this instruction
	 * offset as the exit target for the OP_JUMP_IF_FALSE above */
	patchJump(exitJump);
	/* Pop the last loop expression result which was already stored */
	emitByte(OP_POP);
	/* Pull in listOf from the global namespace */
	KrkToken collectionBuilder = syntheticToken(buildFunc);
	size_t indList = identifierConstant(&collectionBuilder);
	EMIT_CONSTANT_OP(OP_GET_GLOBAL, indList);
	/* And move it into where we were storing the loop iterator */
	EMIT_CONSTANT_OP(OP_SET_LOCAL, indLoopIter);
	/* (And pop it from the top of the stack) */
	emitByte(OP_POP);
	/* Then get the counter for our arg count */
	EMIT_CONSTANT_OP(OP_GET_LOCAL, indLoopCounter);
	/* And then call the native method which should be ^ that many items down */
	emitByte(OP_CALL_STACK);
	/* And return the result back to the original scope */
	emitByte(OP_RETURN);
	/* Now because we made a function we need to fill out its upvalues
	 * and write the closure call for it. */
	KrkFunction *subfunction = endCompiler();
	size_t indFunc = krk_addConstant(currentChunk(), OBJECT_VAL(subfunction));
	EMIT_CONSTANT_OP(OP_CLOSURE, indFunc);
	doUpvalues(&subcompiler, subfunction);
	freeCompiler(&subcompiler);

	/* And finally we can call the subfunction and get the result. */
	emitBytes(OP_CALL, 0);
}

static void singleInner(ssize_t indLoopCounter) {
	expression();
}

static void grouping(int canAssign) {
	startEatingWhitespace();
	if (check(TOKEN_RIGHT_PAREN)) {
		emitBytes(OP_TUPLE,0);
	} else {
		size_t chunkBefore = currentChunk()->count;
		KrkScanner scannerBefore = krk_tellScanner();
		Parser  parserBefore = parser;
		expression();
		if (match(TOKEN_FOR)) {
			currentChunk()->count = chunkBefore;
			comprehension(scannerBefore, parserBefore, "tupleOf", singleInner);
		} else if (match(TOKEN_COMMA)) {
			size_t argCount = 1;
			if (!check(TOKEN_RIGHT_PAREN)) {
				do {
					expression();
					argCount++;
				} while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_PAREN));
			}
			EMIT_CONSTANT_OP(OP_TUPLE, argCount);
		}
	}
	stopEatingWhitespace();
	consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void list(int canAssign) {
	size_t     chunkBefore = currentChunk()->count;

	startEatingWhitespace();

	KrkToken listOf = syntheticToken("listOf");
	size_t ind = identifierConstant(&listOf);
	EMIT_CONSTANT_OP(OP_GET_GLOBAL, ind);

	if (!check(TOKEN_RIGHT_SQUARE)) {
		KrkScanner scannerBefore = krk_tellScanner();
		Parser  parserBefore = parser;
		expression();

		/* This is a bit complicated and the Pratt parser does not handle it
		 * well; if we read an expression and then saw a `for`, we need to back
		 * up and start over, as we'll need to define a variable _after_ it
		 * gets used in this expression; so we record the parser state before
		 * reading the first expression of a list constant. If it _is_ a real
		 * list constant, we'll see a comma next and we can begin the normal
		 * loop of counting arguments. */
		if (match(TOKEN_FOR)) {
			/* Roll back the earlier compiler */
			currentChunk()->count = chunkBefore;

			comprehension(scannerBefore, parserBefore, "listOf", singleInner);
		} else {
			size_t argCount = 1;
			while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_SQUARE)) {
				expression();
				argCount++;
			}
			EMIT_CONSTANT_OP(OP_CALL, argCount);
		}
	} else {
		/* Empty list expression */
		emitBytes(OP_CALL, 0);
	}
	stopEatingWhitespace();
	consume(TOKEN_RIGHT_SQUARE,"Expected ] at end of list expression.");
}

static void dictInner(ssize_t indLoopCounter) {
	expression();
	consume(TOKEN_COLON, "Expect colon after dict key.");
	expression();
	EMIT_CONSTANT_OP(OP_INC, indLoopCounter);
}

static void dict(int canAssign) {
	size_t     chunkBefore = currentChunk()->count;

	startEatingWhitespace();

	KrkToken dictOf = syntheticToken("dictOf");
	size_t ind = identifierConstant(&dictOf);
	EMIT_CONSTANT_OP(OP_GET_GLOBAL, ind);

	if (!check(TOKEN_RIGHT_BRACE)) {
		KrkScanner scannerBefore = krk_tellScanner();
		Parser  parserBefore = parser;

		expression();
		if (match(TOKEN_COMMA) || match(TOKEN_RIGHT_BRACE)) {
			krk_rewindScanner(scannerBefore);
			parser = parserBefore;
			currentChunk()->count = chunkBefore;
			KrkToken setOf = syntheticToken("setOf");
			size_t ind = identifierConstant(&setOf);
			EMIT_CONSTANT_OP(OP_GET_GLOBAL, ind);
			size_t argCount = 0;
			do {
				expression();
				argCount++;
			} while (match(TOKEN_COMMA));
			EMIT_CONSTANT_OP(OP_CALL, argCount);
		} else if (match(TOKEN_FOR)) {
			currentChunk()->count = chunkBefore;
			comprehension(scannerBefore, parserBefore, "setOf", singleInner);
		} else {
			consume(TOKEN_COLON, "Expect colon after dict key.");
			expression();

			if (match(TOKEN_FOR)) {
				/* Roll back the earlier compiler */
				currentChunk()->count = chunkBefore;

				comprehension(scannerBefore, parserBefore, "dictOf", dictInner);
			} else {
				size_t argCount = 2;
				while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_BRACE)) {
					expression();
					consume(TOKEN_COLON, "Expect colon after dict key.");
					expression();
					argCount += 2;
				}
				EMIT_CONSTANT_OP(OP_CALL, argCount);
			}
		}
	} else {
		emitBytes(OP_CALL, 0);
	}
	stopEatingWhitespace();
	consume(TOKEN_RIGHT_BRACE,"Expected } at end of dict expression.");
}

#define RULE(token, a, b, c) [token] = {# token, a, b, c}

ParseRule krk_parseRules[] = {
	RULE(TOKEN_LEFT_PAREN,    grouping, call,   PREC_CALL),
	RULE(TOKEN_RIGHT_PAREN,   NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_LEFT_BRACE,    dict,     NULL,   PREC_NONE),
	RULE(TOKEN_RIGHT_BRACE,   NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_LEFT_SQUARE,   list,     get_,   PREC_CALL),
	RULE(TOKEN_RIGHT_SQUARE,  NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_COLON,         NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_COMMA,         NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_DOT,           NULL,     dot,    PREC_CALL),
	RULE(TOKEN_MINUS,         unary,    binary, PREC_TERM),
	RULE(TOKEN_PLUS,          NULL,     binary, PREC_TERM),
	RULE(TOKEN_SEMICOLON,     NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_SOLIDUS,       NULL,     binary, PREC_FACTOR),
	RULE(TOKEN_ASTERISK,      NULL,     binary, PREC_FACTOR),
	RULE(TOKEN_POW,           NULL,     binary, PREC_EXPONENT),
	RULE(TOKEN_MODULO,        NULL,     binary, PREC_FACTOR),
	RULE(TOKEN_BANG,          unary,    NULL,   PREC_NONE),
	RULE(TOKEN_BANG_EQUAL,    NULL,     binary, PREC_COMPARISON),
	RULE(TOKEN_EQUAL,         NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_EQUAL_EQUAL,   NULL,     binary, PREC_COMPARISON),
	RULE(TOKEN_GREATER,       NULL,     binary, PREC_COMPARISON),
	RULE(TOKEN_GREATER_EQUAL, NULL,     binary, PREC_COMPARISON),
	RULE(TOKEN_LESS,          NULL,     binary, PREC_COMPARISON),
	RULE(TOKEN_LESS_EQUAL,    NULL,     binary, PREC_COMPARISON),
	RULE(TOKEN_IDENTIFIER,    variable, NULL,   PREC_NONE),
	RULE(TOKEN_STRING,        string,   NULL,   PREC_NONE),
	RULE(TOKEN_BIG_STRING,    string,   NULL,   PREC_NONE),
	RULE(TOKEN_PREFIX_B,      string,   NULL,   PREC_NONE),
	RULE(TOKEN_PREFIX_F,      string,   NULL,   PREC_NONE),
	RULE(TOKEN_NUMBER,        number,   NULL,   PREC_NONE),
	RULE(TOKEN_AND,           NULL,     and_,   PREC_AND),
	RULE(TOKEN_CLASS,         NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_ELSE,          NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_FALSE,         literal,  NULL,   PREC_NONE),
	RULE(TOKEN_FOR,           NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_DEF,           NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_DEL,           NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_IF,            NULL,     ternary,PREC_TERNARY),
	RULE(TOKEN_IN,            NULL,     in_,    PREC_COMPARISON),
	RULE(TOKEN_LET,           NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_NONE,          literal,  NULL,   PREC_NONE),
	RULE(TOKEN_NOT,           unary,    not_,   PREC_COMPARISON),
	RULE(TOKEN_IS,            NULL,     is_,    PREC_COMPARISON),
	RULE(TOKEN_OR,            NULL,     or_,    PREC_OR),
	RULE(TOKEN_RETURN,        NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_SELF,          self,     NULL,   PREC_NONE),
	RULE(TOKEN_SUPER,         super_,   NULL,   PREC_NONE),
	RULE(TOKEN_TRUE,          literal,  NULL,   PREC_NONE),
	RULE(TOKEN_WHILE,         NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_BREAK,         NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_CONTINUE,      NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_IMPORT,        NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_RAISE,         NULL,     NULL,   PREC_NONE),

	RULE(TOKEN_AT,            NULL,     NULL,   PREC_NONE),

	RULE(TOKEN_TILDE,         unary,    NULL,   PREC_NONE),
	RULE(TOKEN_PIPE,          NULL,     binary, PREC_BITOR),
	RULE(TOKEN_CARET,         NULL,     binary, PREC_BITXOR),
	RULE(TOKEN_AMPERSAND,     NULL,     binary, PREC_BITAND),
	RULE(TOKEN_LEFT_SHIFT,    NULL,     binary, PREC_SHIFT),
	RULE(TOKEN_RIGHT_SHIFT,   NULL,     binary, PREC_SHIFT),

	RULE(TOKEN_PLUS_EQUAL,    NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_MINUS_EQUAL,   NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_PLUS_PLUS,     NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_MINUS_MINUS,   NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_CARET_EQUAL,   NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_PIPE_EQUAL,    NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_LSHIFT_EQUAL,  NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_RSHIFT_EQUAL,  NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_AMP_EQUAL,     NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_SOLIDUS_EQUAL, NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_ASTERISK_EQUAL,NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_MODULO_EQUAL,  NULL,     NULL,   PREC_NONE),

	RULE(TOKEN_LAMBDA,        lambda,   NULL,   PREC_NONE),

	/* This is going to get interesting */
	RULE(TOKEN_INDENTATION,   NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_ERROR,         NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_EOL,           NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_EOF,           NULL,     NULL,   PREC_NONE),
	RULE(TOKEN_RETRY,         NULL,     NULL,   PREC_NONE),
};

static void actualTernary(size_t count, KrkScanner oldScanner, Parser oldParser) {
	currentChunk()->count = count;

	parsePrecedence(PREC_OR);

	int thenJump = emitJump(OP_JUMP_IF_TRUE);
	emitByte(OP_POP); /* Pop the condition */
	consume(TOKEN_ELSE, "Expected 'else' after ternary condition");

	parsePrecedence(PREC_OR);

	KrkScanner outScanner = krk_tellScanner();
	Parser outParser = parser;

	int elseJump = emitJump(OP_JUMP);
	patchJump(thenJump);
	emitByte(OP_POP);

	krk_rewindScanner(oldScanner);
	parser = oldParser;
	parsePrecedence(PREC_OR);
	patchJump(elseJump);

	krk_rewindScanner(outScanner);
	parser = outParser;
}

static void parsePrecedence(Precedence precedence) {
	size_t count = currentChunk()->count;
	KrkScanner oldScanner = krk_tellScanner();
	Parser oldParser = parser;

	advance();
	ParseFn prefixRule = getRule(parser.previous.type)->prefix;
	if (prefixRule == NULL) {
		errorAtCurrent("Unexpected token.");
		return;
	}
	int canAssign = precedence <= PREC_ASSIGNMENT;
	prefixRule(canAssign);
	while (precedence <= getRule(parser.current.type)->precedence) {
		advance();
		ParseFn infixRule = getRule(parser.previous.type)->infix;
		if (infixRule == ternary) {
			actualTernary(count, oldScanner, oldParser);
		} else {
			infixRule(canAssign);
		}
	}

	if (canAssign && matchAssignment()) {
		error("invalid assignment target");
	}
	if (inDel == 1 && matchEndOfDel()) {
		error("invalid del target");
	}
}

static ssize_t identifierConstant(KrkToken * name) {
	return krk_addConstant(currentChunk(), OBJECT_VAL(krk_copyString(name->start, name->length)));
}

static ssize_t resolveLocal(Compiler * compiler, KrkToken * name) {
	for (ssize_t i = compiler->localCount - 1; i >= 0; i--) {
		Local * local = &compiler->locals[i];
		if (identifiersEqual(name, &local->name)) {
			if (local->depth == -1) {
				error("Can not initialize value recursively (are you shadowing something?)");
			}
			return i;
		}
	}
	return -1;
}

static void addLocal(KrkToken name) {
	if (current->localCount + 1 > current->localsSpace) {
		size_t old = current->localsSpace;
		current->localsSpace = GROW_CAPACITY(old);
		current->locals = GROW_ARRAY(Local,current->locals,old,current->localsSpace);
	}
	Local * local = &current->locals[current->localCount++];
	local->name = name;
	local->depth = -1;
	local->isCaptured = 0;

	if (current->function->localNameCount + 1 > current->localNameCapacity) {
		size_t old = current->localNameCapacity;
		current->localNameCapacity = GROW_CAPACITY(old);
		current->function->localNames = GROW_ARRAY(KrkLocalEntry, current->function->localNames, old, current->localNameCapacity);
	}
	current->function->localNames[current->function->localNameCount].id = current->localCount-1;
	current->function->localNames[current->function->localNameCount].birthday = currentChunk()->count;
	current->function->localNames[current->function->localNameCount].deathday = 0;
	current->function->localNames[current->function->localNameCount].name = krk_copyString(name.start, name.length);
	current->function->localNameCount++;
}

static void declareVariable() {
	if (current->scopeDepth == 0) return;
	KrkToken * name = &parser.previous;
	/* Detect duplicate definition */
	for (ssize_t i = current->localCount - 1; i >= 0; i--) {
		Local * local = &current->locals[i];
		if (local->depth != -1 && local->depth < (ssize_t)current->scopeDepth) break;
		if (identifiersEqual(name, &local->name)) {
			error("Duplicate definition for local '%.*s' in this scope.", (int)name->literalWidth, name->start);
		}
	}
	addLocal(*name);
}

static ssize_t parseVariable(const char * errorMessage) {
	consume(TOKEN_IDENTIFIER, errorMessage);

	declareVariable();
	if (current->scopeDepth > 0) return 0;

	return identifierConstant(&parser.previous);
}

static void defineVariable(size_t global) {
	if (current->scopeDepth > 0) {
		markInitialized();
		return;
	}

	EMIT_CONSTANT_OP(OP_DEFINE_GLOBAL, global);
}

static void call(int canAssign) {
	startEatingWhitespace();
	size_t argCount = 0, specialArgs = 0, keywordArgs = 0, seenKeywordUnpacking = 0;
	if (!check(TOKEN_RIGHT_PAREN)) {
		do {
			if (match(TOKEN_ASTERISK) || check(TOKEN_POW)) {
				specialArgs++;
				if (match(TOKEN_POW)) {
					seenKeywordUnpacking = 1;
					emitBytes(OP_EXPAND_ARGS, 2); /* Outputs something special */
					expression(); /* Expect dict */
					continue;
				} else {
					if (seenKeywordUnpacking) {
						error("Iterable expansion follows keyword argument unpacking.");
						return;
					}
					emitBytes(OP_EXPAND_ARGS, 1); /* outputs something special */
					expression();
					continue;
				}
			}
			if (match(TOKEN_IDENTIFIER)) {
				KrkToken argName = parser.previous;
				if (check(TOKEN_EQUAL)) {
					/* This is a keyword argument. */
					advance();
					/* Output the name */
					size_t ind = identifierConstant(&argName);
					EMIT_CONSTANT_OP(OP_CONSTANT, ind);
					expression();
					keywordArgs++;
					specialArgs++;
					continue;
				} else {
					/*
					 * This is a regular argument that happened to start with an identifier,
					 * roll it back so we can process it that way.
					 */
					krk_ungetToken(parser.current);
					parser.current = argName;
				}
			} else if (seenKeywordUnpacking) {
				error("positional argument follows keyword argument unpacking");
				return;
			} else if (keywordArgs) {
				error("Positional argument follows keyword argument");
				return;
			} else if (specialArgs) {
				emitBytes(OP_EXPAND_ARGS, 0);
				expression();
				specialArgs++;
				continue;
			}
			expression();
			argCount++;
		} while (match(TOKEN_COMMA));
	}
	stopEatingWhitespace();
	consume(TOKEN_RIGHT_PAREN, "Expected ')' after arguments.");
	if (specialArgs) {
		/*
		 * Creates a sentinel at the top of the stack to tell the CALL instruction
		 * how many keyword arguments are at the top of the stack. This value
		 * triggers special handling in the CALL that processes the keyword arguments,
		 * which is relatively slow, so only use keyword arguments if you have to!
		 */
		EMIT_CONSTANT_OP(OP_KWARGS, specialArgs);
		/*
		 * We added two elements - name and value - for each keyword arg,
		 * plus the sentinel object that will show up at the end after the
		 * OP_KWARGS instruction complets, so make sure we have the
		 * right depth into the stack when we execute CALL
		 */
		argCount += 1 /* for the sentinel */ + 2 * specialArgs;
	}
	EMIT_CONSTANT_OP(OP_CALL, argCount);
}

static void and_(int canAssign) {
	int endJump = emitJump(OP_JUMP_IF_FALSE);
	emitByte(OP_POP);
	parsePrecedence(PREC_AND);
	patchJump(endJump);
}

static void ternary(int canAssign) {
	error("This function should not run.");
}

static void or_(int canAssign) {
	int endJump = emitJump(OP_JUMP_IF_TRUE);
	emitByte(OP_POP);
	parsePrecedence(PREC_OR);
	patchJump(endJump);
}

static ParseRule * getRule(KrkTokenType type) {
	return &krk_parseRules[type];
}

#ifdef ENABLE_THREADING
static volatile int _compilerLock = 0;
#endif

KrkFunction * krk_compile(const char * src, int newScope, char * fileName) {
	_obtain_lock(_compilerLock);

	krk_initScanner(src);
	Compiler compiler;
	initCompiler(&compiler, TYPE_MODULE);
	compiler.function->chunk.filename = krk_copyString(fileName, strlen(fileName));

	if (newScope) beginScope();

	parser.hadError = 0;
	parser.panicMode = 0;

	advance();

	if (krk_currentThread.module) {
		KrkValue doc;
		if (!krk_tableGet(&krk_currentThread.module->fields, OBJECT_VAL(krk_copyString("__doc__", 7)), &doc)) {
			if (match(TOKEN_STRING) || match(TOKEN_BIG_STRING)) {
				string(parser.previous.type == TOKEN_BIG_STRING);
				krk_attachNamedObject(&krk_currentThread.module->fields, "__doc__",
					(KrkObj*)AS_STRING(currentChunk()->constants.values[currentChunk()->constants.count-1]));
				emitByte(OP_POP); /* string() actually put an instruction for that, pop its result */
				consume(TOKEN_EOL,"Garbage after docstring");
			} else {
				krk_attachNamedValue(&krk_currentThread.module->fields, "__doc__", NONE_VAL());
			}
		}
	}

	while (!match(TOKEN_EOF)) {
		declaration();
		if (check(TOKEN_EOL) || check(TOKEN_INDENTATION) || check(TOKEN_EOF)) {
			/* There's probably already and error... */
			advance();
		}
	}

	KrkFunction * function = endCompiler();
	freeCompiler(&compiler);
	if (parser.hadError) function = NULL;

	_release_lock(_compilerLock);
	return function;
}

void krk_markCompilerRoots() {
	Compiler * compiler = current;
	while (compiler != NULL) {
		krk_markObject((KrkObj*)compiler->function);
		compiler = compiler->enclosing;
	}
}
