#include "llvm/Transforms/Cocoons/StringObfuscationPass.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <cstdlib>
#include <random>
#include <vector>

namespace cocoons {

// 命令行开关（静态链接时通过 -mllvm -cocoons-enable-str 使用）
static cl::opt<bool> EnableStrObf(
    "cocoons-enable-str",
    cl::init(false),
    cl::desc("Enable String Obfuscation for iOS"));

// pass plugin 模式下 -mllvm 在 dylib 加载前就已解析，cl::opt 无法生效，
// 因此额外支持环境变量 COCOONS_ENABLE_STR=1 作为备选开关。
bool StringObfuscationPass::isEnabled() {
    if (EnableStrObf)
        return true;
    if (const char *Env = std::getenv("COCOONS_ENABLE_STR"))
        return std::string(Env) == "1";
    return false;
}

// xorshift32 PRNG — 编译时和运行时（IR 生成）使用相同算法
uint32_t StringObfuscationPass::xorshift32(uint32_t &state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// ============================================================
// run() — 主入口
// ============================================================
PreservedAnalyses StringObfuscationPass::run(Module &M, ModuleAnalysisManager &AM) {
    // 1. 扫描 llvm.global.annotations，收集标记了 "obfuscate" 的全局变量
    GlobalVariable *AnnoGV = M.getGlobalVariable("llvm.global.annotations");
    if (!AnnoGV) return PreservedAnalyses::all();

    ConstantArray *AnnArr = dyn_cast<ConstantArray>(AnnoGV->getInitializer());
    if (!AnnArr) return PreservedAnalyses::all();

    std::vector<GlobalVariable *> Targets;
    for (unsigned i = 0; i < AnnArr->getNumOperands(); ++i) {
        ConstantStruct *CS = dyn_cast<ConstantStruct>(AnnArr->getOperand(i));
        if (!CS) continue;

        GlobalVariable *TargetGV = dyn_cast<GlobalVariable>(CS->getOperand(0)->stripPointerCasts());
        GlobalVariable *AnnoStrGV = dyn_cast<GlobalVariable>(CS->getOperand(1)->stripPointerCasts());
        if (TargetGV && AnnoStrGV && AnnoStrGV->hasInitializer()) {
            ConstantDataSequential *AnnoData = dyn_cast<ConstantDataSequential>(AnnoStrGV->getInitializer());
            if (!AnnoData) continue;
            if (AnnoData->getAsString().starts_with("obfuscate")) {
                processVariable(TargetGV, Targets, M);
            }
        }
    }

    if (Targets.empty()) {
        errs() << ">>> 没有可混淆的字符串。\n";
        return PreservedAnalyses::all();
    }

    // 2. 加密每个字符串，为每个字符串创建独立的解密函数
    std::vector<EncryptedStringInfo> Entries;
    for (GlobalVariable *GV : Targets) {
        auto Info = encryptRealData(M, GV);
        if (Info.has_value()) {
            EncryptedStringInfo Entry = Info.value();
            Entry.DecryptFunc = createDecryptFunctionForString(M, Entry);
            Entries.push_back(Entry);
        }
    }

    // 3. 在每个使用点前插入解密调用
    instrumentUseSites(M, Entries);

    return PreservedAnalyses::none();
}

// ============================================================
// processVariable() — 递归解析全局变量，定位 i8 字节数组（不变）
// ============================================================
void StringObfuscationPass::processVariable(GlobalVariable *GV,
                                            std::vector<GlobalVariable *> &Targets,
                                            Module &M) {
    if (!GV || !GV->hasInitializer()) return;

    Constant *Init = GV->getInitializer();
    Value *Stripped = Init->stripPointerCasts();

    // 探测点 A: 直接是字节数组
    if (ConstantDataSequential *CDS = dyn_cast<ConstantDataSequential>(Stripped)) {
        if (!CDS->getElementType()->isIntegerTy(8)) return;
        Targets.push_back(GV);
        return;
    }

    // 探测点 B: OC 结构体 (NSConstantString)
    if (ConstantStruct *CS = dyn_cast<ConstantStruct>(Stripped)) {
        if (CS->getNumOperands() >= 3) {
            Value *V = CS->getOperand(2)->stripPointerCasts();
            if (ConstantExpr *CE = dyn_cast<ConstantExpr>(V))
                V = CE->getOperand(0)->stripPointerCasts();
            if (GlobalVariable *NextGV = dyn_cast<GlobalVariable>(V))
                processVariable(NextGV, Targets, M);
        }
        return;
    }

    // 探测点 C: 指针引用
    if (GlobalVariable *NextGV = dyn_cast<GlobalVariable>(Stripped)) {
        processVariable(NextGV, Targets, M);
        return;
    }

    // 探测点 D: 复杂常量表达式 (GEP 等)
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(Stripped)) {
        Value *V = CE->getOperand(0)->stripPointerCasts();
        if (GlobalVariable *NextGV = dyn_cast<GlobalVariable>(V))
            processVariable(NextGV, Targets, M);
        return;
    }
}

// ============================================================
// encryptRealData() — 加密字符串，随机选择 Mode A/B
// ============================================================
std::optional<StringObfuscationPass::EncryptedStringInfo>
StringObfuscationPass::encryptRealData(Module &M, GlobalVariable *TargetGV) {
    if (!TargetGV || !TargetGV->hasInitializer())
        return std::nullopt;

    Constant *ActualInit = TargetGV->getInitializer();
    ConstantDataSequential *CDS = dyn_cast<ConstantDataSequential>(ActualInit);
    if (!CDS) return std::nullopt;

    // 防止重复加密
    std::string GuardName = "__cocoons_guard_" + TargetGV->getName().str();
    if (M.getGlobalVariable(GuardName))
        return std::nullopt;

    uint32_t Len = CDS->getNumElements() * CDS->getElementByteSize();
    uint32_t PayloadLen = Len > 0 ? Len - 1 : 0; // 跳过末尾 \0

    // 随机数引擎
    std::random_device RD;
    std::default_random_engine Engine(RD());
    std::uniform_int_distribution<int> KeyDist(1, 255);
    std::uniform_int_distribution<int> OpDist(0, 2);
    std::uniform_int_distribution<int> ModeDist(0, 1);

    bool UsePRNG = ModeDist(Engine) == 1;

    // 生成 per-byte key 和 op 序列
    std::vector<uint8_t> Keys(PayloadLen);
    std::vector<uint8_t> Ops(PayloadLen);

    if (UsePRNG) {
        // Mode B: 从 seed 派生 key 和 op
        std::uniform_int_distribution<uint32_t> SeedDist(1, UINT32_MAX);
        uint32_t Seed = SeedDist(Engine);
        uint32_t State = Seed;
        for (uint32_t i = 0; i < PayloadLen; ++i) {
            uint32_t KeyVal = xorshift32(State) & 0xFF;
            if (KeyVal == 0) KeyVal = 1;
            Keys[i] = (uint8_t)KeyVal;
            Ops[i] = (uint8_t)(xorshift32(State) % 3);
        }
        // 加密
        std::vector<uint8_t> Data;
        for (unsigned i = 0; i < CDS->getNumElements(); ++i) {
            uint8_t Val = CDS->getElementAsInteger(i);
            if (i < PayloadLen) {
                switch (Ops[i]) {
                    case 0: Val ^= Keys[i]; break;           // XOR
                    case 1: Val = (Val + Keys[i]) & 0xFF; break; // ADD
                    case 2: Val = (Val - Keys[i]) & 0xFF; break; // SUB
                }
            }
            Data.push_back(Val);
        }
        TargetGV->setInitializer(ConstantDataArray::get(M.getContext(), Data));
        TargetGV->setConstant(false);
        TargetGV->setSection("");
        TargetGV->setAlignment(MaybeAlign(1));

        // 创建 guard
        LLVMContext &Ctx = M.getContext();
        Type *Int8Ty = Type::getInt8Ty(Ctx);
        GlobalVariable *GuardGV = new GlobalVariable(
            M, Int8Ty, false, GlobalValue::InternalLinkage,
            ConstantInt::get(Int8Ty, 0), GuardName);

        EncryptedStringInfo Info{};
        Info.GV = TargetGV;
        Info.Len = Len;
        Info.GuardGV = GuardGV;
        Info.UsePRNG = true;
        Info.Seed = Seed;
        return Info;
    }

    // Mode A: 随机生成 key[] 和 op[]
    for (uint32_t i = 0; i < PayloadLen; ++i) {
        Keys[i] = (uint8_t)KeyDist(Engine);
        Ops[i] = (uint8_t)OpDist(Engine);
    }

    // 加密
    std::vector<uint8_t> Data;
    for (unsigned i = 0; i < CDS->getNumElements(); ++i) {
        uint8_t Val = CDS->getElementAsInteger(i);
        if (i < PayloadLen) {
            switch (Ops[i]) {
                case 0: Val ^= Keys[i]; break;
                case 1: Val = (Val + Keys[i]) & 0xFF; break;
                case 2: Val = (Val - Keys[i]) & 0xFF; break;
            }
        }
        Data.push_back(Val);
    }
    TargetGV->setInitializer(ConstantDataArray::get(M.getContext(), Data));
    TargetGV->setConstant(false);
    TargetGV->setSection("");
    TargetGV->setAlignment(MaybeAlign(1));

    LLVMContext &Ctx = M.getContext();
    Type *Int8Ty = Type::getInt8Ty(Ctx);

    // 创建 key 数组全局变量
    auto *KeyArrayInit = ConstantDataArray::get(Ctx, Keys);
    std::string KeyName = "__cocoons_keys_" + TargetGV->getName().str();
    GlobalVariable *KeyArrayGV = new GlobalVariable(
        M, KeyArrayInit->getType(), true, GlobalValue::InternalLinkage,
        KeyArrayInit, KeyName);

    // 创建 op 数组全局变量
    auto *OpArrayInit = ConstantDataArray::get(Ctx, Ops);
    std::string OpName = "__cocoons_ops_" + TargetGV->getName().str();
    GlobalVariable *OpArrayGV = new GlobalVariable(
        M, OpArrayInit->getType(), true, GlobalValue::InternalLinkage,
        OpArrayInit, OpName);

    // 创建 guard
    GlobalVariable *GuardGV = new GlobalVariable(
        M, Int8Ty, false, GlobalValue::InternalLinkage,
        ConstantInt::get(Int8Ty, 0), GuardName);

    EncryptedStringInfo Info{};
    Info.GV = TargetGV;
    Info.Len = Len;
    Info.GuardGV = GuardGV;
    Info.UsePRNG = false;
    Info.KeyArrayGV = KeyArrayGV;
    Info.OpArrayGV = OpArrayGV;
    return Info;
}

// ============================================================
// createDecryptFunctionForString() — 为单个字符串创建独立的解密函数
// ============================================================
Function* StringObfuscationPass::createDecryptFunctionForString(
    Module &M, const EncryptedStringInfo &Info) {

    LLVMContext &Ctx = M.getContext();
    Type *VoidTy = Type::getVoidTy(Ctx);
    Type *Int8Ty = Type::getInt8Ty(Ctx);
    Type *Int32Ty = Type::getInt32Ty(Ctx);

    // 生成随机函数名
    std::random_device RD;
    std::mt19937 Gen(RD());
    std::uniform_int_distribution<uint32_t> Dist(0, 0xFFFFFFFF);
    uint32_t Hash = Dist(Gen);
    std::string FuncName = "__ccs_" + std::to_string(Hash);

    Function *DecryptFunc = nullptr;

    if (!Info.UsePRNG) {
        // Mode A: key/op 数组解密
        FunctionType *FTy = FunctionType::get(VoidTy, {}, false);
        DecryptFunc = Function::Create(FTy, GlobalValue::InternalLinkage, FuncName, &M);

        BasicBlock *EntryBB = BasicBlock::Create(Ctx, "entry", DecryptFunc);
        BasicBlock *DoDecBB = BasicBlock::Create(Ctx, "do_decrypt", DecryptFunc);
        BasicBlock *LoopBB  = BasicBlock::Create(Ctx, "loop", DecryptFunc);
        BasicBlock *XorBB   = BasicBlock::Create(Ctx, "op_xor", DecryptFunc);
        BasicBlock *SubBB   = BasicBlock::Create(Ctx, "op_sub", DecryptFunc);
        BasicBlock *AddBB   = BasicBlock::Create(Ctx, "op_add", DecryptFunc);
        BasicBlock *StoreBB = BasicBlock::Create(Ctx, "store", DecryptFunc);
        BasicBlock *ExitBB  = BasicBlock::Create(Ctx, "exit", DecryptFunc);

        IRBuilder<> B(EntryBB);
        Value *CmpXchg = B.CreateAtomicCmpXchg(
            Info.GuardGV, ConstantInt::get(Int8Ty, 0), ConstantInt::get(Int8Ty, 1),
            MaybeAlign(1), AtomicOrdering::AcquireRelease, AtomicOrdering::Monotonic);
        Value *WasZero = B.CreateExtractValue(CmpXchg, 1);
        B.CreateCondBr(WasZero, DoDecBB, ExitBB);

        B.SetInsertPoint(DoDecBB);
        Value *DecLen = ConstantInt::get(Int32Ty, Info.Len - 1);
        Value *HasPayload = B.CreateICmpSGT(DecLen, ConstantInt::get(Int32Ty, 0));
        B.CreateCondBr(HasPayload, LoopBB, ExitBB);

        B.SetInsertPoint(LoopBB);
        PHINode *Idx = B.CreatePHI(Int32Ty, 2);
        Idx->addIncoming(ConstantInt::get(Int32Ty, 0), DoDecBB);

        Value *KeyPtr = B.CreateGEP(Int8Ty, Info.KeyArrayGV, Idx);
        Value *Key = B.CreateLoad(Int8Ty, KeyPtr);
        Value *OpPtr = B.CreateGEP(Int8Ty, Info.OpArrayGV, Idx);
        Value *Op = B.CreateLoad(Int8Ty, OpPtr);
        Value *BytePtr = B.CreateGEP(Int8Ty, Info.GV, Idx);
        Value *ByteVal = B.CreateLoad(Int8Ty, BytePtr);

        SwitchInst *SW = B.CreateSwitch(Op, XorBB, 2);
        SW->addCase(cast<ConstantInt>(ConstantInt::get(Int8Ty, 1)), SubBB);
        SW->addCase(cast<ConstantInt>(ConstantInt::get(Int8Ty, 2)), AddBB);

        B.SetInsertPoint(XorBB);
        Value *ResXor = B.CreateXor(ByteVal, Key);
        B.CreateBr(StoreBB);

        B.SetInsertPoint(SubBB);
        Value *ResSub = B.CreateSub(ByteVal, Key);
        B.CreateBr(StoreBB);

        B.SetInsertPoint(AddBB);
        Value *ResAdd = B.CreateAdd(ByteVal, Key);
        B.CreateBr(StoreBB);

        B.SetInsertPoint(StoreBB);
        PHINode *Result = B.CreatePHI(Int8Ty, 3);
        Result->addIncoming(ResXor, XorBB);
        Result->addIncoming(ResSub, SubBB);
        Result->addIncoming(ResAdd, AddBB);
        B.CreateStore(Result, BytePtr);

        Value *NextIdx = B.CreateAdd(Idx, ConstantInt::get(Int32Ty, 1));
        Idx->addIncoming(NextIdx, StoreBB);
        Value *Cont = B.CreateICmpULT(NextIdx, DecLen);
        B.CreateCondBr(Cont, LoopBB, ExitBB);

        B.SetInsertPoint(ExitBB);
        B.CreateRetVoid();
    } else {
        // Mode B: seed + PRNG 解密
        FunctionType *FTy = FunctionType::get(VoidTy, {}, false);
        DecryptFunc = Function::Create(FTy, GlobalValue::InternalLinkage, FuncName, &M);

        BasicBlock *EntryBB = BasicBlock::Create(Ctx, "entry", DecryptFunc);
        BasicBlock *DoDecBB = BasicBlock::Create(Ctx, "do_decrypt", DecryptFunc);
        BasicBlock *LoopBB  = BasicBlock::Create(Ctx, "loop", DecryptFunc);
        BasicBlock *XorBB   = BasicBlock::Create(Ctx, "op_xor", DecryptFunc);
        BasicBlock *SubBB   = BasicBlock::Create(Ctx, "op_sub", DecryptFunc);
        BasicBlock *AddBB   = BasicBlock::Create(Ctx, "op_add", DecryptFunc);
        BasicBlock *StoreBB = BasicBlock::Create(Ctx, "store", DecryptFunc);
        BasicBlock *ExitBB  = BasicBlock::Create(Ctx, "exit", DecryptFunc);

        IRBuilder<> B(EntryBB);
        Value *CmpXchg = B.CreateAtomicCmpXchg(
            Info.GuardGV, ConstantInt::get(Int8Ty, 0), ConstantInt::get(Int8Ty, 1),
            MaybeAlign(1), AtomicOrdering::AcquireRelease, AtomicOrdering::Monotonic);
        Value *WasZero = B.CreateExtractValue(CmpXchg, 1);
        B.CreateCondBr(WasZero, DoDecBB, ExitBB);

        B.SetInsertPoint(DoDecBB);
        Value *DecLen = ConstantInt::get(Int32Ty, Info.Len - 1);
        Value *HasPayload = B.CreateICmpSGT(DecLen, ConstantInt::get(Int32Ty, 0));
        B.CreateCondBr(HasPayload, LoopBB, ExitBB);

        B.SetInsertPoint(LoopBB);
        PHINode *Idx = B.CreatePHI(Int32Ty, 2);
        Idx->addIncoming(ConstantInt::get(Int32Ty, 0), DoDecBB);
        PHINode *State = B.CreatePHI(Int32Ty, 2);
        State->addIncoming(ConstantInt::get(Int32Ty, Info.Seed), DoDecBB);

        Value *S1 = B.CreateXor(State, B.CreateShl(State, ConstantInt::get(Int32Ty, 13)));
        Value *S2 = B.CreateXor(S1, B.CreateLShr(S1, ConstantInt::get(Int32Ty, 17)));
        Value *S3 = B.CreateXor(S2, B.CreateShl(S2, ConstantInt::get(Int32Ty, 5)));
        Value *KeyRaw = B.CreateTrunc(S3, Int8Ty);
        Value *IsZero = B.CreateICmpEQ(KeyRaw, ConstantInt::get(Int8Ty, 0));
        Value *Key = B.CreateSelect(IsZero, ConstantInt::get(Int8Ty, 1), KeyRaw);

        Value *S4 = B.CreateXor(S3, B.CreateShl(S3, ConstantInt::get(Int32Ty, 13)));
        Value *S5 = B.CreateXor(S4, B.CreateLShr(S4, ConstantInt::get(Int32Ty, 17)));
        Value *S6 = B.CreateXor(S5, B.CreateShl(S5, ConstantInt::get(Int32Ty, 5)));
        Value *OpRaw = B.CreateURem(S6, ConstantInt::get(Int32Ty, 3));
        Value *Op = B.CreateTrunc(OpRaw, Int8Ty);

        Value *BytePtr = B.CreateGEP(Int8Ty, Info.GV, Idx);
        Value *ByteVal = B.CreateLoad(Int8Ty, BytePtr);

        SwitchInst *SW = B.CreateSwitch(Op, XorBB, 2);
        SW->addCase(cast<ConstantInt>(ConstantInt::get(Int8Ty, 1)), SubBB);
        SW->addCase(cast<ConstantInt>(ConstantInt::get(Int8Ty, 2)), AddBB);

        B.SetInsertPoint(XorBB);
        Value *ResXor = B.CreateXor(ByteVal, Key);
        B.CreateBr(StoreBB);

        B.SetInsertPoint(SubBB);
        Value *ResSub = B.CreateSub(ByteVal, Key);
        B.CreateBr(StoreBB);

        B.SetInsertPoint(AddBB);
        Value *ResAdd = B.CreateAdd(ByteVal, Key);
        B.CreateBr(StoreBB);

        B.SetInsertPoint(StoreBB);
        PHINode *Result = B.CreatePHI(Int8Ty, 3);
        Result->addIncoming(ResXor, XorBB);
        Result->addIncoming(ResSub, SubBB);
        Result->addIncoming(ResAdd, AddBB);
        B.CreateStore(Result, BytePtr);

        Value *NextIdx = B.CreateAdd(Idx, ConstantInt::get(Int32Ty, 1));
        Idx->addIncoming(NextIdx, StoreBB);
        State->addIncoming(S6, StoreBB);
        Value *Cont = B.CreateICmpULT(NextIdx, DecLen);
        B.CreateCondBr(Cont, LoopBB, ExitBB);

        B.SetInsertPoint(ExitBB);
        B.CreateRetVoid();
    }

    return DecryptFunc;
}

// ============================================================
// instrumentUseSites() — 在每个字符串使用点前插入解密调用
// ============================================================
void StringObfuscationPass::instrumentUseSites(
    Module &M, const std::vector<EncryptedStringInfo> &Entries) {

    for (const auto &Entry : Entries) {
        GlobalVariable *GV = Entry.GV;

        // 调用该字符串的专属解密函数
        auto EmitCall = [&](IRBuilder<> &B) {
            B.CreateCall(Entry.DecryptFunc);
        };

        // 递归收集所有 Instruction 级别的使用者
        SmallVector<Instruction *, 16> UseInsts;
        SmallVector<Value *, 16> Worklist;
        SmallPtrSet<Value *, 16> Visited;
        Worklist.push_back(GV);

        while (!Worklist.empty()) {
            Value *V = Worklist.pop_back_val();
            if (!Visited.insert(V).second)
                continue;

            for (User *U : V->users()) {
                if (auto *Inst = dyn_cast<Instruction>(U)) {
                    // 跳过解密函数自身内部的指令
                    if (Inst->getFunction() == Entry.DecryptFunc)
                        continue;
                    UseInsts.push_back(Inst);
                } else if (auto *GVUser = dyn_cast<GlobalVariable>(U)) {
                    StringRef Name = GVUser->getName();
                    if (Name == "llvm.global.annotations" ||
                        Name == "llvm.used" ||
                        Name == "llvm.compiler.used")
                        continue;
                    Worklist.push_back(GVUser);
                } else if (isa<Constant>(U)) {
                    Worklist.push_back(U);
                }
            }
        }

        // 在每个使用点前插入 call
        for (Instruction *Inst : UseInsts) {
            if (auto *PHI = dyn_cast<PHINode>(Inst)) {
                for (unsigned i = 0; i < PHI->getNumIncomingValues(); ++i) {
                    Value *InVal = PHI->getIncomingValue(i)->stripPointerCasts();
                    SmallPtrSet<Value *, 8> Seen;
                    SmallVector<Value *, 4> Stack;
                    Stack.push_back(InVal);
                    bool Related = false;
                    while (!Stack.empty() && !Related) {
                        Value *Cur = Stack.pop_back_val();
                        if (!Seen.insert(Cur).second) continue;
                        if (Cur == GV) { Related = true; break; }
                        if (auto *CE = dyn_cast<ConstantExpr>(Cur))
                            Stack.push_back(CE->getOperand(0));
                        else if (auto *GVRef = dyn_cast<GlobalVariable>(Cur)) {
                            if (GVRef->hasInitializer())
                                Stack.push_back(GVRef->getInitializer());
                        }
                    }
                    if (Related) {
                        BasicBlock *PredBB = PHI->getIncomingBlock(i);
                        IRBuilder<> B(PredBB->getTerminator());
                        EmitCall(B);
                    }
                }
            } else {
                IRBuilder<> B(Inst);
                EmitCall(B);
            }
        }
    }
}

} // namespace cocoons
