/* rsharp/tools/rsdoc/main.c — R# Documentation Generator (stub)
 * Extracts /// doc-comments from .rsl source and generates HTML/Markdown.
 * Usage: rsdoc <src-dir> --out <output-dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: rsdoc <src-dir> --out <out-dir>\n");return 1;}
    printf("rsdoc: documentation generator stub — full implementation coming in v1.1\n");
    /* In real impl: walk src dir, parse /// comments, emit HTML */
    return 0;
}
