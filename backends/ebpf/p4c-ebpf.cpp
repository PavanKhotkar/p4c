/*
Copyright 2013-present Barefoot Networks, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <stdio.h>

#include <iostream>
#include <string>

#include "backends/ebpf/version.h"
#include "control-plane/p4RuntimeSerializer.h"
#include "ebpfBackend.h"
#include "ebpfOptions.h"
#include "frontends/common/applyOptionsPragmas.h"
#include "frontends/common/parseInput.h"
#include "frontends/p4/frontend.h"
#include "fstream"
#include "ir/ir.h"
#include "ir/json_loader.h"
#include "lib/crash.h"
#include "lib/exceptions.h"
#include "lib/gc.h"
#include "lib/log.h"
#include "lib/nullstream.h"
#include "midend.h"

using namespace P4;

void compile(EbpfOptions &options) {
    auto hook = options.getDebugHook();
    bool isv1 = options.langVersion == CompilerOptions::FrontendVersion::P4_14;
    if (isv1) {
        ::P4::error(ErrorType::ERR_UNSUPPORTED_ON_TARGET, "This compiler only handles P4-16");
        return;
    }
    const IR::P4Program *program = nullptr;

    if (options.loadIRFromJson) {
        std::filebuf fb;
        if (fb.open(options.file, std::ios::in) == nullptr) {
            ::P4::error(ErrorType::ERR_IO, "%s: No such file or directory.", options.file);
            return;
        }

        std::istream inJson(&fb);
        JSONLoader jsonFileLoader(inJson);
        if (!jsonFileLoader) {
            ::P4::error(ErrorType::ERR_IO, "%s: Not valid input file", options.file);
            return;
        }
        program = new IR::P4Program(jsonFileLoader);
        fb.close();
    } else {
        program = P4::parseP4File(options);
        if (::P4::errorCount() > 0) return;

        P4::P4COptionPragmaParser optionsPragmaParser(true);
        program->apply(P4::ApplyOptionsPragmas(optionsPragmaParser));

        P4::FrontEnd frontend;
        frontend.addDebugHook(hook);
        program = frontend.run(options, program);
        if (::P4::errorCount() > 0) return;
    }

    if (!options.arch.isNullOrEmpty() && options.arch != "filter") {
        P4::serializeP4RuntimeIfRequired(program, options);
        if (::P4::errorCount() > 0) return;
    }

    EBPF::MidEnd midend;
    midend.addDebugHook(hook);
    auto toplevel = midend.run(options, program);
    if (!options.dumpJsonFile.empty())
        JSONGenerator(*openFile(options.dumpJsonFile, true)).emit(program);
    if (::P4::errorCount() > 0) return;

    EBPF::run_ebpf_backend(options, toplevel, &midend.refMap, &midend.typeMap);
}

int main(int argc, char *const argv[]) {
    setup_gc_logging();
    setup_signals();

    AutoCompileContext autoEbpfContext(new EbpfContext);
    auto &options = EbpfContext::get().options();
    options.compilerVersion = cstring(P4C_EBPF_VERSION_STRING);

    if (options.process(argc, argv) != nullptr) {
        if (options.loadIRFromJson == false) options.setInputFile();
    }
    if (::P4::errorCount() > 0) exit(1);

    options.calculateXDP2TCMode();
    try {
        compile(options);
    } catch (const std::exception &bug) {
        std::cerr << bug.what() << std::endl;
        return 1;
    }

    if (Log::verbose()) std::cerr << "Done." << std::endl;
    return ::P4::errorCount() > 0;
}
