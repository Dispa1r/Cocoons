# Cocoons（作茧）项目规格说明书

> 作茧，不是自缚，每一根丝的加持，都是为了保护！

## 1. 项目概述

Cocoons 是一个基于 LLVM 的 iOS 代码混淆工具。它以 LLVM Pass Plugin（动态库）的形式独立编译，通过 `-fpass-plugin` 加载到编译流程中，在编译期对代码进行混淆处理，从而增加逆向工程的难度，保护 iOS 应用的安全。

- **仓库地址**: https://github.com/Dispa1r/Cocoons
- **当前版本**: v4.0.0（2026-03-06）
- **LLVM 版本**: 21.1.8
- **开源协议**: Apache 2.0

## 2. 功能清单

| 功能 | 状态 | 说明 |
|------|------|------|
| C/OC 字符串混淆 | ✅ 已实现 | 编译期加密字符串，运行时按需懒解密 |
| 指令替换 | ✅ 已实现 | 将简单算术指令替换为等价的复杂形式 |
| 控制流平坦化 | ⬜ 计划中 | 打乱函数控制流结构 |
| 反调试 | ⬜ 计划中 | 检测并阻止调试器附加 |

## 3. 技术栈

- **语言**: C++
- **编译器基础设施**: LLVM 21.1.8
- **构建系统**: CMake + Ninja
- **目标平台**: iOS（AArch64、ARM、X86）
- **集成方式**: Pass Plugin 动态库（`.dylib`），通过 `-fpass-plugin` 加载

## 4. 项目结构

```
Cocoons/
├── llvm/
│   ├── lib/Transforms/Cocoons/          # 核心混淆 Pass 实现
│   │   ├── CocoonsPasses.cpp            # Pass Plugin 入口（52 行）
│   │   ├── StringObfuscationPass.cpp    # 字符串混淆 Pass（593 行）
│   │   ├── SubstitutionPass.cpp         # 指令替换 Pass（75 行）
│   │   └── CMakeLists.txt               # 构建配置（19 行）
│   ├── include/llvm/Transforms/Cocoons/ # 头文件
│   │   ├── StringObfuscationPass.h      # 字符串混淆接口（58 行）
│   │   └── SubstitutionPass.h           # 指令替换接口（19 行）
│   ├── lib/Passes/                      # LLVM Pass 管线
│   │   ├── PassBuilderPipelines.cpp     # 已移除 Cocoons 硬编码注册
│   │   └── CMakeLists.txt               # 已移除 Cocoons 链接依赖
│   └── tools/                           # LLVM 工具链（clang, lld 等）
├── clang/                               # Clang 编译器前端
├── lld/                                 # LLVM 链接器
├── compiler-rt/                         # 编译器运行时库
├── tests/
│   └── test_string_obf_v2.m             # 字符串混淆验证 demo（67 行）
├── build.sh                             # 构建脚本
├── spec.md                              # 本文档
├── README.md                            # 项目文档
├── Documentation/
│   └── RELEASE_NOTE_CN.md               # 更新日志
└── LICENSE.TXT                          # Apache 2.0 许可证
```

## 5. 架构：Pass Plugin 模式

### 5.1 为什么用 Pass Plugin

v1/v2 时代 Cocoons 作为 LLVM 组件库（`add_llvm_component_library(LLVMCocoons ...)`）静态链接到 clang 中，每次修改 Pass 代码都需要重新编译整个 clang（耗时数十分钟）。

v3 改为 Pass Plugin 动态库（`add_llvm_pass_plugin(CocoonsPasses ...)`），只需编译 Pass 本身（几秒钟），通过 `-fpass-plugin=CocoonsPasses.dylib` 动态加载。

### 5.2 Plugin 入口（CocoonsPasses.cpp）

```cpp
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "CocoonsPasses", "3.0.0", registerCocoonsPasses};
}
```

`registerCocoonsPasses` 注册两类回调：

1. **OptimizerLastEPCallback** — 在优化管线末尾自动注入 Pass（需 `-O1` 及以上）
2. **PipelineParsingCallback** — 支持 `opt` 工具通过名字加载（`cocoons-str-obf`、`cocoons-sub`）

