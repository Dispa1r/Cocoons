#ifndef LLVM_TRANSFORMS_COCOONS_STRINGOBFUSCATIONPASS_H
#define LLVM_TRANSFORMS_COCOONS_STRINGOBFUSCATIONPASS_H

#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <cstdint>
#include <optional>
#include <vector>

using namespace llvm;

namespace cocoons {

class StringObfuscationPass : public PassInfoMixin<StringObfuscationPass> {
public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    static bool isEnabled();

private:
    /// 加密字符串的信息记录
    struct EncryptedStringInfo {
        GlobalVariable *GV;       // 加密后的字符串全局变量
        uint32_t Len;             // 字符串长度（含 \0）
        GlobalVariable *GuardGV;  // 每字符串的 guard 变量（i8, init 0）
        Function *DecryptFunc;    // 每字符串独立的解密函数
        bool UsePRNG;             // true = Mode B (seed+PRNG), false = Mode A (key/op arrays)
        // Mode A fields
        GlobalVariable *KeyArrayGV = nullptr;  // [N x i8] per-byte key 数组
        GlobalVariable *OpArrayGV  = nullptr;  // [N x i8] per-byte op 数组
        // Mode B fields
        uint32_t Seed = 0;        // PRNG seed
    };

    /// 递归解析全局变量，定位底层 i8 字节数组
    void processVariable(GlobalVariable *GV,
                         std::vector<GlobalVariable *> &Targets, Module &M);

    /// 加密字符串数据，创建 guard 变量，返回加密信息
    std::optional<EncryptedStringInfo> encryptRealData(Module &M,
                                                       GlobalVariable *TargetGV);

    /// 为单个字符串创建独立的解密函数（随机函数名）
    Function* createDecryptFunctionForString(Module &M, const EncryptedStringInfo &Info);

    /// 在每个字符串使用点前插入解密调用
    void instrumentUseSites(Module &M,
                            const std::vector<EncryptedStringInfo> &Entries);

    /// xorshift32 PRNG（编译时用，和运行时 IR 生成的逻辑一致）
    static uint32_t xorshift32(uint32_t &state);
};

} // namespace cocoons

#endif
