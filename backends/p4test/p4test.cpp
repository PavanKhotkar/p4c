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

#include "p4test.h"

#include <fstream>  // IWYU pragma: keep
#include <iostream>

#include "backends/p4test/version.h"
#include "control-plane/p4RuntimeSerializer.h"
#include "frontends/common/applyOptionsPragmas.h"
#include "frontends/common/parseInput.h"
#include "frontends/p4/evaluator/evaluator.h"
#include "frontends/p4/frontend.h"
#include "frontends/p4/toP4/toP4.h"
#include "ir/ir.h"
#include "ir/json_loader.h"
#include "ir/pass_utils.h"
#include "lib/crash.h"
#include "lib/error.h"
#include "lib/exceptions.h"
#include "lib/gc.h"
#include "lib/log.h"
#include "lib/nullstream.h"
#include "midend.h"

P4TestOptions::P4TestOptions() {
    registerOption(
        "--listMidendPasses", nullptr,
        [this](const char *) {
            listMidendPasses = true;
            loadIRFromJson = false;
            P4Test::MidEnd MidEnd(*this, outStream);
            exit(0);
            return false;
        },
        "[p4test] Lists exact name of all midend passes.\n");
    registerOption(
        "--parse-only", nullptr,
        [this](const char *) {
            parseOnly = true;
            return true;
        },
        "only parse the P4 input, without any further processing");
    registerOption(
        "--validate", nullptr,
        [this](const char *) {
            validateOnly = true;
            return true;
        },
        "Validate the P4 input, running just the front-end");
    registerOption(
        "--fromJSON", "file",
        [this](const char *arg) {
            loadIRFromJson = true;
            file = arg;
            return true;
        },
        "read previously dumped json instead of P4 source code");
    registerOption(
        "--turn-off-logn", nullptr,
        [](const char *) {
            ::P4::Log::Detail::enableLoggingGlobally = false;
            return true;
        },
        "Turn off LOGN() statements in the compiler.\n"
        "Use '@__debug' annotation to enable LOGN on "
        "the annotated P4 object within the source code.\n");
    registerOption(
        "--preferSwitch", nullptr,
        [this](const char *) {
            preferSwitch = true;
            return true;
        },
        "use passes that use general switch instead of action_run");
}

class P4TestPragmas : public P4::P4COptionPragmaParser {
    std::optional<IOptionPragmaParser::CommandLineOptions> tryToParse(
        const IR::Annotation *annotation) {
        if (annotation->name == "test_keep_opassign") {
            test_keepOpAssign = true;
            return std::nullopt;
        }
        return P4::P4COptionPragmaParser::tryToParse(annotation);
    }

 public:
    P4TestPragmas() : P4::P4COptionPragmaParser(true) {}

    bool test_keepOpAssign = false;
};

class TestFEPolicy : public P4::FrontEndPolicy {
    const P4TestPragmas &pragmas;

    P4::ParseAnnotations *getParseAnnotations() const {
        return new P4::ParseAnnotations("p4test", true,
                                        P4::ParseAnnotations::HandlerMap({
                                            PARSE_EMPTY("test_keep_opassign"_cs),
                                        }),
                                        false);
    }

    bool removeOpAssign() const { return !pragmas.test_keepOpAssign; }

 public:
    explicit TestFEPolicy(const P4TestPragmas &pragmas) : pragmas(pragmas) {}
};

using P4TestContext = P4CContextWithOptions<P4TestOptions>;

static void log_dump(const IR::Node *node, const char *head) {
    if (node && LOGGING(1)) {
        if (head)
            std::cout << '+' << std::setw(strlen(head) + 6) << std::setfill('-') << "+\n| " << head
                      << " |\n"
                      << '+' << std::setw(strlen(head) + 3) << "+" << std::endl
                      << std::setfill(' ');
        if (LOGGING(2))
            dump(node);
        else
            std::cout << *node << std::endl;
    }
}