### 5.3 CMake 构建配置

```cmake
add_llvm_pass_plugin(CocoonsPasses
  CocoonsPasses.cpp
  StringObfuscationPass.cpp
  SubstitutionPass.cpp
  ...
)

# macOS 上用 -undefined dynamic_lookup 让 dylib 在加载时从宿主进程解析 LLVM 符号
if(APPLE)
  target_link_options(CocoonsPasses PRIVATE -undefined dynamic_lookup)
endif()
```

关键点：不显式链接 `LLVMCore`/`LLVMSupport` 等静态库，避免 `cl::opt` 重复注册导致的 `Option registered more than once` 崩溃。dylib 在运行时从宿主进程（clang）解析 LLVM 符号。

### 5.4 开关机制

由于 `-mllvm` 选项在 Pass Plugin dylib 加载之前就已被解析，`cl::opt` 变量来不及注册。因此 `isEnabled()` 同时支持两种方式：

| 方式 | 适用场景 | 示例 |
|------|----------|------|
| `cl::opt` | 静态链接到 clang | `-mllvm -cocoons-enable-str` |
| 环境变量 | Pass Plugin 动态加载 | `COCOONS_ENABLE_STR=1` |

```cpp
bool StringObfuscationPass::isEnabled() {
    if (EnableStrObf) return true;                          // cl::opt
    if (const char *Env = std::getenv("COCOONS_ENABLE_STR"))
        return std::string(Env) == "1";                     // 环境变量
    return false;
}
```

## 6. 核心模块详解

### 6.1 字符串混淆 Pass（StringObfuscationPass）

命名空间: `cocoons`
类型: Module Pass（`PassInfoMixin<StringObfuscationPass>`）
启用方式: `COCOONS_ENABLE_STR=1`（环境变量）或 `-mllvm -cocoons-enable-str`（静态链接）

#### 6.1.1 整体工作流程（v4）

```
run()
  ├── 1. 扫描 llvm.global.annotations → 收集 obfuscate 标记的全局变量
  ├── 2. 对每个字符串：
  │     ├── encryptRealData(M, GV) → 随机选模式加密字符串
  │     ├── createDecryptFunctionForString(M, Info) → 为该字符串创建独立解密函数
  │     └── createDummyFunctions(M, 1-3) → 插入随机数量 dummy 函数混淆布局
  └── 3. instrumentUseSites(M, Entries) → 在每个使用点前插入解密调用
```

**v4 核心改进**：
- 每个字符串拥有独立的随机命名解密函数（如 `__ccs_2353710051`）
- 解密函数之间插入 1-3 个 dummy 函数（如 `__dummy_188348847`）
- 消除共享解密函数，防止攻击者 hook 单一入口点
- 函数混合布局，难以批量识别

#### 6.1.2 标记识别与变量解析

扫描 `llvm.global.annotations`，查找带有 `__attribute__((annotate("obfuscate")))` 注解的全局变量。`processVariable()` 递归遍历变量引用，定位实际的 i8 字节数组，支持：

- 直接字节数组（i8 序列）
- Objective-C `NSConstantString` 结构体（提取第 3 个成员）
- C 字符串指针引用
- 复杂常量表达式（GEP 操作）

#### 6.1.3 双模式加密（encryptRealData）

编译时为每个字符串随机选择两种加密模式之一（50/50）：

**Mode A — Key Array（密钥数组）**

- 为每个字节生成独立的随机 key（1-255）和随机 op（0=XOR, 1=ADD, 2=SUB）
- key 序列和 op 序列分别存储为 `[N x i8]` 类型的 internal GlobalVariable
- 优点：密钥完全随机，安全性最高
- 缺点：二进制体积增加 2× 字符串长度

**Mode B — Seed + PRNG（种子派生）**

- 为每个字符串生成一个 `uint32_t` 随机种子
- 解密时用 xorshift32 PRNG 从种子逐字节派生 key 和 op
- 优点：只需存储 4 字节种子，体积极小
- 缺点：PRNG 可被逆向推导，但需要先识别算法

