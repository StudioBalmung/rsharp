/* rsharp/tools/rsrun/main.c — R# Script Runner
 * Parses + type-checks a .rss/.rsl file, then hands to Python interpreter.
 * Usage: rsrun <file.rss> [args...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: rsrun <file.rss>\n");return 1;}
    /* Check extension */
    const char *f=argv[1];
    const char *ext=strrchr(f,'.');
    if(!ext||!(strcmp(ext,".rss")==0||strcmp(ext,".rsl")==0||strcmp(ext,".rsp")==0)){
        fprintf(stderr,"rsrun: expected .rss/.rsl/.rsp file\n");return 1;
    }
    /* Delegate to Python interpreter */
    char cmd[1024];
    snprintf(cmd,sizeof cmd,"python3 $(dirname $(which rsrun))/../runtime/interpreter/interpreter.py '%s'",f);
    return system(cmd);
}