int main(int argc, char *const argv[]) {
    setup_gc_logging();
    setup_signals();

    AutoCompileContext autoP4TestContext(new P4TestContext);
    auto &options = P4TestContext::get().options();
    options.langVersion = CompilerOptions::FrontendVersion::P4_16;
    options.compilerVersion = cstring(P4TEST_VERSION_STRING);

    if (options.process(argc, argv) != nullptr) {
        if (options.loadIRFromJson == false) options.setInputFile();
    }
    if (::P4::errorCount() > 0) return 1;
    const IR::P4Program *program = nullptr;
    auto hook = options.getDebugHook();
    if (options.loadIRFromJson) {
        std::ifstream json(options.file);
        if (json) {
            JSONLoader loader(json);
            const IR::Node *node = nullptr;
            loader >> node;
            if (!(program = node->to<IR::P4Program>()))
                error(ErrorType::ERR_INVALID, "%s is not a P4Program in json format", options.file);
        } else {
            error(ErrorType::ERR_IO, "Can't open %s", options.file);
        }
    } else {
        P4::DiagnosticCountInfo info;
        program = P4::parseP4File(options);
        info.emitInfo("PARSER");

        if (program != nullptr && ::P4::errorCount() == 0) {
            P4TestPragmas testPragmas;
            program->apply(P4::ApplyOptionsPragmas(testPragmas));
            info.emitInfo("PASS P4COptionPragmaParser");

            if (!options.parseOnly) {
                try {
                    TestFEPolicy fe_policy(testPragmas);
                    P4::FrontEnd fe(&fe_policy);
                    fe.addDebugHook(hook);
                    // use -TdiagnosticCountInPass:1 / -TdiagnosticCountInPass:4 to get output of
                    // this hook
                    fe.addDebugHook(info.getPassManagerHook());
                    program = fe.run(options, program);
                } catch (const std::exception &bug) {
                    std::cerr << bug.what() << std::endl;
                    return 1;
                }
            }
        }
    }

    log_dump(program, "Initial program");
    if (program != nullptr && ::P4::errorCount() == 0) {
        P4::serializeP4RuntimeIfRequired(program, options);

        if (!options.parseOnly && !options.validateOnly) {
            P4Test::MidEnd midEnd(options);
            midEnd.addDebugHook(hook);
#if 0
            /* doing this breaks the output until we get dump/undump of srcInfo */
            if (options.debugJson) {
                std::stringstream tmp;
                JSONGenerator gen(tmp);
                gen << program;
                JSONLoader loader(tmp);
                loader >> program;
            }
#endif
            const IR::ToplevelBlock *top = nullptr;
            try {
                top = midEnd.process(program);
                // This can modify program!
                log_dump(program, "After midend");
                log_dump(top, "Top level block");
            } catch (const std::exception &bug) {
                std::cerr << bug.what() << std::endl;
                return 1;
            }
        }
        if (program) {
            if (!options.dumpJsonFile.empty())
                JSONGenerator(*openFile(options.dumpJsonFile, true), true).emit(program);
            if (options.debugJson) {
                std::stringstream ss1, ss2;
                JSONGenerator gen1(ss1), gen2(ss2);
                gen1.emit(program);

                const IR::Node *node = nullptr;
                JSONLoader loader(ss1);
                loader >> node;

                gen2.emit(node);
                if (ss1.str() != ss2.str()) {
                    error(ErrorType::ERR_UNEXPECTED, "json mismatch");
                    std::ofstream t1("t1.json"), t2("t2.json");
                    t1 << ss1.str() << std::flush;
                    t2 << ss2.str() << std::flush;
                    auto rv = system("json_diff t1.json t2.json");
                    if (rv != 0)
                        ::P4::warning(ErrorType::WARN_FAILED, "json_diff failed with code %1%", rv);
                }
            }
        }
    }

    if (Log::verbose()) std::cerr << "Done." << std::endl;
    return ::P4::errorCount() > 0;
}