**Per-byte 加密逻辑**（两种模式共用）：

```
编译时加密：                    运行时解密：
op=0: encrypted = plain ^ key   decrypted = encrypted ^ key
op=1: encrypted = plain + key   decrypted = encrypted - key
op=2: encrypted = plain - key   decrypted = encrypted + key
```

末尾 `\0` 不加密，保证 printf 兼容性。加密后字符串留在默认 `__DATA` 段，与普通数据混在一起。

#### 6.1.4 Per-String 独立解密函数（createDecryptFunctionForString）

**v4 架构**：每个字符串生成独立的解密函数，无需传参，所有参数硬编码在函数内部。

**函数签名**：
```cpp
void __ccs_<random_hash>()  // 例如：__ccs_2353710051
```

**随机命名**：使用 `std::random_device` 生成 32 位随机数作为函数名后缀，每次编译不同。

**Mode A 解密函数（key/op 数组）**：
```
entry:
    cmpxchg Info.GuardGV, 0 → 1 (acq_rel/monotonic)
    br was_zero → do_decrypt / exit
do_decrypt:
    dec_len = Info.Len - 1
    br (dec_len > 0) → loop / exit
loop:
    key = load Info.KeyArrayGV[idx]
    op = load Info.OpArrayGV[idx]
    byte = load Info.GV[idx]
    switch op:
        0 → res = byte ^ key
        1 → res = byte - key
        2 → res = byte + key
    store res → Info.GV[idx]
    idx++
    br (idx < dec_len) → loop / exit
exit:
    ret void
```

**Mode B 解密函数（seed + PRNG）**：
```
entry:
    cmpxchg Info.GuardGV, 0 → 1
    br was_zero → do_decrypt / exit
do_decrypt:
    dec_len = Info.Len - 1
    br (dec_len > 0) → loop / exit
loop:
    state = PHI(Info.Seed, next_state)
    // xorshift32 生成 key
    s1 = state ^ (state << 13)
    s2 = s1 ^ (s1 >> 17)
    s3 = s2 ^ (s2 << 5)
    key = (s3 & 0xFF) ?: 1
    // xorshift32 生成 op
    s4 = s3 ^ (s3 << 13)
    s5 = s4 ^ (s4 >> 17)
    s6 = s5 ^ (s5 << 5)
    op = s6 % 3
    // 解密
    byte = load Info.GV[idx]
    switch op: ...
    next_state = s6
    idx++
    br (idx < dec_len) → loop / exit
exit:
    ret void
```

**关键特性**：
- `InternalLinkage`：函数仅在当前编译单元可见
- 无参数：所有数据（地址、长度、密钥、guard）硬编码
- 原子 guard：`cmpxchg` 保证多线程安全的懒解密
- 随机函数名：每次编译生成不同名称

#### 6.1.5 Dummy 函数混淆布局（createDummyFunctions）

为防止解密函数连续布局被批量识别，在每个解密函数后插入 1-3 个随机 dummy 函数。

**Dummy 函数特征**：
```cpp
i32 __dummy_<random_hash>(i32 a, i32 b) {
    return a * b + (a ^ b);
}
```

**实现细节**：
- 随机函数名：使用 `std::random_device` 生成 32 位随机数
- 简单计算逻辑：乘法 + 异或，看起来像正常代码
- 添加到 `llvm.used`：防止优化器删除未使用的函数
- 随机数量：每个解密函数后插入 1-3 个（均匀分布）

**布局效果**：
```
0x3a14  __ccs_1039492916      ← 解密函数
0x3560  __dummy_188348847     ← dummy
0x356c  __dummy_813738223     ← dummy
0x3578  __dummy_3062846195    ← dummy
0x3584  __ccs_564746404       ← 解密函数
0x3628  __dummy_2484314989    ← dummy
0x3634  __dummy_3749835815    ← dummy
0x3640  __ccs_2754162014      ← 解密函数
```

攻击者无法通过地址范围或函数名模式批量定位解密函数。

#### 6.1.6 使用点插桩（instrumentUseSites）
#### 6.1.6 使用点插桩（instrumentUseSites）

