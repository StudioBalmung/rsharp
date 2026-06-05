/* rsharp/tools/rsl/main.c
 * rsl — R# Unified CLI  (build tool + package manager)
 * Also installed as: rsharp  (rsharp --version)
 * C11
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#endif

#define RSL_VERSION    "1.0.2"
#define RSC_VERSION    "1.0.2"
#define RSHARP_EDITION "2025"

static bool g_color = false;
static void log_info(const char *tag, const char *msg) {
    if (g_color) fprintf(stderr, "\033[1m\033[32m%-12s\033[0m %s\n", tag, msg);
    else         fprintf(stderr, "%-12s %s\n", tag, msg);
}
static void log_warn(const char *msg) {
    if (g_color) fprintf(stderr, "\033[1m\033[33mwarning\033[0m: %s\n", msg);
    else         fprintf(stderr, "warning: %s\n", msg);
}
static void log_err(const char *msg) {
    if (g_color) fprintf(stderr, "\033[1m\033[31merror\033[0m:   %s\n", msg);
    else         fprintf(stderr, "error:   %s\n", msg);
}

static bool fexists(const char *p) { struct stat s; return stat(p, &s) == 0; }
static void mkdirp(const char *path) {
    char t[512]; strncpy(t, path, 511);
    for (char *p = t+1; *p; p++) if (*p=='/') { *p=0;
#ifdef _WIN32
        _mkdir(t);
#else
        mkdir(t,0755);
#endif
        *p='/'; }
#ifdef _WIN32
    _mkdir(t);
#else
    mkdir(t, 0755);
#endif
}
static void wfile(const char *p, const char *c) {
    FILE *f = fopen(p, "w"); if (f) { fputs(c, f); fclose(f); }
}
static bool toml_get(const char *path, const char *sec, const char *key,
                     char *out, size_t n) {
    FILE *f = fopen(path,"r"); if (!f) return false;
    char ln[256]; bool in = false;
    while (fgets(ln, 256, f)) {
        char *nl = strchr(ln,'\n'); if (nl) *nl=0;
        if (ln[0]=='[') { char s[64]={0}; sscanf(ln,"[%63[^]]",s); in=strcmp(s,sec)==0; continue; }
        if (!in) continue;
        char k[64]={0}, v[128]={0};
        if (sscanf(ln," %63[^ =] = \"%127[^\"]\"",k,v)==2 && strcmp(k,key)==0) {
            strncpy(out,v,n-1); fclose(f); return true;
        }
    }
    fclose(f); return false;
}

static int cmd_new(const char *name) {
    if (!name) { log_err("rsl new requires a project name"); return 1; }
    if (fexists(name)) { char b[128]; snprintf(b,128,"'%s' already exists",name); log_err(b); return 1; }
    char p[512];
    snprintf(p,512,"%s/src",name); mkdirp(p);
    snprintf(p,512,"%s/tests",name); mkdirp(p);
    snprintf(p,512,"%s/docs",name); mkdirp(p);

    char toml[1024];
    snprintf(toml,1024,
        "[package]\nname        = \"%s\"\nversion     = \"0.1.0\"\nedition     = \"%s\"\n"
        "authors     = [\"\"]\ndescription = \"\"\n\n[dependencies]\n\n"
        "[profile.debug]\nopt_level = 0\nassertions = true\n\n"
        "[profile.release]\nopt_level = 2\nlto = true\n", name, RSHARP_EDITION);
    snprintf(p,512,"%s/Rsharp.toml",name); wfile(p,toml);

    char ms[512];
    snprintf(ms,512,
        "// %s — R# project\n\nfn main() => i32 {\n    @print(\"Hello from %s!\")\n    0\n}\n",
        name, name);
    snprintf(p,512,"%s/src/main.rsl",name); wfile(p,ms);
    snprintf(p,512,"%s/.gitignore",name);   wfile(p,"/build\n/target\n*.o\n*.a\n");
    snprintf(p,512,"%s/.gitattributes",name);
    wfile(p, "*.rsl  linguist-language=RSharp\n"
             "*.rsh  linguist-language=RSharp\n"
             "*.rss  linguist-language=RSharp\n"
             "*.rsp  linguist-language=RSharp\n"
             "*.rslib linguist-language=RSharp\n");
    char rm[256]; snprintf(rm,256,"# %s\n\n```bash\nrsl build\nrsl run\n```\n",name);
    snprintf(p,512,"%s/README.md",name); wfile(p,rm);

    char msg[128]; snprintf(msg,128,"project '%s'",name); log_info("Created",msg);
    fprintf(stderr,"  cd %s && rsl run\n",name); return 0;
}

static int do_build(bool is_pkg, bool release, bool verbose) {
    if (!fexists("Rsharp.toml") && !fexists("src/main.rsl")) {
        log_err("no Rsharp.toml. Run 'rsl new <name>' first."); return 1;
    }
    char name[64]="app";
    if (fexists("Rsharp.toml")) toml_get("Rsharp.toml","package","name",name,64);
    mkdirp("build");
    const char *opt = release ? "--release" : "-O0";
    const char *v   = verbose ? "--verbose" : "";
    char cmd[512];
    if (is_pkg) snprintf(cmd,512,"rsc src/lib.rsl   -o build/lib%s.a  %s %s",name,opt,v);
    else        snprintf(cmd,512,"rsc src/main.rsl  -o build/%s       %s %s",name,opt,v);
    char msg[128]; snprintf(msg,128,"%s (%s)",name,release?"release":"debug");
    log_info("Compiling",msg);
    int rc = system(cmd);
    if (rc==0) { char m[64]; snprintf(m,64,"build/%s",name); log_info("Finished",m); }
    return rc;
}

static int cmd_run(int argc, char **argv, bool release) {
    if (do_build(false,release,false) != 0) return 1;
    char name[64]="app";
    if (fexists("Rsharp.toml")) toml_get("Rsharp.toml","package","name",name,64);
    char cmd[1024]; snprintf(cmd,1024,"./build/%s",name);
    for (int i=2;i<argc;i++) {
        strncat(cmd," ",sizeof cmd-strlen(cmd)-1);
        strncat(cmd,argv[i],sizeof cmd-strlen(cmd)-1);
    }
    log_info("Running",cmd); return system(cmd);
}

static int cmd_install(const char *pkg) {
    if (!pkg) { log_err("rsl install <package-name>"); return 1; }
    char m[128]; snprintf(m,128,"'%s'",pkg); log_info("Fetching",m);
    if (fexists("Rsharp.toml")) {
        FILE *f = fopen("Rsharp.toml","a");
        if (f) { fprintf(f,"%s = \"*\"\n",pkg); fclose(f); }
        snprintf(m,128,"added '%s' to Rsharp.toml",pkg); log_info("Added",m);
    }
    log_warn("Package registry coming soon — see docs/PACKAGES.md for manual install");
    fprintf(stderr,
        "  Manual install:\n"
        "    1. Place <pkg>.rslib in ./lib/\n"
        "    2. Add to Rsharp.toml:  %s = { path = \"./lib/%s.rslib\" }\n"
        "    3. In source:  use %s\n", pkg, pkg, pkg);
    return 0;
}

static void print_version(void) {
    printf("rsharp %s (rsl %s, rsc %s, edition %s)\n",
           RSL_VERSION, RSL_VERSION, RSC_VERSION, RSHARP_EDITION);
    printf("host: "); fflush(stdout);
    system("uname -ms 2>/dev/null || echo unknown");
}

static void print_help(void) {
    fprintf(stderr,
        "R# build system v%s\n\n"
        "USAGE:\n"
        "  rsl <command> [options]\n\n"
        "COMMANDS:\n"
        "  new <name>       Create a new R# project\n"
        "  build            Compile the project\n"
        "  build pkg        Compile as a library\n"
        "  run [-- args]    Build and run\n"
        "  check            Type-check only (no codegen)\n"
        "  test             Run tests\n"
        "  fmt              Format source files\n"
        "  doc              Generate API documentation\n"
        "  install <pkg>    Install package  (e.g. rsl install qt5)\n"
        "  add <pkg>        Add dependency to Rsharp.toml\n"
        "  remove <pkg>     Remove dependency\n"
        "  update           Update all dependencies\n"
        "  clean            Remove build artifacts\n"
        "  version          Print version information\n\n"
        "FLAGS:\n"
        "  --release        Optimised release build\n"
        "  --verbose        Verbose compiler output\n"
        "  --target <t>     Cross-compile target triple\n\n"
        "ALSO:\n"
        "  rsharp --version\n",
        RSL_VERSION);
}

int main(int argc, char **argv) {
    g_color = isatty(STDERR_FILENO);
    const char *prog = argv[0];
    const char *base = strrchr(prog,'/'); base = base ? base+1 : prog;

    /* rsharp --version or rsharp --help */
    if (strcmp(base,"rsharp")==0) {
        if (argc>=2 && strcmp(argv[1],"--version")==0) { print_version(); return 0; }
        if (argc>=2 && (strcmp(argv[1],"--help")==0||strcmp(argv[1],"-h")==0)) { print_help(); return 0; }
        if (argc<2) { print_help(); return 0; }
    }
    if (argc < 2) { print_help(); return 0; }

    const char *cmd = argv[1];
    bool release=false, verbose=false;
    for (int i=2;i<argc;i++) {
        if (!strcmp(argv[i],"--release")) release=true;
        if (!strcmp(argv[i],"--verbose")) verbose=true;
    }

    if (!strcmp(cmd,"new"))     return cmd_new(argc>2?argv[2]:NULL);
    if (!strcmp(cmd,"build"))   return do_build(argc>2&&!strcmp(argv[2],"pkg"),release,verbose);
    if (!strcmp(cmd,"run"))     return cmd_run(argc,argv,release);
    if (!strcmp(cmd,"check"))   { log_info("Checking",""); return system("rsc src/main.rsl --check"); }
    if (!strcmp(cmd,"test"))    { log_info("Testing",""); return system("rsc src/ tests/ --test -o build/tests && ./build/tests"); }
    if (!strcmp(cmd,"fmt"))     { log_info("Formatting","src/"); return system("rsfmt src/ 2>/dev/null || echo 'rsfmt not installed'"); }
    if (!strcmp(cmd,"doc"))     { log_info("Documenting","src/"); return system("rsdoc src/ --out docs/api/ 2>/dev/null || echo 'rsdoc not installed'"); }
    if (!strcmp(cmd,"install")) return cmd_install(argc>2?argv[2]:NULL);
    if (!strcmp(cmd,"add")) {
        if (argc<3) { log_err("rsl add <pkg[@version]>"); return 1; }
        char nm[64]={0}, ver[32]="*";
        const char *at = strchr(argv[2],'@');
        if (at) { strncpy(nm,argv[2],at-argv[2]); strncpy(ver,at+1,31); }
        else strncpy(nm,argv[2],63);
        if (!fexists("Rsharp.toml")) { log_err("no Rsharp.toml"); return 1; }
        FILE *f = fopen("Rsharp.toml","a");
        if (f) { fprintf(f,"%s = \"%s\"\n",nm,ver); fclose(f); }
        char m[128]; snprintf(m,128,"'%s' = \"%s\"",nm,ver); log_info("Added",m); return 0;
    }
    if (!strcmp(cmd,"remove"))  { log_warn("not yet implemented"); return 1; }
    if (!strcmp(cmd,"update"))  { log_warn("not yet implemented"); return 1; }
    if (!strcmp(cmd,"clean"))   { log_info("Cleaning","build/"); return system("rm -rf build target *.o"); }
    if (!strcmp(cmd,"version")||!strcmp(cmd,"--version")) { print_version(); return 0; }
    if (!strcmp(cmd,"help")||!strcmp(cmd,"--help")||!strcmp(cmd,"-h")) { print_help(); return 0; }

    char msg[128]; snprintf(msg,128,"unknown command '%s'  (try 'rsl --help')",cmd);
    log_err(msg); return 1;
}
