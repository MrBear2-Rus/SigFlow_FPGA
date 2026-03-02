#ifndef LEXER_API_H
#define LEXER_API_H

struct TOKEN{
    char* type;
    char* text;
    int sl;
    int sc;
    int el;
    int ec;
};

struct TOKENS{
    struct TOKEN* tokens;
    int length;
    int capacity;
};

struct TOKENS* lex_code(const char* code);

void free_tokens(struct TOKENS* tokens);

void print_tokens(const struct TOKENS* tokens);
#endif