/* rsharp/tools/rsfmt/main.c — R# Formatter (stub)
 * Full implementation: tokenize, rebuild with canonical whitespace/indent.
 * Current: delegates to a Python formatter script.
 * Usage: rsfmt [--check] [--inplace] <files...>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: rsfmt [--check] [--inplace] <file.rsl>...\n");return 1;}
    printf("rsfmt: formatter stub — full implementation coming in v1.1\n");
    /* In real impl: lex file, pretty-print AST with canonical indent */
    return 0;
}