递归遍历每个加密字符串的 user 链（worklist 算法），收集所有 Instruction 级别的使用者：

```
GV → ConstantExpr → GlobalVariable → ... → Instruction
```

跳过规则：
- `llvm.global.annotations`、`llvm.used`、`llvm.compiler.used` 等元数据
- 解密函数自身内部的指令（`Entry.DecryptFunc`）

在每个使用点前插入解密调用：
```cpp
call Entry.DecryptFunc()  // 无参数，直接调用
```

PHI 节点特殊处理：在对应前驱块的 terminator 前插入 call。

#### 6.1.7 关键实现细节

- 加密后字符串标记为非常量（`setConstant(false)`），清除 section（`setSection("")`），留在默认 `__DATA` 段
- 每个字符串独立的解密函数使用 `InternalLinkage`，仅当前编译单元可见
- 使用 `cmpxchg` 原子操作（`AcquireRelease` / `Monotonic`）保证线程安全
- 每次编译产生不同的加密结果和函数名（`std::random_device` 驱动）
- xorshift32 PRNG 编译时（C++ 函数）和运行时（IR 生成）使用完全相同的算法

#### 6.1.8 数据结构

```cpp
struct EncryptedStringInfo {
    GlobalVariable *GV;                    // 加密后的字符串全局变量
    uint32_t Len;                          // 字符串长度（含 \0）
    GlobalVariable *GuardGV;               // per-string guard 变量（i8, init 0）
    Function *DecryptFunc;                 // 该字符串的独立解密函数
    bool UsePRNG;                          // true = Mode B, false = Mode A
    GlobalVariable *KeyArrayGV = nullptr;  // Mode A: [N x i8] per-byte key 数组
    GlobalVariable *OpArrayGV  = nullptr;  // Mode A: [N x i8] per-byte op 数组
    uint32_t Seed = 0;                     // Mode B: PRNG seed
};
```
    GlobalVariable *OpArrayGV  = nullptr;  // Mode A: [N x i8] per-byte op 数组
    uint32_t Seed = 0;                     // Mode B: PRNG seed
};
```

### 6.2 指令替换 Pass（SubstitutionPass）

命名空间: `cocoons`
类型: Function Pass（`PassInfoMixin<SubstitutionPass>`）
启用方式: `COCOONS_ENABLE_SUB=1`（环境变量）或 `-mllvm -cocoons-enable-sub`（静态链接）

#### 工作流程

1. **扫描**: 遍历函数中所有基本块，收集全部 `BinaryOperator` 指令
2. **替换**: 对整数加法指令执行等价变换：
   - 原始: `a + b`
   - 变换: `(a ^ b) + 2 * (a & b)`
3. **清理**: 移除已无引用的原始指令

#### 数学等价性证明

`a + b = (a ^ b) + 2 * (a & b)` 成立的原因：
- `a ^ b` 计算不进位加法（每一位独立相加，不考虑进位）
- `a & b` 找出所有需要进位的位
- `2 * (a & b)` 将进位左移一位
- 两者相加即为完整的加法结果

## 7. 构建与使用

### 7.1 构建 Plugin

```bash
cd Cocoons/build

# 配置（只需一次）
cmake -G Ninja ../llvm \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang" \
  -DLLVM_TARGETS_TO_BUILD="AArch64;ARM;X86"

# 编译 Plugin（几秒钟）
ninja CocoonsPasses

# 产物：build/lib/CocoonsPasses.dylib（~95KB）
```

### 7.2 使用 Plugin 编译

```bash
# 启用字符串混淆
COCOONS_ENABLE_STR=1 clang -O1 \
  -isysroot $(xcrun --show-sdk-path) \
  -fpass-plugin=/path/to/CocoonsPasses.dylib \
  -framework Foundation \
  source.m -o output

# 启用指令替换
COCOONS_ENABLE_SUB=1 clang -O1 \
  -fpass-plugin=/path/to/CocoonsPasses.dylib \
  source.m -o output

