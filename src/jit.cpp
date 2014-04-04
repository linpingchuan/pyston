// Copyright (c) 2014 Dropbox, Inc.
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//    http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <csignal>
#include <cassert>
#include <stdint.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/time.h>

#include "llvm/Support/ManagedStatic.h"
//#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"

#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"

#include "core/ast.h"
#include "core/util.h"

#include "codegen/entry.h"
#include "codegen/llvm_interpreter.h"
#include "codegen/parser.h"


#ifndef GITREV
#error
#endif

using namespace pyston;

int main(int argc, char** argv) {
    Timer _t("for jit startup");
    //llvm::sys::PrintStackTraceOnErrorSignal();
    //llvm::PrettyStackTraceProgram X(argc, argv);
    llvm::llvm_shutdown_obj Y;

    int code;
    bool caching = true;
    bool force_repl = false;
    bool repl = true;
    bool stats = false;
    while ((code = getopt(argc, argv, "+Oqcdibpjtrsvn")) != -1) {
        if (code == 'O')
            FORCE_OPTIMIZE = true;
        else if (code == 't')
            TRAP = true;
        else if (code == 'q')
            GLOBAL_VERBOSITY = 0;
        else if (code == 'v')
            GLOBAL_VERBOSITY++;
        //else if (code == 'c') // now always enabled
            //caching = true;
        else if (code == 'd')
            SHOW_DISASM = true;
        else if (code == 'i')
            force_repl = true;
        else if (code == 'b') {
            BENCH = true;
        } else if (code == 'n') {
            ENABLE_INTERPRETER = false;
        } else if (code == 'p') {
            PROFILE = true;
        } else if (code == 'j') {
            DUMPJIT = true;
        } else if (code == 's') {
            stats = true;
        } else if (code == 'r') {
            USE_STRIPPED_STDLIB = true;
        } else if (code == '?')
            abort();
    }

    const char* fn = NULL;
    if (optind != argc-1 && optind != argc) {
        fprintf(stderr, "Error: python-level arguments not supported yet (first given was %s)\n", argv[optind+1]);
        exit(1);
    }
    if (optind != argc) {
        fn = argv[optind];
        if (!force_repl)
            repl = false;
    }

    // end of argument parsing

    {
        Timer _t("for initCodegen");
        initCodegen();
    }

    BoxedModule* main = createMainModule(fn);

    _t.split("to run");
    if (fn != NULL) {
        int num_iterations = 1;
        if (BENCH)
            num_iterations = 1000;

        for (int i = 0; i < num_iterations; i++) {
            AST_Module *m;
            if (caching)
                m = caching_parse(fn);
            else
                m = parse(fn);

            if (VERBOSITY() >= 1) {
                fprintf(stderr, "Parsed code; ast:\n");
                print_ast(m);
                fprintf(stderr, "==============\n");
            }

            CompiledFunction* compiled = compileModule(m, main);
            if (VERBOSITY() >= 1)
                fprintf(stderr, "compiled module.main to machine code; running:\n");
            if (compiled->is_interpreted)
                interpretFunction(compiled->func, 0, NULL, NULL, NULL, NULL);
            else
                ((void (*)())compiled->code)();
            if (VERBOSITY() >= 1)
                fprintf(stderr, "finished running\n");
        }
    }

    if (repl && BENCH) {
        timeval start, end;
        gettimeofday(&start, NULL);
        const int MAX_RUNS = 1000;
        const int MAX_TIME = 30;
        int run = 0;
        while (true) {
            run++;

            AST_Module *m = new AST_Module();
            CompiledFunction* compiled = compileModule(m, main);
            if (compiled->is_interpreted)
                interpretFunction(compiled->func, 0, NULL, NULL, NULL, NULL);
            else
                ((void (*)())compiled->code)();

            if (run >= MAX_RUNS) {
                printf("Quitting after %d iterations\n", run);
                break;
            }
            gettimeofday(&end, NULL);
            if (end.tv_sec - start.tv_sec > MAX_TIME) {
                printf("Quitting after %d seconds (%d iterations)\n", MAX_TIME, run);
                break;
            }
        }
        gettimeofday(&end, NULL);
        long ms = 1000 * (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000;
        printf("%ldms (%.2fms per)\n", ms, 1.0 * ms / run);

        repl = force_repl;
    }

    if (repl) {
        printf("Pyston v0.1, rev " STRINGIFY(GITREV) "\n");
    }
    while (repl) {
        printf(">> ");
        fflush(stdout);

        char* line = NULL;
        size_t size;
        int read;
        if ((read = getline(&line, &size, stdin)) == -1) {
            repl = false;
        } else {
            timeval start, end;
            gettimeofday(&start, NULL);

            char buf[] = "pystontmp_XXXXXX";
            char *tmpdir = mkdtemp(buf);
            assert(tmpdir);
            std::string tmp = std::string(tmpdir) + "/in.py";
            if (VERBOSITY() >= 1) {
                printf("writing %d bytes to %s\n", read, tmp.c_str());
            }

            FILE* f = fopen(tmp.c_str(), "w");
            fwrite(line, 1, read, f);
            fclose(f);

            AST_Module* m = parse(tmp.c_str());
            removeDirectoryIfExists(tmpdir);

            if (m->body.size() > 0 && m->body[0]->type == AST_TYPE::Expr) {
                AST_Expr *e = static_cast<AST_Expr*>(m->body[0]);
                AST_Print *p = new AST_Print();
                p->dest = NULL;
                p->nl = true;
                p->values.push_back(e->value);
                m->body[0] = p;
            }

            CompiledFunction* compiled = compileModule(m, main);
            if (compiled->is_interpreted)
                interpretFunction(compiled->func, 0, NULL, NULL, NULL, NULL);
            else
                ((void (*)())compiled->code)();

            if (VERBOSITY() >= 1) {
                gettimeofday(&end, NULL);
                long ms = 1000 * (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000;
                printf("%ldms\n", ms);
            }
        }
    }
    _t.split("joinRuntime");

    int rtncode = joinRuntime();
    _t.split("finishing up");

    if (VERBOSITY() >= 1 || stats)
        Stats::dump();

    return rtncode;
}
