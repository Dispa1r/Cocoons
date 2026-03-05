#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Transforms/Cocoons/StringObfuscationPass.h"
#include "llvm/Transforms/Cocoons/SubstitutionPass.h"

using namespace llvm;

static void registerCocoonsPasses(PassBuilder &PB) {
    // 在优化管线末尾注册 Cocoons passes
    PB.registerOptimizerLastEPCallback(
        [](ModulePassManager &MPM, OptimizationLevel Level,
           ThinOrFullLTOPhase) {
            if (Level == OptimizationLevel::O0)
                return;

            if (cocoons::StringObfuscationPass::isEnabled())
                MPM.addPass(cocoons::StringObfuscationPass());

            if (cocoons::SubstitutionPass::isEnabled()) {
                FunctionPassManager FPM;
                FPM.addPass(cocoons::SubstitutionPass());
                MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
            }
        });

    // 支持 opt 工具通过名字加载
    PB.registerPipelineParsingCallback(
        [](StringRef Name, ModulePassManager &MPM,
           ArrayRef<PassBuilder::PipelineElement>) {
            if (Name == "cocoons-str-obf") {
                MPM.addPass(cocoons::StringObfuscationPass());
                return true;
            }
            return false;
        });

    PB.registerPipelineParsingCallback(
        [](StringRef Name, FunctionPassManager &FPM,
           ArrayRef<PassBuilder::PipelineElement>) {
            if (Name == "cocoons-sub") {
                FPM.addPass(cocoons::SubstitutionPass());
                return true;
            }
            return false;
        });
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "CocoonsPasses", "2.0.0",
            registerCocoonsPasses};
}
