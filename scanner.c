#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "kuroko.h"
#include "scanner.h"

typedef struct {
	const char * start;
	const char * cur;
	size_t line;
	int startOfLine;
} KrkScanner;

KrkScanner scanner;

void krk_initScanner(const char * src) {
	scanner.start = src;
	scanner.cur   = src;
	scanner.line  = 1;
	scanner.startOfLine = 1;
	/* file, etc. ? */
}

static int isAtEnd() {
	return *scanner.cur == '\0';
}

static KrkToken makeToken(KrkTokenType type) {
	return (KrkToken){
		.type = type,
		.start = scanner.start,
		.length = (size_t)(scanner.cur - scanner.start),
		.line = scanner.line
	};
}

static KrkToken errorToken(const char * errorStr) {
	return (KrkToken){
		.type = TOKEN_ERROR,
		.start = errorStr,
		.length = strlen(errorStr),
		.line = scanner.line
	};
}

static char advance() {
	return *(scanner.cur++);
}

static int match(char expected) {
	if (isAtEnd()) return 0;
	if (*scanner.cur != expected) return 0;
	scanner.cur++;
	return 1;
}

static char peek() {
	return *scanner.cur;
}

static char peekNext() {
	if (isAtEnd()) return '\0';
	return scanner.cur[1];
}

static void skipWhitespace() {
	for (;;) {
		char c = peek();
		switch (c) {
			case ' ':
			case '\t':
				advance();
				break;
			case '\n':
				scanner.line++;
				scanner.startOfLine = 1;
			default:
				return;
		}
	}
}

static KrkToken makeIndentation() {
	while (!isAtEnd() && peek() == ' ') advance();
	if (peek() == '\n') {
		return errorToken("Empty indentation line is invalid.");
	}
	return makeToken(TOKEN_INDENTATION);
}

static KrkToken string() {
	while (peek() != '"' && !isAtEnd()) {
		if (peek() == '\\') advance(); /* Advance twice */
		if (peek() == '\n') scanner.line++; /* Not start of line because string */
		advance();
	}

	if (isAtEnd()) return errorToken("Unterminated string.");

	assert(peek() == '"');
	advance();

	return makeToken(TOKEN_STRING);
}

static KrkToken codepoint() {
	while (peek() != '\'' && !isAtEnd()) {
		if (peek() == '\\') advance();
		if (peek() == '\n') return makeToken(TOKEN_RETRY);
		advance();
	}

	if (isAtEnd()) return errorToken("Unterminated codepoint literal.");

	assert(peek() == '\'');
	advance();

	return makeToken(TOKEN_CODEPOINT);
}

static int isDigit(char c) {
	return c >= '0' && c <= '9';
}

static KrkToken number(char c) {
	if (c == 0) {
		/* Hexadecimal */
		if (peek() == 'x' || peek() == 'X') {
			advance();
			do {
				char n = peek();
				if (isDigit(n) || (n >= 'a' && n <= 'f') || (n >= 'A' && n <= 'F')) {
					advance();
					continue;
				}
			} while (0);
			return makeToken(TOKEN_NUMBER);
		}

		/* Binary */
		if (peek() == 'b' || peek() == 'B') {
			advance();
			while (peek() == '0' || peek() == '1') advance();
			return makeToken(TOKEN_NUMBER);
		}

		/* Octal */
		while (peek() >= '0' && peek() <= '7') advance();
		return makeToken(TOKEN_NUMBER);
	}

	/* Decimal */
	while (isDigit(peek())) advance();

	/* Floating point */
	if (peek() == '.' && isDigit(peekNext())) {
		advance();
		while (isDigit(peek())) advance();
	}

	return makeToken(TOKEN_NUMBER);
}

static int isAlpha(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c == '_');
}