# 同时启用两者
COCOONS_ENABLE_STR=1 COCOONS_ENABLE_SUB=1 clang -O1 \
  -fpass-plugin=/path/to/CocoonsPasses.dylib \
  -framework Foundation \
  source.m -o output
```

注意：需要 `-O1` 及以上优化级别，`-O0` 时 Pass 不会运行。

### 7.3 标记需要混淆的字符串

```objective-c
__attribute__((annotate("obfuscate")))
const char c_array[] = "C Array Hello World!";

__attribute__((annotate("obfuscate")))
const char *c_string = "C String Hello World!";

__attribute__((annotate("obfuscate")))
NSString *ocString = @"OC String Hello World!";
```

未标记的字符串不受影响，保持明文。

### 7.4 验证混淆效果

```bash
# 验证加密字符串不可读
strings output | grep "Hello"  # 应无输出

# 验证无自定义段
otool -l output | grep __obf_strings    # 应无输出
otool -l output | grep __cocoons        # 应无输出

# 验证运行正确
./output  # 所有字符串正常输出

# 验证随机性（两次编译产生不同二进制）
md5 output1 output2  # MD5 应不同
```

### 7.5 构建完整 Toolchain（可选）

如果需要安装为 Xcode Toolchain（包含 clang + plugin）：

```bash
cmake -G Ninja ../llvm \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_CREATE_XCODE_TOOLCHAIN=ON \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_ENABLE_RUNTIMES="compiler-rt" \
  -DLLVM_TARGETS_TO_BUILD="AArch64;ARM;X86" \
  -DAPPLE_CONFIG_SANS_IPHONEOS=ON

