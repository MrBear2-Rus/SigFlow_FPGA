#include "v_lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 外部 Flex 自动生成函数
extern void* yy_scan_string(const char* str);  
extern int yylex(void);                       
extern void yy_delete_buffer(void* buffer);   

struct TOKENS g_tokens_storage;
struct TOKENS* g_tokens = NULL;

struct TOKENS* lex_code(const char* code) {
    // 初始化全局 TOKENS
    g_tokens_storage.tokens = NULL;
    g_tokens_storage.length = 0;
    g_tokens_storage.capacity = 0;
    g_tokens = &g_tokens_storage;

    // 扫描输入
    void* buffer = yy_scan_string(code);
    yylex();
    yy_delete_buffer(buffer);


    return g_tokens;
}

void free_tokens(struct TOKENS* tokens) {
    if (!tokens) return;
    for (int i = 0; i < tokens->length; i++) {
        free(tokens->tokens[i].type);
        free(tokens->tokens[i].text);
    }
    free(tokens->tokens);

    tokens->tokens = NULL;
    tokens->length = 0;
    tokens->capacity = 0;
    g_tokens = NULL;
}

void print_tokens(const struct TOKENS* tokens) {
    if (!tokens) return;
    for (int i = 0; i < tokens->length; i++) {
        struct TOKEN* t = &tokens->tokens[i];
        printf("[%3d:%2d-%3d:%2d] %-16s : %s\n", t->sl, t->sc, t->el, t->ec, t->type, t->text);
    }
}