static int checkKeyword(size_t start, const char * rest, KrkTokenType type) {
	size_t length = strlen(rest);
	if (scanner.cur - scanner.start == start + length &&
		memcmp(scanner.start + start, rest, length) == 0) return type;
	return TOKEN_IDENTIFIER;
}

static KrkTokenType identifierType() {
#define MORE(i) (scanner.cur - scanner.start > i)
	switch (*scanner.start) {
		case 'a': return checkKeyword(1, "nd", TOKEN_AND);
		case 'c': return checkKeyword(1, "lass", TOKEN_CLASS);
		case 'd': return checkKeyword(1, "ef", TOKEN_DEF);
		case 'e': return checkKeyword(1, "lse", TOKEN_ELSE);
		case 'f': return checkKeyword(1, "or", TOKEN_FOR);
		case 'F': return checkKeyword(1, "alse", TOKEN_FALSE);
		case 'i': if (MORE(1)) switch (scanner.start[1]) {
			case 'f': return checkKeyword(2, "f", TOKEN_IF);
			case 'n': return checkKeyword(2, "n", TOKEN_IN);
		} break;
		case 'l': return checkKeyword(1, "et", TOKEN_LET);
		case 'n': return checkKeyword(1, "ot", TOKEN_NOT);
		case 'N': return checkKeyword(1, "one", TOKEN_NONE);
		case 'o': return checkKeyword(1, "r", TOKEN_OR);
		case 'p': return checkKeyword(1, "rint", TOKEN_PRINT);
		case 'r': return checkKeyword(1, "eturn", TOKEN_RETURN);
		case 's': if (MORE(1)) switch(scanner.start[1]) {
			case 'e': return checkKeyword(2, "lf", TOKEN_SELF);
			case 'u': return checkKeyword(2, "per", TOKEN_SUPER);
		} break;
		case 'T': return checkKeyword(1, "rue", TOKEN_TRUE);
		case 'w': return checkKeyword(1, "hile", TOKEN_WHILE);
	}
	return TOKEN_IDENTIFIER;
}

static KrkToken identifier() {
	while (isAlpha(peek()) || isDigit(peek())) advance();

	return makeToken(identifierType());
}

KrkToken krk_scanToken() {

	/* If at start of line, do thing */
	if (scanner.startOfLine && peek() == ' ') {
		scanner.start = scanner.cur;
		return makeIndentation();
	} else {
		scanner.startOfLine = 0;
	}

	/* Eat whitespace */
	skipWhitespace();

	/* Skip comments */
	if (peek() == '#') while (peek() != '\n' && !isAtEnd()) advance();

	scanner.start = scanner.cur;
	if (isAtEnd()) return makeToken(TOKEN_EOF);

	char c = advance();

	if (isAlpha(c)) return identifier();
	if (isDigit(c)) return number(c);

	switch (c) {
		case '(': return makeToken(TOKEN_LEFT_PAREN);
		case ')': return makeToken(TOKEN_RIGHT_PAREN);
		case '{': return makeToken(TOKEN_LEFT_BRACE);
		case '}': return makeToken(TOKEN_RIGHT_BRACE);
		case '[': return makeToken(TOKEN_LEFT_SQUARE);
		case ']': return makeToken(TOKEN_RIGHT_SQUARE);
		case ':': return makeToken(TOKEN_COLON);
		case ',': return makeToken(TOKEN_COMMA);
		case '.': return makeToken(TOKEN_DOT);
		case '-': return makeToken(TOKEN_MINUS);
		case '+': return makeToken(TOKEN_PLUS);
		case ';': return makeToken(TOKEN_SEMICOLON);
		case '/': return makeToken(TOKEN_SOLIDUS);
		case '*': return makeToken(TOKEN_ASTERISK);

		case '!': return makeToken(match('=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
		case '=': return makeToken(match('=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
		case '<': return makeToken(match('=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
		case '>': return makeToken(match('=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);

		case '"': return string();
		case '\'': return codepoint();
	}


	return errorToken("Unexpected character.");
}