ninja install-xcode-toolchain
# 安装至 ~/Library/Developer
```

## 8. 源文件清单

| 文件 | 类型 | 行数 | 说明 |
|------|------|------|------|
| `llvm/lib/Transforms/Cocoons/CocoonsPasses.cpp` | 实现 | 52 | Pass Plugin 入口，注册回调 |
| `llvm/lib/Transforms/Cocoons/StringObfuscationPass.cpp` | 实现 | 500+ | 字符串混淆核心逻辑（v4 per-string 独立解密） |
| `llvm/lib/Transforms/Cocoons/SubstitutionPass.cpp` | 实现 | 75 | 指令替换核心逻辑 |
| `llvm/include/llvm/Transforms/Cocoons/StringObfuscationPass.h` | 头文件 | 59 | 字符串混淆 Pass 接口定义 |
| `llvm/include/llvm/Transforms/Cocoons/SubstitutionPass.h` | 头文件 | 19 | 指令替换 Pass 接口定义 |
| `llvm/lib/Transforms/Cocoons/CMakeLists.txt` | 构建 | 19 | Pass Plugin 构建配置 |
| `tests/test_string_obf_v2.m` | 测试 | 67 | 字符串混淆验证 demo |
| `tests/test_50_strings.c` | 测试 | 68 | 50 个字符串混淆效果验证 |
| `build.sh` | 脚本 | 50 | 编译与安装脚本 |

## 9. 版本历史

| 版本 | 发布时间 | 变更内容 |
|------|----------|----------|
| v1.0.0 | 2026-02-09 | 初始版本：C/OC 字符串混淆（启动时批量解密）、加法指令替换 |
| v1.0.1 | 2026-02-10 | 修复字符串混淆在多 module 下的 bug |
| v2.0.0 | 2026-02-25 | 字符串混淆升级为懒解密：按需解密、cmpxchg 线程安全、加密字符串留在默认 `__DATA` 段 |
| v3.0.0 | 2026-02-26 | 每字节独立密钥 + 随机加密方式（XOR/ADD/SUB）+ 双模式（key 数组 / seed+PRNG）；改为 Pass Plugin 动态库架构 |
| v4.0.0 | 2026-03-06 | **Per-string 独立解密函数**：每个字符串生成独立的随机命名解密函数，消除共享解密函数；插入 dummy 函数混淆布局，防止批量识别 |

## 10. v4 架构详解：Per-String 独立解密

### 10.1 设计动机

**v3 的安全问题**：
- 所有字符串共享两个解密函数（`__cocoons_decrypt_a` 和 `__cocoons_decrypt_b`）
- 攻击者只需 hook 这两个函数即可拦截所有字符串解密
- 解密函数名称固定，容易被静态分析识别

**v4 的改进**：
- 每个字符串生成独立的解密函数，随机命名
- 攻击者无法通过 hook 单一入口点批量拦截
- 解密函数与 dummy 函数混合布局，难以批量识别

### 10.2 核心技术

#### 10.2.1 随机函数命名

```cpp
std::random_device RD;
std::mt19937 Gen(RD());
std::uniform_int_distribution<uint32_t> Dist(0, 0xFFFFFFFF);
uint32_t Hash = Dist(Gen);
std::string FuncName = "__ccs_" + std::to_string(Hash);
```

每次编译生成不同的函数名，如 `__ccs_2353710051`、`__ccs_564746404`。

#### 10.2.2 参数硬编码

解密函数无参数，所有数据直接引用全局变量：
- 字符串地址：`Info.GV`
- 字符串长度：`Info.Len`（编译期常量）
- 密钥数组：`Info.KeyArrayGV` / `Info.OpArrayGV`（Mode A）
- PRNG 种子：`Info.Seed`（Mode B，编译期常量）
- Guard 变量：`Info.GuardGV`

#### 10.2.3 Dummy 函数混淆

每个解密函数后插入 1-3 个 dummy 函数：

```cpp
std::uniform_int_distribution<int> DummyCount(1, 3);
createDummyFunctions(M, DummyCount(Gen));
```

Dummy 函数特征：
- 随机命名：`__dummy_<random_hash>`
- 简单计算：`i32 func(i32 a, i32 b) { return a * b + (a ^ b); }`
- 添加到 `llvm.used`：防止被优化删除

### 10.3 安全性分析

| 攻击方式 | v3 防御能力 | v4 防御能力 |
|---------|------------|------------|
| Hook 共享解密函数 | ❌ 两个函数即可拦截所有字符串 | ✅ 需要 hook N 个独立函数 |
| 静态分析识别解密函数 | ❌ 固定函数名 `__cocoons_decrypt_a/b` | ✅ 随机函数名，每次编译不同 |
| 批量定位解密函数 | ❌ 函数连续布局 | ✅ 与 dummy 函数混合，难以批量识别 |
| 逆向单个字符串 | ✅ 需要分析 PRNG 或密钥数组 | ✅ 同 v3 |
| 内存 dump | ✅ 运行时解密后可见 | ✅ 同 v3 |

### 10.4 性能影响

- **二进制体积**：每个字符串增加约 100-200 字节（解密函数 IR）+ 1-3 个 dummy 函数（每个约 12 字节）
- **运行时开销**：与 v3 相同，懒解密 + cmpxchg guard
- **编译时间**：略有增加（生成更多函数），但仍在秒级

### 10.5 使用示例

```bash
# 编译
COCOONS_ENABLE_STR=1 build/bin/clang -O1 \
  -isysroot $(xcrun --show-sdk-path) \
  -fpass-plugin=build/lib/CocoonsPasses.dylib \
  tests/test_50_strings.c -o test_50_strings

# 验证：字符串已加密
strings test_50_strings | grep "String"  # 无输出

# 验证：函数混合布局
nm test_50_strings | grep -E "(__ccs_|__dummy_)" | sort
# 输出示例：
# 0x3a14  __ccs_1039492916
# 0x3560  __dummy_188348847
# 0x356c  __dummy_813738223
# 0x3578  __dummy_3062846195
# 0x3584  __ccs_564746404
# ...

# 验证：程序正常运行
./test_50_strings
# String 1
# String 2
# ...
```

### 10.6 未来改进方向

1. **控制流混淆**：对解密函数内部进行控制流平坦化
2. **虚假控制流**：插入永不执行的分支迷惑静态分析
3. **多态解密**：同一字符串生成多个等价解密函数，随机选择
4. **反调试检测**：在解密函数中插入反调试代码
5. **代码虚拟化**：将解密逻辑编译为自定义虚拟机字节码
